/*
 *  ctr/powersupply.c
 *
 *  Copyright (C) 2020-2021 Wolfvak
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */


#define DRIVER_NAME "3dsmcu-powersupply"
#define pr_fmt(fmt) DRIVER_NAME ": " fmt

#include <linux/of.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/power_supply.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>

#define REGISTER_BASE	0x0A
/* temp, cap, capfrac, voltage, subdevice, status */

#define STATUS_AC_PLUGGED   BIT(3)
#define STATUS_BAT_CHARGED  BIT(4)

#define OFF_TEMPERATURE	0x00
#define OFF_CAPACITY	0x01
#define OFF_CAPFRACTION	0x02
#define OFF_SYS_VOLTAGE	0x03
#define OFF_SUB_STATUS	0x04
#define OFF_SYS_STATUS	0x05

static int ctr_mcu_powersupply_getdata(struct regmap *map, u8 *data)
{
	return regmap_raw_read(map, REGISTER_BASE, data, 6);
}

static int battery_getprop(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	int err;
	u8 data[6];
	struct regmap *regmap = power_supply_get_drvdata(psy);

	err = ctr_mcu_powersupply_getdata(regmap, data);
	if (err)
		return err;

	switch(psp) {
		case POWER_SUPPLY_PROP_PRESENT:
		case POWER_SUPPLY_PROP_ONLINE:
			val->intval = 1; // assume there's always a battery present
			break;

		case POWER_SUPPLY_PROP_STATUS:
			if (data[OFF_SYS_STATUS] & STATUS_BAT_CHARGED) {
				val->intval = POWER_SUPPLY_STATUS_FULL;
			} else if (data[OFF_SYS_STATUS] & STATUS_AC_PLUGGED) {
				val->intval = POWER_SUPPLY_STATUS_CHARGING;
			} else {
				val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
			}
			break;

		case POWER_SUPPLY_PROP_CAPACITY:
			val->intval = data[OFF_CAPACITY];
			break;

		case POWER_SUPPLY_PROP_TEMP:
			val->intval = (s8)(data[OFF_TEMPERATURE]) * 10;
			// tenths of degrees celsius
			break;

		case POWER_SUPPLY_PROP_VOLTAGE_NOW:
			val->intval = (unsigned)(data[OFF_SYS_VOLTAGE]) * 20000;
			// units are given in 20mV steps
			break;

		default:
			return -EINVAL;
	}
	return 0;
}

static int ac_getprop(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	int err;
	u8 data[6];
	struct regmap *regmap = power_supply_get_drvdata(psy);

	err = ctr_mcu_powersupply_getdata(regmap, data);
	if (err)
		return err;

	switch(psp) {
		case POWER_SUPPLY_PROP_PRESENT:
			val->intval = 1;
			break;

		case POWER_SUPPLY_PROP_ONLINE:
			val->intval = (data[OFF_SYS_STATUS] & STATUS_AC_PLUGGED) ? 1 : 0;
			break;

		case POWER_SUPPLY_PROP_STATUS:
			val->intval = (data[OFF_SYS_STATUS] & STATUS_AC_PLUGGED) ?
				POWER_SUPPLY_STATUS_CHARGING : POWER_SUPPLY_STATUS_DISCHARGING;
			break;

		default:
			return -EINVAL;
	}
	return 0;
}

static enum power_supply_property battery_properties[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
};
static struct power_supply_desc battery_desc = {
	.name = "BAT0",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = battery_properties,
	.num_properties = ARRAY_SIZE(battery_properties),
	.get_property = battery_getprop,
};

static enum power_supply_property ac_properties[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
};
static struct power_supply_desc ac_desc = {
	.name = "AC",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = ac_properties,
	.num_properties = ARRAY_SIZE(ac_properties),
	.get_property = ac_getprop,
};

static int ctr_mcu_powersupply_probe(struct platform_device *pdev)
{
	struct device *dev;
	struct power_supply *psy;
	struct regmap *mcu_regmap;
	struct power_supply_config psy_cfg = {};

	dev = &pdev->dev;
	if (!dev->parent)
		return -ENODEV;

	mcu_regmap = dev_get_regmap(dev->parent, NULL);
	if (!mcu_regmap)
		return -ENODEV;

	psy_cfg.drv_data = mcu_regmap;

	psy = devm_power_supply_register(dev, &battery_desc, &psy_cfg);
	if (IS_ERR(psy)) {
		dev_err(dev, "unable to register battery driver");
		return PTR_ERR(psy);
	}

	psy = devm_power_supply_register(dev, &ac_desc, &psy_cfg);
	if (IS_ERR(psy)) {
		dev_err(dev, "unable to register AC driver");
		return PTR_ERR(psy);
	}

	return 0;
}

static const struct of_device_id ctr_mcu_powersupply_of_match[] = {
	{ .compatible = "nintendo," DRIVER_NAME },
	{}
};
MODULE_DEVICE_TABLE(of, ctr_mcu_powersupply_of_match);

static struct platform_driver ctr_mcu_powersupply_driver = {
	.probe = ctr_mcu_powersupply_probe,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = ctr_mcu_powersupply_of_match,
	},
};
module_platform_driver(ctr_mcu_powersupply_driver);

MODULE_DESCRIPTION("Nintendo 3DS battery/AC driver");
MODULE_AUTHOR("Wolfvak");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
