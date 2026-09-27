# hw_access.md — Low-level hardware access + Intel/AMD registers (Phawx ON)

Engineering reference for user-mode C (mingw-w64, Win32) direct hardware access on Windows 10/11.
All facts cite primary sources. Units and bit fields verified against Linux kernel headers, PawnIO module source, and intel-undervolt. Uncertain items are marked **[UNCERTAIN]**.

---

## 0. Driver-access strategy summary (read first)

Ring-0 access (MSRs, PCI config, port I/O, MMIO) goes exclusively through **PawnIO 2.x** (`PawnIO.sys`):
signed, actively maintained, not flagged by Defender, loads with Memory Integrity enabled, and used by
LibreHardwareMonitor. Each signed module whitelists the operations it allows, so the app can never
perform arbitrary physical-memory writes.

Phawx ON wraps PawnIO behind a small provider abstraction (`ph_drv`). Anything the stock signed
modules do not allow (see the verdict in section 2) is simply unavailable in the UI until a signed
module that allows it exists.

---

## 2. PawnIO

Sources: github.com/namazso/PawnIO (`PawnIOLib`, `pawnio_um.h`, `vm.cpp`),
github.com/namazso/PawnIO.Modules (module `.p` files + `include/native.inc`). Site pawnio.eu.

### 2.1 User-mode API — `PawnIOLib.dll`
```c
HRESULT __stdcall pawnio_version(PULONG version);        // 0x00020000 = 2.0.0
HRESULT __stdcall pawnio_open(PHANDLE handle);           // opens \Device\PawnIO
HRESULT __stdcall pawnio_load(HANDLE h, const UCHAR* blob, SIZE_T size); // signed module blob
HRESULT __stdcall pawnio_execute(HANDLE h, PCSTR name,
        const ULONG64* in, SIZE_T in_count,
        ULONG64* out, SIZE_T out_count, PSIZE_T return_count);
HRESULT __stdcall pawnio_close(HANDLE h);
```
Also `*_win32` (BOOL+SetLastError) and `*_nt` (NTSTATUS) variants; `pawnio_execute_async` for
overlapped. `in/out` are arrays of ULONG64; `name` ≤31 chars. One HANDLE = one loaded module; load
several modules on several handles. HRESULT via `HRESULT_FROM_WIN32(RtlNtStatusToDosError(status))`.

### 2.2 Driver internals (informational)
Device type 41394; IOCTLs `IOCTL_PIO_LOAD_BINARY=CTL_CODE(41394,0x821,...)`,
`EXECUTE_FN=0x841`, `VERSION=0x861`. Module blob = `[u32 sig_len][sig][amx64 body]`; body SHA-256
verified against trusted Ed25519-style pubkeys (`k_pubkey_namazso_*`). Only namazso-signed modules
load unless driver built `PAWNIO_UNRESTRICTED` (self-built/test-signed only). **You cannot load an
unsigned custom module on the retail signed driver** — either use existing modules, get a module
signed by namazso, or ship your own (test-signed, needs test mode) driver build.

### 2.3 Module → function map (verified from .p source)
- **IntelMSR.p** — `ioctl_read_msr(in[msr])→out[val]`, `ioctl_write_msr(in[msr,val])`.
  Read whitelist: 0x1B1,0x198,0x1A2,0x19C, 0x601,0x607,0x608,0x610,0x611,0x613,0x614,
  0x618,0x619,0x61B,0x61C, 0x620,0x621, 0x638,0x639,0x63A,0x63B, 0x640,0x641,0x642,
  0x64D, 0x606, 0xCE, 0x2A, 0x1AD, 0xE7,0xE8, 0x150, 0x1A4.
  **Write whitelist (only these!):** `0x601 VR_CURRENT_CONFIG, 0x610 PKG_POWER_LIMIT,
  0x150 OC_MAILBOX, 0x607 VR_MAILBOX_INTERFACE, 0x608 VR_MAILBOX_DATA, 0x1A4 MISC_FEATURE_CONTROL`.
  → PL1/PL2 (0x610) and undervolt (0x150) writes are supported. **HWP 0x774, PP0 0x638,
  TURBO_RATIO 0x1AD, POWER_CTL 0x1FC, PERF_CTL 0x199, CONFIG_TDP 0x648-B, TEMP_TARGET 0x1A2 are NOT
  writable via IntelMSR** → unavailable with stock modules (would need an additional signed module).
- **IntelMCHBAR.p** — `ioctl_get_mchbar_addr()→out[addr]`, `ioctl_read_dword(off)`,
  `ioctl_read_qword(off)`. **Read-only** (no MMIO write exposed). MCHBAR discovered from PCI 0:0.0
  off 0x48/0x4C, `&0x3FFFFFF8000`, size 32K/64K/128K by codename. So RAPL MMIO **writes** (PL via
  0x59A0) are NOT available through the stock module — MSR 0x610 write covers PL1/PL2 instead.
- **AMDFamily17.p** — `ioctl_read_msr`, `ioctl_write_msr`, `ioctl_read_smn(off)→val`.
  Write whitelist: `0xC0010015 HWCR, 0xC0010064-6B PSTATE_0..7, 0xC0010292 PMGT_MISC,
  0xC0010296 CSTATE_CONFIG, 0xC00102B1 CPPC_ENABLE, 0xC00102B3 CPPC_REQ, 0xC00102B4 CPPC_STATUS,
  0xC0011020/21/22/2B LS/IC/DC/LS2_CFG`. SMN read only (no SMN write in this module).
  ⇒ AMD CpbDis, P-state override, CPPC/EPP req all writable. SMN reads via
  PCI 0:0.0: write index @0x60, read data @0x64; requires `\BaseNamedObjects\Access_PCI` mutex.
- **RyzenSMU.p** — full SMU mailbox: `ioctl_send_smu_command(in[msg,a0..a5])→out[a0..a5]`,
  `ioctl_read/write_smu_register`, `ioctl_read_pm_table`, `ioctl_get_smu_version`,
  `ioctl_get_code_name`. Mailbox addrs per codename table `k_addrinfo` (cmd/rsp/args), e.g. Matisse
  `0x3B10524/0x3B10570/0x3B10A40`, Renoir/APU `0x3B10A20/0x3B10A80/0x3B10A88`. Protocol: poll rsp≠0,
  write 0 to rsp, write args, write msg, poll rsp≠0, rsp==1 (SMU_OK). Register write range gated to
  0x3B10xxx / 0x56000-0x5AFFF (SVI2 planes) / 0x13000000 (pre-Ryzen). Acquire `Access_PCI` mutex.
  **This is the RyzenAdj-equivalent path for STAPM/Fast/Slow/TctlMax on APUs.**
- **LpcACPIEC.p** — `ioctl_pio_read(port)`, `ioctl_pio_write(port,val)`; **ports gated to 0x62/0x66
  only** (ACPI EC cmd/data). Acquire `\BaseNamedObjects\Access_EC` mutex. GPD fan via non-0x62/66
  ports (e.g. SIO index/data) needs LpcIO.p or a custom module.
- **LpcIO.p** — SuperIO: `ioctl_find_bars`, `ioctl_pio_inb/outb`, `ioctl_superio_inb/inw/outb`,
  `ioctl_select_slot`. General LPC/SIO index-data access (fan/EC that live on SuperIO).
- **Nvidia.p** — `ioctl_read_thermal_registers`, `ioctl_read_memory_temperatures` (read-only; no
  clock control — NVIDIA clocks go through NVAPI/NVML in user mode, see hw_access peers).
- Others: AMDFamily0F/10, ARMMSR, ZhaoxinMSR, Smbus* (I801/PIIX4/NCT/SkylakeIMC), DellSMM,
  IsaBridgeEC, IntelPCHThermal, IntelOOBMSM, LedsValve (Steam Deck LEDs), AMDReset, Echo.

### 2.4 PawnIO natives available to a *custom* module (native.inc)
`msr_read/msr_write`, `cpu_set_affinity(which,old[2])`/`cpu_restore_affinity`, `cpuid`,
`pci_config_read/write_{byte,word,dword,qword}(bus,dev,fn,off,..)`, `io_in/out_{byte,word,dword}`,
`io_space_map(pa,size)→VA`/`io_space_unmap`, `virtual_read/write_{byte..qword}(va,..)`,
`physical_read/write_{byte..qword}(pa,..)`, `interrupts_disable/enable`, `microsleep(us)`,
`cr_read/write`, `dr_read/write`, `get_arch/get_cpu_vendor/get_cpu_fms/cpu_count`,
`get_proc_address`/`invoke`. ⇒ A custom signed module can do **everything** we need (RAPL 0x610,
undervolt 0x150, HWP 0x774, MCHBAR MMIO writes at 0x59A0, AMD SMN, EC port I/O). The retail-driver
limitation is only that the module must be **signed by namazso**. Plan: request signing, or ship a
custom driver for advanced writes and fall back to IntelMSR/AMDFamily17 whitelisted ops otherwise.

**Verdict on "can PawnIO do everything?"** Via *stock* modules: PL1/PL2 (0x610), Intel undervolt
(0x150), AMD CpbDis/P-state/CPPC-EPP, AMD SMU (STAPM etc.), EC 0x62/0x66, SuperIO — **yes**. NOT via
stock modules: Intel HWP/EPP MSR 0x774 write, PP0 0x638, MCHBAR MMIO PL writes, TjMax 0x1A2, arbitrary
EC ports → unavailable with stock modules.

---

## 3. Intel registers (exact fields)

Primary: Intel SDM Vol 4 (MSR tables); Linux `intel_rapl_msr.c`, `msr-index.h`; intel-undervolt.

### 3.1 RAPL units — MSR 0x606 `MSR_RAPL_POWER_UNIT`
- PWR unit bits **3:0** → watts = 1/2^PU (typ 0x3 → 0.125 W).
- ENERGY unit bits **12:8** → joules = 1/2^EU (typ 0x0E → 61.035 µJ; ADL/newer 0x0E).
- TIME unit bits **19:16** → seconds = 1/2^TU (typ 0x0A → 976.5 µs).

### 3.2 PKG power limit — MSR 0x610 `MSR_PKG_POWER_LIMIT` (writable via IntelMSR)
- **PL1 power** bits 14:0 (units of PWR unit); **PL1 enable** 15; **PL1 clamp** 16.
- **PL1 time window**: Y bits 21:17, Z bits 23:22 → `t = 2^Y * (1 + Z/4) * time_unit`.
- **PL2 power** bits 46:32; **PL2 enable** 47; **PL2 clamp** 48; **PL2 time window** 55:49 (same YZ).
- **LOCK** bit 63 (once set, register RO until reset). Check before writing.
Time-window encode (from intel-undervolt): `val = ((exp&0x1F)<<1)|(((mult-1)*4 &0x3)<<6)` inverse of
`t = 2^((v>>1)&0x1F) * (1+((v>>6)&0x3)/4) / time_unit`.

### 3.3 Other Intel power/limit MSRs
- 0x614 `PKG_POWER_INFO`: TDP(thermal spec) 14:0, min 30:16, max 46:32, max-time-win 53:48 (RO).
- 0x611 `PKG_ENERGY_STATUS`: 31:0 consumed energy (×energy_unit); for power readout, differentiate.
- 0x601 `MSR_VR_CURRENT_CONFIG` (writable): holds **PL4/current limit** on newer parts (per Linux it
  is the PL4 register; low bits current limit in 1/8 A units). **[UNCERTAIN: exact PL4 field width;
  Linux uses POWER_LIMIT4_MASK GENMASK(12,0) on the PL4 primitive — treat 12:0 as PL4 power/current.]**
- 0x638 `PP0_POWER_LIMIT` (IA cores), 0x640 `PP1_POWER_LIMIT` (uncore/GT); 0x63A/0x642 PP0/PP1
  policy (bits 4:0). **Not writable via stock IntelMSR.**
- 0x615 `MSR_PL3_CONTROL` **[UNCERTAIN — not in Linux msr-index; ThrottleStop-era; verify on target]**.
- 0x65C `PLATFORM_POWER_LIMIT` (PSys): PL1 16:0, en 17, PL2 48:32, en 49, TW1 25:19, TW2 57:51, lock 63.
- 0x648 CONFIG_TDP_NOMINAL, 0x649/0x64A LEVEL_1/2, 0x64B CONFIG_TDP_CONTROL (cTDP level select 1:0,
  lock 31). 0x620 `MSR_UNCORE_RATIO_LIMIT`: max 6:0, min 14:8.

### 3.4 MCHBAR RAPL mirror (MMIO)
Find MCHBAR: PCI 0:0.0 offset **0x48** (low32, bit0 = MCHBAREN) + **0x4C** (high32); mask
`&0x3FFFFFF8000` (per-gen; classic desktop base often `0xFED10000`, RAPL block at `0xFED159A0`).
Offsets within MCHBAR (Linux `processor_thermal_rapl.c`): `reg_unit=0x5938`, PKG
`limit=0x59A0, status=0x593C, info=0x58F0, policy=0x5930, perf=0x59B0`; DRAM
`limit=0x58E0,...`. PKG limit MMIO field layout == MSR 0x610. There is a **lock** in the MMIO path;
some SKUs mirror-lock. Stock PawnIO IntelMCHBAR is read-only → use MSR 0x610 for PL writes;
MMIO writes are unavailable with stock modules.

### 3.5 OC mailbox — MSR 0x150 (undervolt; writable via IntelMSR)
64-bit request layout (reverse-engineered; undervolt.py / intel-undervolt / VoltageShift):
- bit **63** = run/busy (set to issue; poll until clear on read-back).
- bits **42:40** = **domain/plane index**.
- bits **39:32** = **command**: `0x80` selects... actually command byte at 32; use `0x10`=read,
  `0x11`=write in the CMD field. Common encoding (undervolt.py `pack_offset`):
  `msr = (1<<63) | (plane<<40) | (1<<36) | (is_write<<32) | offset11`.
  i.e. bit36 = "OC mailbox v offset" selector, bit32 = write flag, offset in 31:21.
- **Voltage offset** bits **31:21**, 11-bit signed, unit **1/1.024 mV** (i.e. `raw = round(mV*1.024)`,
  `field = (raw & 0xFFF) << 21`, masked `0xFFE00000`). Negative mV = undervolt.
- **Planes**: 0 = Core, 1 = GT/iGPU, 2 = Cache/Ring (CLR), 3 = System Agent (SA/uncore),
  4 = Analog I/O. Core & Cache share a rail on many parts (highest of the two applies).
Read sequence: write `(1<<63)|(plane<<40)|(1<<36)` (no write bit) to 0x150, then read 0x150; offset
in 31:21. Write: set bit32 and offset field, write, then read-back to confirm (compare low 32 bits).
- **Plundervolt / CVE-2019-11157**: on 10th-gen+ and post-microcode systems the undervolt interface
  is often **locked in BIOS** (Overclocking Lock / "Undervolt Protection"). Detect: after a write,
  read-back does not match, or 0x150 returns error/0. Also gated by BIOS `OverClocking Lock` bit;
  no clean MSR probe — rely on write-then-verify. Do not assume undervolt works on locked SKUs.

### 3.6 Frequency / turbo / thermal MSRs
- 0xCE `MSR_PLATFORM_INFO`: **max non-turbo ratio 15:8**, **min (LFM) ratio 47:40**, programmable
  ratio bit 28, programmable TDP-limit-turbo bit 29, prog TjMax bit 30. RO.
- 0x1AD `TURBO_RATIO_LIMIT` (+0x1AE/0x1AF): per-core-count max ratios, 8 bits each (bytes = ratio for
  1C,2C,... active cores). 0x1AE/0x1AF extend core-count groups on newer parts.
- 0x1A0 `IA32_MISC_ENABLE`: **bit 38 = Turbo Disable** (1 disables turbo). bit 16 = EIST enable.
- 0x1FC `MSR_IA32_POWER_CTL`: **bit 1 = C1E enable**; **bit 19 = Energy-Efficient (EE) Turbo disable**
  (Linux `MSR_IA32_POWER_CTL_BIT_EE=19`; setting per platform). ("Race-to-halt"/EET control lives
  here — verify polarity per gen.) **[bit 20 not standardized — UNCERTAIN.]**
- 0x199 `IA32_PERF_CTL`: target ratio bits 15:8 (legacy SpeedStep). 0x198 PERF_STATUS (current).
- 0x1A2 `IA32_TEMPERATURE_TARGET`: **TjMax bits 23:16**, **TCC/temp offset bits 29:24** (writable on
  K/HK SKUs when 0xCE bit30 set). Read: TjMax = bits 23:16 °C.
- 0x19C `IA32_THERM_STATUS`: digital readout bits 22:16 (°C below TjMax); 0x1B1
  `IA32_PACKAGE_THERM_STATUS` same for package. 0x1B0 `IA32_ENERGY_PERF_BIAS` (EPB) bits 3:0
  (0=perf..15=powersave) — legacy pre-HWP EPP analog.

### 3.7 HWP / EPP (hardware-managed P-states) — the modern EPP path
MSRs (msr-index.h): 0x770 `IA32_PM_ENABLE` (bit0 = HWP enable, sticky until reset),
0x771 `HWP_CAPABILITIES` (RO: highest 7:0, guaranteed 15:8, most-efficient 23:16, lowest 31:24),
0x774 `IA32_HWP_REQUEST`, 0x772 `HWP_REQUEST_PKG`, 0x773 `HWP_INTERRUPT`, 0x777 `HWP_STATUS`.
`IA32_HWP_REQUEST` (0x774) fields:
- **min perf** 7:0, **max perf** 15:8, **desired perf** 23:16 (0 = HW autonomous),
- **EPP (Energy-Perf Preference)** bits **31:24** (0x00=max perf … 0x80 balance-perf,
  0xC0 balance-powersave, 0xFF=max powersave),
- **activity window** 41:32, **package control** bit 42.
AutoTDP "EPP=0" = write 0x00 into 31:24 to bias to max performance/clocks. Ramping clocks down for an
FPS cap = raise EPP toward 0xFF and/or lower max-perf (15:8) ratio. **Write of 0x774 is NOT in stock
IntelMSR whitelist** → use the Windows PERFEPP/PROCFREQMAX power settings instead (see epp.md).
(Per-core: set thread affinity; HWP_REQUEST is per-logical-CPU unless package control bit set.)

---

## 4. AMD MSRs (Zen)

Primary: AMD PPR (per family), Linux `msr-index.h`, AMDFamily17.p, RyzenAdj.

- 0xC0010015 `HWCR`: **bit 25 = CpbDis** (1 = disable Core Performance Boost/turbo). bit0 SMMLOCK.
  Writable via AMDFamily17.
- 0xC0010064–0xC001006B `PSTATE_0..7` (def base 0xC0010064). Per P-state (Zen):
  **FID bits 7:0, DID bits 13:8, VID bits 21:14, EN bit 63**.
  Core freq = `(FID/DID) * 200 MHz` with DID as divisor → `f = 25 MHz * FID / (DID/8)`; practical
  Zen2+ formula: `f_MHz = (FID * 25) / (DID / 8)` — i.e. `FrequencyMHz = 200 * FID/DID` is the older
  approximation; **use `Fclk = 25*FID/(DID/8)`**, DID in 1/8 steps. VID→volts `V = 1.55 - VID*0.00625`.
  Writable via AMDFamily17 (custom P-state / P0 pin for clock control).
- 0xC0010061 PSTATE_CURRENT_LIMIT, 0xC0010062 PERF_CTL (P-state request 2:0),
  0xC0010063 PERF_STATUS (current 2:0), 0xC0010293 HW_PSTATE_STATUS.
- 0xC0010296 `CSTATE_CONFIG` (writable), 0xC0010292 PMGT_MISC (writable).
- **CPPC (Collaborative Processor Perf Control) — AMD's EPP path**:
  0xC00102B0 CPPC_CAP1 (lowest 7:0, low-nonlin 15:8, nominal 23:16, highest 31:24),
  0xC00102B1 CPPC_ENABLE (bit0), 0xC00102B3 **CPPC_REQ**: **max_perf 7:0, min_perf 15:8,
  des_perf 23:16, EPP 31:24** (0=perf, 0xFF=powersave — same semantics as Intel EPP),
  0xC00102B4 CPPC_STATUS. **CPPC_ENABLE/REQ/STATUS writable via AMDFamily17** → AMD EPP=0 for AutoTDP
  boost, EPP→0xFF + lower max_perf to throttle for FPS cap. Per-thread MSR; use affinity.
- RAPL: 0xC0010299 `RAPL_PWR_UNIT` (same bit layout as Intel 0x606: PU 3:0, EU 12:8, TU 19:16),
  0xC001029A `CORE_ENERGY_STATUS`, 0xC001029B `PKG_ENERGY_STATUS` (31:0 energy count; read-only,
  differentiate for watts). AMD has **no writable PL MSR** — power limits go through **SMU mailbox**
  (RyzenSMU.p / RyzenAdj: STAPM_LIMIT, PPT/Fast/Slow, TCTL_TEMP, etc.).

---

## 5. Embedded Controller (EC) & GPD fan

### 5.1 ACPI EC via ports 0x62 (data) / 0x66 (cmd/status)
Protocol (ACPI spec §12; supported by LpcACPIEC.p, ports gated to 0x62/0x66):
- Status @0x66 read: **bit0 OBF** (output buffer full), **bit1 IBF** (input buffer full),
  bit3 CMD, bit5 BURST.
- **RD_EC = 0x80**: outb(0x66,0x80); wait IBF=0; outb(0x62,addr); wait OBF=1; val=inb(0x62).
- **WR_EC = 0x81**: outb(0x66,0x81); wait IBF=0; outb(0x62,addr); wait IBF=0; outb(0x62,data).
- **QR_EC = 0x84**, BE_EC=0x82, BD_EC=0x83.
Always poll IBF before writing a byte, OBF before reading. Acquire `\BaseNamedObjects\Access_EC`
mutex (shared with other tools) to avoid corrupting multi-byte transactions. Add a timeout on polls.

### 5.2 GPD Win Mini fan register
Linux `gpd-fan` driver (docs.kernel.org/hwmon/gpd-fan.html): manual mode via `pwm1_enable`, duty via
`pwm1` [0–255]. Devices: GPD Win Mini 7840U/8840U, Win Max 2, Win 4, Pocket 3.
- The driver accesses fan control through EC/SuperIO registers (IT8528-class EC) using an
  index/data mechanism, **not** the 0x62/0x66 ACPI ports — it uses a SuperIO port pair. The task's
  "register 0x7A" corresponds to the **fan PWM duty register** in the GPD EC map on some models; the
  GPD driver writes the manual-mode enable then the duty (0–244/255 range varies by model).
  **[UNCERTAIN: exact 0x7A semantics per model — the kernel driver abstracts model-specific offset
  tables (quirk per DMI). Read `drivers/hwmon/gpd-fan.c` register tables at implementation time;
  values differ Win Mini vs Win4 vs Max2.]** Use LpcIO.p (SuperIO index/data) or a custom module,
  since 0x7A is not on the ACPI 0x62/0x66 ports.

---

## 6. Implementation notes for Phawx ON

- **Backend abstraction**: `struct hw_backend { read_msr, write_msr, read_pci, write_pci, in_port,
  out_port, mmio_map, smu_cmd, ec_read, ec_write; }`. Provide `pawnio` impl (load IntelMSR +
  AMDFamily17 + RyzenSMU + LpcACPIEC on separate handles). Vendor/feature
  detection via CPUID at init picks module set.
- **Per-core ops**: PawnIO custom module uses `cpu_set_affinity`; with stock modules use
  `SetThreadAffinityMask` around each read/write. HWP/CPPC/undervolt are per-thread.
- **Mutexes**: honor `\BaseNamedObjects\Access_PCI` (SMN/SMU) and `Access_EC` (EC) named mutexes.
- **AutoTDP fast loop**: cheapest high-rate knob is **HWP EPP (Intel 0x774 31:24) / CPPC EPP+max_perf
  (AMD 0xC00102B3)** — no lock, per-core, effective in <1 ms. Lower max_perf ratio to cap clocks for
  an FPS target; EPP=0 + high max_perf to recover. Avoid re-writing 0x610/SMU at high frequency
  (slower, may lock). GPU clocks: AMD ADLX/SMU, NVIDIA NVAPI/NVML, Intel via IGCL — user-mode SDKs,
  separate research file.
- **Undervolt**: probe with write-then-verify on 0x150; disable UI if locked (Plundervolt mitigation).

## 7. Sources
- PawnIO: github.com/namazso/PawnIO (`PawnIOLib/PawnIOLib.cpp`, `PawnIO/include/pawnio_um.h`,
  `src/vm.cpp`), github.com/namazso/PawnIO.Modules (`IntelMSR.p, IntelMCHBAR.p, AMDFamily17.p,
  RyzenSMU.p, LpcACPIEC.p, LpcIO.p, include/native.inc`); pawnio.eu.
- Intel: Intel SDM Vol 3B/4; Linux `drivers/powercap/intel_rapl_{common,msr}.c`,
  `drivers/thermal/intel/int340x_thermal/processor_thermal_rapl.c`, `arch/x86/include/asm/msr-index.h`.
- Undervolt/OC mailbox: kitsunyan/intel-undervolt (`undervolt.c`,`power.c`,`config.h`),
  georgewhewell/undervolt (`undervolt.py`), sicreative/VoltageShift; CVE-2019-11157 (Plundervolt).
- AMD: AMD PPR Family 17h/19h; RyzenAdj; Linux amd-pstate + msr-index.h CPPC defs.
- EC/GPD: ACPI Spec §12 EC; Linux `Documentation/hwmon/gpd-fan.rst`, `drivers/hwmon/gpd-fan.c`;
  jertel/p37-ec.
