// SPDX-License-Identifier: GPL-2.0
/*
 * ctr/accel.c
 *
 * Copyright (C) 2021 Santiago Herrera
 */

#define DRIVER_NAME "3dsmcu-accel"
#define pr_fmt(fmt) DRIVER_NAME ": " fmt

/*
 * the hardware device is actually an ST LIS331DLH which is
 * supported by Linux but only when connected to an I2C bus
 * here it's hooked up directly to the MCU device
 * which actually simplifies reading data
*/

#include <linux/of.h>
#include <linux/pm.h>
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

#define ACCELEROMETER_OFF	(0)
#define ACCELEROMETER_ON	(BIT(0))

#define ACCELEROMETER_UPDATE_PERIOD	(HZ / 50)
/* update at most 50 times per second */

#define REG_MODE	0x00
#define REG_DATA	0x05

#define CTR_ACCEL_NSCALE	(598755)

struct ctr_accel {
	struct regmap *map;

	int pwr;
	s16 data[3];
	unsigned io_addr;

	unsigned long last_chk;
};

static int ctr_accel_set_power(struct ctr_accel *acc, int pwr)
{
	int err = regmap_write(acc->map, acc->io_addr + REG_MODE,
			       pwr > 0 ? ACCELEROMETER_ON : ACCELEROMETER_OFF);

	if (!err) {
		acc->pwr = pwr;
		usleep_range(250, 350);
		memset(acc->data, 0, sizeof(acc->data));
	}
	return err;
}

static void ctr_accel_update_data(struct ctr_accel *acc)
{
	int err;

	if (time_is_after_jiffies(acc->last_chk + ACCELEROMETER_UPDATE_PERIOD))
		return;

	if (!acc->pwr)
		return;

	err = regmap_bulk_read(acc->map, acc->io_addr + REG_DATA, acc->data,
			       sizeof(acc->data));
	if (err)
		memset(acc->data, 0, sizeof(acc->data));
	else
		acc->last_chk = jiffies;
}

static int ctr_accel_read_raw(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan, int *val,
			      int *val2, long mask)
{
	int err;
	struct ctr_accel *acc = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ctr_accel_update_data(acc);
		if (chan->address < 3) {
			*val = sign_extend32(acc->data[chan->address], 15);
			err = IIO_VAL_INT;
		} else {
			err = -EINVAL;
		}
		break;

	case IIO_CHAN_INFO_ENABLE:
		*val = acc->pwr;
		err = IIO_VAL_INT;
		break;

	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		*val2 = CTR_ACCEL_NSCALE;
		err = IIO_VAL_INT_PLUS_NANO;
		break;

	default:
		err = -EINVAL;
		break;
	}

	return err;
}

static int ctr_accel_write_raw(struct iio_dev *indio_dev,
			       struct iio_chan_spec const *chan, int val,
			       int val2, long mask)
{
	int err;
	struct ctr_accel *acc = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_ENABLE:
		err = ctr_accel_set_power(acc, val);
		break;

	default:
		err = -EINVAL;
		break;
	}

	return err;
}

static const struct iio_info ctr_accel_ops = {
	.read_raw = ctr_accel_read_raw,
	.write_raw = ctr_accel_write_raw,
};

#define CTR_ACCEL_CHANNEL(addr, subchan)                                       \
	{                                                                      \
		.type = IIO_ACCEL, \
		.address = (addr), \
		.channel2 = (subchan), \
		.modified = 1, \
		.scan_type = { \
			.sign = 's', \
			.realbits = 16, \
			.storagebits = 16, \
			.endianness = IIO_LE, \
		}, \
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
		.info_mask_shared_by_type = \
			BIT(IIO_CHAN_INFO_SCALE) | BIT(IIO_CHAN_INFO_ENABLE),  \
	}

static const struct iio_chan_spec ctr_accel_channels[] = {
	CTR_ACCEL_CHANNEL(0, IIO_MOD_X),
	CTR_ACCEL_CHANNEL(1, IIO_MOD_Y),
	CTR_ACCEL_CHANNEL(2, IIO_MOD_Z),
};

static int ctr_accel_probe(struct platform_device *pdev)
{
	u32 io_addr;
	int err;
	struct device *dev;
	struct regmap *regmap;
	struct ctr_accel *acc;
	struct iio_dev *indio_dev;

	dev = &pdev->dev;
	if (!dev->parent)
		return -ENODEV;

	regmap = dev_get_regmap(dev->parent, NULL);
	if (!regmap)
		return -ENODEV;

	if (of_property_read_u32(dev->of_node, "reg", &io_addr))
		return -EINVAL;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*acc));
	if (!indio_dev)
		return -ENOMEM;

	acc = iio_priv(indio_dev);

	acc->map = regmap;
	acc->io_addr = io_addr;
	acc->last_chk = jiffies;

	err = ctr_accel_set_power(acc, 0);
	if (err < 0)
		return err;

	indio_dev->name = dev_name(dev);
	indio_dev->channels = ctr_accel_channels;
	indio_dev->num_channels = ARRAY_SIZE(ctr_accel_channels);
	indio_dev->info = &ctr_accel_ops;
	indio_dev->modes = INDIO_DIRECT_MODE;

	dev_set_drvdata(dev, indio_dev);
	err = devm_iio_device_register(dev, indio_dev);
	if (err < 0)
		return err;

	return 0;
}

static int ctr_accel_remove(struct platform_device *pdev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(&pdev->dev);
	struct ctr_accel *acc = iio_priv(indio_dev);
	return ctr_accel_set_power(acc, 0);
}

#ifdef CONFIG_PM_SLEEP
static int ctr_accel_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct ctr_accel *acc = iio_priv(indio_dev);
	return ctr_accel_set_power(acc, 0);
}

static int ctr_accel_resume(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct ctr_accel *acc = iio_priv(indio_dev);
	return ctr_accel_set_power(acc, acc->pwr);
}
#endif

static SIMPLE_DEV_PM_OPS(ctr_accel_pm_ops, ctr_accel_suspend, ctr_accel_resume);

static const struct of_device_id ctr_accel_of_match[] = {
	{ .compatible = "nintendo," DRIVER_NAME },
	{}
};
MODULE_DEVICE_TABLE(of, ctr_accel_of_match);

static struct platform_driver ctr_accel_driver = {
	.probe = ctr_accel_probe,
	.remove = ctr_accel_remove,

	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.pm = &ctr_accel_pm_ops,
		.of_match_table = of_match_ptr(ctr_accel_of_match),
	},
};
module_platform_driver(ctr_accel_driver);

MODULE_DESCRIPTION("Nintendo 3DS MCU Accelerometer driver");
MODULE_AUTHOR("Santiago Herrera");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
