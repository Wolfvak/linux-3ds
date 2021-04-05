// SPDX-License-Identifier: GPL-2.0
/*
 * ctr/accel.c
 *
 * Copyright (C) 2021 Santiago Herrera
 */

#define DRIVER_NAME	"3dsmcu-accel"
#define pr_fmt(fmt)	DRIVER_NAME ": " fmt

/*
 * the hardware device is actually an ST LIS331DLH which is
 * supported by Linux but only when connected to an I2C bus
 * here it's hooked up directly to the MCU device
 * which actually simplifies reading data
*/

#include <linux/of.h>
#include <linux/mutex.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/property.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>

#include <linux/iio/iio.h>
#include <linux/iio/driver.h>
#include <linux/iio/machine.h>

#define REGISTER_ACCEL_MODE	0x40
#define REGISTER_ACCEL_BASE	0x45
// XL, XH, YL, YH, ZL, ZH

#define ACCEL_MODE_DISABLE	(0)
#define ACCEL_MODE_ENABLE	BIT(0)

struct ctr_accel {
	struct device *dev;
	struct regmap *map;

	unsigned pwr;
	struct mutex lock;
};

static int ctr_mcu_accel_toggle_power(struct regmap *map, bool enable)
{
	return regmap_write(map, REGISTER_ACCEL_MODE,
		enable ? ACCEL_MODE_ENABLE : ACCEL_MODE_DISABLE);
}

static int ctr_mcu_accel_read(struct regmap *map, unsigned reg, int *res)
{
	int err;
	u8 data[2];
	s16 data_word;

	err = regmap_raw_read(map, REGISTER_ACCEL_BASE + reg, data, 2);
	if (err)
		return err;

	data_word = (data[1] << 8) | data[0];
	*res = data_word;
	return 0;
}

static int ctr_mcu_accel_read_raw(struct iio_dev *indio_dev,
								struct iio_chan_spec const *chan,
								int *val, int *val2, long mask)
{
	int err;
	struct ctr_accel *accel = iio_priv(indio_dev);

	switch(mask) {
		case IIO_CHAN_INFO_RAW:
			err = ctr_mcu_accel_read(accel->map, chan->address, val);
			return err ? err : IIO_VAL_INT;

		case IIO_CHAN_INFO_ENABLE:
			*val = accel->pwr;
			return IIO_VAL_INT;

		case IIO_CHAN_INFO_SCALE:
			*val = 0;
			*val2 = 598755; // *= ~0.000598755
			return IIO_VAL_INT_PLUS_NANO;

		default:
			return -EINVAL;
	}
}

static int ctr_mcu_accel_write_raw(struct iio_dev *indio_dev,
								struct iio_chan_spec const *chan,
								int val, int val2, long mask)
{
	int err;
	struct ctr_accel *accel = iio_priv(indio_dev);

	switch(mask) {
		case IIO_CHAN_INFO_ENABLE:
		{
			mutex_lock(&accel->lock);
			val = val ? 1 : 0;
			err = ctr_mcu_accel_toggle_power(accel->map, val);
			if (!err)
				accel->pwr = val;
			mutex_unlock(&accel->lock);
			return err;
		}

		default:
			return -EINVAL;
	}
}

static const struct iio_info ctr_mcu_accel_info = {
	.read_raw = ctr_mcu_accel_read_raw,
	.write_raw = ctr_mcu_accel_write_raw,
};

#define CTR_MCU_ACCEL_CHANNEL(_chan, _subchan, _addr) \
	{ \
		.indexed = 1, \
		.type = IIO_ACCEL, \
		.channel = _chan, \
		.channel2 = _subchan, \
		.address = _addr, \
		.scan_type = { \
			.sign = 's', \
			.realbits = 16, \
			.storagebits = 16, \
			.shift = 0, \
		}, \
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
		.info_mask_shared_by_type = \
			BIT(IIO_CHAN_INFO_SCALE) | BIT(IIO_CHAN_INFO_ENABLE), \
	}

static const struct iio_chan_spec ctr_mcu_accel_channels[] = {
	CTR_MCU_ACCEL_CHANNEL(0, IIO_MOD_X, 0), // X
	CTR_MCU_ACCEL_CHANNEL(1, IIO_MOD_Y, 2), // Y
	CTR_MCU_ACCEL_CHANNEL(2, IIO_MOD_Z, 4), // Z
};

static int ctr_mcu_accel_probe(struct platform_device *pdev)
{
	int err;
	struct device *dev;
	struct regmap *regmap;
	struct ctr_accel *accel;
	struct iio_dev *indio_dev;

	dev = &pdev->dev;
	if (!dev->parent)
		return -ENODEV;

	regmap = dev_get_regmap(dev->parent, NULL);
	if (!regmap)
		return -ENODEV;

	platform_set_drvdata(pdev, indio_dev);

	err = ctr_mcu_accel_toggle_power(regmap, false);
	if (err)
		return err;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*accel));
	if (!indio_dev)
		return -ENOMEM;

	accel = iio_priv(indio_dev);

	accel->pwr = 0;
	accel->dev = dev;
	accel->map = regmap;
	mutex_init(&accel->lock);

	indio_dev->name = DRIVER_NAME;
	indio_dev->channels = ctr_mcu_accel_channels;
	indio_dev->num_channels = ARRAY_SIZE(ctr_mcu_accel_channels);
	indio_dev->info = &ctr_mcu_accel_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	return devm_iio_device_register(dev, indio_dev);
}

static const struct of_device_id ctr_mcu_accel_of_match[] = {
	{ .compatible = "nintendo," DRIVER_NAME, },
	{}
};
MODULE_DEVICE_TABLE(of, ctr_mcu_accel_of_match);

static struct platform_driver ctr_mcu_accel_driver = {
	.probe = ctr_mcu_accel_probe,

	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(ctr_mcu_accel_of_match),
	},
};
module_platform_driver(ctr_mcu_accel_driver);

MODULE_DESCRIPTION("Nintendo 3DS MCU Accelerometer driver");
MODULE_AUTHOR("Santiago Herrera");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
