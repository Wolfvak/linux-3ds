// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  ctr_pxi.c
 *
 *  Copyright (C) 2020-2021 Santiago Herrera
 *
 *  Based on virtio_mmio.c
 */

#define DRIVER_NAME "3ds-pxi"
#define pr_fmt(str) DRIVER_NAME ": " str

#include <linux/io.h>
#include <linux/of.h>
#include <linux/list.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ring.h>

#include "ctr_pxi.h"

#define REG_PXI_SYNCRX	0x00
#define REG_PXI_SYNCTX	0x01
#define REG_PXI_SYNCIRQ	0x03

#define REG_PXI_CNT	0x04
#define REG_PXI_TX	0x08
#define REG_PXI_RX	0x0C

#define PXI_CNT_TX_FULL	BIT(1)
#define PXI_CNT_TX_IRQ	BIT(2)
#define PXI_CNT_FIFO_FLUSH	BIT(3)
#define PXI_CNT_RX_EMPTY	BIT(8)
#define PXI_CNT_RX_IRQ	BIT(10)
#define PXI_CNT_ERRACK	BIT(14)
#define PXI_CNT_ENABLE	BIT(15)

#define PXI_SYNCIRQ_TRIGGER	BIT(6)
#define PXI_SYNCIRQ_ENABLE	BIT(7)

#define PXI_FIFO_DEPTH	(16)

#define PXI_FIFO_TIMEOUT	msecs_to_jiffies(1000)

/**
 * PXI hardware interfacing routines
 */
static int pxi_tx_full(struct pxi_host *pxi)
{
	return ioread16(pxi->regs + REG_PXI_CNT) & PXI_CNT_TX_FULL;
}

static int pxi_rx_empty(struct pxi_host *pxi)
{
	return ioread16(pxi->regs + REG_PXI_CNT) & PXI_CNT_RX_EMPTY;
}

static int pxi_check_err(struct pxi_host *pxi)
{
	if (unlikely(ioread32(pxi->regs + REG_PXI_CNT) & PXI_CNT_ERRACK)) {
		iowrite32(PXI_CNT_FIFO_FLUSH | PXI_CNT_ERRACK | PXI_CNT_ENABLE,
			  pxi->regs + REG_PXI_CNT);
		return -EIO;
	}
	return 0;
}

static int pxi_txrx(struct pxi_host *pxi, const u32 *ww, int nw, u32 *wr,
		    int nr)
{
	long err;

	might_sleep();
	mutex_lock(&pxi->fifo_lock);

	while (nw > 0) { // send
		err = wait_event_interruptible_timeout(
			pxi->fifo_wq, !(pxi_tx_full(pxi)), PXI_FIFO_TIMEOUT);
		if (unlikely(err <= 0)) {
			err = -ETIMEDOUT;
			goto fifo_err;
		}

		iowrite32(*(ww++), pxi->regs + REG_PXI_TX);

		err = pxi_check_err(pxi);
		if (err)
			goto fifo_err;
		nw--;
	}

	while (nr > 0) { // recv
		err = wait_event_interruptible_timeout(
			pxi->fifo_wq, !(pxi_rx_empty(pxi)), PXI_FIFO_TIMEOUT);
		if (unlikely(err <= 0)) {
			err = -ETIMEDOUT;
			goto fifo_err;
		}

		*(wr++) = ioread32(pxi->regs + REG_PXI_RX);

		err = pxi_check_err(pxi);
		if (err)
			goto fifo_err;
		nr--;
	}

fifo_err:
	mutex_unlock(&pxi->fifo_lock);
	return err;
}

static void pxi_initialize_host(struct pxi_host *pxi)
{
	int i;

	iowrite8(0, pxi->regs + REG_PXI_SYNCTX);
	iowrite8(0, pxi->regs + REG_PXI_SYNCIRQ);
	iowrite16(PXI_CNT_FIFO_FLUSH | PXI_CNT_ERRACK | PXI_CNT_ENABLE,
		  pxi->regs + REG_PXI_CNT);

	for (i = 0; i < PXI_FIFO_DEPTH; i++)
		ioread32(pxi->regs + REG_PXI_RX);

	iowrite16(0, pxi->regs + REG_PXI_CNT);

	iowrite8(PXI_SYNCIRQ_ENABLE, pxi->regs + REG_PXI_SYNCIRQ);
	iowrite16(PXI_CNT_RX_IRQ | PXI_CNT_TX_IRQ | PXI_CNT_ERRACK |
			  PXI_CNT_FIFO_FLUSH | PXI_CNT_ENABLE,
		  pxi->regs + REG_PXI_CNT);

	mutex_init(&pxi->fifo_lock);
}

/**
 * Protocol interface helpers
 */
static inline int vpxi_multiread_reg(struct pxi_host *pxi, int nr, u32 dev,
				     u32 *regs, u32 *data)
{
	int i;
	for (i = 0; i < nr; i++)
		regs[i] = VPXI_CMD_READ(dev, regs[i]);
	return pxi_txrx(pxi, regs, nr, data, nr);
}

static inline int vpxi_multiwrite_reg(struct pxi_host *pxi, int nw, u32 dev,
				      u32 *regdata)
{
	int i;
	for (i = 0; i < nw; i++)
		regdata[i * 2] = VPXI_CMD_WRITE(dev, regdata[i * 2]);
	return pxi_txrx(pxi, regdata, nw * 2, NULL, 0);
}

static int vpxi_read_reg(struct pxi_host *pxi, u32 dev, u32 reg, u32 *val)
{
	return vpxi_multiread_reg(pxi, 1, dev, &reg, val);
}

static int vpxi_write_reg(struct pxi_host *pxi, u32 dev, u32 reg, u32 val)
{
	u32 cmd[2] = { reg, val };
	return vpxi_multiwrite_reg(pxi, 1, dev, cmd);
}

/*
 * VirtIO over PXI operation implementations
 */
static u32 vpxi_generation(struct virtio_device *vdev)
{
	u32 gen;
	struct virtio_pxi_dev *vpd = to_vpxi_dev(vdev);
	struct pxi_host *pxi = to_pxi_host(vpd);
	return vpxi_read_reg(pxi, vpd->id, VPXI_REG_CFG_GEN, &gen) ? 0 : gen;
}

/* TODO: add optimized versions that read on 1, 2 and 4 byte chunks */
static void vpxi_get_config(struct virtio_device *vdev, unsigned offset,
			    void *buf, unsigned len)
{
	int i;
	u8 *bytes;
	u32 cmd[8];
	struct virtio_pxi_dev *vpd = to_vpxi_dev(vdev);
	struct pxi_host *pxi = to_pxi_host(vpd);

	BUG_ON(len > 8);

	for (i = 0; i < len; i++)
		cmd[i] = VPXI_REG_CFG(offset + i);

	bytes = buf;
	if (vpxi_multiread_reg(pxi, len, vpd->id, cmd, cmd)) {
		/* set all to zero on error */
		for (i = 0; i < len; i++)
			bytes[i] = 0;
	} else {
		for (i = 0; i < len; i++)
			bytes[i] = cmd[i];
	}
}

static void vpxi_set_config(struct virtio_device *vdev, unsigned offset,
			    const void *buf, unsigned len)
{
	int i, err;
	u32 cmd[16];
	struct virtio_pxi_dev *vpd = to_vpxi_dev(vdev);
	struct pxi_host *pxi = to_pxi_host(vpd);

	BUG_ON(len > 8);

	for (i = 0; i < (len * 2); i += 2) {
		cmd[i + 0] = VPXI_REG_CFG(offset + i);
		cmd[i + 1] = *(u8 *)(buf + i);
	}

	err = vpxi_multiwrite_reg(pxi, len, vpd->id, cmd);
}

static u8 vpxi_get_status(struct virtio_device *vdev)
{
	int err;
	u32 status;
	struct virtio_pxi_dev *vpd = to_vpxi_dev(vdev);
	struct pxi_host *pxi = to_pxi_host(vpd);
	err = vpxi_read_reg(pxi, vpd->id, VPXI_REG_DEV_STATUS, &status);
	if (err)
		return ~0;
	return status & 0xFF;
}

static void vpxi_set_status(struct virtio_device *vdev, u8 status)
{
	struct virtio_pxi_dev *vpd = to_vpxi_dev(vdev);
	struct pxi_host *pxi = to_pxi_host(vpd);

	BUG_ON(status == 0);
	vpxi_write_reg(pxi, vpd->id, VPXI_REG_DEV_STATUS, status);
}

static void vpxi_reset(struct virtio_device *vdev)
{
	struct virtio_pxi_dev *vpd = to_vpxi_dev(vdev);
	struct pxi_host *pxi = to_pxi_host(vpd);
	vpxi_write_reg(pxi, vpd->id, VPXI_REG_DEV_STATUS, 0);
}

static bool vpxi_notify(struct virtqueue *vq)
{
	struct virtio_pxi_dev *vpd = to_vpxi_dev(vq->vdev);
	struct pxi_host *pxi = to_pxi_host(vpd);
	vpxi_write_reg(pxi, vpd->id, VPXI_REG_QUEUE_NOTIFY(vq->index), 1);
	return true;
}

static void vpxi_del_vq(struct virtqueue *vq)
{
	struct virtio_pxi_dev *vpd = to_vpxi_dev(vq->vdev);
	struct pxi_host *pxi = to_pxi_host(vpd);
	struct virtio_pxi_vqinfo *info = vq->priv;
	unsigned long flags;

	spin_lock_irqsave(&vpd->lock, flags);
	list_del(&info->node);
	spin_unlock_irqrestore(&vpd->lock, flags);

	/* deactivate the queue, delete it from the vring and free it */
	vpxi_write_reg(pxi, vpd->id, VPXI_REG_QUEUE_READY(vq->index), 0);
	vring_del_virtqueue(vq);
	devm_kfree(pxi->dev, info);
}

static void vpxi_del_vqs(struct virtio_device *vdev)
{
	struct virtqueue *vq, *n;
	list_for_each_entry_safe (vq, n, &vdev->vqs, list)
		vpxi_del_vq(vq);
}

static struct virtqueue *vpxi_setup_vq(struct virtio_device *vdev,
				       unsigned index,
				       void (*callback)(struct virtqueue *vq),
				       const char *name, bool ctx)
{
	int err;
	u32 vregs[10];
	unsigned long flags;
	struct virtqueue *vq;
	struct virtio_pxi_vqinfo *info;
	struct virtio_pxi_dev *vpd = to_vpxi_dev(vdev);
	struct pxi_host *pxi = to_pxi_host(vpd);

	if (!name)
		return NULL;

	vregs[0] = VPXI_REG_QUEUE_READY(index);
	vregs[1] = VPXI_REG_QUEUE_NUM_MAX(index);
	err = vpxi_multiread_reg(pxi, 2, vpd->id, vregs, vregs);
	if (err)
		return ERR_PTR(err);

	if (vregs[0] != 0) {
		pr_err("queue %d is already enabled on device %d", index,
		       vpd->id);
		return ERR_PTR(-ENOENT);
	}

	if (vregs[1] == 0) {
		pr_err("queue %d is zero on dev %d", index, vpd->id);
		return ERR_PTR(-ENOENT);
	}

	info = devm_kzalloc(pxi->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return ERR_PTR(-ENOMEM);

	vq = vring_create_virtqueue(index, vregs[1], VPXI_VRING_ALIGN, vdev,
				    false, true, ctx, vpxi_notify, callback,
				    name);
	if (!vq) {
		devm_kfree(pxi->dev, info);
		return ERR_PTR(-ENOMEM);
	}

	/* hardware setup */
	vregs[0] = VPXI_REG_QUEUE_NUM_CUR(index);
	vregs[1] = virtqueue_get_vring_size(vq);

	vregs[2] = VPXI_REG_QUEUE_DESC(index);
	vregs[3] = virtqueue_get_desc_addr(vq);

	vregs[4] = VPXI_REG_QUEUE_AVAIL(index);
	vregs[5] = virtqueue_get_avail_addr(vq);

	vregs[6] = VPXI_REG_QUEUE_USED(index);
	vregs[7] = virtqueue_get_used_addr(vq);

	vregs[8] = VPXI_REG_QUEUE_READY(index);
	vregs[9] = 1; /* READY */

	err = vpxi_multiwrite_reg(pxi, 5, vpd->id, vregs);
	if (err)
		return ERR_PTR(err);

	vq->priv = info;
	info->vq = vq;

	/* add the virtqueue to the vdev queue list */
	spin_lock_irqsave(&vpd->lock, flags);
	list_add(&info->node, &vpd->vqs);
	spin_unlock_irqrestore(&vpd->lock, flags);

	return vq;
}

static int vpxi_find_vqs(struct virtio_device *vdev, unsigned nvqs,
			 struct virtqueue *vqs[], vq_callback_t *callbacks[],
			 const char *const names[], const bool *ctx,
			 struct irq_affinity *desc)
{
	int i, queue_idx = 0;

	for (i = 0; i < nvqs; ++i) {
		if (!names[i]) {
			vqs[i] = NULL;
			continue;
		}

		vqs[i] = vpxi_setup_vq(vdev, queue_idx++, callbacks[i],
				       names[i], ctx ? ctx[i] : false);
		if (IS_ERR(vqs[i])) {
			vpxi_del_vqs(vdev);
			return PTR_ERR(vqs[i]);
		}
	}

	return 0;
}

static u64 vpxi_get_features(struct virtio_device *vdev)
{
	u32 featreg[2];
	struct virtio_pxi_dev *vpd = to_vpxi_dev(vdev);
	struct pxi_host *pxi = to_pxi_host(vpd);

	featreg[0] = VPXI_REG_DEV_FEAT0;
	featreg[1] = VPXI_REG_DEV_FEAT1;
	if (vpxi_multiread_reg(pxi, 2, vpd->id, featreg, featreg))
		return 0;
	return ((u64)featreg[1] << 32) | featreg[0];
}

static int vpxi_finalize_features(struct virtio_device *vdev)
{
	u32 vdata[4];
	struct virtio_pxi_dev *vpd = to_vpxi_dev(vdev);
	struct pxi_host *pxi = to_pxi_host(vpd);

	vring_transport_features(vdev);
	if (!__virtio_test_bit(vdev, VIRTIO_F_VERSION_1))
		return -EINVAL;

	vdata[0] = VPXI_REG_DRV_FEAT0;
	vdata[1] = vdev->features;

	vdata[2] = VPXI_REG_DRV_FEAT1;
	vdata[3] = vdev->features >> 32;
	return vpxi_multiwrite_reg(pxi, 2, vpd->id, vdata);
}

static const char *vpxi_bus_name(struct virtio_device *vdev)
{
	struct virtio_pxi_dev *vpd = to_vpxi_dev(vdev);
	struct pxi_host *pxi = to_pxi_host(vpd);
	return dev_name(pxi->dev);
}

static const struct virtio_config_ops vpxi_config_ops = {
	.reset	= vpxi_reset,
	.get	= vpxi_get_config,
	.set	= vpxi_set_config,
	.generation	= vpxi_generation,
	.get_status	= vpxi_get_status,
	.set_status	= vpxi_set_status,
	.find_vqs	= vpxi_find_vqs,
	.del_vqs	= vpxi_del_vqs,
	.get_features	= vpxi_get_features,
	.finalize_features	= vpxi_finalize_features,
	.bus_name	= vpxi_bus_name,
};

static u64 vpxi_get_irqbank(struct pxi_host *pxi, u32 id)
{
	u32 irqdata[2];

	irqdata[0] = VPXI_REG_MANAGER_IRQ_VQUEUE(id);
	irqdata[1] = VPXI_REG_MANAGER_IRQ_CONFIG(id);
	if (vpxi_multiread_reg(pxi, 2, 0, irqdata, irqdata))
		return 0;
	return ((u64)irqdata[1] << 32) | irqdata[0];
}

static void vpxi_irq_worker(struct work_struct *work)
{
	int i;
	u64 pending[VPXI_MAX_IRQBANK], any;
	struct pxi_host *pxi = container_of(work, struct pxi_host, irq_worker);

	any = 0;
	for (i = 0; i < VPXI_MAX_IRQBANK; i++) {
		/* get all pending interrupts */
		pending[i] = vpxi_get_irqbank(pxi, i);
		any |= pending[i];
	}

	if (!any)
		return;

	for (i = 0; i < pxi->vpd_count; i++) {
		u32 qirq, cirq;
		u64 pending_mask;
		struct virtio_pxi_vqinfo *info;
		struct virtio_pxi_dev *vpd = &pxi->vpdevs[i];

		qirq = i % 32; /* queue interrupt */
		cirq = (i % 32) + 32; /* config interrupt */
		pending_mask = pending[i / 32];

		if (pending_mask & BIT(qirq)) {
			unsigned long flags;
			spin_lock_irqsave(&vpd->lock, flags);
			list_for_each_entry(info, &vpd->vqs, node)
				vring_interrupt(0, info->vq);
			spin_unlock_irqrestore(&vpd->lock, flags);
		}

		if (pending_mask & BIT(cirq)) {
			virtio_config_changed(&vpd->vdev);
		}
	}

	/* reschedule until there are no pending irqs */
	schedule_work(&pxi->irq_worker);
}

static irqreturn_t vpxi_irq(int irq, void *data)
{
	struct pxi_host *pxi = data;
	schedule_work(&pxi->irq_worker);
	return IRQ_HANDLED;
}

static irqreturn_t pxi_txrx_fifo_irq(int irq, void *data)
{
	struct pxi_host *pxi = data;
	wake_up_interruptible(&pxi->fifo_wq);
	return IRQ_HANDLED;
}

static void vpxi_release_dev(struct device *dev)
{
}

/* driver initialization / initial probing */
static int pxi_init_virtio(struct pxi_host *pxi)
{
	int err, i;
	u32 vdata[2];

	vdata[0] = VPXI_REG_MANAGER_VERSION;
	vdata[1] = VPXI_REG_MANAGER_DEVCOUNT;
	err = vpxi_multiread_reg(pxi, 2, 0, vdata, vdata);
	if (err)
		return err;

	pxi->version = vdata[0];
	if ((pxi->version < VPXI_VERSION_MIN) ||
	    (pxi->version > VPXI_VERSION_MAX))
		return -ENOTSUPP;

	pxi->vpd_count = vdata[1];
	if (!pxi->vpd_count)
		return 0;

	if (pxi->vpd_count >= VPXI_MAXDEV)
		return -EINVAL;

	pxi->vpdevs = devm_kcalloc(pxi->dev, pxi->vpd_count,
				   sizeof(struct virtio_pxi_dev), GFP_KERNEL);
	if (!pxi->vpdevs)
		return -ENOMEM;

	err = dma_set_mask_and_coherent(pxi->dev, DMA_BIT_MASK(32));
	if (err)
		return err;

	/* acknowledge any possible pending interrupts */
	for (i = 0; i < VPXI_MAX_IRQBANK; i++)
		vpxi_get_irqbank(pxi, i);

	for (i = 0; i < pxi->vpd_count; i++) {
		u32 devinfo[2];
		struct virtio_pxi_dev *vpd = &pxi->vpdevs[i];

		/* initialize each virtdev present in the remote system */
		vpd->id = i;
		vpd->host = pxi;
		INIT_LIST_HEAD(&vpd->vqs);
		spin_lock_init(&vpd->lock);

		vpd->vdev.dev.parent = pxi->dev;
		vpd->vdev.dev.release = vpxi_release_dev;
		vpd->vdev.config = &vpxi_config_ops;

		/* also grab the device and vendor IDs */
		devinfo[0] = VPXI_REG_DEVICE_ID;
		devinfo[1] = VPXI_REG_VENDOR_ID;
		err = vpxi_multiread_reg(pxi, 2, i, devinfo, devinfo);
		if (err)
			return err;

		vpd->vdev.id.device = devinfo[0];
		vpd->vdev.id.vendor = devinfo[1];

		err = register_virtio_device(&vpd->vdev);
		if (err)
			return err;
	}

	return 0;
}

static int ctr_pxi_probe(struct platform_device *pdev)
{
	int err;
	struct device *dev;
	struct pxi_host *pxi;
	int sync_irq, tx_irq, rx_irq;

	dev = &pdev->dev;

	sync_irq = platform_get_irq(pdev, 0);
	tx_irq = platform_get_irq(pdev, 1);
	rx_irq = platform_get_irq(pdev, 2);

	if ((sync_irq < 0) || (tx_irq < 0) || (rx_irq < 0)) {
		pr_err("failed to retrieve interrupts");
		return -EINVAL;
	}

	pxi = devm_kzalloc(dev, sizeof(*pxi), GFP_KERNEL);
	if (!pxi)
		return -ENOMEM;

	pxi->dev = dev;
	pxi->tx_irq = tx_irq;
	pxi->rx_irq = rx_irq;
	pxi->sync_irq = sync_irq;
	init_waitqueue_head(&pxi->fifo_wq);
	INIT_WORK(&pxi->irq_worker, vpxi_irq_worker);

	pxi->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(pxi->regs))
		return PTR_ERR(pxi->regs);

	pxi_initialize_host(pxi);
	platform_set_drvdata(pdev, pxi);

	err = devm_request_irq(&pdev->dev, sync_irq, vpxi_irq, 0, "pxi_sync",
			       pxi);
	if (err)
		return err;

	err = devm_request_irq(&pdev->dev, tx_irq, pxi_txrx_fifo_irq, 0,
			       "pxi_tx", pxi);
	if (err)
		return err;

	err = devm_request_irq(&pdev->dev, rx_irq, pxi_txrx_fifo_irq, 0,
			       "pxi_rx", pxi);
	if (err)
		return err;

	err = pxi_init_virtio(pxi);
	if (err) {
		pr_err("failed to init virtio bridge (%d)", err);
		return err;
	}

	pr_info("discovered %d virtio devices", pxi->vpd_count);
	return 0;
}

static int ctr_pxi_remove(struct platform_device *pdev)
{
	return -EINVAL;
}

static const struct of_device_id ctr_pxi_of_match[] = {
	{ .compatible = "nintendo," DRIVER_NAME },
	{},
};
MODULE_DEVICE_TABLE(of, ctr_pxi_of_match);

static struct platform_driver ctr_pxi_driver = {
	.probe	= ctr_pxi_probe,
	.remove = ctr_pxi_remove,

	.driver	= {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
		.of_match_table	= of_match_ptr(ctr_pxi_of_match),
	},
};
module_platform_driver(ctr_pxi_driver);

MODULE_AUTHOR("Santiago Herrera");
MODULE_DESCRIPTION("Nintendo 3DS PXI virtio bridge");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform: " DRIVER_NAME);
