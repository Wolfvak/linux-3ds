// SPDX-License-Identifier: GPL-2.0-only
/**
 * ctr_tsc.c
 *
 * Provides a linear regmap interface for the TSC2117
 * chip used in the Nintendo (3)DS consoles
 */

#define DRIVER_NAME "3ds-tsc"
#define pr_fmt(fmt) DRIVER_NAME ": " fmt

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>
#include <linux/of_platform.h>

struct ctr_tsc {
	struct device *dev;
	struct spi_device *spi;
	int banksel; /* currently selected bank */
};

static int ctr_tsc_switch_bank(struct ctr_tsc *cdc, const u8 *bank)
{
	u8 banksel[2];
	int err, bank_id;

	bank_id = *bank;

	/* already using the selected bank */
	if (cdc->banksel == bank_id)
		return 0;

	banksel[0] = 0; /* register 0, write */
	banksel[1] = bank_id;
	err = spi_write(cdc->spi, banksel, 2);
	cdc->banksel = err ? -1 : bank_id;
	return err;
}

static int ctr_tsc_read(void *context, const void *reg_buf, size_t reg_len,
			void *val_buf, size_t val_len)
{
	int err;
	struct ctr_tsc *cdc = context;

	if (reg_len != 2)
		return -EINVAL;

	err = ctr_tsc_switch_bank(cdc, reg_buf);
	if (err)
		return err;

	return spi_write_then_read(cdc->spi, reg_buf + 1, 1, val_buf, val_len);
}

static int ctr_tsc_write(void *context, const void *data, size_t len)
{
	int err;
	struct ctr_tsc *cdc = context;

	if (len < 3)
		return -EINVAL;

	err = ctr_tsc_switch_bank(cdc, data);
	if (err)
		return err;

	return spi_write(cdc->spi, data + 1, len - 1);
}

static int ctr_tsc_gather_write(void *context, const void *reg, size_t reg_len,
				const void *val, size_t val_len)
{
	int err;
	struct spi_transfer xfer[2];
	struct ctr_tsc *cdc = context;

	if (reg_len != 2)
		return -EINVAL;

	err = ctr_tsc_switch_bank(cdc, reg);
	if (err)
		return err;

	memset(xfer, 0, sizeof(xfer));

	xfer[0].tx_buf = reg + 1;
	xfer[0].len = 1;

	xfer[1].tx_buf = val;
	xfer[1].len = val_len;

	return spi_sync_transfer(cdc->spi, xfer, 2);
}

static const struct regmap_bus ctr_tsc_map_bus = {
	.read = ctr_tsc_read,
	.write = ctr_tsc_write,
	.gather_write = ctr_tsc_gather_write,

	.reg_format_endian_default = REGMAP_ENDIAN_BIG,
	.val_format_endian_default = REGMAP_ENDIAN_LITTLE,
};

static const struct regmap_config ctr_tsc_map_cfg = {
	.reg_bits = 15, /* [8:7:1] = [page:index:read], byteswapped */
	.pad_bits = 1,

	.val_bits = 8,

	.read_flag_mask = 0x100,
	.write_flag_mask = 0,
	.zero_flag_mask = true,

	.cache_type = REGCACHE_NONE,
};

static int ctr_tsc_sw_reset(struct ctr_tsc *cdc)
{
	/* bank 0, register 1, write 1 */
	static const u8 cdc_init_data[] = { 0x00, (1 << 1) | 1, 0x01 };
	return ctr_tsc_write(cdc, cdc_init_data, 3);
}

static int ctr_tsc_probe(struct spi_device *spi)
{
	int err;
	struct device *dev;
	struct regmap *map;
	struct ctr_tsc *cdc;

	dev = &spi->dev;
	cdc = devm_kzalloc(dev, sizeof(*cdc), GFP_KERNEL);
	if (!cdc)
		return -ENOMEM;

	cdc->dev = dev;
	cdc->spi = spi;
	cdc->banksel = -1; /* don't assume any selected bank by default */

	/* reset the chip into a known-good state */
	err = ctr_tsc_sw_reset(cdc);
	mdelay(20);
	if (err)
		return err;

	map = devm_regmap_init(dev, &ctr_tsc_map_bus, cdc, &ctr_tsc_map_cfg);
	if (IS_ERR(map))
		return PTR_ERR(map);

	return devm_of_platform_populate(dev);
}

static const struct of_device_id ctr_tsc_of_match[] = {
	{ .compatible = "nintendo," DRIVER_NAME },
	{}
};
MODULE_DEVICE_TABLE(of, ctr_tsc_of_match);

static struct spi_driver ctr_tsc_driver = {
	.probe = ctr_tsc_probe,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = ctr_tsc_of_match,
	},
};
module_spi_driver(ctr_tsc_driver);

MODULE_AUTHOR("Santiago Herrera");
MODULE_DESCRIPTION("Nintendo 3DS TSC regmap driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" DRIVER_NAME);
