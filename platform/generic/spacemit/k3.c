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
#include <sbi/sbi_console.h>
#include <sbi/sbi_timer.h>
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

static int spacemit_k3_set_vector(unsigned long vector, unsigned long entry)
{
	writel((u32)entry, (void *)vector);
	writel((u32)(entry >> 32), (void *)(vector + 4));
	/* Order both vector halves before readback and wakeup. */
	mb();
	if (readl((void *)vector) != (u32)entry ||
	    readl((void *)(vector + 4)) != (u32)(entry >> 32))
		return SBI_EIO;
	return 0;
}

#define K3_DCIU_BASE 0xd8440000
#define K3_DMASYS_RESET (K3_DCIU_BASE + 0x22c)
#define K3_DMASYS_CLOCK (K3_DCIU_BASE + 0x234)

unsigned long spacemit_k3_prewarm_data[K3_PREWARM_WORDS] __aligned(64);

static bool prewarm_done(void *arg)
{
	const unsigned long *data = arg;

	return readl((void *)data[K3_PREWARM_VECTOR]) == data[K3_PREWARM_VECTOR_LO] &&
	       readl((void *)(data[K3_PREWARM_VECTOR] + 4)) == data[K3_PREWARM_VECTOR_HI];
}

static int spacemit_k3_prewarm(void)
{
	static const struct {
		u32 hart;
		unsigned long idle, wake, vector;
	} clusters[] = {
		{ 8, PMU_CAP_CORE8_IDLE_CFG,
		  PMU_CAP_CORE8_WAKEUP, C2_RVBADDR_LO_ADDR },
		{ 12, PMU_CAP_CORE12_IDLE_CFG,
		  PMU_CAP_CORE12_WAKEUP, C3_RVBADDR_LO_ADDR },
	};
	unsigned long entry = (unsigned long)_spacemit_k3_prewarm_entry;
	unsigned long *data = spacemit_k3_prewarm_data;
	int rc;

	/* Cold init has published the firmware and enabled CCI snooping. */
	for (u32 i = 0; i < array_size(clusters); i++) {
		rc = spacemit_k3_prepare_hart(clusters[i].hart);
		if (rc)
			return rc;

		data[K3_PREWARM_IDLE] = clusters[i].idle;
		data[K3_PREWARM_VECTOR] = clusters[i].vector;
		data[K3_PREWARM_VECTOR_LO] = readl((void *)clusters[i].vector);
		data[K3_PREWARM_VECTOR_HI] = readl((void *)(clusters[i].vector + 4));
		/* The restored vector is the completion token; it must differ. */
		if (data[K3_PREWARM_VECTOR_LO] == (u32)entry &&
		    data[K3_PREWARM_VECTOR_HI] == (u32)(entry >> 32))
			return SBI_EINVAL;
		/* Order context stores before cleaning their cache line. */
		mb();
		_spacemit_k3_clean_block((unsigned long)data);
		/* Publish the context before installing its reset entry. */
		mb();
		rc = spacemit_k3_set_vector(clusters[i].vector, entry);
		if (rc)
			return rc;
		writel(BIT(clusters[i].hart), (void *)clusters[i].wake);
		/* Complete wakeup before polling the MMIO completion token. */
		mb();
		if (!sbi_timer_waitms_until(prewarm_done, data, 10)) {
			sbi_printf("K3: hart %u pre-warm timed out\n", clusters[i].hart);
			return SBI_ETIMEDOUT;
		}
	}

	/* Release DMASYS reset before enabling its clock for TCM access. */
	writel(readl((void *)K3_DMASYS_RESET) | BIT(0), (void *)K3_DMASYS_RESET);
	/* Release reset before enabling the DMASYS clock. */
	mb();
	writel(readl((void *)K3_DMASYS_CLOCK) | BIT(0), (void *)K3_DMASYS_CLOCK);
	/* Finish clock setup before the next stage accesses TCM. */
	mb();
	return 0;
}

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
		rc = spacemit_k3_set_vector(vector[i], entry);
		if (rc)
			return rc;
	}

	for (i = 0; i < array_size(cci_map); i++) {
		rc = cci_enable_snoop_dvm_reqs(cci_map, i);
		if (rc)
			return rc;
	}

	rc = spacemit_k3_prepare_hart(current_hartid());
	if (rc)
		return rc;

	return spacemit_k3_prewarm();
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
