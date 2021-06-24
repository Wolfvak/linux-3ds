// SPDX-License-Identifier: GPL-2.0
/*
 * ctr/rtc.c
 *
 * Copyright (C) 2020-2021 Santiago Herrera
 */

#define DRIVER_NAME "3dsmcu-rtc"
#define pr_fmt(fmt) DRIVER_NAME ": " fmt

#include <linux/of.h>
#include <linux/bcd.h>
#include <linux/rtc.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>

struct ctr_rtc {
	struct regmap *map;
	unsigned io_addr;
};

static int ctr_rtc_get_time(struct device *dev, struct rtc_time *tm)
{
	int err;
	u8 buf[8];
	struct ctr_rtc *rtc = dev_get_drvdata(dev);

	err = regmap_bulk_read(rtc->map, rtc->io_addr, buf, 7);
	if (err)
		return err;

	tm->tm_sec = bcd2bin(buf[0]) % 60;
	tm->tm_min = bcd2bin(buf[1]) % 60;
	tm->tm_hour = bcd2bin(buf[2]) % 24;
	tm->tm_mday = bcd2bin(buf[4]) % 32;
	tm->tm_mon = (bcd2bin(buf[5]) - 1) % 12;
	tm->tm_year = bcd2bin(buf[6]) + 100;
	return 0;
}

static int ctr_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	u8 buf[8];
	struct ctr_rtc *rtc = dev_get_drvdata(dev);

	buf[0] = bin2bcd(tm->tm_sec);
	buf[1] = bin2bcd(tm->tm_min);
	buf[2] = bin2bcd(tm->tm_hour);
	buf[4] = bin2bcd(tm->tm_mday);
	buf[5] = bin2bcd(tm->tm_mon + 1);
	buf[6] = bin2bcd(tm->tm_year - 100);

	return regmap_bulk_write(rtc->map, rtc->io_addr, buf, 7);
}

static const struct rtc_class_ops ctr_rtc_ops = {
	.read_time = ctr_rtc_get_time,
	.set_time = ctr_rtc_set_time,
};

static int ctr_rtc_probe(struct platform_device *pdev)
{
	u32 io_addr;
	struct device *dev;
	struct ctr_rtc *rtc;
	struct regmap *regmap;

	dev = &pdev->dev;
	if (!dev->parent)
		return -ENODEV;

	regmap = dev_get_regmap(dev->parent, NULL);
	if (!regmap)
		return -ENODEV;

	if (of_property_read_u32(dev->of_node, "reg", &io_addr))
		return -EINVAL;

	rtc = devm_kzalloc(dev, sizeof(*rtc), GFP_KERNEL);
	if (!rtc)
		return -ENOMEM;

	rtc->map = regmap;
	rtc->io_addr = io_addr;
	platform_set_drvdata(pdev, rtc);

	return PTR_ERR_OR_ZERO(devm_rtc_device_register(
		dev, dev_name(dev), &ctr_rtc_ops, THIS_MODULE));
}

static int ctr_rtc_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id ctr_rtc_of_match[] = {
	{ .compatible = "nintendo," DRIVER_NAME },
	{}
};
MODULE_DEVICE_TABLE(of, ctr_rtc_of_match);

static struct platform_driver ctr_rtc_driver = {
	.probe = ctr_rtc_probe,
	.remove = ctr_rtc_remove,

	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(ctr_rtc_of_match),
	},
};
module_platform_driver(ctr_rtc_driver);

MODULE_DESCRIPTION("Nintendo 3DS MCU Real Time Clock driver");
MODULE_AUTHOR("Santiago Herrera");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
