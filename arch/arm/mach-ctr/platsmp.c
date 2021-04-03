// SPDX-License-Identifier: GPL-2.0-only
/*
 * SMP support for the Nintendo 3DS
 *
 * Copyright (C) 2016 Sergi Granell
 * Copyright (C) 2021 Santiago Herrera
 * Copyright (C) 2021 Nick Desaulniers
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/memory.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/smp.h>

#include <asm/smp_scu.h>

#include <mach/platsmp.h>

/*
 * CPUn waits for event
 * [0x1FFFFFF0+ n*4] is where it expects the entrypoint.
 */

#define SECONDARY_STARTUP_ADDR(n)	(0x1FFFFFF0 + ((n)*4))

static int ctr_smp_boot_secondary(unsigned int cpu,
				    struct task_struct *idle)
{
	void __iomem *boot_addr;
	boot_addr = ioremap((phys_addr_t)SECONDARY_STARTUP_ADDR(cpu),
			       sizeof(phys_addr_t));

	/* Set CPU boot address */
	writel(virt_to_phys(ctr_secondary_startup),
		boot_addr);

	iounmap(boot_addr);

	/* Trigger event */
	sev();
	return 0;
}

static void ctr_smp_prepare_cpus(unsigned int max_cpus)
{
	struct device_node *np;
	void __iomem *scu_base;

	np = of_find_compatible_node(NULL, NULL, "arm,arm11mp-scu");
	if (np) {
		scu_base = of_iomap(np, 0);
		scu_enable(scu_base);
		of_node_put(np);
	}
}

static const struct smp_operations ctr_smp_ops __initconst = {
	.smp_prepare_cpus	= ctr_smp_prepare_cpus,
	.smp_boot_secondary	= ctr_smp_boot_secondary,
};
CPU_METHOD_OF_DECLARE(ctr_smp, "nintendo,3ds-smp", &ctr_smp_ops);
