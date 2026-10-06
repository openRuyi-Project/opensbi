/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Aurelien Jarno
 *
 * Authors:
 *   Aurelien Jarno <aurelien@aurel32.net>
 */

#include <libfdt.h>
#include <sbi/riscv_io.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_timer.h>
#include <sbi_utils/fdt/fdt_helper.h>
#include <sbi_utils/i2c/fdt_i2c.h>

/* Controller registers */
#define ICR_OFFSET	0x00		/* I2C control register */
#define IDBR_OFFSET	0x0c		/* I2C data buffer register */

/* Control register bits */
#define ICR_START	BIT(0)		/* start */
#define ICR_STOP	BIT(1)		/* stop */
#define ICR_ACKNAK	BIT(2)		/* ACK(0) or NAK(1) */
#define ICR_TB		BIT(3)		/* transfer byte */
#define ICR_UR		BIT(10)		/* unit reset */
#define ICR_SCLE	BIT(13)		/* SCL enable  */
#define ICR_IUE		BIT(14)		/* unit enable */

/* Timing */
#define I2C_RESET_US	10
#define I2C_TIMEOUT_US	1000

/* K3 clock integration, matching its APBC/MPMU/PLL DT providers. */
#define K3_PLL1_CONFIG0		0x100
#define K3_PLL1_CONTROL		0x104
#define K3_PLL1_CONFIG2		0x108
#define K3_PLL1_DIV4_ENABLE	BIT(3)
#define K3_PLL_STATUS		0x10
#define K3_PLL1_LOCKED		BIT(24)
#define K3_MPMU_CLOCK_GATES	0x1024
#define K3_PLL1_DIV78_ENABLE	BIT(6)
#define K3_TWSI8_CLOCK_RESET	0x20
#define K3_TWSI_CLOCK_MUX	(7U << 4)
#define K3_TWSI_RESET		BIT(2)
#define K3_TWSI_CLOCKS		(BIT(1) | BIT(0))
#define K3_CLK_TWSI8		79
#define K3_CLK_TWSI8_BUS		86
#define K3_RESET_TWSI8		41

struct spacemit_i2c_adapter {
	unsigned long base;
	unsigned long apbc, pll, mpmu;
	struct i2c_adapter adapter;
};

static inline void spacemit_i2c_set_reg(struct spacemit_i2c_adapter *adap,
					uint8_t reg, uint32_t val)
{
	writel(val, (void *)adap->base + reg);
}

static inline uint32_t spacemit_i2c_get_reg(struct spacemit_i2c_adapter *adap,
					    uint32_t reg)
{
	return readl((void *)adap->base + reg);
}

/* K3 TWSI8 is used by the P1 reset provider. Restore its clock path at
 * transfer time: neither a native boot nor OS shutdown guarantees retention.
 * The mux selects PLL1 / 78; do not retune the shared PLL1 VCO.
 */
static int spacemit_k3_prepare(struct spacemit_i2c_adapter *adap)
{
	uint32_t val;

	if (!adap->apbc)
		return 0;
	if (readl((void *)(adap->pll + K3_PLL1_CONFIG0)) != 0x0b330ccc ||
	    readl((void *)(adap->pll + K3_PLL1_CONFIG2)) != 0xa0558989 ||
	    !(readl((void *)(adap->mpmu + K3_PLL_STATUS)) & K3_PLL1_LOCKED))
		return SBI_EIO;
	val = readl((void *)(adap->pll + K3_PLL1_CONTROL));
	writel(val | K3_PLL1_DIV4_ENABLE, (void *)(adap->pll + K3_PLL1_CONTROL));
	val = readl((void *)(adap->mpmu + K3_MPMU_CLOCK_GATES));
	writel(val | K3_PLL1_DIV78_ENABLE, (void *)(adap->mpmu + K3_MPMU_CLOCK_GATES));
	val = readl((void *)(adap->apbc + K3_TWSI8_CLOCK_RESET));
	val &= ~K3_TWSI_CLOCK_MUX;
	writel(val | K3_TWSI_RESET | K3_TWSI_CLOCKS, (void *)(adap->apbc + K3_TWSI8_CLOCK_RESET));
	sbi_timer_udelay(2);
	writel((val | K3_TWSI_CLOCKS) & ~K3_TWSI_RESET,
	       (void *)(adap->apbc + K3_TWSI8_CLOCK_RESET));
	if ((readl((void *)(adap->apbc + K3_TWSI8_CLOCK_RESET)) &
	     (K3_TWSI_CLOCK_MUX | K3_TWSI_RESET | K3_TWSI_CLOCKS)) != K3_TWSI_CLOCKS)
		return SBI_EIO;
	return 0;
}

static int spacemit_i2c_reset(struct spacemit_i2c_adapter *adap)
{
	int rc = spacemit_k3_prepare(adap);

	if (rc)
		return rc;
	/* disable unit */
	spacemit_i2c_set_reg(adap, ICR_OFFSET, 0);
	sbi_timer_udelay(I2C_RESET_US);

	/* reset unit */
	spacemit_i2c_set_reg(adap, ICR_OFFSET, ICR_UR);
	sbi_timer_udelay(I2C_RESET_US);

	/* clear reset and enable unit and SCL */
	spacemit_i2c_set_reg(adap, ICR_OFFSET, ICR_IUE | ICR_SCLE);
	return 0;
}

static int spacemit_i2c_wait_xfer_done(struct spacemit_i2c_adapter *adap)
{
	for (int i = 0; i < I2C_TIMEOUT_US; i++) {
		uint32_t val = spacemit_i2c_get_reg(adap, ICR_OFFSET);

		if (!(val & ICR_TB))
			return 0;

		sbi_timer_udelay(1);
	};

	return SBI_ETIMEDOUT;
}

static void spacemit_i2c_start_xfer(struct spacemit_i2c_adapter *adap,
				    uint32_t ctrl)
{
	const uint32_t ctrl_mask = ICR_START | ICR_STOP | ICR_ACKNAK;
	uint32_t val;

	val = spacemit_i2c_get_reg(adap, ICR_OFFSET);
	val &= ~ctrl_mask;
	val |= (ctrl & ctrl_mask);
	val |= ICR_TB;

	spacemit_i2c_set_reg(adap, ICR_OFFSET, val);
}

static int spacemit_i2c_xfer_write(struct spacemit_i2c_adapter *adap,
				   uint8_t byte, uint32_t ctrl)
{
	spacemit_i2c_set_reg(adap, IDBR_OFFSET, byte);
	spacemit_i2c_start_xfer(adap, ctrl);

	return spacemit_i2c_wait_xfer_done(adap);
}

static int spacemit_i2c_xfer_read(struct spacemit_i2c_adapter *adap,
				  uint8_t *byte, uint32_t ctrl)
{
	int rc;

	spacemit_i2c_start_xfer(adap, ctrl);

	rc = spacemit_i2c_wait_xfer_done(adap);
	if (rc)
		return rc;

	*byte = spacemit_i2c_get_reg(adap, IDBR_OFFSET);
	return 0;
}

static int spacemit_i2c_adapter_write(struct i2c_adapter *ia, uint8_t addr,
				      uint8_t reg, uint8_t *buffer, int len)
{
	struct spacemit_i2c_adapter *adap =
		container_of(ia, struct spacemit_i2c_adapter, adapter);
	int rc;

	/* reset controller to a known state */
	rc = spacemit_i2c_reset(adap);
	if (rc)
		return rc;

	/* send device address (in write mode) */
	rc = spacemit_i2c_xfer_write(adap, addr << 1, ICR_START);
	if (rc)
		return rc;

	/* send register + data bytes */
	for (int i = 0; i < len + 1; i++) {
		uint32_t ctrl = (i == len) ? ICR_STOP : 0;
		uint8_t byte = (i == 0) ? reg : buffer[i - 1];

		rc = spacemit_i2c_xfer_write(adap, byte, ctrl);
		if (rc)
			return rc;
	}

	return 0;
}

static int spacemit_i2c_adapter_read(struct i2c_adapter *ia, uint8_t addr,
				     uint8_t reg, uint8_t *buffer, int len)
{
	struct spacemit_i2c_adapter *adap =
		container_of(ia, struct spacemit_i2c_adapter, adapter);
	int rc;

	/* reset controller to a known state */
	rc = spacemit_i2c_reset(adap);
	if (rc)
		return rc;

	/* send device address (in write mode) */
	rc = spacemit_i2c_xfer_write(adap, addr << 1, ICR_START);
	if (rc)
		return rc;

	/* send register */
	rc = spacemit_i2c_xfer_write(adap, reg, 0);
	if (rc)
		return rc;

	/* repeated start and send device address (in read mode) */
	rc = spacemit_i2c_xfer_write(adap, (addr << 1) | 1, ICR_START);
	if (rc)
		return rc;

	/* read data bytes */
	for (int i = 0; i < len; i++) {
		uint32_t ctrl = (i == len - 1) ? (ICR_ACKNAK | ICR_STOP) : 0;

		rc = spacemit_i2c_xfer_read(adap, &buffer[i], ctrl);
		if (rc)
			return rc;
	}

	return 0;
}

static int spacemit_k3_region(const void *fdt, int node, const char *compat,
			      uint64_t expected, uint64_t required,
			      unsigned long *out)
{
	uint64_t base, size;
	int rc;

	if (node < 0 || !fdt_node_is_enabled(fdt, node) ||
	    fdt_node_check_compatible(fdt, node, compat))
		return SBI_EINVAL;
	rc = fdt_get_node_addr_size(fdt, node, 0, &base, &size);
	if (rc)
		return rc;
	if (base != expected || size < required)
		return SBI_EINVAL;
	*out = base;
	return 0;
}

static int spacemit_k3_resources(const void *fdt, int node,
				struct spacemit_i2c_adapter *adap)
{
	struct fdt_phandle_args func, bus, reset;
	const fdt32_t *ref;
	int rc, pll, len;

	/* Only the P1 reset bus needs firmware clock restoration. */
	if (adap->base != 0xd401d800)
		return 0;
	rc = fdt_parse_phandle_with_args(fdt, node, "clocks", "#clock-cells", 0, &func);
	if (rc)
		return rc;
	rc = fdt_parse_phandle_with_args(fdt, node, "clocks", "#clock-cells", 1, &bus);
	if (rc)
		return rc;
	rc = fdt_parse_phandle_with_args(fdt, node, "resets", "#reset-cells", 0, &reset);
	if (rc)
		return rc;
	if (func.args_count != 1 || bus.args_count != 1 || reset.args_count != 1 ||
	    func.args[0] != K3_CLK_TWSI8 || bus.args[0] != K3_CLK_TWSI8_BUS ||
	    reset.args[0] != K3_RESET_TWSI8 ||
	    func.node_offset != bus.node_offset || func.node_offset != reset.node_offset)
		return SBI_EINVAL;
	rc = spacemit_k3_region(fdt, func.node_offset, "spacemit,k3-syscon-apbc",
				0xd4015000, 0x24, &adap->apbc);
	if (rc)
		return rc;
	pll = fdt_node_offset_by_compatible(fdt, -1, "spacemit,k3-pll");
	rc = spacemit_k3_region(fdt, pll, "spacemit,k3-pll", 0xd4090000,
				0x10c, &adap->pll);
	if (rc)
		return rc;
	ref = fdt_getprop(fdt, pll, "spacemit,mpmu", &len);
	if (!ref || len != sizeof(*ref))
		return SBI_EINVAL;
	return spacemit_k3_region(fdt, fdt_node_offset_by_phandle(fdt, fdt32_to_cpu(*ref)),
				 "spacemit,k3-syscon-mpmu", 0xd4050000,
				 0x1028, &adap->mpmu);
}

static int spacemit_i2c_init(const void *fdt, int nodeoff,
			     const struct fdt_match *match)
{
	struct spacemit_i2c_adapter *adapter;
	uint64_t base;
	int rc;

	adapter = sbi_zalloc(sizeof(*adapter));
	if (!adapter)
		return SBI_ENOMEM;

	rc = fdt_get_node_addr_size(fdt, nodeoff, 0, &base, NULL);
	if (rc) {
		sbi_free(adapter);
		return rc;
	}

	adapter->base = base;
	if (!fdt_node_check_compatible(fdt, nodeoff, "spacemit,k3-i2c")) {
		rc = spacemit_k3_resources(fdt, nodeoff, adapter);
		if (rc) {
			sbi_free(adapter);
			return rc;
		}
	}
	adapter->adapter.id = nodeoff;
	adapter->adapter.write = spacemit_i2c_adapter_write;
	adapter->adapter.read = spacemit_i2c_adapter_read;
	rc = i2c_adapter_add(&adapter->adapter);
	if (rc) {
		sbi_free(adapter);
		return rc;
	}

	return 0;
}

static const struct fdt_match spacemit_i2c_match[] = {
	{ .compatible = "spacemit,k1-i2c" },
	{ },
};

const struct fdt_driver fdt_i2c_adapter_spacemit = {
	.match_table = spacemit_i2c_match,
	.init = spacemit_i2c_init,
};
