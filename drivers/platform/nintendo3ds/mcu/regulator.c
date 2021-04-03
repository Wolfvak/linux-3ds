/*
 * ctr/regulator.c
 *
 * Copyright (C) 2021 Wolfvak
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define DRIVER_NAME	"3dsmcu-regulator"
#define pr_fmt(fmt)	DRIVER_NAME ": " fmt

#include <linux/of.h>
#include <linux/mutex.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/property.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>

#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>

#define REGISTER_POWER_BASE	0x20

#define REGULATOR_DEFAULT_DELAY	150000 // 150ms
#define REGULATOR_MAX_TOGGLE	24

struct ctr_regulator {
	struct device *dev;
	struct regmap *map;

	int on;
	int off;
	unsigned u_delay;
	struct mutex lock;

	struct regulator_desc rdesc;
};

static int ctrmcu_regulator_toggle(struct ctr_regulator *regulator, int on)
{
	int err, mask;
	unsigned u_delay = regulator->u_delay;

	mask = on ? regulator->on : regulator->off;

	if (mask < 0)
		return -ENOTSUPP;
	/* the regulator cant perform the requested operation */

	if (mask >= REGULATOR_MAX_TOGGLE)
		return -EINVAL;
	/* wont write out of bounds */

	mutex_lock(&regulator->lock);
	err = regmap_write(regulator->map,
		REGISTER_POWER_BASE + (mask / 8), mask % 8);
	if (!err)
		usleep_range(u_delay, u_delay + 1);
	mutex_unlock(&regulator->lock);

	return err;
}

static int ctrmcu_regulator_enable(struct regulator_dev *rdev)
{
	return ctrmcu_regulator_toggle(rdev_get_drvdata(rdev), 1);
}

static int ctrmcu_regulator_disable(struct regulator_dev *rdev)
{
	return ctrmcu_regulator_toggle(rdev_get_drvdata(rdev), 0);
}

static struct regulator_ops ctrmcu_regulator_ops = {
	.enable = ctrmcu_regulator_enable,
	.disable = ctrmcu_regulator_disable,
};

static int ctrmcu_regulator_probe(struct platform_device *pdev)
{
	struct device *dev;
	struct regmap *regmap;
	u32 on, off, toggle_delay;
	struct regulator_dev *rdev;
	struct regulator_desc *rdesc;
	struct ctr_regulator *regulator;
	struct regulator_config rconfig = {};

	dev = &pdev->dev;
	if (!dev->parent)
		return -ENODEV;

	regmap = dev_get_regmap(dev->parent, NULL);
	if (!regmap)
		return -ENODEV;

	regulator = devm_kzalloc(dev, sizeof(*regulator), GFP_KERNEL);
	if (!regulator)
		return -ENOMEM;
	platform_set_drvdata(pdev, regulator);

	regulator->dev = dev;
	regulator->map = regmap;
	mutex_init(&regulator->lock);

	if (of_property_read_u32(dev->of_node, "on", &on))
		on = -1;

	if (of_property_read_u32(dev->of_node, "off", &off))
		off = -1;

	if (of_property_read_u32(dev->of_node, "delay-us", &toggle_delay))
		toggle_delay = REGULATOR_DEFAULT_DELAY;
	else
		toggle_delay = usecs_to_jiffies(toggle_delay);

	regulator->on = on;
	regulator->off = off;
	regulator->u_delay = toggle_delay;

	rdesc = &regulator->rdesc;

	rdesc->name = dev_name(dev);
	rdesc->id = -1;
	rdesc->type = REGULATOR_VOLTAGE; // i think?
	rdesc->owner = THIS_MODULE;
	rdesc->ops = &ctrmcu_regulator_ops;

	rconfig.dev = dev;
	rconfig.driver_data = regulator;
	rconfig.of_node = dev->of_node;

	rdev = devm_regulator_register(dev, rdesc, &rconfig);
	if (IS_ERR(rdev))
		return PTR_ERR(rdev);

	return 0;
}

static const struct of_device_id ctrmcu_regulator_of_match[] = {
	{ .compatible = "nintendo," DRIVER_NAME, },
	{}
};
MODULE_DEVICE_TABLE(of, ctrmcu_regulator_of_match);

static struct platform_driver ctrmcu_regulator_driver = {
	.probe = ctrmcu_regulator_probe,

	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(ctrmcu_regulator_of_match),
	},
};
module_platform_driver(ctrmcu_regulator_driver);

MODULE_DESCRIPTION("Nintendo 3DS MCU power regulator driver");
MODULE_AUTHOR("Wolfvak");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
