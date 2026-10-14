/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (c) 2026 SpacemiT
 * Authors:
 *   Xianbin Zhu <xianbin.zhu@linux.spacemit.com>
 *   Troy Mitchell <troy.mitchell@linux.spacemit.com>
 */

#define __ASSEMBLY__
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
