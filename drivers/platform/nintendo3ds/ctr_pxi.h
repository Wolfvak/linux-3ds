// SPDX-License-Identifier: GPL-2.0
/*
 *  ctr_pxi.c
 *
 *  Copyright (C) 2020-2021 Santiago Herrera
 *
 *  Based on virtio_mmio.c
 */

#pragma once

#define VPXI_MAXDEV	(128)
#define VPXI_VRING_ALIGN	(PAGE_SIZE)

#define VPXI_VERSION_MIN	(0x01)
#define VPXI_VERSION_MAX	(0x01)

/*
 virtio transport protocol over PXI
 [23:0] = 24bit payload
 [30:24] = 7bit internal pxi device id
 [31] = command type (0 = read, 1 = write)
*/
#define VPXI_CMD(dev, data, cmd)	\
	((((cmd) & 1) << 31) | (((dev) & 0x7F) << 24) | ((data) & 0xFFFFFF))
#define VPXI_CMD_READ(dev, data)	VPXI_CMD(dev, data, 0)
#define VPXI_CMD_WRITE(dev, data)	VPXI_CMD(dev, data, 1)

#define VPXI_RTYPE_DEVICE	0
#define VPXI_RTYPE_CONFIG	1
#define VPXI_RTYPE_QUEUE	2
#define VPXI_RTYPE_MANAGER	3

/* virtio transport registers */

/*
 device register cmd format:
 	[1:0] = VPXI_RTYPE_DEVICE
 	[15:2] = device register
 	[24:16] = reserved, SBZ
*/
#define VPXI_REG_DEV(reg)	((((reg) & 0x3FFF) << 2) | VPXI_RTYPE_DEVICE)
#define VPXI_REG_DEVICE_ID	VPXI_REG_DEV(0x00)
#define VPXI_REG_VENDOR_ID	VPXI_REG_DEV(0x01)
#define VPXI_REG_DEV_STATUS	VPXI_REG_DEV(0x02)
#define VPXI_REG_DEV_FEAT0	VPXI_REG_DEV(0x03)
#define VPXI_REG_DEV_FEAT1	VPXI_REG_DEV(0x04)
#define VPXI_REG_DRV_FEAT0	VPXI_REG_DEV(0x05)
#define VPXI_REG_DRV_FEAT1	VPXI_REG_DEV(0x06)
#define VPXI_REG_CFG_GEN	VPXI_REG_DEV(0x07)

/*
 config register cmd format:
 	[1:0] = VPXI_RTYPE_CONFIG
 	[15:2] = register offset
 	[24:15] = reserved, SBZ
*/
#define VPXI_REG_CFG(reg)	((((reg) & 0x3FFF) << 2) | VPXI_RTYPE_CONFIG)

/*
 queue register cmd format:
 	[1:0] = VPXI_RTYPE_QUEUE
 	[8:2] = queue index
 	[15:9] = queue register
 	[24:16] = reserved, SBZ
*/
#define VPXI_REG_QUEUE(qidx, reg)	\
	((((reg) & 0x7F) << 9) | (((qidx) & 0x7F) << 2) | VPXI_RTYPE_QUEUE)

#define VPXI_REG_QUEUE_NUM_MAX(q)	VPXI_REG_QUEUE(q, 0x00)
#define VPXI_REG_QUEUE_NUM_CUR(q)	VPXI_REG_QUEUE(q, 0x01)
#define VPXI_REG_QUEUE_READY(q)		VPXI_REG_QUEUE(q, 0x02)
#define VPXI_REG_QUEUE_NOTIFY(q)	VPXI_REG_QUEUE(q, 0x03)
#define VPXI_REG_QUEUE_DESC(q)	VPXI_REG_QUEUE(q, 0x04)
#define VPXI_REG_QUEUE_AVAIL(q)	VPXI_REG_QUEUE(q, 0x05)
#define VPXI_REG_QUEUE_USED(q)	VPXI_REG_QUEUE(q, 0x06)

/*
 manager register cmd format:
 	[1:0] = VPXI_RTYPE_MANAGER
 	[15:2] = register
 	[24:16] = reserved, SBZ
 */
#define VPXI_REG_MANAGER(reg)	((((reg) & 0x3FFF) << 2) | VPXI_RTYPE_MANAGER)

#define VPXI_REG_MANAGER_VERSION	VPXI_REG_MANAGER(0x00)
#define VPXI_REG_MANAGER_DEVCOUNT	VPXI_REG_MANAGER(0x01)
#define VPXI_REG_MANAGER_IRQ_VQUEUE(b)	VPXI_REG_MANAGER(0x08 + ((b) & 3))
#define VPXI_REG_MANAGER_IRQ_CONFIG(b)	VPXI_REG_MANAGER(0x0C + ((b) & 3))

/* 32 int bits per register */
#define VPXI_MAX_IRQBANK	(VPXI_MAXDEV / 32)

#define to_vpxi_dev(_vd) \
	container_of(_vd, struct virtio_pxi_dev, vdev)

#define to_pxi_host(_vd) \
	((_vd)->host)

struct virtio_pxi_dev {
	unsigned id;
	struct pxi_host *host;

	spinlock_t lock;
	struct list_head vqs;
	struct virtio_device vdev;
};

struct virtio_pxi_vqinfo {
	struct list_head node;
	struct virtqueue *vq;
};

struct pxi_host {
	struct device *dev;
	void __iomem *regs;

	int sync_irq;
	int tx_irq;
	int rx_irq;

	struct mutex fifo_lock;
	wait_queue_head_t fifo_wq;

	struct work_struct irq_worker;

	unsigned version;
	unsigned vpd_count;
	struct virtio_pxi_dev *vpdevs;
};
