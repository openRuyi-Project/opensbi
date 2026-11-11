// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2026 Han Gao <gaohan@iscas.ac.cn>
 *
 * EIC7700 BootROM security service protocol, as described by ESWIN's
 * U-Boot eswin-uMsg.h.
 */

#include <sbi/riscv_io.h>
#include <sbi/sbi_bitops.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_math.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_string.h>
#include <sbi/sbi_timer.h>
#include <eswin/eic770x.h>

#define MBOX_TX			EIC770X_MBOX_BASE
#define MBOX_RX			(EIC770X_MBOX_BASE + 0x10000)
#define MBOX_WRITE_LOW		0x00
#define MBOX_WRITE_HIGH		0x04
#define MBOX_READ_LOW		0x08
#define MBOX_READ_HIGH		0x0c
#define MBOX_FIFO_STATUS		0x10
#define MBOX_INTERRUPT		0x18
#define MBOX_WRITE_LOCK		0x1c
#define MBOX_FIFO_EMPTY		BIT(1)
#define MBOX_CLOCKS		(BIT(0) | BIT(1))
#define CCACHE_CONFIG		(EIC770X_P550INT_LOCAL + 0x2010000)
#define CCACHE_FLUSH64		(CCACHE_CONFIG + 0x200)
#define CCACHE_LINE_SIZE		64
#define REQUEST_MAGIC		0xbb4565a6
#define RESPONSE_MAGIC		0xcc4578b5
#define SERVICE_REGION		0x0b
#define SERVICE_AXPROT		0x0e
#define SERVICE_READ		0
#define SERVICE_WRITE		1
#define AXPROT_NS_MASK		0x22222222
#define MCPU_SYSTEM_PORTS_NS	0x2222
#define REGION_MIN_ORDER		15
#define REGION_AUTO		0xff
#define REGION_SECURE		BIT(31)
#define REGION_CHECK_RW		3
#define BOOTROM_ADDRESS_MAX	0xffffffffUL

/* IDs are part of the BootROM AXPROT service ABI. */
enum eic7700_master {
	MASTER_LPCPU = 0,
	MASTER_AONDMA = 1,
	MASTER_DSP = 2,
	MASTER_NPU = 3,
	MASTER_MCPU = 5,
	MASTER_GPU = 6,
	MASTER_G2D0 = 7,
	MASTER_G2D1 = 8,
	MASTER_VD = 9,
	MASTER_VC = 10,
	MASTER_JE = 11,
	MASTER_JD = 12,
	MASTER_ISP0 = 13,
	MASTER_ISP1 = 14,
	MASTER_DVP2AXI = 15,
	MASTER_DWE = 16,
	MASTER_DC = 17,
	MASTER_HDMI = 18,
	MASTER_ETH0 = 19,
	MASTER_ETH1 = 20,
	MASTER_SATA = 21,
	MASTER_DMA = 22,
	MASTER_EMMC = 23,
	MASTER_SD = 24,
	MASTER_SDIO = 25,
	MASTER_PCIE = 26,
	MASTER_SCPU = 27,
	MASTER_USB0 = 33,
	MASTER_USB1 = 34,
};

/* Both the request and its response must fit in this firmware-owned buffer. */
static u32 packet[32] __aligned(CCACHE_LINE_SIZE);
static u32 sequence;

static u8 xor_bytes(const u8 *data, unsigned int size)
{
	u8 sum = 0;

	while (size--)
		sum ^= *data++;
	return sum;
}

static void flush_packet(void)
{
	unsigned long addr = (unsigned long)packet;

	asm volatile("fence rw, rw" ::: "memory");
	for (unsigned int offset = 0; offset < sizeof(packet);
	     offset += CCACHE_LINE_SIZE)
		writeq(addr + offset, (void *)CCACHE_FLUSH64);
	asm volatile("fence iorw, iorw" ::: "memory");
}

static int security_call(u32 service, const u32 *data, unsigned int size,
			unsigned int reply_size, u32 *reply)
{
	u8 frame[8] = { 0xbb, 4 }, response[8];
	u8 *bytes = (void *)packet;
	u32 low, high, reply_addr = 0;
	unsigned long addr = (unsigned long)packet;
	unsigned int retry, i;

	if (addr > BOOTROM_ADDRESS_MAX || size > 16 || reply_size > sizeof(*reply))
		return SBI_EINVAL;
	if (!(readl((void *)(MBOX_TX + MBOX_FIFO_STATUS)) & MBOX_FIFO_EMPTY) ||
	    !(readl((void *)(MBOX_RX + MBOX_FIFO_STATUS)) & MBOX_FIFO_EMPTY) ||
	    readl((void *)(MBOX_TX + MBOX_WRITE_LOCK)))
		return SBI_EINVALID_STATE;
	writel(BIT(0), (void *)(MBOX_TX + MBOX_WRITE_LOCK));
	if (readl((void *)(MBOX_TX + MBOX_WRITE_LOCK)) != BIT(0))
		return SBI_EINVALID_STATE;

	sbi_memset(packet, 0, sizeof(packet));
	packet[0] = REQUEST_MAGIC;
	packet[1] = ++sequence;
	packet[2] = service;
	packet[3] = size;
	sbi_memcpy(packet + 4, data, size);
	bytes[16 + size] = xor_bytes(bytes, 16 + size);
	for (i = 0; i < 4; i++)
		frame[2 + i] = addr >> (8 * i);
	frame[6] = xor_bytes(frame, 6);
	frame[7] = 0x80; /* FIFO push bit, outside the seven-byte descriptor. */
	sbi_memcpy(&low, frame, 4);
	sbi_memcpy(&high, frame + 4, 4);
	flush_packet();
	writel(0, (void *)(MBOX_RX + MBOX_INTERRUPT));
	writel(low, (void *)(MBOX_TX + MBOX_WRITE_LOW));
	writel(high, (void *)(MBOX_TX + MBOX_WRITE_HIGH));
	writel(BIT(1), (void *)(MBOX_TX + MBOX_INTERRUPT));
	for (retry = 0; retry < 10000; retry++) {
		if (!(readl((void *)(MBOX_RX + MBOX_FIFO_STATUS)) & MBOX_FIFO_EMPTY))
			break;
		sbi_timer_udelay(100);
	}
	if (retry == 10000)
		return SBI_ETIMEDOUT;

	low = readl((void *)(MBOX_RX + MBOX_READ_LOW));
	high = readl((void *)(MBOX_RX + MBOX_READ_HIGH));
	sbi_memcpy(response, &low, 4);
	sbi_memcpy(response + 4, &high, 4);
	writel(0, (void *)(MBOX_RX + MBOX_READ_HIGH));
	writel(0, (void *)(MBOX_RX + MBOX_INTERRUPT));
	for (i = 0; i < 7; i++)
		frame[i] = response[6 - i];
	for (i = 0; i < 4; i++)
		reply_addr |= (u32)frame[2 + i] << (8 * i);
	if (frame[0] != 0xcc || frame[1] != 4 || xor_bytes(frame, 7) ||
	    reply_addr != addr)
		return SBI_EIO;

	/* BootROM can signal before its response stores have completed. */
	sbi_timer_mdelay(5);
	flush_packet();
	if (packet[0] != RESPONSE_MAGIC || packet[1] != sequence ||
	    packet[2] != service || packet[3] || packet[4] ||
	    packet[5] != reply_size || xor_bytes(bytes, 25 + reply_size))
		return SBI_EIO;
	*reply = 0;
	sbi_memcpy(reply, packet + 6, reply_size);
	writel(0, (void *)(MBOX_RX + MBOX_WRITE_LOCK));
	return 0;
}

static int master_security(u32 master, u32 nonsecure_mask)
{
	u32 data[3] = { SERVICE_READ, master, 0 };
	u32 value, expected, result;
	int rc = security_call(SERVICE_AXPROT, data, sizeof(data), 4, &value);

	if (rc)
		return rc;
	expected = (value & ~AXPROT_NS_MASK) | nonsecure_mask;
	if (expected == value)
		return 0;
	data[0] = SERVICE_WRITE;
	data[2] = expected;
	rc = security_call(SERVICE_AXPROT, data, sizeof(data), 4, &result);
	if (rc)
		return rc;
	data[0] = SERVICE_READ;
	data[2] = 0;
	rc = security_call(SERVICE_AXPROT, data, sizeof(data), 4, &value);
	return rc ? rc : (value == expected ? 0 : SBI_EIO);
}

int eic770x_mpmp_init(const struct sbi_scratch *scratch)
{
	/* BootROM master IDs and implemented read/write AXPROT fields. */
	static const struct { u32 id, nonsecure; } masters[] = {
		{ MASTER_LPCPU, 0x2222 }, { MASTER_AONDMA, 0x2222 },
		{ MASTER_DSP, 0x22222222 }, { MASTER_NPU, 0x22222222 },
		{ MASTER_GPU, 0x22 }, { MASTER_G2D0, 0x22 },
		{ MASTER_G2D1, 0x22 }, { MASTER_VD, 0x22 },
		{ MASTER_VC, 0x22 }, { MASTER_JE, 0x22 },
		{ MASTER_JD, 0x22 }, { MASTER_ISP0, 0x22 },
		{ MASTER_ISP1, 0x22 }, { MASTER_DVP2AXI, 0x22 },
		{ MASTER_DWE, 0x22 }, { MASTER_DC, 0x22 },
		{ MASTER_HDMI, 0x22 }, { MASTER_ETH0, 0x22 },
		{ MASTER_ETH1, 0x22 }, { MASTER_SATA, 0x22 },
		{ MASTER_DMA, 0x22 }, { MASTER_EMMC, 0x22 },
		{ MASTER_SD, 0x22 }, { MASTER_SDIO, 0x22 },
		{ MASTER_PCIE, 0x22 }, { MASTER_USB0, 0x22 },
		{ MASTER_USB1, 0x22 },
	};
	u32 clocks, region, data[4];
	unsigned long order = log2roundup(scratch->fw_size);
	unsigned long size;
	int rc;

	/* The current BootROM mailbox ABI carries a 32-bit memory-port address. */
	if (current_hart_die() || !scratch->fw_size ||
	    order < REGION_MIN_ORDER || order >= 32)
		return SBI_ENOTSUPP;
	size = 1UL << order;
	if (scratch->fw_start < EIC770X_D0_MEM_BASE ||
	    scratch->fw_start > BOOTROM_ADDRESS_MAX - size ||
	    (scratch->fw_start & (size - 1)))
		return SBI_EINVAL;

	clocks = readl((void *)EIC770X_SYSCRG_LSPCLK1);
	writel(clocks | MBOX_CLOCKS, (void *)EIC770X_SYSCRG_LSPCLK1);
	rc = master_security(MASTER_SCPU, 0);
	if (rc)
		goto fail;
	/* Only the cached memory port needs secure access to firmware RAM. */
	rc = master_security(MASTER_MCPU, MCPU_SYSTEM_PORTS_NS);
	if (rc)
		goto fail;
	for (unsigned int i = 0; i < array_size(masters); i++) {
		rc = master_security(masters[i].id, masters[i].nonsecure);
		if (rc)
			goto fail;
	}

	data[0] = REGION_AUTO;
	data[1] = 0;
	data[2] = scratch->fw_start - EIC770X_D0_MEM_BASE;
	data[3] = REGION_SECURE | ((order - 1) << 8) | REGION_CHECK_RW;
	rc = security_call(SERVICE_REGION, data, sizeof(data), 1, &region);
	if (rc || !region || region >= 16) {
		if (!rc)
			rc = SBI_EIO;
		goto fail;
	}
	writel((readl((void *)EIC770X_SYSCRG_LSPCLK1) & ~MBOX_CLOCKS) |
	       (clocks & MBOX_CLOCKS), (void *)EIC770X_SYSCRG_LSPCLK1);
	if (!(scratch->options & SBI_SCRATCH_NO_BOOT_PRINTS))
		sbi_printf("EIC7700 Memory PMP: region %u protects 0x%lx-0x%lx\n",
			   region, scratch->fw_start, scratch->fw_start + size);
	return 0;
fail:
	/* Fail boot. A timed-out remote request may still own packet and clocks. */
	sbi_printf("EIC7700 Memory PMP initialization failed: %d\n", rc);
	return rc;
}
