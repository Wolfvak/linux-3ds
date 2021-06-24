// SPDX-License-Identifier: GPL-2.0
/*
 * ctr/regulator.c
 *
 * Copyright (C) 2021 Santiago Herrera
 */

#define DRIVER_NAME "3dsmcu-regulator"
#define pr_fmt(fmt) DRIVER_NAME ": " fmt

#include <linux/of.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/property.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>

#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>

#define REGULATOR_DEFAULT_DELAY	150000 // 150ms

static struct regulator_ops ctr_regulator_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
};

static int ctr_regulator_probe(struct platform_device *pdev)
{
	struct device *dev;
	struct regmap *map;
	u32 base, on, off, tdelay;
	struct regulator_desc *rdesc;
	struct regulator_config rcfg = {};

	dev = &pdev->dev;
	if (!dev->parent)
		return -ENODEV;

	map = dev_get_regmap(dev->parent, NULL);
	if (!map)
		return -ENODEV;

	if (of_property_read_u32(dev->of_node, "reg", &base))
		return -EINVAL;

	if (of_property_read_u32(dev->of_node, "on", &on))
		return -EINVAL;

	if (of_property_read_u32(dev->of_node, "off", &off))
		return -EINVAL;

	rdesc = devm_kzalloc(dev, sizeof(*rdesc), GFP_KERNEL);
	if (!rdesc)
		return -ENOMEM;

	if (of_property_read_u32(dev->of_node, "delay-us", &tdelay))
		tdelay = REGULATOR_DEFAULT_DELAY;

	rdesc->name = dev_name(dev);
	rdesc->id = -1;
	rdesc->type = REGULATOR_VOLTAGE;
	rdesc->owner = THIS_MODULE;
	rdesc->enable_time = tdelay;
	rdesc->off_on_delay = tdelay;
	rdesc->enable_reg = base;
	rdesc->enable_mask = on | off;
	rdesc->enable_val = on;
	rdesc->disable_val = off;
	rdesc->ops = &ctr_regulator_ops;

	rcfg.dev = dev;
	rcfg.of_node = dev->of_node;
	rcfg.regmap = map;

	return PTR_ERR_OR_ZERO(devm_regulator_register(dev, rdesc, &rcfg));
}

static int ctr_regulator_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id ctr_regulator_of_match[] = {
	{ .compatible = "nintendo," DRIVER_NAME },
	{}
};
MODULE_DEVICE_TABLE(of, ctr_regulator_of_match);

static struct platform_driver ctr_regulator_driver = {
	.probe = ctr_regulator_probe,
	.remove = ctr_regulator_remove,

	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(ctr_regulator_of_match),
	},
};
module_platform_driver(ctr_regulator_driver);

MODULE_DESCRIPTION("Nintendo 3DS MCU power regulator driver");
MODULE_AUTHOR("Santiago Herrera");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
