// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Nintendo 3DS Secure Digital Host Controller driver
 *
 *  Copyright (C) 2021 Santiago Herrera
 *
 *  Based on toshsd.c, copyright (C) 2014 Ondrej Zary and 2007 Richard Betts
 */

#define DRIVER_NAME	"3ds-sdhc"
#define pr_fmt(fmt) DRIVER_NAME ": " fmt

#include <linux/io.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio.h>
#include <linux/dmaengine.h>
#include <linux/interrupt.h>
#include <linux/pm_runtime.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/dma-direction.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>

#include "ctr_sdhc.h"

enum {
	DMABUF_UNMAPPED = 1,
	DMABUF_MAPPED = 2,
};

#define SDHC_ERRMASK \
	(SDHC_ERR_BAD_CMD | SDHC_ERR_CRC_FAIL | SDHC_ERR_STOP_BIT | \
	 SDHC_ERR_DATATIMEOUT | SDHC_ERR_TX_OVERFLOW | SDHC_ERR_RX_UNDERRUN | \
	 SDHC_ERR_CMD_TIMEOUT | SDHC_ERR_ILLEGAL_ACC)

#define SDHC_IRQMASK \
	(SDHC_STAT_CARDREMOVE | SDHC_STAT_CARDINSERT | \
	 SDHC_STAT_DATA_END | \
	 SDHC_ERRMASK | SDHC_STAT_CMDRESPEND)

#define SDHC_DEFAULT_CARDOPT \
	(SDHC_CARD_OPTION_RETRIES(14) | \
	SDHC_CARD_OPTION_TIMEOUT(14) | \
	SDHC_CARD_OPTION_NOC2) // maximum timeouts and a magic bit

// freeze the CLK pin when inactive if running above 10MHz
#define SDHC_CLKFREEZE_THRESHOLD	(10000000)

#define ctr_sdhc_reg16_set(h, o, v)	iowrite16((v), (h)->regs + (o))
#define ctr_sdhc_reg32_set(h, o, v)	iowrite32((v), (h)->regs + (o))

#define ctr_sdhc_reg16_get(h, o)	ioread16((h)->regs + (o))
#define ctr_sdhc_reg32_get(h, o)	ioread32((h)->regs + (o))

static int ctr_sdhc_reset(struct ctr_sdhc *host)
{
	/* reset controller */
	ctr_sdhc_reg16_set(host, SDHC_SOFTRESET, 0);
	ctr_sdhc_reg16_set(host, SDHC_SOFTRESET, 1);

	/* clear registers */
	ctr_sdhc_reg16_set(host, SDHC_CARD_PORTSEL, 0);
	ctr_sdhc_reg16_set(host, SDHC_CARD_CLKCTL, 0);
	ctr_sdhc_reg32_set(host, SDHC_ERROR_STATUS, 0);
	ctr_sdhc_reg16_set(host, SDHC_STOP_INTERNAL, 0);

	ctr_sdhc_reg16_set(host, SDHC_DATA16_BLK_CNT, 0);
	ctr_sdhc_reg16_set(host, SDHC_DATA16_BLK_LEN, 0);

	ctr_sdhc_reg16_set(host, SDHC_DATA32_BLK_CNT, 0);
	ctr_sdhc_reg16_set(host, SDHC_DATA32_BLK_LEN, 0);

	/* always use the 32bit FIFO */
	ctr_sdhc_reg16_set(host, SDHC_DATA_CTL, SDHC_DATA_CTL_WORD_FIFO_EN);
	ctr_sdhc_reg16_set(host, SDHC_DATA32_CTL,
			   SDHC_DATA32_CTL_WORD_FIFO_EN |
			   SDHC_DATA32_CTL_WORD_FIFO_CLR);

	/* set interrupt masks */
	ctr_sdhc_reg32_set(host, SDHC_IRQ_MASK, ~SDHC_IRQMASK);
	ctr_sdhc_reg32_set(host, SDHC_IRQ_STAT, ~SDHC_IRQMASK);

	ctr_sdhc_reg16_set(host, SDHC_CARD_OPTION, SDHC_DEFAULT_CARDOPT);
	return 0;
}

static int ctr_sdhc_set_clk_opt(struct ctr_sdhc *host, u16 clk, u16 opt)
{
	ctr_sdhc_reg16_set(host, SDHC_CARD_CLKCTL, clk);
	ctr_sdhc_reg16_set(host, SDHC_CARD_OPTION, opt);
	return 0;
}

static int ctr_sdhc_send_cmdarg(struct ctr_sdhc *host, u16 cmd, u32 arg)
{
	ctr_sdhc_reg32_set(host, SDHC_CMD_PARAM, arg);
	ctr_sdhc_reg16_set(host, SDHC_CMD, cmd);
	return 0;
}

static int ctr_sdhc_set_blk_len_cnt(struct ctr_sdhc *host, u16 len, u16 cnt)
{
	ctr_sdhc_reg16_set(host, SDHC_DATA16_BLK_LEN, len);
	ctr_sdhc_reg16_set(host, SDHC_DATA16_BLK_CNT, cnt);

	ctr_sdhc_reg16_set(host, SDHC_DATA32_BLK_LEN, len);
	ctr_sdhc_reg16_set(host, SDHC_DATA32_BLK_CNT, cnt);
	return 0;
}

static int ctr_sdhc_get_resp(struct ctr_sdhc *host, u32 *resp, unsigned n)
{
	int i;
	for (i = 0; i < n; i++)
		resp[i] = ctr_sdhc_reg32_get(host, SDHC_CMD_RESPONSE + (i * 4));
	return 0;
}

static int ctr_sdhc_stop_internal_set(struct ctr_sdhc *host, u16 val)
{
	ctr_sdhc_reg16_set(host, SDHC_STOP_INTERNAL, val);
	return 0;
}

static u32 ctr_sdhc_irqstat_get(struct ctr_sdhc *host)
{
	return ctr_sdhc_reg32_get(host, SDHC_IRQ_STAT);
}

static int ctr_sdhc_irqstat_ack(struct ctr_sdhc *host, u32 ack)
{
	ctr_sdhc_reg32_set(host, SDHC_IRQ_STAT, ~ack);
	return 0;
}

static int ctr_sdhc_irqmask_set(struct ctr_sdhc *host, u32 mask)
{
	ctr_sdhc_reg32_set(host, SDHC_IRQ_MASK, mask);
	return 0;
}

static int ctr_sdhc_sdioirq_test(struct ctr_sdhc *host)
{
	u16 state = ctr_sdhc_reg16_get(host, SDHC_CARD_IRQ_STAT);

	if (state & 1) {
		/* acknowledge the SDIO IRQ */
		ctr_sdhc_reg16_set(host, SDHC_CARD_IRQ_STAT, state & ~1);
		return 1;
	}
	return 0;
}

static int ctr_sdhc_sdioirq_set(struct ctr_sdhc *host, int enable)
{
	/* always acknowledge the card interrupts first */
	ctr_sdhc_reg16_set(host, SDHC_CARD_IRQ_STAT, 0);

	/* either disable all interrupts _except_ SDIO IRQ, or disable all */
	ctr_sdhc_reg16_set(host, SDHC_CARD_IRQ_MASK, enable ? ~1 : ~0);

	return 0;
}

static void __ctr_sdhc_set_ios(struct ctr_sdhc *host, struct mmc_ios *ios)
{
	u16 clk_ctl, card_opt;

	if (ios->clock) {
		unsigned int clkdiv = clk_get_rate(host->sdclk) / ios->clock;

		// get the divider that best achieves the desired clkrate
		clk_ctl = (clkdiv <= 1) ? 0 : (roundup_pow_of_two(clkdiv) / 4);
		clk_ctl |= SDHC_CARD_CLKCTL_PIN_ENABLE;

		if (ios->clock >= SDHC_CLKFREEZE_THRESHOLD)
			clk_ctl |= SDHC_CARD_CLKCTL_PIN_FREEZE;
	} else {
		// power off the clock entirely
		clk_ctl = 0;
	}

	switch (ios->bus_width) {
	default:
		dev_err(host->dev, "invalid bus width %d\n", ios->bus_width);
		return;
	case MMC_BUS_WIDTH_1:
		card_opt = SDHC_DEFAULT_CARDOPT | SDHC_CARD_OPTION_1BIT;
		break;
	case MMC_BUS_WIDTH_4:
		card_opt = SDHC_DEFAULT_CARDOPT | SDHC_CARD_OPTION_4BIT;
		break;
	}

	if (ios->power_mode == MMC_POWER_OFF)
		clk_ctl = 0; // force-disable the clock altogether

	/* set the desired clock divider and card option config */
	ctr_sdhc_set_clk_opt(host, clk_ctl, card_opt);
	mdelay(10);
}

static void ctr_sdhc_dma_unmap(struct ctr_sdhc*, struct mmc_data*);

static void ctr_sdhc_check_done(struct ctr_sdhc *host, int err)
{
	u32 stat;
	struct mmc_request *mrq = host->mrq;
	if (!mrq)
		return;

	stat = atomic_read(&host->stat);

	if (stat & SDHC_FULL_DONE)
		return;

	u32 expected = SDHC_CMD_DONE | (mrq->data ? (SDHC_SD_DONE | SDHC_DMAC_DONE) : 0);

	if (expected == stat || (err < 0)) {
		struct mmc_command *cmd = mrq->cmd;
		struct mmc_data *data = mrq->data;

		atomic_or(SDHC_FULL_DONE, &host->stat); // mark as fully processed

		if (err < 0 && cmd)
			cmd->error = err;

		if (data) {
			ctr_sdhc_dma_unmap(host, data);
		}

		mmc_request_done(host->mmc, mrq);
		host->mrq = NULL;
	}
}

static void ctr_sdhc_respend_irq(struct ctr_sdhc *host, u32 irqstat)
{
	struct mmc_request *mrq = host->mrq;
	struct mmc_command *cmd;

	if (!mrq)
		return;

	cmd = mrq->cmd;

	if (!(irqstat & SDHC_STAT_CMDRESPEND))
		return;

	if (!cmd) {
		dev_err(host->dev, "spurious CMD IRQ: got end of response "
			"but no command is active\n");
		return;
	}

	if (cmd->flags & MMC_RSP_PRESENT) {
		if (cmd->flags & MMC_RSP_136) { /* 136bit response, fill 32 */
			u32 respbuf[4], *resp;
			resp = cmd->resp;

			ctr_sdhc_get_resp(host, respbuf, 4);
			resp[0] = (respbuf[3] << 8) | (respbuf[2] >> 24);
			resp[1] = (respbuf[2] << 8) | (respbuf[1] >> 24);
			resp[2] = (respbuf[1] << 8) | (respbuf[0] >> 24);
			resp[3] = respbuf[0] << 8;
		} else { /* plain 32 bit response */
			ctr_sdhc_get_resp(host, cmd->resp, 1);
		}
	}

	dev_dbg(host->dev, "command IRQ complete %d %d %x\n", cmd->opcode,
		cmd->error, cmd->flags);

	atomic_or(SDHC_CMD_DONE, &host->stat);
}

static int ctr_sdhc_card_hotplug_irq(struct ctr_sdhc *host, u32 irqstat)
{
	if (!(irqstat & (SDHC_STAT_CARDREMOVE | SDHC_STAT_CARDINSERT)))
		return 0;

	/* finish any pending requests and do a full hw reset */
	ctr_sdhc_reset(host);
	if (!(irqstat & SDHC_STAT_CARDPRESENT)) {
		ctr_sdhc_check_done(host, -ENOMEDIUM);
	}
	mmc_detect_change(host->mmc, 1);
	return 1;
}

static void ctr_sdhc_dataend_irq(struct ctr_sdhc *host, u32 irqstat)
{
	if (irqstat & SDHC_STAT_DATA_END) {
		// mark the SD data xfer as done
		atomic_or(SDHC_SD_DONE, &host->stat);
		// dev_err(host->dev, "ctr_sdhc_dataend_irq: %08X\n", irqstat);
	}
}

static irqreturn_t ctr_sdhc_irq_thread(int irq, void *data)
{
	u32 irqstat;
	struct ctr_sdhc *host = data;
	int error = 0, err = IRQ_HANDLED;

	mutex_lock(&host->lock);

	irqstat = ctr_sdhc_irqstat_get(host);
	dev_dbg(host->dev, "IRQ status: %x\n", irqstat);

	/* immediately acknowledge all pending IRQs */
	ctr_sdhc_irqstat_ack(host, irqstat & SDHC_IRQMASK);

	/* handle any pending hotplug events */
	if (ctr_sdhc_card_hotplug_irq(host, irqstat))
		goto irq_end;

	/* skip the command/data events when there's no active request */
	if (unlikely(host->mrq == NULL))
		goto irq_end;

	if (irqstat & SDHC_ERR_CMD_TIMEOUT) {
		error = -ETIMEDOUT;
	} else if (irqstat & SDHC_ERR_CRC_FAIL) {
		error = -EILSEQ;
	} else if (irqstat & SDHC_ERRMASK) {
		dev_err(host->dev, "buffer error: %08X\n",
			irqstat & SDHC_ERRMASK);
		/*dev_err(host->dev, "detail error status %08X\n",
			ioread32(host->regs + SDHC_ERROR_STATUS));*/
		error = -EIO;
	}

	if (error) {
		/* error during transfer */
		struct mmc_command *cmd = host->mrq->cmd;
		if (cmd)
			cmd->error = error;

		if (error != -ETIMEDOUT)
			goto irq_end; /* serious error */
	}

	ctr_sdhc_respend_irq(host, irqstat);
	ctr_sdhc_dataend_irq(host, irqstat);

	ctr_sdhc_check_done(host, 0);

irq_end:
	mutex_unlock(&host->lock);
	return err;
}


/** Set clock and power state */
static void ctr_sdhc_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct ctr_sdhc *host = mmc_priv(mmc);
	mutex_lock(&host->lock);
	__ctr_sdhc_set_ios(host, ios);
	mutex_unlock(&host->lock);
}


/** Write-Protect & Card Detect handling */
static int ctr_sdhc_get_ro(struct mmc_host *mmc)
{
	struct ctr_sdhc *host = mmc_priv(mmc);
	return !(ctr_sdhc_irqstat_get(host) & SDHC_STAT_WRITEPROT);
}

static int ctr_sdhc_get_cd(struct mmc_host *mmc)
{
	struct ctr_sdhc *host = mmc_priv(mmc);
	return !!(ctr_sdhc_irqstat_get(host) & SDHC_STAT_CARDPRESENT);
}

static void ctr_sdhc_dma_callback(void *async_param)
{
	enum dma_status status;
	struct ctr_sdhc *host = async_param;
	struct mmc_data *data = host->mrq->data;

	if (!data)
		return;

	if (status == DMA_COMPLETE) {
		data->bytes_xfered = data->blocks * data->blksz;
		data->error = 0;
	} else {
		data->bytes_xfered = 0;
		data->error = -EIO;
	}

	// dev_err(host->dev, "DMA callback finished!\n");

	atomic_or(SDHC_DMAC_DONE, &host->stat);
	ctr_sdhc_check_done(host, data->error);
}


/** Data and command request issuing */
static int ctr_sdhc_dma_map(struct ctr_sdhc *host, struct mmc_data *data)
{
	int res;
	struct dma_chan *dma = host->dma_chan;

	if (data->host_cookie == DMABUF_MAPPED)
		return 0;

	res = dma_map_sg(dma->device->dev, data->sg,
		data->sg_len, mmc_get_dma_dir(data) <= 0);

	if (res <= 0) {
		data->host_cookie = DMABUF_UNMAPPED;
		dev_err(host->dev, "failed to dma_map_sg\n");
		return -ENOMEM;
	}

	data->sg_count = res;
	data->host_cookie = DMABUF_MAPPED;
	return 0;
}

static int ctr_sdhc_start_data(struct ctr_sdhc *host, struct mmc_data *data)
{
	int err;
	struct dma_slave_config dmacfg = {};
	struct dma_chan *dma = host->dma_chan;

	err = ctr_sdhc_dma_map(host, data);
	if (err < 0) {
		dev_err(host->dev, "failed to map sg %d\n", err);
		return -EINVAL;
	}

	dmacfg.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	dmacfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	dmacfg.src_maxburst = 256 / 4;
	dmacfg.dst_maxburst = 256 / 4;
	dmacfg.src_addr = host->fifo_addr;
	dmacfg.dst_addr = host->fifo_addr;

	if (data->flags & MMC_DATA_WRITE) {
		dmacfg.direction = DMA_MEM_TO_DEV;
	} else {
		dmacfg.direction = DMA_DEV_TO_MEM;
	}

	dmaengine_slave_config(dma, &dmacfg);

	host->txdesc = dmaengine_prep_slave_sg(dma, data->sg, data->sg_count,
		dmacfg.direction, DMA_PREP_INTERRUPT);
	if (!host->txdesc) {
		dev_err(host->dev,
			"failed to create DMA transfer descriptor\n");
		return -ENOMEM;
	}

	host->txdesc->callback = ctr_sdhc_dma_callback;
	host->txdesc->callback_param = host;

	ctr_sdhc_set_blk_len_cnt(host, data->blksz, data->blocks);

	host->dma_cookie = dmaengine_submit(host->txdesc);
	dma_async_issue_pending(dma);
	return 0;
}

static void ctr_sdhc_pre_request(struct mmc_host *mmc,
				struct mmc_request *mrq)
{
	struct ctr_sdhc *host = mmc_priv(mmc);
	struct mmc_data *data = mrq->data;

	if (data) {
		ctr_sdhc_dma_map(host, data);
	}
}

static void ctr_sdhc_dma_unmap(struct ctr_sdhc *host, struct mmc_data *data)
{
	if (data->host_cookie == DMABUF_UNMAPPED)
		return;

	dma_unmap_sg(host->dma_chan->device->dev, data->sg,
		data->sg_len, mmc_get_dma_dir(data));
	data->host_cookie = DMABUF_UNMAPPED;
}

static void ctr_sdhc_post_request(struct mmc_host *mmc,
				struct mmc_request *mrq, int err)
{
	struct mmc_data *data = mrq->data;
	struct ctr_sdhc *host = mmc_priv(mmc);
	struct dma_chan *dma = host->dma_chan;

	if (err) { // clean up the leftovers if needed
		dmaengine_terminate_all(dma);
	}

	if (data) {
		ctr_sdhc_dma_unmap(host, data);
	}
}

static int ctr_sdhc_start_request(struct ctr_sdhc *host,
				struct mmc_request *mrq)
{
	u16 cmd_reg;
	int err, cmd_op;
	struct mmc_data *data = mrq->data;
	struct mmc_command *cmd = mrq->cmd;

	cmd_op = cmd->opcode;
	cmd_reg = cmd_op;

	atomic_set(&host->stat, 0);

	if (cmd_op == MMC_STOP_TRANSMISSION) {
		/*
		 * the hardware supports automatically issuing a
		 * STOP_TRANSMISSION command, so do it and
		 * fake the response to make it look fine
		 */
		ctr_sdhc_stop_internal_set(host, SDHC_STOP_INTERNAL_ISSUE);

		cmd->resp[0] = cmd->opcode;
		cmd->resp[1] = 0;
		cmd->resp[2] = 0;
		cmd->resp[3] = 0;

		mmc_request_done(host->mmc, mrq);
		return 0;
	}

	switch (mmc_resp_type(cmd)) {
	case MMC_RSP_NONE:
		cmd_reg |= SDHC_CMDRSP_NONE;
		break;
	case MMC_RSP_R1:
		cmd_reg |= SDHC_CMDRSP_R1;
		break;
	case MMC_RSP_R1B:
		cmd_reg |= SDHC_CMDRSP_R1B;
		break;
	case MMC_RSP_R2:
		cmd_reg |= SDHC_CMDRSP_R2;
		break;
	case MMC_RSP_R3:
		cmd_reg |= SDHC_CMDRSP_R3;
		break;
	default:
		dev_err(host->dev, "Unknown response type %d\n",
			mmc_resp_type(cmd));
		break;
	}

	/* handle SDIO and APP_CMD cmd bits */
	switch(cmd_op) {
	case SD_IO_RW_DIRECT:
	case SD_IO_RW_EXTENDED:
		cmd_reg |= SDHC_CMD_SECURE;
		break;
	case MMC_APP_CMD:
		cmd_reg |= SDHC_CMDTYPE_APP;
		break;
	default:
		break;
	}

	/* start the data transfer if needed */
	if (data) {
		cmd_reg |= SDHC_CMD_DATA_XFER;

		if (data->blocks > 1) {
			err = ctr_sdhc_stop_internal_set(host,
				SDHC_STOP_INTERNAL_ENABLE);
			if (err)
				return err;

			cmd_reg |= SDHC_CMD_DATA_MULTI;
		}

		if (data->flags & MMC_DATA_READ)
			cmd_reg |= SDHC_CMD_DATA_READ;

		err = ctr_sdhc_start_data(host, data);
		if (err)
			return err;
	}

	// dev_dbg(host->dev, "send cmdarg: %d %08X\n", cmd->opcode, cmd->arg);

	return ctr_sdhc_send_cmdarg(host, cmd_reg, cmd->arg);
}

static void ctr_sdhc_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct mmc_command *cmd = mrq->cmd;
	struct ctr_sdhc *host = mmc_priv(mmc);

	mutex_lock(&host->lock);
	if (ctr_sdhc_get_cd(mmc) <= 0) {
		/* card not present, immediately return an error */
		cmd->error = -ENOMEDIUM;
	} else {
		if (host->mrq != NULL) {
			/* already another request in progrsss */
			cmd->error = -EBUSY;
		} else {
			host->mrq = mrq;
			cmd->error = ctr_sdhc_start_request(host, mrq);
		}
	}

	if (cmd->error) {
		// finish early if there was an error during setup
		mmc_request_done(mmc, mrq);
	}
	mutex_unlock(&host->lock);
}


/* SDIO IRQ support */
static irqreturn_t ctr_sdhc_sdio_irq_thread(int irq, void *data)
{
	irqreturn_t err = IRQ_NONE;
	struct ctr_sdhc *host = data;

	mutex_lock(&host->lock);
	if (ctr_sdhc_sdioirq_test(host)) {
		mmc_signal_sdio_irq(host->mmc);
		err = IRQ_HANDLED;
	}

	mutex_unlock(&host->lock);
	return err;
}

static void ctr_sdhc_enable_sdio_irq(struct mmc_host *mmc, int enable)
{
	struct ctr_sdhc *host = mmc_priv(mmc);
	mutex_lock(&host->lock);
	ctr_sdhc_sdioirq_set(host, enable);
	mutex_unlock(&host->lock);
}

static const struct mmc_host_ops ctr_sdhc_ops = {
	.request	= ctr_sdhc_request,
	.pre_req	= ctr_sdhc_pre_request,
	.post_req	= ctr_sdhc_post_request,
	.set_ios	= ctr_sdhc_set_ios,
	.get_ro		= ctr_sdhc_get_ro,
	.get_cd		= ctr_sdhc_get_cd,
	.enable_sdio_irq = ctr_sdhc_enable_sdio_irq,
};

static int ctr_sdhc_probe(struct platform_device *pdev)
{
	int err;
	struct clk *sdclk;
	struct device *dev;
	struct mmc_host *mmc;
	unsigned long clkrate;
	struct ctr_sdhc *host;

	dev = &pdev->dev;

	sdclk = devm_clk_get(dev, NULL);
	if (IS_ERR(sdclk)) {
		pr_err("no clock provided\n");
		return PTR_ERR(sdclk);
	}

	err = clk_prepare_enable(sdclk);
	if (err)
		return err;

	clkrate = clk_get_rate(sdclk);
	if (!clkrate)
		return -EINVAL;

	mmc = mmc_alloc_host(sizeof(struct ctr_sdhc), dev);
	if (!mmc)
		return -ENOMEM;

	host = mmc_priv(mmc);
	platform_set_drvdata(pdev, host);

	/* set up host data */
	if (of_property_read_u32(dev->of_node, "fifo-addr", &host->fifo_addr)) {
		err = -EINVAL;
		goto free_mmc;
	}

	host->dev = dev;
	host->mmc = mmc;
	host->sdclk = sdclk;

	host->dma_chan = dma_request_chan(dev, "fifo");
	if (IS_ERR(host->dma_chan)) {
		dev_err(dev, "failed to get DMA channel");
		err = PTR_ERR(host->dma_chan);
		goto free_mmc;
	}

	host->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(host->regs)) {
		err = PTR_ERR(host->regs);
		goto free_dmachan;
	}

	mmc->ops = &ctr_sdhc_ops;
	mmc->caps = MMC_CAP_4_BIT_DATA | MMC_CAP_MMC_HIGHSPEED |
		    MMC_CAP_SD_HIGHSPEED | MMC_CAP_SDIO_IRQ;
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;

	mmc->max_blk_size = 0x200;
	mmc->max_blk_count = 0xFFFF;

	mmc->f_max = clkrate / 2;
	mmc->f_min = clkrate / 512;

	mmc->max_segs = 1;
	mmc->max_seg_size = mmc->max_blk_size * mmc->max_blk_count;
	mmc->max_req_size = mmc->max_blk_size * mmc->max_blk_count;

	err = mmc_of_parse(mmc);
	if (err < 0)
		goto free_dmachan;

	mutex_init(&host->lock);

	ctr_sdhc_reset(host);

	err = devm_request_threaded_irq(dev, platform_get_irq(pdev, 0),
					NULL, ctr_sdhc_irq_thread,
					IRQF_ONESHOT, dev_name(dev), host);
	if (err)
		goto free_mmc;

	err = devm_request_threaded_irq(dev, platform_get_irq(pdev, 1),
					NULL, ctr_sdhc_sdio_irq_thread,
					IRQF_ONESHOT, dev_name(dev), host);
	if (err)
		goto free_mmc;

	mmc_add_host(mmc);
	pm_suspend_ignore_children(&pdev->dev, 1);
	return 0;

free_dmachan:
	dma_release_channel(host->dma_chan);

free_mmc:
	mmc_free_host(mmc);
	return err;
}

static const struct of_device_id ctr_sdhc_of_match[] = {
	{ .compatible = "nintendo," DRIVER_NAME },
	{},
};
MODULE_DEVICE_TABLE(of, ctr_sdhc_of_match);

static struct platform_driver ctr_sdhc_driver = {
	.probe = ctr_sdhc_probe,

	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(ctr_sdhc_of_match),
	},
};

module_platform_driver(ctr_sdhc_driver);

MODULE_DESCRIPTION("Nintendo 3DS SDHC driver");
MODULE_AUTHOR("Santiago Herrera");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
