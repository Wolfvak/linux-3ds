// SPDX-License-Identifier: GPL-2.0-only
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irqchip/arm-gic.h>
#include <linux/platform_device.h>

#include <linux/clk-provider.h>
#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/smp_twd.h>

#include <mach/hardware.h>
#include <mach/platform.h>
#include <mach/bottom_lcd.h>

static void __init ctr_pdn_setup(void)
{
	void __iomem *pdn_spi_cnt;

	pdn_spi_cnt = ioremap(NINTENDO3DS_REG_PDN_SPI_CNT, 4);
	iowrite16(ioread16(pdn_spi_cnt) | 7, pdn_spi_cnt);
	iounmap(pdn_spi_cnt);
}

static void __init ctr_dt_init_machine(void)
{
	printk("ctr_dt_init_machine\n");

	nintendo3ds_bottom_setup_fb();
	nintendo3ds_bottom_lcd_map_fb();
	ctr_pdn_setup();

	of_platform_populate(NULL, of_default_bus_match_table, NULL, NULL);
}

static const char __initconst *ctr_dt_platform_compat[] = {
	"nintendo,3ds",
	NULL,
};

DT_MACHINE_START(CTR_DT, "Nintendo 3DS/CTR (Device Tree)")
	.init_machine	= ctr_dt_init_machine,
	.dt_compat	= ctr_dt_platform_compat,
MACHINE_END
