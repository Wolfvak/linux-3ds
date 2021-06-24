// SPDX-License-Identifier: GPL-2.0
/*
 *  ctr/charger.c
 *
 *  Copyright (C) 2020-2021 Santiago Herrera
 */

#define DRIVER_NAME "3dsmcu-charger"
#define pr_fmt(fmt) DRIVER_NAME ": " fmt

#include <linux/of.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/power_supply.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>

#define REG_TEMPERATURE	0x00
#define REG_CAPACITY	0x01
#define REG_VOLTAGE	0x03
#define REG_STATUS	0x05

#define STATUS_AC_PLUGGED	BIT(3)
#define STATUS_BAT_CHARGING	BIT(4)

struct ctr_charger {
	unsigned io_addr;
	struct regmap *map;

	struct power_supply *ac;
	struct power_supply *bat;
};

static int ctr_charger_read(struct ctr_charger *psy, u8 *data)
{
	return regmap_bulk_read(psy->map, psy->io_addr, data, 6);
}

static int battery_getprop(struct power_supply *psy,
			   enum power_supply_property psp,
			   union power_supply_propval *val)
{
	int err;
	u8 data[6];
	struct ctr_charger *charger = power_supply_get_drvdata(psy);

	err = ctr_charger_read(charger, data);
	if (err)
		return err;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
	case POWER_SUPPLY_PROP_ONLINE:
		/* assume there's always a battery present */
		val->intval = 1;
		break;

	case POWER_SUPPLY_PROP_STATUS:
		if (data[REG_STATUS] & STATUS_BAT_CHARGING)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else if (data[REG_STATUS] & STATUS_AC_PLUGGED)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		break;

	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = data[REG_CAPACITY];
		break;

	case POWER_SUPPLY_PROP_TEMP:
		/* data is given in tenths of degrees celsius */
		val->intval = sign_extend32(data[REG_TEMPERATURE], 7) * 10;
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = ((unsigned)data[REG_VOLTAGE]) * 20000;
		/* units are given in 20mV steps */
		break;

	default:
		return -EINVAL;
	}
	return 0;
}

static int ac_getprop(struct power_supply *psy, enum power_supply_property psp,
		      union power_supply_propval *val)
{
	int err;
	u8 data[6];
	struct ctr_charger *charger = power_supply_get_drvdata(psy);

	err = ctr_charger_read(charger, data);
	if (err)
		return err;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;

	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = data[REG_STATUS] & STATUS_AC_PLUGGED ? 1 : 0;
		break;

	default:
		return -EINVAL;
	}
	return 0;
}

static enum power_supply_property bat_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
}, ac_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc bat_desc = {
	.name = "BAT0",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = bat_props,
	.num_properties = ARRAY_SIZE(bat_props),
	.get_property = battery_getprop,
}, ac_desc = {
	.name = "ADP0",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = ac_props,
	.num_properties = ARRAY_SIZE(ac_props),
	.get_property = ac_getprop,
};

static int ctr_charger_probe(struct platform_device *pdev)
{
	unsigned io_addr;
	struct device *dev;
	struct regmap *map;
	struct ctr_charger *charger;
	struct power_supply_config psy_cfg = {};

	dev = &pdev->dev;
	if (!dev->parent)
		return -ENODEV;

	if (of_property_read_u32(dev->of_node, "reg", &io_addr))
		return -EINVAL;

	map = dev_get_regmap(dev->parent, NULL);
	if (!map)
		return -ENODEV;

	charger = devm_kzalloc(dev, sizeof(*charger), GFP_KERNEL);
	if (!charger)
		return -ENOMEM;

	charger->map = map;
	charger->io_addr = io_addr;

	psy_cfg.of_node = dev->of_node;
	psy_cfg.fwnode = dev_fwnode(dev);

	psy_cfg.drv_data = charger;

	charger->ac = devm_power_supply_register(dev, &ac_desc, &psy_cfg);
	if (IS_ERR(charger->ac))
		return PTR_ERR(charger->ac);

	charger->bat = devm_power_supply_register(dev, &bat_desc, &psy_cfg);
	if (IS_ERR(charger->bat))
		return PTR_ERR(charger->bat);

	return 0;
}

static int ctr_charger_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id ctr_charger_of_match[] = {
	{ .compatible = "nintendo," DRIVER_NAME },
	{}
};
MODULE_DEVICE_TABLE(of, ctr_charger_of_match);

static struct platform_driver ctr_charger_driver = {
	.probe = ctr_charger_probe,
	.remove = ctr_charger_remove,

	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = ctr_charger_of_match,
	},
};
module_platform_driver(ctr_charger_driver);

MODULE_DESCRIPTION("Nintendo 3DS battery/AC driver");
MODULE_AUTHOR("Santiago Herrera");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
