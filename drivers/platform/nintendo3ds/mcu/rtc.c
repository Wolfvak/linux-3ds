// SPDX-License-Identifier: GPL-2.0
/*
 * ctr/rtc.c
 *
 * Copyright (C) 2020-2021 Santiago Herrera
 */

#define DRIVER_NAME	"3dsmcu-rtc"
#define pr_fmt(fmt)	DRIVER_NAME ": " fmt

#include <linux/of.h>
#include <linux/bcd.h>
#include <linux/rtc.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>

#define REGISTER_RTC	0x30

static int ctr_mcu_rtc_get_time(struct device *dev, struct rtc_time *tm)
{
	int err;
	u8 buf[8];
	struct regmap *regmap = dev_get_drvdata(dev);

	err = regmap_raw_read(regmap, REGISTER_RTC, buf, 7);
	if (err)
		return err;

	tm->tm_sec	= bcd2bin(buf[0]) % 60;
	tm->tm_min	= bcd2bin(buf[1]) % 60;
	tm->tm_hour	= bcd2bin(buf[2]) % 24;
	tm->tm_mday	= bcd2bin(buf[4]) % 32;
	tm->tm_mon	= (bcd2bin(buf[5]) - 1) % 12;
	tm->tm_year	= bcd2bin(buf[6]) + 100;
	return 0;
}

static int ctr_mcu_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	u8 buf[8];
	struct regmap *regmap = dev_get_drvdata(dev);

	buf[0] = bin2bcd(tm->tm_sec);
	buf[1] = bin2bcd(tm->tm_min);
	buf[2] = bin2bcd(tm->tm_hour);
	buf[4] = bin2bcd(tm->tm_mday);
	buf[5] = bin2bcd(tm->tm_mon + 1);
	buf[6] = bin2bcd(tm->tm_year - 100);

	return regmap_raw_write(regmap, REGISTER_RTC, buf, 7);
}

static const struct rtc_class_ops ctr_mcu_rtc_ops = {
	.read_time	= ctr_mcu_rtc_get_time,
	.set_time	= ctr_mcu_rtc_set_time,
};

static int ctr_mcu_rtc_probe(struct platform_device *pdev)
{	
	struct device *dev;
	struct regmap *regmap;
	struct rtc_device *rtc;

	dev = &pdev->dev;
	if (!dev->parent)
		return -ENODEV;

	regmap = dev_get_regmap(dev->parent, NULL);
	if (!regmap)
		return -ENODEV;

	platform_set_drvdata(pdev, regmap);

	rtc = devm_rtc_device_register(dev, DRIVER_NAME,
		&ctr_mcu_rtc_ops, THIS_MODULE);
	if (IS_ERR(rtc))
		return PTR_ERR(rtc);

	return 0;
}

static const struct of_device_id ctr_mcu_rtc_of_match[] = {
	{ .compatible = "nintendo," DRIVER_NAME, },
	{}
};
MODULE_DEVICE_TABLE(of, ctr_mcu_rtc_of_match);

static struct platform_driver ctr_mcu_rtc_driver = {
	.probe = ctr_mcu_rtc_probe,

	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(ctr_mcu_rtc_of_match),
	},
};
module_platform_driver(ctr_mcu_rtc_driver);

MODULE_DESCRIPTION("Nintendo 3DS MCU Real Time Clock driver");
MODULE_AUTHOR("Santiago Herrera");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
