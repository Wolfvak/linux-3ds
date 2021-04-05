// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/arch/arm/mach-nintendo3ds/headsmp.S
 *
 *  Cloned from linux/arch/arm/plat-versatile/headsmp.S
 *
 *  Copyright (c) 2016 Sergi Granell <xerpi.g.12@gmail.com>
 */
#include <linux/linkage.h>
#include <linux/init.h>

/*
 * CPU1 comes with the cache dirty from the bootrom,
 * so clean the cache before jumping to the kernel.
 */

ENTRY(ctr_secondary_startup)
	cpsid aif

	mov r0, #0

	/* Invalidate Both Caches & flush the branch target cache */
	mcr p15, 0, r0, c7, c7, 0

	/* Flush Prefetch Buffer */
	mcr p15, 0, r0, c7, c5, 4

	/* Flush Entire Branch Target Cache */
	mcr p15, 0, r0, c7, c5, 6

	/* Data Synchronization Barrier */
	mcr p15, 0, r0, c7, c10, 4

	b secondary_startup
ENDPROC(ctr_secondary_startup)
