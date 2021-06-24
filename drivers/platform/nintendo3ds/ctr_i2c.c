// SPDX-License-Identifier: GPL-2.0
/*
 *  ctr_i2c.c
 *
 *  Copyright (C) 2020-2021 Santiago Herrera
 */

#define DRIVER_NAME "3ds-i2c"
#define pr_fmt(fmt) DRIVER_NAME ": " fmt

#include <linux/io.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>

/* CNT register bits */
#define I2C_CNT_LAST	BIT(0)
#define I2C_CNT_START	BIT(1)
#define I2C_CNT_PAUSE	BIT(2)
#define I2C_CNT_ERRACK	BIT(4)
#define I2C_CNT_WRITE	(0)
#define I2C_CNT_READ	BIT(5)
#define I2C_CNT_IRQEN	BIT(6)
#define I2C_CNT_BUSY	BIT(7)

#define CTR_I2C_TIMEOUT	msecs_to_jiffies(100)

struct ctr_i2c {
	unsigned irq;
	void __iomem *base;

	wait_queue_head_t wq;
	struct i2c_adapter adap;
};

static u8 ctr_i2c_read_data(struct ctr_i2c *i2c)
{
	return ioread8(i2c->base + 0x00);
}

static u8 ctr_i2c_read_cnt(struct ctr_i2c *i2c)
{
	return ioread8(i2c->base + 0x01);
}

static void ctr_i2c_write_data(struct ctr_i2c *i2c, u8 val)
{
	iowrite8(val, i2c->base + 0x00);
}

static void ctr_i2c_write_cnt(struct ctr_i2c *i2c, u8 val)
{
	iowrite8(val, i2c->base + 0x01);
}

static void ctr_i2c_write_cntex(struct ctr_i2c *i2c, u16 cntex)
{
	iowrite16(cntex, i2c->base + 0x02);
}

static void ctr_i2c_write_scl(struct ctr_i2c *i2c, u16 scl)
{
	iowrite16(scl, i2c->base + 0x04);
}

static int ctr_i2c_wait_busy(struct ctr_i2c *i2c)
{
	long res = wait_event_interruptible_timeout(
		i2c->wq, !(ctr_i2c_read_cnt(i2c) & I2C_CNT_BUSY),
		CTR_I2C_TIMEOUT);

	if (res > 0)
		return 0;
	if (res == 0)
		return -ETIMEDOUT;
	return res;
}

static int ctr_i2c_send(struct ctr_i2c *i2c, u8 byte, unsigned flags)
{
	ctr_i2c_write_data(i2c, byte);
	ctr_i2c_write_cnt(i2c,
			  I2C_CNT_BUSY | I2C_CNT_IRQEN | I2C_CNT_WRITE | flags);
	return ctr_i2c_wait_busy(i2c);
}

static int ctr_i2c_recv(struct ctr_i2c *i2c, u8 *byte, unsigned flags)
{
	int err;
	ctr_i2c_write_cnt(i2c,
			  I2C_CNT_BUSY | I2C_CNT_IRQEN | I2C_CNT_READ | flags);
	err = ctr_i2c_wait_busy(i2c);
	*byte = ctr_i2c_read_data(i2c);
	return err;
}

static int ctr_i2c_select_device(struct ctr_i2c *i2c, struct i2c_msg *msg)
{
	return ctr_i2c_send(i2c, i2c_8bit_addr_from_msg(msg), I2C_CNT_START);
}

static int ctr_i2c_msg_read(struct ctr_i2c *i2c, u8 *buf, int len, bool last)
{
	int i;
	for (i = 0; i < len; i++) {
		unsigned flag = (last && (i == (len - 1))) ? I2C_CNT_LAST :
							     I2C_CNT_ERRACK;
		if (ctr_i2c_recv(i2c, &buf[i], flag))
			return i;
	}
	return len;
}

static int ctr_i2c_msg_write(struct ctr_i2c *i2c, u8 *buf, int len, bool last)
{
	int i;
	for (i = 0; i < len; i++) {
		unsigned flag = (last && (i == (len - 1))) ? I2C_CNT_LAST : 0;
		if (ctr_i2c_send(i2c, buf[i], flag))
			return i;

		if (!(ctr_i2c_read_cnt(i2c) & I2C_CNT_ERRACK)) {
			ctr_i2c_write_cnt(i2c, I2C_CNT_BUSY | I2C_CNT_IRQEN |
						       I2C_CNT_PAUSE |
						       I2C_CNT_WRITE);
			return i;
		}
	}
	return len;
}

static int ctr_i2c_master_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
			       int num)
{
	int i, plen;
	struct i2c_msg *msg;
	struct ctr_i2c *i2c = container_of(adap, struct ctr_i2c, adap);

	if (!num)
		return 0;

	for (i = 0; i < num; i++) {
		msg = &msgs[i];

		if (!(msg->flags & I2C_M_NOSTART))
			ctr_i2c_select_device(i2c, msg);

		if (msg->len != 0) {
			bool last = i == (num - 1);
			if (msg->flags & I2C_M_RD) {
				plen = ctr_i2c_msg_read(i2c, msg->buf, msg->len,
							last);
			} else {
				plen = ctr_i2c_msg_write(i2c, msg->buf,
							 msg->len, last);
			}

			if (plen != msg->len)
				break;
		}

		msg++;
	}

	return i;
}

static u32 ctr_i2c_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_NOSTART;
}

static const struct i2c_algorithm ctr_i2c_algo = {
	.master_xfer	= ctr_i2c_master_xfer,
	.functionality	= ctr_i2c_functionality,
};

static irqreturn_t ctr_i2c_irq(int irq, void *data)
{
	struct ctr_i2c *i2c = data;
	wake_up_interruptible(&i2c->wq);
	return IRQ_HANDLED;
}

static int ctr_i2c_probe(struct platform_device *pdev)
{
	int err;
	struct device *dev;
	struct ctr_i2c *i2c;
	struct i2c_adapter *adap;

	dev = &pdev->dev;

	i2c = devm_kzalloc(dev, sizeof(*i2c), GFP_KERNEL);
	if (IS_ERR(i2c))
		return PTR_ERR(i2c);

	init_waitqueue_head(&i2c->wq);
	adap = &i2c->adap;

	i2c->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(i2c->base))
		return PTR_ERR(i2c->base);

	i2c->irq = platform_get_irq(pdev, 0);
	if (!i2c->irq)
		return -EINVAL;

	err = devm_request_irq(dev, i2c->irq, ctr_i2c_irq, 0, dev_name(dev),
			       i2c);
	if (err)
		return err;

	/* hardware sanity reset */
	ctr_i2c_write_cnt(i2c, 0);
	ctr_i2c_write_cntex(i2c, BIT(1));
	ctr_i2c_write_scl(i2c, 5 << 8);

	/* setup the i2c_adapter */
	adap->owner	= THIS_MODULE;
	strlcpy(adap->name, dev_name(dev), sizeof(adap->name));
	adap->dev.parent	= dev;
	adap->dev.of_node	= dev->of_node;
	adap->algo	= &ctr_i2c_algo;
	adap->algo_data	= i2c;

	dev_set_drvdata(dev, i2c);
	return i2c_add_adapter(&i2c->adap);
}

static int ctr_i2c_remove(struct platform_device *pdev)
{
	struct ctr_i2c *i2c = platform_get_drvdata(pdev);
	i2c_del_adapter(&i2c->adap);
	return 0;
}

static const struct of_device_id ctr_i2c_of_match[] = {
	{ .compatible = "nintendo," DRIVER_NAME },
	{},
};
MODULE_DEVICE_TABLE(of, ctr_i2c_of_match);

static struct platform_driver ctr_i2c_driver = {
	.probe		= ctr_i2c_probe,
	.remove		= ctr_i2c_remove,

	.driver		= {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(ctr_i2c_of_match),
	},
};

module_platform_driver(ctr_i2c_driver);

MODULE_DESCRIPTION("Nintendo 3DS I2C bus driver");
MODULE_AUTHOR("Santiago Herrera");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
