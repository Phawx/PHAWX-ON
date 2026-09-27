# Energy Performance Preference (EPP), HWP, AMD CPPC and Windows PPM — implementer reference

Scope: what Phawx ON needs to read/write EPP and max-clock limits on Intel (HWP / Speed Shift) and AMD (CPPC), how Windows' PPM drivers interfere, and which mechanism to use for the AutoTDP fast path.

Confidence tags: **[V]** verified against primary source text during research (Linux `msr-index.h`, `intel_pstate.c`, `amd-pstate.c`, LibreHardwareMonitor, Windows GUID lists). **[K]** well-established from Intel SDM / AMD PPR / Microsoft docs, not re-fetched (learn.microsoft.com was unreachable from the research sandbox). **[U]** uncertain or empirical; verify on hardware.

---

## 1. Intel HWP (Hardware P-states / Speed Shift)

### 1.1 Feature detection — CPUID.06H **[K]** (bit names match Linux `X86_FEATURE_*`)

| Reg.bit | Meaning |
|---|---|
| EAX[1] | Turbo Boost available |
| EAX[7] | HWP base regs (0x770, 0x771, 0x774, 0x777) |
| EAX[8] | HWP_Notification (0x773) |
| EAX[9] | HWP_Activity_Window (bits 41:32 of 0x774 honored) |
| EAX[10] | HWP_Energy_Performance_Preference (EPP field bits 31:24 honored). **If 0, EPP is ignored and the EPB MSR 0x1B0 is used instead** |
| EAX[11] | HWP_Package_Level_Request (0x772 exists) |
| EAX[13] | HDC (hardware duty cycling, 0xDB0) |
| EAX[14] | Turbo Boost Max 3.0 (per-core highest perf differs → favored cores) |
| EAX[15] | HWP capabilities "highest perf change" interrupt |
| EAX[16] | HWP PECI override |
| EAX[17] | Flexible HWP |
| EAX[18] | Fast access mode for IA32_HWP_REQUEST |
| EAX[19] | HW_FEEDBACK (HFI) |
| EAX[20] | Ignoring idle logical processor HWP request |
| EAX[22] | IA32_HWP_CTL (0x776) present |
| EAX[23] | Intel Thread Director (enhanced HFI) |
| ECX[0] | APERF/MPERF (0xE8/0xE7) |
| ECX[3] | IA32_ENERGY_PERF_BIAS (0x1B0) |

Hybrid detection: CPUID.07H:EDX[15] (Hybrid). Per-logical-CPU core type: CPUID.1AH:EAX[31:24] = 0x20 Atom (E-core), 0x40 Core (P-core) **[K]**. Must run CPUID with thread affinity pinned to each CPU (SetThreadAffinityMask / SetThreadGroupAffinity), or use `GetSystemCpuSetInformation().CpuSet.EfficiencyClass` (0 = E-core, 1 = P-core on hybrid; higher = less efficient/faster).

### 1.2 MSR map **[V]** addresses/field macros from Linux `arch/x86/include/asm/msr-index.h`

| MSR | Name | Scope | Notes |
|---|---|---|---|
| 0x770 | IA32_PM_ENABLE | package | bit0 HWP_ENABLE. **Write-once**: cannot be cleared until reset. Windows sets it at boot when firmware grants HWP via _OSC. |
| 0x771 | IA32_HWP_CAPABILITIES | thread (RO) | 7:0 Highest, 15:8 Guaranteed, 23:16 Most_Efficient, 31:24 Lowest. Guaranteed can change dynamically (thermal/cTDP) → HWP_STATUS bit0. |
| 0x772 | IA32_HWP_REQUEST_PKG | package | same layout as 0x774 bits 0–41; used only by threads whose 0x774 bit42=1. |
| 0x773 | IA32_HWP_INTERRUPT | thread | bit0 change-to-guaranteed int enable, bit1 excursion-to-minimum int enable, bit2 highest-change (needs EAX[15]). |
| 0x774 | IA32_HWP_REQUEST | thread | see 1.3 |
| 0x776 | IA32_HWP_CTL | package | bit0 PKG_CTL_POLARITY (needs EAX[22]). **[K]** |
| 0x777 | IA32_HWP_STATUS | thread | bit0 guaranteed perf changed (sticky, write 0 to clear), bit2 excursion to minimum, bit4 highest perf changed. |
| 0x1B0 | IA32_ENERGY_PERF_BIAS | thread (often effectively package/core) | bits 3:0: 0 perf, 4 balance-perf, 6 normal, 7 normal-powersave, 8 balance-power, 15 powersave (Linux constants). |
| 0x199 | IA32_PERF_CTL | thread | **Ignored once HWP is enabled.** Do not use for clock caps on HWP systems. |
| 0x198 | IA32_PERF_STATUS | | 15:8 current ratio (legacy; on HWP still readable). |
| 0x64E | MSR_PPERF | thread | productive perf counter. |
| 0x64F | MSR_PERF_LIMIT_REASONS (core) | | why clocks are limited (good for UI diagnostics). |
| 0xE7/0xE8 | IA32_MPERF / IA32_APERF | thread | effective freq = base × ΔAPERF/ΔMPERF. |

### 1.3 IA32_HWP_REQUEST (0x774) layout

```c
typedef union {
    unsigned long long raw;
    struct {
        unsigned long long min_perf      : 8;  /* 7:0   */
        unsigned long long max_perf      : 8;  /* 15:8  */
        unsigned long long desired_perf  : 8;  /* 23:16 0 = fully autonomous (what you want) */
        unsigned long long epp           : 8;  /* 31:24 0 = max perf .. 255 = max efficiency */
        unsigned long long act_window    : 10; /* 41:32 mantissa 38:32 (7b), exponent 41:39 (3b) */
        unsigned long long pkg_control   : 1;  /* 42    1 = take fields from HWP_REQUEST_PKG */
        unsigned long long reserved      : 16; /* 58:43 */
        unsigned long long min_valid     : 1;  /* 59 */
        unsigned long long max_valid     : 1;  /* 60 */
        unsigned long long desired_valid : 1;  /* 61 */
        unsigned long long epp_valid     : 1;  /* 62 */
        unsigned long long window_valid  : 1;  /* 63 */
    } f;
} HWP_REQUEST;
```

- Activity window: `window_us = mantissa * 10^exponent`, 0 = hardware chooses (recommended). Range ~1 us to 1270 s. Linux masks the field with `0xff3` (sic) **[V]**.
- Bits 59–63 ("valid" bits) matter only when bit42 = 1: a field whose valid bit is set is taken from the per-thread register, the rest from 0x772 **[K]**. Windows writes per-thread requests with bit42 = 0, so the package register is effectively unused on Windows. Leave 42 and 59–63 as 0.
- Min ≤ max needed. Clamp both to `[Lowest, Highest]` from 0x771. Setting max < min, or out of range, causes #GP on some parts **[U]**; always clamp.
- Linux EPP string values **[V]**: performance 0x00, balance_performance 0x80, balance_power 0xC0, power 0xFF.

EPB fallback: when CPUID.06H:EAX[10] = 0 (e.g. Skylake parts without EPP), HWP uses 0x1B0[3:0]. Rough mapping `epp ≈ epb * 17` (0→0, 6→102, 15→255). On EPP-capable parts EPB still affects non-HWP things (uncore, turbo/C-state policy, and on older client parts the fallback). Setting EPB = 0 with EPP = 0 is harmless for AutoTDP.

### 1.4 Perf units ↔ MHz (hybrid scaling) **[V]** from `intel_pstate.c`

Non-hybrid Intel client: 1 HWP unit = 100 MHz (`INTEL_PSTATE_CORE_SCALING = 100000` kHz).

Hybrid: E-cores (Atom) stay at 100 MHz per unit. P-cores use a smaller kHz-per-unit factor, because HWP perf on P-cores is normalized so that both core types sit on one perf scale for the scheduler:

| Platform (VFM match in Linux) | P-core kHz per HWP unit |
|---|---|
| Alder Lake, Alder Lake-L, Raptor Lake (all), Bartlett Lake | 78741 (≈ 1/1.27) |
| Meteor Lake-L | 80000 |
| Lunar Lake-M | 86957 (≈ 1/1.15) |
| Other hybrid (Arrow Lake, Panther Lake, ...) | no constant: use ACPI _CPC `nominal_freq*1000 / nominal_perf` |

```c
/* P-core on ADL: HWP highest 0x3F (63) -> 63*78741 kHz = 4.96 GHz, not 6.3 GHz */
static unsigned perf_to_mhz(unsigned perf, unsigned khz_per_unit) { return (perf * khz_per_unit + 500) / 1000; }
static unsigned mhz_to_perf(unsigned mhz, unsigned khz_per_unit) { return (mhz * 1000 + khz_per_unit - 1) / khz_per_unit; }
```

Linux also rounds frequencies down to the PERF_CTL granularity (100 MHz), because hardware really runs 100 MHz ratio bins; a P-core HWP request of N units lands on the nearest bin. Practical rule for Phawx ON:
- Build a per-core-type table `khz_per_unit[type]` at startup: 100000 for Atom and non-hybrid, the Linux constant for known hybrid P-cores, else (fallback without ACPI) `max_turbo_ratio*100000 / HWP_CAP.highest` for that core (turbo ratio from MSR 0x1AD `TURBO_RATIO_LIMIT` byte 0; approximate).
- Write per-core max_perf with `mhz_to_perf(cap_mhz, khz_per_unit[type])`. Never write one raw byte to all cores on hybrid parts.
- Unknown platform: don't guess a factor. Use a self-calibration loop instead: write a max_perf, measure APERF/MPERF, and fit the scale.

### 1.5 Writing on Windows
Every 0x774 write must happen on the target logical CPU. Options: kernel driver does per-CPU wrmsr (IPI), or user code pins a thread (SetThreadGroupAffinity) and calls the driver's single-CPU wrmsr. Cost: a few µs per CPU via a driver; around 20–50 µs per CPU if you migrate threads **[U]**. So a 24-thread CPU takes well under 5 ms. Do read-modify-write, preserving fields you don't own.

---

## 2. Windows processor power management (PPM)

### 2.1 Drivers
- `intelppm.sys` (Intel), `amdppm.sys` (AMD), `processr.sys` (generic). All are PEP/PPM drivers on top of the kernel PPM engine (ntoskrnl `Ppm*`). On CPPC/HWP systems in *autonomous mode* (PERFAUTONOMOUS = 1, the default when HWP/CPPC is present), the kernel computes min/max/EPP/activity-window from the power settings and the driver writes them to 0x774 (Intel) or CPPC_REQ / the _CPC registers (AMD).
- Percent → EPP byte: `epp = round(PERFEPP% * 255 / 100)` **[K/U]**: 0%→0, 50%→~128, 100%→255. Tools observe e.g. 33% → 0x54, 60% → 0x99. Treat the rounding as ±1.
- Windows "processor performance %" is relative to **nominal (base/guaranteed) frequency**. 100% of PROCTHROTTLEMAX means unlimited (turbo allowed). ≤99% caps to that percent of nominal, which disables turbo. This is why "Max processor state 99%" turns off turbo **[K]**.
- In autonomous mode Windows sets desired_perf = 0 and lets hardware pick. min_perf follows PROCTHROTTLEMIN. max_perf follows min(PROCTHROTTLEMAX, PROCFREQMAX, boost mode, thermal/PPC limits).
- Hybrid: settings with suffix `1` apply to **Processor Power Efficiency Class 1** (the *less* efficient class = P-cores on Intel hybrid). Unsuffixed settings apply to class 0 (E-cores) and, on non-hybrid systems, to all cores. Lower class = more efficient **[V]** (Microsoft processor power management docs, via search). The overlay/slider (see 2.4) usually changes both. Always write both variants.

### 2.2 GUIDs — subgroup and settings **[V]** (GUID list from mkht/DSCR_PowerPlan, cross-checked with bitsum list)

Subgroup `GUID_PROCESSOR_SETTINGS_SUBGROUP` (SUB_PROCESSOR) = `54533251-82be-4824-96c1-47b60b740d00`.

Performance / EPP / frequency:

| Alias | GUID | Units / values |
|---|---|---|
| PERFEPP | 36687f9e-e3a5-4dbf-b1dc-15eb381c6863 | 0–100 % (→ EPP 0–255), class 0 |
| PERFEPP1 | 36687f9e-e3a5-4dbf-b1dc-15eb381c6864 | same, class 1 (P-cores) |
| PERFAUTONOMOUS | 8baa4a8a-14c6-4451-8e8b-14bdbd197537 | 0 disabled (OS picks desired perf), 1 enabled (HW autonomous) |
| PERFAUTONOMOUSWINDOW | cfeda3d0-7697-4566-a922-a9086cd49dfa | microseconds; 0 = HW default. Encoded into 0x774[41:32] **[K]** |
| PERFBOOSTMODE | be337238-0d82-4146-a960-4f3749d470c7 | 0 Disabled, 1 Enabled, 2 Aggressive, 3 Efficient Enabled, 4 Efficient Aggressive, 5 Aggressive At Guaranteed, 6 Efficient Aggressive At Guaranteed **[K]**. On HWP/CPPC: 0 caps max_perf at guaranteed/nominal (no turbo); 2 = go straight to highest |
| PERFBOOSTPOL | 45bcc044-d885-43e2-8605-ee0ec6e96b59 | 0–100 %, how much boost is used (only matters for non-autonomous/legacy) |
| PROCFREQMAX | 75b0ae3f-bce0-45a7-8c89-c9611c25e100 | MHz, 0 = no limit, class 0 |
| PROCFREQMAX1 | 75b0ae3f-bce0-45a7-8c89-c9611c25e101 | MHz, class 1 |
| PROCTHROTTLEMAX | bc5038f7-23e0-4960-96da-33abaf5935ec | % of nominal (100 = unlimited) |
| PROCTHROTTLEMAX1 | bc5038f7-23e0-4960-96da-33abaf5935ed | class 1 |
| PROCTHROTTLEMIN | 893dee8e-2bef-41e0-89c6-b55d0929964c | % |
| PROCTHROTTLEMIN1 | 893dee8e-2bef-41e0-89c6-b55d0929964d | class 1 |
| PERFCHECK | 4d2b0152-7d5c-498b-88e2-34345392a2c5 | ms, PPM evaluation interval (default ~15–30 ms) |
| PERFINCPOL / 1 | 465e1f50-b610-473a-ab58-00d1077dc418 / …dc419 | 0 Ideal, 1 Single, 2 Rocket, 3 IdealAggressive |
| PERFDECPOL / 1 | 40fbefc7-2e9d-4d25-a185-0cfd8574bac6 / …bac7 | 0 Ideal, 1 Single, 2 Rocket |
| PERFINCTHRESHOLD / 1 | 06cadf0e-64ed-448a-8927-ce7bf90eb35d / …b35e | % util |
| PERFDECTHRESHOLD / 1 | 12a0ab44-fe28-4fa9-b3bd-4b64f44960a6 / …60a7 | % util |
| PERFINCTIME / 1 | 984cf492-3bed-4488-a8f9-4286c97bf5aa / …f5ab | check intervals |
| PERFDECTIME / 1 | d8edeb9b-95cf-4f95-a73c-b061973693c8 / …93c9 | check intervals |
| PERFHISTORY / 1 | 7d24baa7-0b84-480f-840c-1b0743c00f5f / …0f60 | count |
| PERFDUTYCYCLING | 4e4450b3-6179-4e91-b8f1-5bb9938f81a1 | 0/1 |
| LATENCYHINTPERF / 1 | 619b7505-003b-4e82-b7a6-4dd29c300971 / …0972 | % perf floor during latency-sensitive hint (input, app launch) |
| LATENCYHINTUNPARK / 1 | 616cdaa5-695e-4545-97ad-97dc2d1bdd88 / …dd89 | % cores kept unparked during hint |
| THROTTLING | 3b04d4fd-1cc7-4f23-ab1c-d1337819c4bb | allow T-states |
| SYSCOOLPOL | 94d3a615-a899-4ac5-ae2b-e4d8f634367f | 0 passive, 1 active |

Core parking / heterogeneous scheduling (details belong in the hetero-cores doc):

| Alias | GUID |
|---|---|
| CPMINCORES / 1 | 0cc5b647-c1df-4637-891a-dec35c318583 / …8584 |
| CPMAXCORES / 1 | ea062031-0e34-4ff1-9b6d-eb1059334028 / …4029 |
| CPPERF / 1 (parked perf state) | 447235c7-6a8d-4cc0-8e24-9eaf70b96e2b / …6e2c |
| CPCONCURRENCY | 2430ab6f-a520-44a2-9601-f7f23b5134b1 |
| CPHEADROOM | f735a673-2066-4f80-a0c5-ddee0cf1bf5d |
| CPDISTRIBUTION | 4bdaf4e9-d103-46d7-a5f0-6280121616ef |
| CPOVERUTIL | 943c8cb6-6f93-4227-ad87-e9a3feec08d1 |
| CPINCREASEPOL / CPDECREASEPOL | c7be0679-2817-4d69-9d02-519a537ed0c6 / 71021b41-c749-4d21-be74-a00f335d582b |
| CPINCREASETIME / CPDECREASETIME | 2ddd5a84-5a71-437e-912a-db0b8c788732 / dfd10d17-d5eb-45dd-877a-9a34ddd15c82 |
| DISTRIBUTEUTIL | e0007330-f589-42ed-a401-5ddb10e785d3 |
| HETEROPOLICY | 7f2f5cfa-f10c-4823-b5e1-e93ae85f46b5 |
| SCHEDPOLICY | 93b8b6dc-0698-4d1c-9ee4-0644e900c85d |
| SHORTSCHEDPOLICY | bae08b81-2d5e-4688-ad6a-13243356654b |
| HETEROINCREASETHRESHOLD / DECREASE | b000397d-9b0b-483d-98c9-692a6060cfbf / f8861c27-95e7-475c-865b-13c0cb3f9d6b |
| HETEROINCREASETIME / DECREASE | 4009efa7-e72d-4cba-9edf-91084ea8cbc3 / 7f2492b6-60b1-45e5-ae55-773f8cd5caec |
| HETEROCLASS1INITIALPERF | 1facfc65-a930-4bc5-9f38-504ec097bbc0 |
| HETEROCLASS0FLOORPERF | fddc842b-8364-4edc-94cf-c17f60de1c80 |
| IDLEDISABLE | 5d76a2ca-e8c0-402f-a133-2158492d58ad |
| IDLESTATEMAX | 9943e905-9a30-4ec1-9b99-44dd3b76f7a2 |

Most of these are hidden (`ATTRIBUTE_HIDE`). Unhide with `PowerWriteSettingAttributes(&SUB, &SETTING, 0)` or `powercfg -attributes SUB_PROCESSOR <alias> -ATTRIB_HIDE`. Hidden settings can still be written with PowerWrite*ValueIndex without unhiding.

### 2.3 Win32 API (powrprof.dll, link `-lpowrprof`)

```c
#include <windows.h>
#include <powrprof.h>
DWORD WINAPI PowerGetActiveScheme(HKEY UserRootPowerKey, GUID **ActivePolicyGuid);      /* LocalFree(*ActivePolicyGuid) */
DWORD WINAPI PowerSetActiveScheme(HKEY UserRootPowerKey, const GUID *SchemeGuid);
DWORD WINAPI PowerReadACValueIndex(HKEY RootPowerKey, const GUID *SchemeGuid,
          const GUID *SubGroupOfPowerSettingsGuid, const GUID *PowerSettingGuid, LPDWORD AcValueIndex);
DWORD WINAPI PowerReadDCValueIndex(HKEY RootPowerKey, const GUID *SchemeGuid,
          const GUID *SubGroupOfPowerSettingsGuid, const GUID *PowerSettingGuid, LPDWORD DcValueIndex);
DWORD WINAPI PowerWriteACValueIndex(HKEY RootPowerKey, const GUID *SchemeGuid,
          const GUID *SubGroupOfPowerSettingsGuid, const GUID *PowerSettingGuid, DWORD AcValueIndex);
DWORD WINAPI PowerWriteDCValueIndex(HKEY RootPowerKey, const GUID *SchemeGuid,
          const GUID *SubGroupOfPowerSettingsGuid, const GUID *PowerSettingGuid, DWORD DcValueIndex);
DWORD WINAPI PowerWriteSettingAttributes(const GUID *SubGroupGuid, const GUID *PowerSettingGuid, DWORD Attributes);
DWORD WINAPI PowerDuplicateScheme(HKEY RootPowerKey, const GUID *SourceSchemeGuid, GUID **DestinationSchemeGuid);
DWORD WINAPI PowerSettingRegisterNotification(LPCGUID SettingGuid, DWORD Flags /*DEVICE_NOTIFY_CALLBACK*/,
          HANDLE Recipient, PHPOWERNOTIFY RegistrationHandle);
/* Undocumented exports (resolve with GetProcAddress; return ERROR_SUCCESS = 0) */
typedef DWORD (WINAPI *PFN_PowerSetActiveOverlayScheme)(const GUID *OverlaySchemeGuid);
typedef DWORD (WINAPI *PFN_PowerGetActualOverlayScheme)(GUID *ActualOverlayGuid);
typedef DWORD (WINAPI *PFN_PowerGetEffectiveOverlayScheme)(GUID *EffectiveOverlayGuid);
```

Pass `NULL` for the HKEY. Writes change the stored scheme only. They take effect when the active scheme is re-applied: call `PowerSetActiveScheme(NULL, active)` after writing (the standard trick; `powercfg /setactive SCHEME_CURRENT` does the same). Re-applying costs roughly 10–50 ms of wall time (all settings are re-evaluated and notifications broadcast) **[U]**. That is too slow and heavy for a per-frame control loop, but fine for static config.

AC/DC: write both indexes; the system uses AC on power, DC on battery. Handheld users switch often, so write both every time.

Tip: work on a duplicated "Phawx ON" scheme (PowerDuplicateScheme from the current one, then activate it). Restore the user's scheme on exit or crash-recovery. Never edit the built-in schemes in place.

### 2.4 Power mode overlays (Settings → Power mode slider) **[V/K]**

| Overlay | GUID |
|---|---|
| Balanced / Recommended (none) | 00000000-0000-0000-0000-000000000000 |
| Best performance | ded574b5-45a0-4f42-8737-46345c09c238 |
| Better performance | 3af9b8d9-7c97-431d-ad78-34a8bfea439f **[K]** |
| Best power efficiency (Better battery) | 961cc777-2547-4f9d-8174-7d86181b8a7a |

Overlays are PPKG-defined deltas on top of the base scheme (EPP, boost, parking; OEM PPM provisioning packages can redefine them). **An overlay value overrides your scheme value for the same setting.** For example, Best power efficiency on battery can force PERFEPP to ~50–70% no matter what you wrote. If you rely on scheme values, set overlay = Best performance (EPP overlay ≈ 0–10%) or the balanced overlay, then write your values. Overlays only exist on the Balanced base scheme; with High performance/Ultimate active, the slider is greyed out.

### 2.5 Does Windows rewrite 0x774 / CPPC_REQ behind our back? **[U — empirical, widely observed]**

Yes. The PPM driver reprograms the request register on:
- power scheme apply, any power setting change, overlay/slider change, AC↔DC transition;
- resume from S3/S4/Modern Standby, and a logical processor coming online/reinitializing;
- Game Mode / "latency sensitivity hint" episodes (LATENCYHINTPERF raises min_perf temporarily; app launch, input, foreground change);
- on hybrid Windows 11, QoS-based policy: when a thread's QoS changes (EcoQoS / Utility / foreground), the PPM engine can move a core's EPP and max between the class-specific values. This can happen very often (tens of ms granularity). Direct MSR writes can survive seconds or only milliseconds, depending on workload;
- PERFAUTONOMOUS = 0 (OS-directed): Windows writes desired_perf on each PERFCHECK tick (~15–30 ms), which rewrites the whole register, including your max and EPP.

ThrottleStop's Speed Shift EPP feature exists for this reason: it re-applies its value on its polling interval (default 1 s).

Implications:
1. **Durable state (EPP, static max freq, boost on/off): use power settings** (PERFEPP/PERFEPP1, PROCFREQMAX/PROCFREQMAX1, PERFBOOSTMODE) + `PowerSetActiveScheme`. Then Windows itself writes our values each time it reprograms the register, and we never fight it.
2. **Fast path (AutoTDP clock ceiling changes): write the MSR directly**, plus a **guard**: on each AutoTDP tick (for example every 16–50 ms), read 0x774 on one representative core per class (cheap). If the max/EPP bytes differ from ours, re-write all cores. Also re-apply on `WM_POWERBROADCAST` (PBT_APMRESUMEAUTOMATIC, PBT_POWERSETTINGCHANGE for GUID_ACDC_POWER_SOURCE and the power-scheme-personality GUID) and on overlay change.
3. Keep the power settings consistent with the MSR target (EPP 0%, boost Aggressive). Then when Windows does rewrite, only the max byte is wrong, and the guard fixes that within one tick.
4. Set `PERFAUTONOMOUS = 1` while AutoTDP runs so that desired_perf stays 0 and Windows does not rewrite on every PERFCHECK.

---

## 3. AMD CPPC

### 3.1 Detection
- MSR-based ("full") CPPC: CPUID **0x80000008:EBX[27]** (`X86_FEATURE_CPPC`) **[K]**. Present on Zen 3 and later client (Cezanne/Rembrandt/Phoenix/Hawk Point/Strix/Krackan, Vermeer/Raphael/Granite Ridge; Van Gogh supports it).
- If absent (Zen 2: Renoir/Lucienne/Matisse; some Zen+), CPPC is "shared memory" via ACPI _CPC + PCC mailbox. **The 0xC00102Bx MSRs do not exist there (#GP)**. Fall back to power settings and SMU only.
- Always read with an exception-safe read (driver returns failure on #GP) before relying on it.

### 3.2 MSRs **[V]** addresses/masks from Linux msr-index.h

| MSR | Name | Fields |
|---|---|---|
| 0xC00102B0 | CPPC_CAP1 (RO) | 7:0 Lowest, 15:8 LowestNonlinear, 23:16 Nominal, 31:24 Highest, 39:32 FloorPerfCnt (new parts) |
| 0xC00102B1 | CPPC_ENABLE | bit0 CPPC_EN. **Write-once**: once 1, stays 1 until reset. Windows/firmware normally sets it. |
| 0xC00102B2 | CPPC_CAP2 | 7:0 constrained max perf (thermal/platform-limited highest) **[U]** |
| 0xC00102B3 | CPPC_REQ | 7:0 **Max**, 15:8 **Min**, 23:16 **Desired** (0 = autonomous), 31:24 **EPP** |
| 0xC00102B4 | CPPC_STATUS | bit0 status/"min excursion"-style flags; see PPR **[U]** |
| 0xC00102B5 | CPPC_REQ2 | 7:0 floor perf (newest parts only) |
| 0xC0010061 | PStateCurLim | 2:0 CurPstateLimit, 6:4 PstateMaxVal |
| 0xC0010062 | PStateCtl | 2:0 requested P-state (legacy, ignored under CPPC autonomous) |
| 0xC0010063 | PStateStat | 2:0 current P-state |
| 0xC0010064–6B | PStateDef[0..7] | bit63 PstateEn; Zen1–4: 7:0 CpuFid, 13:8 CpuDfsId, 21:14 CpuVid; **Family 1Ah (Zen 5): 11:0 CpuFid** |
| 0xC0010293 | HwPstateStatus | current FID/DID, same encoding as PStateDef |
| 0xC00000E7/E8 | MPERF_RO / APERF_RO | read-only copies |

```c
typedef union { unsigned long long raw; struct {
    unsigned long long max_perf:8, min_perf:8, des_perf:8, epp:8, rsvd:32; } f; } AMD_CPPC_REQ;
typedef union { unsigned long long raw; struct {
    unsigned long long lowest:8, lowest_nonlinear:8, nominal:8, highest:8, floor_cnt:8, rsvd:24; } f; } AMD_CPPC_CAP1;
```

Note the **field order is the reverse of Intel**: AMD CPPC_REQ is max in 7:0 and min in 15:8. Intel is min in 7:0 and max in 15:8. Easy bug; keep separate writers.

### 3.3 Perf ↔ frequency
AMD perf is an abstract, per-core scale, not MHz. Linux amd-pstate is linear through the origin anchored at nominal **[V]**:

```c
perf = ceil(freq_mhz * nominal_perf / nominal_freq_mhz);  /* clamp [lowest, highest] */
freq = ceil(nominal_freq_mhz * perf / nominal_perf);
```

- `nominal_freq` comes from ACPI _CPC `NominalFrequency` (Linux `cppc_perf.nominal_freq`). Without ACPI, use **P0 from PStateDef[0]** (0xC0010064): Zen1–4 `MHz = CpuFid / CpuDfsId * 200` (i.e., `FID*25` when DfsId=8); Family 1Ah `MHz = CpuFid[11:0] * 5` **[V]** (LibreHardwareMonitor Amd17Cpu.cs). P0 = base/nominal clock.
- `highest_perf` is **not** "max boost MHz / scale". It is often 166 or 196, or up to 255 on preferred cores. On Zen 4+ with preferred cores, highest_perf is a **ranking value** per core. Linux uses a separate "boost numerator" (typically 166, or 196 for some parts) to compute max freq: `max_freq = nominal_freq * boost_numerator / nominal_perf` **[K]**. So on preferred-core systems, don't use raw per-core highest_perf for MHz math. Use the min of highest_perf across cores, or 166/196 as the numerator, and clamp writes to each core's own highest.
- Example (Z1 Extreme / Phoenix): nominal_perf ≈ 120 **[U]**, nominal 3300 MHz, numerator 166 → max ≈ 4565 MHz (spec 5.1 GHz; boosts above "highest" can still happen via firmware). Treat the mapping as approximate. Close the loop with APERF/MPERF.
- `lowest_nonlinear` is the efficiency knee. Below it, perf/W gets worse. A good floor for AutoTDP CPU max caps.

### 3.4 Linux amd-pstate modes (reference behavior) **[V/K]**
- `passive` (amd-pstate): governor-driven desired_perf, EPP unused.
- `active` (amd-pstate-epp): desired = 0, firmware autonomous, driven by EPP + min/max. EPP strings: default (firmware), performance **0x00**, balance_performance **0x80**, balance_power **0xBF**, power **0xFF**. Linux forces EPP=0 when policy = performance.
- `guided`: desired = 0, min/max set by governor, firmware picks within the range.

### 3.5 Windows on AMD
- `amdppm.sys` writes CPPC_REQ (MSR CPPC) or the _CPC registers (shared memory) using the same kernel PPM policy as Intel: PERFEPP → EPP byte, PROCTHROTTLEMAX/PROCFREQMAX/PERFBOOSTMODE → max_perf, desired 0 in autonomous mode.
- PERFBOOSTMODE = 0 caps max_perf at nominal (no boost), the simplest "no-turbo" switch on AMD.
- **Preferred cores**: firmware exposes per-core highest_perf. Windows uses it (via _CPC HighestPerformance + the "Performance Changed" notify 0x85) to rank cores for scheduling. Capping max_perf does not change the ranking, which is good.
- **AMD PPM Provisioning File Driver** (part of the chipset package) installs PPM provisioning packages. These override power-scheme and overlay defaults for EPP, boost, parking and hetero settings, per SKU. On X3D dual-CCD it works with `amd3dvcache.sys` (3D V-Cache Performance Optimizer), which parks the non-V-Cache CCD during Game Mode via parking policy. Installing or updating the chipset driver can reset scheme values; re-apply ours on startup.
- "AMD Ryzen Power Plan" (legacy, Zen/Zen+) is a separate scheme GUID. Obsolete on Win11.
- Do direct CPPC_REQ writes stick? Same as Intel **[U]**: they persist until amdppm reprograms (settings change, AC/DC, resume, QoS/latency hint). Use the same guard strategy. Observed in community tools (e.g., Universal x86 Tuning Utility, Handheld Companion) as "works, needs periodic re-apply".
- SMU route (RyzenAdj-style): mailbox commands exist for STAPM/fast/slow limits, `set_max_gfxclk`/`set_min_gfxclk` (some APUs), and per-family CPU caps (e.g., Van Gogh `set_max_cpuclk`?) **[U]**. Not a general CPU max-clock control on Phoenix+. SMU calls are slow-ish (~1 ms mailbox round-trip) **[U]**. Prefer CPPC_REQ.max for CPU clock caps; keep SMU for power limits and iGPU clocks.

---

## 4. AutoTDP recommendations (EPP = 0, cap clocks instead)

### 4.1 Why EPP = 0 + clock ceiling
EPP = 0 tells HWP/CPPC to race to the highest frequency allowed by min/max and to use aggressive ramp-up (short activity window). Frame-time spikes get absorbed as fast as possible. Power is then controlled by the **max_perf ceiling**, which AutoTDP moves: frequency ceiling → lower V/f point → power falls roughly with f·V². This gives deterministic clocks and lower latency than high EPP values, which make the firmware slow to ramp and cause stutter.

### 4.2 Mechanism ranking for fast (<5 ms) ceiling changes

| Vendor | Mechanism | Latency | Durability | Verdict |
|---|---|---|---|---|
| Intel HWP | **0x774 max_perf per thread (direct wrmsr)** | µs per CPU; HW reacts in ~<1 ms (Speed Shift) | overwritten by PPM events → guard | **Primary** |
| Intel | PROCFREQMAX/PROCFREQMAX1 + PowerSetActiveScheme | 10–50 ms+, broadcasts | durable | static/fallback; sync periodically |
| Intel | PROCTHROTTLEMAX % | same as above; granularity = % of nominal | durable | fallback only |
| Intel | IA32_PERF_CTL 0x199 | — | ignored under HWP | **don't** |
| Intel | turbo ratio limits 0x1AD / PL1/PL2 (RAPL) | fast but power-, not clock-based | — | TDP layer, not the clock loop |
| AMD MSR-CPPC | **CPPC_REQ max_perf (direct wrmsr)** | µs per CPU; CCLK DPM reacts ~1 ms | overwritten by PPM events → guard | **Primary** |
| AMD | PROCFREQMAX/1 + scheme apply | 10–50 ms+ | durable | static/fallback (only option on Zen 2 shared-memory CPPC) |
| AMD | SMU mailbox | ~ms, family-specific | durable until reset/sleep | power limits + iGPU clocks; CPU clocks only where supported |

### 4.3 Concrete control recipe

Startup (once, and after resume / AC-DC change):
1. Detect vendor, HWP/CPPC capability, hybrid core types, per-core capabilities (0x771 / 0xC00102B0). Build per-core `{lowest, lowest_nonlinear/most_efficient, nominal/guaranteed, highest, khz_per_unit}`.
2. Duplicate and activate a "Phawx ON" scheme. Write AC and DC: PERFEPP = PERFEPP1 = 0, PERFBOOSTMODE = 2 (Aggressive), PERFAUTONOMOUS = 1, PERFAUTONOMOUSWINDOW = 0, PROCTHROTTLEMIN = PROCTHROTTLEMIN1 = 0–5, PROCTHROTTLEMAX = PROCTHROTTLEMAX1 = 100, PROCFREQMAX = PROCFREQMAX1 = 0. Optionally set overlay = Best performance. Then PowerSetActiveScheme(NULL, &phawx).
3. Intel only: EPB 0x1B0 = 0 on every CPU. (Needed where EPP is unsupported; harmless otherwise.)

Per AutoTDP tick (event-driven off frame times, e.g. every frame batch / ≥ 8 ms):
```c
/* Intel */
v = rdmsr_on(cpu, 0x774);
v &= ~((0xFFull<<8) | (0xFFull<<16) | (0xFFull<<24) | (1ull<<42) | (0x1Full<<59));
v |= ((u64)clamp(max_perf[cpu], cap.lowest, cap.highest) << 8) /* desired=0, epp=0 */;
v = (v & ~0xFFull) | clamp(min_perf[cpu], cap.lowest, max_perf[cpu]);
wrmsr_on(cpu, 0x774, v);

/* AMD */
v = rdmsr_on(cpu, 0xC00102B3);
v &= ~0xFFFFFFFFull;
v |= (u64)clamp(max_perf[cpu], c1.lowest, c1.highest)            /* 7:0 max   */
   | (u64)clamp(min_perf[cpu], c1.lowest, max_perf[cpu]) << 8     /* 15:8 min  */
   | (0ull << 16) | (0ull << 24);                                  /* desired 0, EPP 0 */
wrmsr_on(cpu, 0xC00102B3, v);
```
- Skip the write when the value is unchanged (cache last-written per CPU). Saves IPIs and power.
- Guard: every tick, read one CPU per class. If `(v >> 8 & 0xFF) != expected_max` (Intel) or `(v & 0xFF) != expected_max` (AMD), or EPP ≠ 0, re-write all.
- Asymmetric control (the user's spec: fast up, slow down). On an FPS drop below target, jump max_perf straight to `highest` (or +N steps) immediately. Decay downward in small steps (1 HWP unit ≈ 78–100 MHz Intel; ~25 MHz-equivalent per AMD unit) only after M stable frames. Floor at `most_efficient`/`lowest_nonlinear`. Going lower saves little power and hurts frame times.
- Hybrid Intel: control P-core and E-core ceilings separately (two targets). Lowering E-core max rarely helps games. Cap P-cores first, and keep E-cores near their efficient point.
- Min perf: keep at `lowest` (or `most_efficient`). Raising min is a way to "pin" clocks when the user asks for a fixed CPU clock.
- Manual fixed CPU clock (static mode): min = max = mhz_to_perf(f). Windows may still lower it under thermal/PL limits (HW overrides requests).
- On exit or disable: restore the original 0x774/CPPC_REQ values you saved, and the user's scheme; then PowerSetActiveScheme(NULL, original) so Windows rewrites everything consistently.

### 4.4 Pitfalls checklist
- Hybrid P-core perf units are not 100 MHz (see 1.4). AMD perf units are not MHz and highest_perf may be a ranking (3.3).
- Intel and AMD min/max byte order are swapped.
- Write per logical CPU. With SMT, both siblings' requests combine (HW takes the max of the requests of active threads on a core), so cap both siblings.
- Processor groups (>64 LPs): use SetThreadGroupAffinity / GetLogicalProcessorInformationEx.
- Don't touch IA32_PM_ENABLE / CPPC_ENABLE. They're write-once, and enabling HWP when Windows booted without it confuses the PPM driver.
- Zen 2 and older: no CPPC MSRs → PROCFREQMAX path only.
- Power settings written without re-applying the scheme do nothing until the next apply.
- Overlays override scheme values; OEM PPKGs redefine overlays.
- Use PawnIO (signed, maintained, works with Memory Integrity) for any MSR access.
- Thermal / PL / PROCHOT / platform (_PPC) limits override max_perf. Read MSR 0x64F (Intel) or compare APERF/MPERF to detect it, so AutoTDP doesn't keep raising a ceiling that has no effect.

---

## 5. Sources
- Linux `arch/x86/include/asm/msr-index.h` (HWP, CPPC, EPB definitions): https://raw.githubusercontent.com/torvalds/linux/master/arch/x86/include/asm/msr-index.h
- Linux `drivers/cpufreq/intel_pstate.c` (hybrid scaling factors 78741/80000/86957, hwp_get_cpu_scaling, CPPC fallback): https://raw.githubusercontent.com/torvalds/linux/master/drivers/cpufreq/intel_pstate.c
- Linux `drivers/cpufreq/amd-pstate.c` (EPP values 0x00/0x80/0xBF/0xFF, freq_to_perf/perf_to_freq): https://raw.githubusercontent.com/torvalds/linux/master/drivers/cpufreq/amd-pstate.c
- Linux docs: Documentation/admin-guide/pm/intel_pstate.rst, amd-pstate.rst
- Intel SDM Vol. 3B §15.4 "Hardware-Controlled Performance States (HWP)", CPUID leaf 06H (Vol. 2A)
- AMD PPR (Family 19h/1Ah) MSRC001_02B0–02B5, MSRC001_0064–006B
- LibreHardwareMonitor Amd17Cpu.cs (FID decoding Zen1–4 vs Zen5): https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/blob/master/LibreHardwareMonitorLib/Hardware/Cpu/Amd17Cpu.cs
- Power setting GUID list: https://github.com/mkht/DSCR_PowerPlan/blob/master/DSCResources/cPowerPlanSetting/DATA/GUID_LIST_SETTING ; https://bitsum.com/known-windows-power-guids/
- Microsoft: Processor power management options / PerfEnergyPreference: https://learn.microsoft.com/en-us/windows-hardware/customize/power-settings/configure-processor-power-management-options
- Overlay APIs: https://github.com/AaronKelley/PowerMode ; https://github.com/simosako/powermode-tray
- ThrottleStop Speed Shift EPP behavior: https://throttlestopsoftware.com/guides/speed-shift-epp/
