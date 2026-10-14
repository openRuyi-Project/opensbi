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
#include <sbi/sbi_error.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_scratch.h>
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

/* Program global state before the HSM starts any secondary hart. */
static int spacemit_k3_pre_init(void)
{
	static const unsigned long vector[] = {
		C0_RVBADDR_LO_ADDR, C1_RVBADDR_LO_ADDR,
		C2_RVBADDR_LO_ADDR, C3_RVBADDR_LO_ADDR,
	};
	struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();
	unsigned long entry = (unsigned long)&_spacemit_k3_warm_start;
	unsigned long address, end;
	int i, rc;

	if (!sbi_hart_has_extension(scratch, SBI_HART_EXT_ZICBOM))
		return SBI_ENOTSUPP;
	if (!scratch->fw_size || scratch->fw_start > ~0UL - scratch->fw_size)
		return SBI_EINVAL;

	/* Coreboot loads OpenSBI with caches enabled. A reset hart fetches
	 * instructions before enabling snooping, so publish the full image.
	 */
	end = scratch->fw_start + scratch->fw_size;
	address = scratch->fw_start & ~63UL;
	/* Order cached handoff stores before publishing the firmware. */
	mb();
	for (;;) {
		_spacemit_k3_clean_block(address);
		if (end - address <= 64)
			break;
		address += 64;
	}
	/* Complete cache publication before installing reset vectors. */
	mb();

	for (i = 0; i < array_size(vector); i++) {
		writel((u32)entry, (void *)vector[i]);
		writel((u32)(entry >> 32), (void *)(vector[i] + 4));
		/* Order vector writes before their readback. */
		mb();
		if (readl((void *)vector[i]) != (u32)entry ||
		    readl((void *)(vector[i] + 4)) != (u32)(entry >> 32))
			return SBI_EIO;
	}

	for (i = 0; i < array_size(cci_map); i++) {
		rc = cci_enable_snoop_dvm_reqs(cci_map, i);
		if (rc)
			return rc;
	}

	return spacemit_k3_prepare_hart(current_hartid());
}

static int spacemit_k3_early_init(bool cold_boot)
{
	int rc;

	rc = generic_early_init(cold_boot);
	if (rc)
		return rc;

	if (cold_boot)
		return spacemit_k3_pre_init();

	return spacemit_k3_prepare_hart(current_hartid());
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
