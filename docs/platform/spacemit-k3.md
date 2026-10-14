# SpacemiT K3

K3 platform and start-only HSM support are based on the
[OpenSBI v4 series](https://lore.kernel.org/opensbi/20260910-spacemit-k3-v4-0-0793b62329f6@linux.spacemit.com/).
The backend supports hart IDs 0–15. The incoming firmware device tree
selects which harts OpenSBI registers.

```sh
make PLATFORM=generic CROSS_COMPILE=riscv64-linux-gnu- \
    FW_TEXT_START=0x100000000
```

The coreboot boot path uses `fw_dynamic.elf`. Local adaptations publish
the loaded firmware before reset-hart instruction fetches, check cluster
reset vectors, bound CCI transitions, and retain X100 cache, TCM and AI
initialization. Core and cluster idle votes and per-core system
power-down votes are cleared before startup. Stopped harts park and
restart through the standard SBI IPI path; power-loss suspend is not
implemented.

Cold boot briefly starts hart 8 and hart 12 outside SBI initialization to
prepare the auxiliary clusters for clock changes. Each restores its
cluster reset vector and parks after cleaning and disabling its local
cache. This prewarm sequence is independent of Linux CPU enumeration.
DMASYS reset and clock setup preserve access to the complete TCM range.
P1 reset restores its I2C bus clock and reset path before transferring.

## CPU selection in the native firmware

Coreboot reads the persistent UEFI CPU selection before entering OpenSBI.
X100-only mode preserves the original eight-hart description. Mixed mode
exposes sixteen harts without vector or hypervisor ISA declarations.

A100-only mode initially describes all sixteen harts because OpenSBI cold
initialization runs on X100 hart 0. EDK2 then uses standard HSM calls to
start hart 8 and stop hart 0 before creating its handoff tables. Its OS
copy of the device tree disables X100 and retains A100 vector support.
This policy needs no private SBI extension or change to the HSM backend.
