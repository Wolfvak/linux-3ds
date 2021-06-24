// SPDX-License-Identifier: GPL-2.0
/*
 * ctr/leds.c
 *
 * Copyright (C) 2020-2021 Santiago Herrera
 */

#define DRIVER_NAME "3dsmcu-led"
#define pr_fmt(fmt) DRIVER_NAME ": " fmt

#include <linux/of.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/property.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/led-class-multicolor.h>

struct ctr_led {
	struct regmap *map;
	unsigned io_addr;

	struct led_classdev_mc led;
	struct mc_subled subled[3];
};

static void ctr_led_build_data(u8 *data, u8 r, u8 g, u8 b)
{
	data[0] = 0; // delay
	data[1] = 0; // smoothing
	data[2] = 0; // loop_delay
	data[3] = 0; // unknown

	memset(data + 4, r, 32);
	memset(data + 36, g, 32);
	memset(data + 68, b, 32);
}

static int ctr_led_brightness_set_blocking(struct led_classdev *cdev,
					   enum led_brightness brightness)
{
	u8 data[100];
	struct ctr_led *led;

	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(cdev);
	led_mc_calc_color_components(mc_cdev, brightness);

	led = container_of(mc_cdev, struct ctr_led, led);
	ctr_led_build_data(data, led->subled[0].brightness,
			   led->subled[1].brightness,
			   led->subled[2].brightness);

	return regmap_bulk_write(led->map, led->io_addr, data, 100);
}

static int ctr_led_probe(struct platform_device *pdev)
{
	u32 io_addr;
	struct device *dev;
	struct regmap *map;
	struct ctr_led *led;
	struct led_classdev_mc *mc_led;

	dev = &pdev->dev;
	if (!dev->parent)
		return -ENODEV;

	map = dev_get_regmap(dev->parent, NULL);
	if (!map)
		return -ENODEV;

	if (of_property_read_u32(dev->of_node, "reg", &io_addr))
		return -EINVAL;

	led = devm_kzalloc(dev, sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;

	led->map = map;
	led->io_addr = io_addr;
	dev_set_drvdata(dev, led);

	mc_led = &led->led;

	/* initialize main led class */
	mc_led->led_cdev.name = dev_name(dev);
	mc_led->led_cdev.max_brightness = 255;
	mc_led->led_cdev.brightness_set_blocking =
		ctr_led_brightness_set_blocking;

	/* initialize led subchannels */
	mc_led->num_colors = 3;
	mc_led->subled_info = led->subled;

	mc_led->subled_info[0].color_index = LED_COLOR_ID_RED;
	mc_led->subled_info[1].color_index = LED_COLOR_ID_GREEN;
	mc_led->subled_info[2].color_index = LED_COLOR_ID_BLUE;

	return devm_led_classdev_multicolor_register(dev, mc_led);
}

static int ctr_led_remove(struct platform_device *pdev)
{
	struct ctr_led *led = dev_get_drvdata(&pdev->dev);
	struct led_classdev *cdev = &led->led.led_cdev;
	return ctr_led_brightness_set_blocking(cdev, 0);
}

static const struct of_device_id ctr_led_of_match[] = {
	{ .compatible = "nintendo," DRIVER_NAME },
	{}
};
MODULE_DEVICE_TABLE(of, ctr_led_of_match);

static struct platform_driver ctr_led_driver = {
	.probe = ctr_led_probe,
	.remove = ctr_led_remove,

	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = ctr_led_of_match,
	},
};
module_platform_driver(ctr_led_driver);

MODULE_DESCRIPTION("Nintendo 3DS RGB LED driver");
MODULE_AUTHOR("Santiago Herrera");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
