// SPDX-License-Identifier: GPL-2.0
/*
 *  ctr_i2c.c
 *
 *  Copyright (C) 2021 Sönke Holz
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/backlight.h>

#define DRIVER_NAME "3ds-bl"
#define MAX_BRIGHTNESS 0x3ff

struct ctr_backlight {
	void __iomem *reg;
};

static int ctr_backlight_update_status(struct backlight_device *bl)
{
	struct ctr_backlight *ctr_bl = bl_get_data(bl);

	iowrite16(backlight_get_brightness(bl), ctr_bl->reg);

	return 0;
}

static int ctr_backlight_get_brightness(struct backlight_device *bl)
{
	struct ctr_backlight *ctr_bl = bl_get_data(bl);

	return ioread16(ctr_bl->reg);
}

static const struct backlight_ops ctr_backlight_ops = {
	.get_brightness = ctr_backlight_get_brightness,
	.update_status = ctr_backlight_update_status,
	.options = BL_CORE_SUSPENDRESUME,
};

static int ctr_backlight_probe(struct platform_device *pdev)
{
	struct backlight_properties props;
	struct backlight_device *bl;
	struct ctr_backlight *ctr_bl;

	ctr_bl = devm_kmalloc(&pdev->dev, sizeof(*ctr_bl), GFP_KERNEL);
	if (IS_ERR(ctr_bl))
		return PTR_ERR(ctr_bl);

	ctr_bl->reg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ctr_bl->reg))
		return PTR_ERR(ctr_bl->reg);

	memset(&props, 0, sizeof(props));
	props.max_brightness = MAX_BRIGHTNESS;
	props.type = BACKLIGHT_RAW;
	bl = devm_backlight_device_register(&pdev->dev, pdev->name, &pdev->dev,
					ctr_bl, &ctr_backlight_ops, &props);
	if (IS_ERR(bl)) {
		dev_err(&pdev->dev, "failed to register backlight\n");
		return PTR_ERR(bl);
	}

	platform_set_drvdata(pdev, bl);

	return 0;
}

static const struct of_device_id ctr_backlight_of_match[] = {
	{ .compatible = "nintendo," DRIVER_NAME, },
	{ /* sentinel */ },
};

MODULE_DEVICE_TABLE(of, ctr_backlight_of_match);

static struct platform_driver ctr_backlight_driver = {
	.driver	= {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
		.of_match_table	= of_match_ptr(ctr_backlight_of_match),
	},
	.probe	= ctr_backlight_probe,
};

module_platform_driver(ctr_backlight_driver);

MODULE_DESCRIPTION("Nintendo 3DS backlight driver");
MODULE_AUTHOR("Sönke Holz");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
