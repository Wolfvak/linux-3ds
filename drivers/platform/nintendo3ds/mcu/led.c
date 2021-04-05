// SPDX-License-Identifier: GPL-2.0
/*
 * ctr/leds.c
 *
 * Copyright (C) 2020 Santiago Herrera
 */

#define DRIVER_NAME	"3dsmcu-led"
#define pr_fmt(fmt)	DRIVER_NAME ": " fmt

#include <linux/of.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/property.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/led-class-multicolor.h>

#define REGISTER_LED	0x2D

struct ctr_mcu_led {
	struct regmap *regmap;
	struct led_classdev_mc led;
	struct mc_subled subled[3];
};

static void ctr_mcu_led_build_data(u8 *data, u8 r, u8 g, u8 b)
{
	data[0] = 0; // delay
	data[1] = 0; // smoothing
	data[2] = 0; // loop_delay
	data[3] = 0; // unknown

	memset(data + 4, r, 32);
	memset(data + 36, g, 32);
	memset(data + 68, b, 32);
}

static int ctr_mcu_led_brightness_set_blocking(struct led_classdev *cdev,
											enum led_brightness brightness)
{
	u8 led_data[100];
	struct ctr_mcu_led *ctr_led;

	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(cdev);
	led_mc_calc_color_components(mc_cdev, brightness);

	ctr_led = container_of(mc_cdev, struct ctr_mcu_led, led);
	ctr_mcu_led_build_data(led_data,
		ctr_led->subled[0].brightness,
		ctr_led->subled[1].brightness,
		ctr_led->subled[2].brightness
	);

	return regmap_raw_write(ctr_led->regmap, REGISTER_LED, led_data, 100);
}

static int ctr_mcu_led_probe(struct platform_device *pdev)
{
	struct device *dev;
	struct regmap *mcu_regmap;
	struct ctr_mcu_led *mcu_led;
	struct led_classdev_mc *led;

	dev = &pdev->dev;
	if (!dev->parent)
		return -ENODEV;

	mcu_regmap = dev_get_regmap(dev->parent, NULL);
	if (!mcu_regmap)
		return -ENODEV;

	mcu_led = devm_kzalloc(dev, sizeof(*mcu_led), GFP_KERNEL);
	if (!mcu_led)
		return -ENOMEM;

	mcu_led->regmap = mcu_regmap;
	platform_set_drvdata(pdev, mcu_led);

	led = &mcu_led->led;

	/* initialize main led class */
	led->led_cdev.name = DRIVER_NAME;
	led->led_cdev.max_brightness = 255;
	led->led_cdev.brightness_set_blocking =
		ctr_mcu_led_brightness_set_blocking;

	/* initialize led subchannels */
	led->num_colors = 3;
	led->subled_info = mcu_led->subled;

	led->subled_info[0].color_index = LED_COLOR_ID_RED;
	led->subled_info[1].color_index = LED_COLOR_ID_GREEN;
	led->subled_info[2].color_index = LED_COLOR_ID_BLUE;

	return devm_led_classdev_multicolor_register(dev, led);
}

static const struct of_device_id ctr_mcu_led_of_match[] = {
	{ .compatible = "nintendo," DRIVER_NAME },
	{}
};
MODULE_DEVICE_TABLE(of, ctr_mcu_led_of_match);

static struct platform_driver ctr_mcu_led_driver = {
	.probe = ctr_mcu_led_probe,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = ctr_mcu_led_of_match,
	},
};
module_platform_driver(ctr_mcu_led_driver);

MODULE_DESCRIPTION("Nintendo 3DS RGB LED driver");
MODULE_AUTHOR("Santiago Herrera");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
