/*
 *  ctr/intc.c
 *
 *  Copyright (C) 2021 Wolfvak
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define DRIVER_NAME	"3dsmcu-intc"
#define pr_fmt(fmt)	DRIVER_NAME ": " fmt

#include <linux/of.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/property.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>

#define REG_IP	0x10
#define REG_IE	0x18

struct ctr_mcu_intc {
	struct regmap *regmap;
	struct regmap_irq_chip chip;
	struct regmap_irq_chip_data *data;
};

static const struct regmap_irq ctr_mcu_irqs[] = {
	REGMAP_IRQ_REG_LINE(0, 8),
	REGMAP_IRQ_REG_LINE(1, 8),
	REGMAP_IRQ_REG_LINE(2, 8),
	REGMAP_IRQ_REG_LINE(3, 8),
	REGMAP_IRQ_REG_LINE(4, 8),
	REGMAP_IRQ_REG_LINE(5, 8),
	REGMAP_IRQ_REG_LINE(6, 8),
	REGMAP_IRQ_REG_LINE(7, 8),
	REGMAP_IRQ_REG_LINE(8, 8),
	REGMAP_IRQ_REG_LINE(9, 8),
	REGMAP_IRQ_REG_LINE(10, 8),
	REGMAP_IRQ_REG_LINE(11, 8),
	REGMAP_IRQ_REG_LINE(12, 8),
	REGMAP_IRQ_REG_LINE(13, 8),
	REGMAP_IRQ_REG_LINE(14, 8),
	REGMAP_IRQ_REG_LINE(15, 8),
	REGMAP_IRQ_REG_LINE(16, 8),
	REGMAP_IRQ_REG_LINE(17, 8),
	REGMAP_IRQ_REG_LINE(18, 8),
	REGMAP_IRQ_REG_LINE(19, 8),
	REGMAP_IRQ_REG_LINE(20, 8),
	REGMAP_IRQ_REG_LINE(21, 8),
	REGMAP_IRQ_REG_LINE(22, 8),
	REGMAP_IRQ_REG_LINE(23, 8),
	REGMAP_IRQ_REG_LINE(24, 8),
	REGMAP_IRQ_REG_LINE(25, 8),
	REGMAP_IRQ_REG_LINE(26, 8),
	REGMAP_IRQ_REG_LINE(27, 8),
	REGMAP_IRQ_REG_LINE(28, 8),
	REGMAP_IRQ_REG_LINE(29, 8),
	REGMAP_IRQ_REG_LINE(30, 8),
	REGMAP_IRQ_REG_LINE(31, 8)
	// there's 32 possible interrupts
	// spread out on 4 8bit registers
};

static const struct regmap_irq_chip ctr_mcu_irqchip = {
	.name = DRIVER_NAME,

	.irqs = ctr_mcu_irqs,
	.num_irqs = ARRAY_SIZE(ctr_mcu_irqs),
	.num_regs = 4,

	.status_base = REG_IP,
	.mask_base = REG_IE,
	.mask_invert = false,
	.mask_writeonly = true,

	.init_ack_masked = true,
	.ack_base = 0,
	.use_ack = false, // acknowledged on clear
};

static int ctr_mcu_intc_probe(struct platform_device *pdev)
{
	int irq;
	struct device *dev;
	struct regmap *regmap;
	struct ctr_mcu_intc *mcu_intc;

	dev = &pdev->dev;
	if (!dev->parent)
		return -ENODEV;

	regmap = dev_get_regmap(dev->parent, NULL);
	if (!regmap)
		return -ENODEV;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	mcu_intc = devm_kzalloc(dev, sizeof(*mcu_intc), GFP_KERNEL);
	if (!mcu_intc)
		return -ENOMEM;

	mcu_intc->regmap = regmap;
	mcu_intc->chip = ctr_mcu_irqchip;
	platform_set_drvdata(pdev, mcu_intc);

	return devm_regmap_add_irq_chip_fwnode(dev, dev_fwnode(dev),
		regmap, irq, 0, 0, &mcu_intc->chip, &mcu_intc->data);
}

static const struct of_device_id ctr_mcu_intc_of_match[] = {
	{ .compatible = "nintendo," DRIVER_NAME },
	{}
};
MODULE_DEVICE_TABLE(of, ctr_mcu_intc_of_match);

static struct platform_driver ctr_mcu_intc_driver = {
	.probe = ctr_mcu_intc_probe,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = ctr_mcu_intc_of_match,
	},
};
module_platform_driver(ctr_mcu_intc_driver);

MODULE_DESCRIPTION("Nintendo 3DS MCU Interrupt Controller driver");
MODULE_AUTHOR("Wolfvak");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
