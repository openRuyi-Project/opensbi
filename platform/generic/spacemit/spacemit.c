/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 SpacemiT
 * Authors:
 *   Xianbin Zhu <xianbin.zhu@linux.spacemit.com>
 *   Troy Mitchell <troy.mitchell@linux.spacemit.com>
 */

#include <platform_override.h>
#include <sbi/riscv_io.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_timer.h>
#include <spacemit/common.h>

static bool cci_transition_done(void *arg)
{
	return !(readl(arg) & CCI_550_STATUS_CHANGE_PENDING);
}

int cci_enable_snoop_dvm_reqs(const int *cci_map, unsigned int master_id)
{
	const struct sbi_timer_device *timer = sbi_timer_get_device();
	int slave_if_id = cci_map[master_id];

	if (!timer || timer->timer_freq < 1000)
		return SBI_ENODEV;

	/*
	 * Enable Snoops and DVM messages, no need for Read/Modify/Write as
	 * rest of bits are write ignore
	 */
	writel(CCI_550_SNOOP_CTRL_ENABLE_DVMS | CCI_550_SNOOP_CTRL_ENABLE_SNOOPS,
	       (void *)(unsigned long)CCI_550_PLATFORM_CCI_ADDR +
	       CCI_550_SLAVE_IFACE_OFFSET(slave_if_id) + CCI_550_SNOOP_CTRL);

	/*
	 * Wait for the completion of the write to the Snoop Control Register
	 * before testing the change_pending bit
	 */
	mb();

	return sbi_timer_waitms_until(cci_transition_done,
		(void *)(unsigned long)(CCI_550_PLATFORM_CCI_ADDR + CCI_550_STATUS),
		1) ? 0 : SBI_ETIMEDOUT;
}
