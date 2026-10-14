/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (c) 2026 SpacemiT
 * Authors:
 *   Xianbin Zhu <xianbin.zhu@linux.spacemit.com>
 *   Troy Mitchell <troy.mitchell@linux.spacemit.com>
 */

#ifndef __RISCV_SPACEMIT_K3_ASM_H__
#define __RISCV_SPACEMIT_K3_ASM_H__

#ifndef __ASSEMBLY__
void _spacemit_k3_warm_start(void);
void _spacemit_k3_hart_init(void);
void _spacemit_k3_clean_block(unsigned long address);
#endif

#endif /* __RISCV_SPACEMIT_K3_ASM_H__ */
