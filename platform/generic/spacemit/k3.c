/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 SpacemiT
 * Authors:
 *   Xianbin Zhu <xianbin.zhu@linux.spacemit.com>
 *   Troy Mitchell <troy.mitchell@linux.spacemit.com>
 */

#include <platform_override.h>
#include <sbi/riscv_asm.h>
#include <sbi/riscv_io.h>
#include <spacemit/k3.h>
#include <spacemit/k3_asm.h>
#include <spacemit/spacemit.h>

static const int cci_map[] = {
	PLAT_CCI_K3_AIDMA_IFACE_IX,
	PLAT_CCI_K3_CLUSTER0_IFACE_IX,
	PLAT_CCI_K3_CLUSTER1_IFACE_IX,
	PLAT_CCI_K3_CLUSTER2_0_IFACE_IX,
	PLAT_CCI_K3_CLUSTER2_1_IFACE_IX,
	PLAT_CCI_K3_CLUSTER3_0_IFACE_IX,
	PLAT_CCI_K3_CLUSTER3_1_IFACE_IX,
};

/* The boot hart is already running, so it is never started by the HSM. */
static void spacemit_k3_keep_boot_hart_powered(void)
{
	u32 value;

	value = readl((void *)(unsigned long)PMU_CX_CAPMP_IDLE_CFG0);
	value &= ~CLUSTER_PWR_DOWN_VALUE;
	writel(value, (void *)(unsigned long)PMU_CX_CAPMP_IDLE_CFG0);

	value = readl((void *)(unsigned long)PMU_AP_CORE0_IDLE_CFG);
	value &= ~PMU_AP_IDLE_PWRDOWN_K3_MASK;
	writel(value, (void *)(unsigned long)PMU_AP_CORE0_IDLE_CFG);
}

/* Program global state before the HSM starts any secondary hart. */
static int spacemit_k3_pre_init(void)
{
	unsigned long entry = (unsigned long)&_spacemit_k3_warm_start;
	int i, rc;

	writel((u32)entry, (void *)(unsigned long)C0_RVBADDR_LO_ADDR);
	writel((u32)(entry >> 32), (void *)(unsigned long)C0_RVBADDR_HI_ADDR);

	writel((u32)entry, (void *)(unsigned long)C1_RVBADDR_LO_ADDR);
	writel((u32)(entry >> 32), (void *)(unsigned long)C1_RVBADDR_HI_ADDR);

	writel((u32)entry, (void *)(unsigned long)C2_RVBADDR_LO_ADDR);
	writel((u32)(entry >> 32), (void *)(unsigned long)C2_RVBADDR_HI_ADDR);

	writel((u32)entry, (void *)(unsigned long)C3_RVBADDR_LO_ADDR);
	writel((u32)(entry >> 32), (void *)(unsigned long)C3_RVBADDR_HI_ADDR);

	for (i = 0; i < array_size(cci_map); i++) {
		rc = cci_enable_snoop_dvm_reqs(cci_map, i);
		if (rc)
			return rc;
	}

	spacemit_k3_keep_boot_hart_powered();
	return 0;
}

static int spacemit_k3_early_init(bool cold_boot)
{
	int rc;

	if (cold_boot) {
		rc = spacemit_k3_pre_init();
		if (rc)
			return rc;
	}

	return generic_early_init(cold_boot);
}

static bool spacemit_k3_cold_boot_allowed(u32 hartid)
{
	return !hartid;
}

static int spacemit_k3_platform_init(const void *fdt, int nodeoff,
				     const struct fdt_match *match)
{
	_spacemit_k3_hart_init();
	generic_platform_ops.early_init = spacemit_k3_early_init;
	generic_platform_ops.cold_boot_allowed = spacemit_k3_cold_boot_allowed;

	return 0;
}

static const struct fdt_match spacemit_k3_match[] = {
	{ .compatible = "spacemit,k3" },
	{ /* sentinel */ }
};

const struct fdt_driver spacemit_k3 = {
	.match_table = spacemit_k3_match,
	.init = spacemit_k3_platform_init,
};
