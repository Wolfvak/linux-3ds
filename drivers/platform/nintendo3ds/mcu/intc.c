// SPDX-License-Identifier: GPL-2.0
/*
 *  ctr/intc.c
 *
 *  Copyright (C) 2021 Santiago Herrera
 */

#define DRIVER_NAME "3dsmcu-intc"
#define pr_fmt(fmt) DRIVER_NAME ": " fmt

#include <linux/of.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/property.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>

#define OFFSET_STAT	0x00
#define OFFSET_MASK	0x08

static const struct regmap_irq ctr_mcu_irqs[] = {
	REGMAP_IRQ_REG_LINE(0, 8), // power button press
	REGMAP_IRQ_REG_LINE(1, 8), // power button held
	REGMAP_IRQ_REG_LINE(2, 8), // home button press
	REGMAP_IRQ_REG_LINE(3, 8), // home button release
	REGMAP_IRQ_REG_LINE(4, 8), // wifi switch
	REGMAP_IRQ_REG_LINE(5, 8), // shell close
	REGMAP_IRQ_REG_LINE(6, 8), // shell open
	//REGMAP_IRQ_REG_LINE(7, 8), // fatal condition
	//REGMAP_IRQ_REG_LINE(8, 8), // charger removed
	//REGMAP_IRQ_REG_LINE(9, 8), // charger plugged in
	//REGMAP_IRQ_REG_LINE(10, 8), // rtc alarm
	//REGMAP_IRQ_REG_LINE(11, 8),
	//REGMAP_IRQ_REG_LINE(12, 8),
	//REGMAP_IRQ_REG_LINE(13, 8),
	//REGMAP_IRQ_REG_LINE(14, 8),
	//REGMAP_IRQ_REG_LINE(15, 8),
	//REGMAP_IRQ_REG_LINE(16, 8),
	//REGMAP_IRQ_REG_LINE(17, 8),
	//REGMAP_IRQ_REG_LINE(18, 8),
	//REGMAP_IRQ_REG_LINE(19, 8),
	//REGMAP_IRQ_REG_LINE(20, 8),
	//REGMAP_IRQ_REG_LINE(21, 8),
	REGMAP_IRQ_REG_LINE(22, 8), // volume slider change
	//REGMAP_IRQ_REG_LINE(23, 8),
	//REGMAP_IRQ_REG_LINE(24, 8),
	//REGMAP_IRQ_REG_LINE(25, 8),
	//REGMAP_IRQ_REG_LINE(26, 8),
	//REGMAP_IRQ_REG_LINE(27, 8),
	//REGMAP_IRQ_REG_LINE(28, 8),
	//REGMAP_IRQ_REG_LINE(29, 8),
	//REGMAP_IRQ_REG_LINE(30, 8),
	//REGMAP_IRQ_REG_LINE(31, 8)
	// there's 32 possible interrupts
	// spread out on 4 8bit registers
};

static int ctr_mcu_intc_probe(struct platform_device *pdev)
{
	int irq;
	u32 io_base;
	struct device *dev;
	struct regmap *map;
	struct regmap_irq_chip *irqc;
	struct regmap_irq_chip_data *data;

	dev = &pdev->dev;
	if (!dev->parent)
		return -ENODEV;

	map = dev_get_regmap(dev->parent, NULL);
	if (!map)
		return -ENODEV;

	if (of_property_read_u32(dev->of_node, "reg", &io_base))
		return -EINVAL;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	irqc = devm_kzalloc(dev, sizeof(*irqc), GFP_KERNEL);
	if (!irqc)
		return -ENOMEM;

	irqc->name = dev_name(dev);

	irqc->irqs = ctr_mcu_irqs;
	irqc->num_irqs = ARRAY_SIZE(ctr_mcu_irqs);
	irqc->num_regs = 4;

	irqc->status_base = io_base + OFFSET_STAT;
	irqc->mask_base = io_base + OFFSET_MASK;

	irqc->init_ack_masked = true;

	return devm_regmap_add_irq_chip_fwnode(dev, dev_fwnode(dev), map, irq,
					       0, 0, irqc, &data);
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
MODULE_AUTHOR("Santiago Herrera");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
