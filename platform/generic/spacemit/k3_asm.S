/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (c) 2026 SpacemiT
 * Authors:
 *   Xianbin Zhu <xianbin.zhu@linux.spacemit.com>
 *   Troy Mitchell <troy.mitchell@linux.spacemit.com>
 */

#define __ASSEMBLY__
#include <sbi/riscv_encoding.h>
#include <sbi/riscv_asm.h>
#include <spacemit/common.h>
#include <spacemit/k3.h>

	.section .entry, "ax", %progbits
	.align 3
	.globl _spacemit_k3_warm_start
_spacemit_k3_warm_start:
	lla	t0, _spacemit_k3_entry_fault
	csrw	mtvec, t0
	csrw	mie, zero
	call	_spacemit_k3_hart_init
	j	_start_warm

	/* No stack or AMO before local cache coherency is enabled. */
	.globl _spacemit_k3_hart_init
_spacemit_k3_hart_init:
	/* Program PMA attributes before enabling the caches. */
	li	t0, (PMACFG0_AUDIO_BUF_FIELD_MASK << \
		     PMACFG0_AUDIO_BUF_FIELD_SHIFT)
	csrc	CSR_PMACFG0, t0
	li	t0, (PMACFG0_AUDIO_BUF_ATTR_CACHEABLE << \
		     PMACFG0_AUDIO_BUF_FIELD_SHIFT)
	csrs	CSR_PMACFG0, t0
	li	t0, PMACFG0_XIP_FIELD_CLEAR
	csrc	CSR_PMACFG0, t0
	li	t0, PMACFG0_XIP_ATTR_IO
	csrs	CSR_PMACFG0, t0
	sfence.vma

	/* Enable L1 snooping and L2 prefetch for this hart. */
	csrr	t0, mhartid
	andi	t0, t0, (PLATFORM_MAX_CPUS_PER_CLUSTER - 1)
	li	t1, 1
	sll	t1, t1, t0
	li	t2, (ML2SETUP_IPRF | ML2SETUP_TPRF)
	or	t1, t1, t2
	csrs	CSR_ML2SETUP, t1
	li	t0, ML2HINT_TRACE_TOP_ICGEN
	csrs	CSR_ML2HINT, t0

	li	t0, MSETUP_BOOT_FLAGS
	csrs	CSR_MSETUP, t0

	/* Enable H before common code selects the per-hart trap entry. */
	csrr	t0, mhartid
	li	t1, PLATFORM_A100_FIRST_ID
	bgeu	t0, t1, 1f
	li	t0, MISA_EXT_H
	csrs	misa, t0
	li	t0, MVMD1_FUSION_TYPE5_DIS
	csrs	CSR_MVMD1, t0
	csrwi	CSR_TCMCFG, 1
	csrsi	CSR_MHCR, 1
	.word	0x62000073	/* hfence.gvma */
1:
	fence	iorw, iorw
	ret

	.globl _spacemit_k3_entry_fault
_spacemit_k3_entry_fault:
	j	_spacemit_k3_entry_fault

	/* Publish code before a reset hart fetches it without snooping. */
	.option push
	.option arch, +zicbom
	.globl _spacemit_k3_clean_block
_spacemit_k3_clean_block:
	cbo.clean (a0)
	ret
	.option pop

	/* Prewarm C2/C3 outside SBI initialization. No stack or AMO is needed.
	 * The boot hart publishes this code and context before PMU wakeup.
	 */
	.balign 64
	.globl _spacemit_k3_prewarm_entry
_spacemit_k3_prewarm_entry:
	lla t0, _spacemit_k3_entry_fault
	csrw CSR_MTVEC, t0
	csrw CSR_MIE, zero
	csrr t0, CSR_MHARTID
	li t1, 8
	beq t0, t1, 1f
	li t1, 12
	bne t0, t1, _spacemit_k3_entry_fault
1:
	call _spacemit_k3_hart_init
	li t0, K3_A100_VECTOR_L1_BYPASS
	csrs CSR_K3_PERF_CTRL, t0
	li t0, K3_A100_L2_PREFETCH_DIST
	csrs CSR_K3_PREFETCH_CTRL, t0
	csrci CSR_ML2HINT, K3_ML2HINT_DEPENDENCY_DIS
	li t0, K3_ML2HINT_MERGE_DIS | ML2HINT_TRACE_TOP_ICGEN
	csrs CSR_ML2HINT, t0

	lla t0, spacemit_k3_prewarm_data
	REG_L a0, (K3_PREWARM_IDLE * __SIZEOF_LONG__)(t0)
	REG_L a3, (K3_PREWARM_VECTOR * __SIZEOF_LONG__)(t0)
	REG_L a4, (K3_PREWARM_VECTOR_LO * __SIZEOF_LONG__)(t0)
	REG_L a5, (K3_PREWARM_VECTOR_HI * __SIZEOF_LONG__)(t0)

	/* Cluster votes were cleared by prepare_hart(); park only this core. */
	lw t0, 0(a0)
	ori t0, t0, PMU_AP_IDLE_PWRDOWN_K3_MASK
	sw t0, 0(a0)
	fence iorw, iorw

	li t0, -1
	csrw CSR_STIMECMP, t0
	li t0, MSETUP_PFE
	csrc CSR_MSETUP, t0
	fence iorw, iorw
	csrsi CSR_K3_CACHE_OP, 3
	fence iorw, iorw
	csrci CSR_MSETUP, 3
	fence iorw, iorw
	csrci CSR_ML2SETUP, 1
	fence iorw, iorw

	/* MMIO completion avoids a shared cached flag after snooping is off.
	 * Restore high first; the boot hart checks both halves of the vector.
	 * All context loads are finished before it may reuse the context.
	 */
	sw a5, 4(a3)
	fence iorw, iorw
	sw a4, 0(a3)
	fence iorw, iorw
2:
	wfi
	j 2b
