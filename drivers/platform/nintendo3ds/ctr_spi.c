// SPDX-License-Identifier: GPL-2.0
/*
 *  ctr_spi.c
 *
 *  Copyright (C) 2016 Sergi Granell
 *  Copyright (C) 2019-2021 Santiago Herrera
 */

#define DRIVER_NAME "3ds-spi"
#define pr_fmt(str) DRIVER_NAME ": " str

#include <linux/io.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/spi/spi.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/workqueue.h>
#include <linux/platform_device.h>

struct ctr_spi {
	u32 cs;
	void __iomem *base;

	struct spi_master *master;
	wait_queue_head_t wq;
};

/* CNT register bits */
#define SPI_CNT_CHIPSELECT(n)	((n) << 6)
#define SPI_CNT_XFER_READ	(0 << 13)
#define SPI_CNT_XFER_WRITE	(1 << 13)
#define SPI_CNT_BUSY		BIT(15)
#define SPI_CNT_ENABLE		BIT(15)

#define SPI_FIFO_BUSY	BIT(0)
#define SPI_FIFO_WIDTH	0x20

#define CTR_SPI_TIMEOUT	msecs_to_jiffies(100)

static u32 ctr_spi_freq_to_rate(u32 freq)
{
	/*
	 0 -> 512KHz
	 1 -> 1MHz
	 2 -> 2MHz
	 3 -> 4MHz
	 4 -> 8MHz
	 5,6,7 -> 16MHz
	*/
	return min(ilog2(max_t(u32, freq, 1 << 19) >> 19), 5);
}

static void ctr_spi_write_cnt(struct ctr_spi *spi, u32 cnt)
{
	iowrite32(cnt, spi->base + 0x00);
}

static u32 ctr_spi_read_cnt(struct ctr_spi *spi)
{
	return ioread32(spi->base + 0x00);
}

static void ctr_spi_write_blklen(struct ctr_spi *spi, u32 len)
{
	iowrite32(len, spi->base + 0x08);
}

static u32 ctr_spi_read_fifo(struct ctr_spi *spi)
{
	return ioread32(spi->base + 0x0C);
}

static void ctr_spi_write_fifo(struct ctr_spi *spi, u32 data)
{
	iowrite32(data, spi->base + 0x0C);
}

static u32 ctr_spi_read_status(struct ctr_spi *spi)
{
	return ioread32(spi->base + 0x10);
}

static int ctr_spi_wait_busy(struct ctr_spi *spi)
{
	long res = wait_event_interruptible_timeout(
		spi->wq, !(ctr_spi_read_cnt(spi) & SPI_CNT_BUSY),
		CTR_SPI_TIMEOUT);

	if (res > 0)
		return 0;
	if (res == 0)
		return -ETIMEDOUT;
	return res;
}

static int ctr_spi_wait_fifo(struct ctr_spi *spi)
{
	while (ctr_spi_read_status(spi) & SPI_FIFO_BUSY)
		usleep_range(1, 5);
	return 0;
}

static int ctr_spi_done(struct ctr_spi *spi)
{
	int err = ctr_spi_wait_busy(spi);
	if (err)
		return err;
	iowrite32(0, spi->base + 0x04);
	return 0;
}

static void ctr_spi_setup_xfer(struct ctr_spi *spidrv, struct spi_device *spi,
			       bool read)
{
	ctr_spi_write_cnt(
		spidrv,
		SPI_CNT_ENABLE | ctr_spi_freq_to_rate(spi->max_speed_hz) |
			SPI_CNT_CHIPSELECT(spidrv->cs) |
			(read ? SPI_CNT_XFER_READ : SPI_CNT_XFER_WRITE));
}

static int ctr_spi_xfer_read(struct ctr_spi *spibus, u32 *buf, u32 len)
{
	u32 idx = 0;
	do {
		int err = ctr_spi_wait_fifo(spibus);
		if (err)
			return err;
		*(buf++) = ctr_spi_read_fifo(spibus);
		idx += 4;
	} while (idx < len);

	return 0;
}

static int ctr_spi_xfer_write(struct ctr_spi *spibus, u32 *buf, u32 len)
{
	u32 idx = 0;
	do {
		int err = ctr_spi_wait_fifo(spibus);
		if (err)
			return err;
		ctr_spi_write_fifo(spibus, *(buf++));
		idx += 4;
	} while (idx < len);

	return 0;
}

static void ctr_spi_set_cs(struct spi_device *spi, bool enable)
{
	struct ctr_spi *spidrv = spi_master_get_devdata(spi->master);
	spidrv->cs = spi->chip_select;
}

static int ctr_spi_transfer_one(struct spi_master *master,
				struct spi_device *spi,
				struct spi_transfer *xfer)
{
	int err;
	bool read;
	u32 *buf, len;
	struct ctr_spi *spidrv = spi_master_get_devdata(master);

	len = xfer->len;

	if (xfer->tx_buf) {
		read = false;
		buf = (u32*)xfer->tx_buf;
	} else if (xfer->rx_buf) {
		read = true;
		buf = (u32*)xfer->rx_buf;
	} else {
		return -EINVAL;
	}

	err = ctr_spi_wait_busy(spidrv);
	if (err)
		return err;

	ctr_spi_write_blklen(spidrv, len); /* set up BLKLEN */
	ctr_spi_setup_xfer(spidrv, spi, read); /* set up CNT */

	/* begin xfer */
	if (read) {
		err = ctr_spi_xfer_read(spidrv, buf, len);
	} else {
		err = ctr_spi_xfer_write(spidrv, buf, len);
	}
	if (err)
		return err;

	if (spi_transfer_is_last(master, xfer))
		ctr_spi_done(spidrv);
	spi_finalize_current_transfer(master);
	return 0;
}

static size_t ctr_spi_max_transfer_size(struct spi_device *spi)
{
	return 2097152; /* BLKLEN is 21 bits wide, so that's a hard limit */
}

static irqreturn_t ctr_spi_irq(int irq, void *data)
{
	struct ctr_spi *spi = data;
	wake_up_interruptible(&spi->wq);
	return IRQ_HANDLED;
}

static int ctr_spi_probe(struct platform_device *pdev)
{
	int err;
	struct device *dev;
	struct ctr_spi *spi;
	struct spi_master *master;

	dev = &pdev->dev;

	master = devm_spi_alloc_master(dev, sizeof(*spi));
	if (IS_ERR(master))
		return PTR_ERR(master);

	platform_set_drvdata(pdev, master);
	spi = spi_master_get_devdata(master);

	init_waitqueue_head(&spi->wq);

	/* set up the spi device structure */
	spi->master = master;
	master->bus_num	= pdev->id;
	master->set_cs	= ctr_spi_set_cs;
	master->transfer_one = ctr_spi_transfer_one;
	master->max_transfer_size = ctr_spi_max_transfer_size;
	master->num_chipselect = 3;
	master->bits_per_word_mask = SPI_BPW_MASK(8);
	master->flags = SPI_MASTER_HALF_DUPLEX;
	master->dev.of_node = dev->of_node;

	spi->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(spi->base))
		return PTR_ERR(spi->base);

	/* stop any running transfer */
	ctr_spi_write_cnt(spi, 0);

	/* enable only the transfer done interrupt */
	iowrite32(~BIT(0), spi->base + 0x18); // mask
	iowrite32(~0, spi->base + 0x1C); // acknowledge

	err = devm_request_irq(dev, platform_get_irq(pdev, 0), ctr_spi_irq, 0,
			       DRIVER_NAME, spi);
	if (err)
		return err;

	return devm_spi_register_master(dev, master);
}

static int ctr_spi_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id ctr_spi_of_match[] = {
	{ .compatible = "nintendo," DRIVER_NAME },
	{}
};
MODULE_DEVICE_TABLE(of, ctr_spi_of_match);

static struct platform_driver ctr_spi_driver = {
	.probe	= ctr_spi_probe,
	.remove	= ctr_spi_remove,

	.driver = {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
		.of_match_table	= of_match_ptr(ctr_spi_of_match),
	},
};
module_platform_driver(ctr_spi_driver);

MODULE_DESCRIPTION("Nintendo 3DS SPI bus driver");
MODULE_AUTHOR("Santiago Herrera");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
