# Hybrid (heterogeneous) CPU cores and real-time parking — implementer reference

Scope: detecting P/E/LP-E cores on Intel and AMD, the Windows scheduler and parking knobs, steering individual processes, and a design for managing cores in real time inside the Phawx ON AutoTDP loop. Target: C with mingw-w64, Win10 1709+ / Win11, running elevated.

Confidence markers: **[V]** verified against a primary or source-code reference, **[C]** common knowledge that matches vendor docs, **[U]** uncertain, so check it on real hardware at runtime.

---

## 0. TL;DR for the implementer

1. **The EfficiencyClass direction is easy to get backwards.** A higher `EfficiencyClass` means a *more performant* core. On Intel hybrid, P-cores = class 1 and E-cores/LP-E = class 0. Every power setting with a `...1` suffix (`CPMAXCORES1`, `PROCFREQMAX1`, `PERFEPP1`, `PROCTHROTTLEMAX1`) applies to **class 1 = P-cores**. The base setting applies to class 0 = E-cores once the system is heterogeneous. So **parking E-cores means `CPMAXCORES` (base) = 0, not `CPMAXCORES1`**. Setting `CPMAXCORES1=0` parks the P-cores. **[C]** (MS doc wording is "for Processor Power Efficiency Class 1". The community has confirmed on ADL/RPL that `PROCFREQMAX1` caps the P-cores. On new SKUs, check at runtime by reading `Parked` bits after a change.)
2. Parking is a **soft scheduler hint**, not taking a core offline. Parked cores still take interrupts and DPCs, and still run threads whose hard affinity lists only parked CPUs. Windows can unpark them on a latency hint (touch or mouse input, app launch).
3. Parking and power-plan changes are **slow**. They go through a registry write, the power service and a kernel PPM policy push. Treat them as a 0.5–2 s tier and never drive them at 10–20 Hz. The fast loop moves **frequency** (EPP / HWP max / CPPC max / PROCFREQMAX via MSR) and **CPU sets**, which take effect on the next scheduling decision.
4. Detection order: `GetSystemCpuSetInformation` (EfficiencyClass, SchedulingClass, Parked, LLC index) → CPUID 0x1A (Intel) / 0x80000026 (AMD), run pinned to each logical CPU → cache topology (no L3 = Intel LP-E) → AMD CPPC highest_perf ranking.
5. On AMD Phoenix2 / Strix / Krackan, Windows may report **all cores as EfficiencyClass 0** because AMD relies on CPPC "preferred core" ranking. **[U]** Always fall back to CPUID 0x80000026 and L3 grouping.

---

## 1. Detection

### 1.1 `GetLogicalProcessorInformationEx(RelationProcessorCore)` **[V]**

```c
// winnt.h
typedef struct _PROCESSOR_RELATIONSHIP {
    BYTE  Flags;            // LTP_PC_SMT (0x1) => core has >1 logical processor
    BYTE  EfficiencyClass;  // Win10+: higher = more performance, less efficiency; 0 on homogeneous
    BYTE  Reserved[20];
    WORD  GroupCount;       // 1 for RelationProcessorCore
    GROUP_AFFINITY GroupMask[ANYSIZE_ARRAY];
} PROCESSOR_RELATIONSHIP;

typedef struct _SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX {
    LOGICAL_PROCESSOR_RELATIONSHIP Relationship; // RelationProcessorCore=0, RelationNumaNode=1, RelationCache=2, RelationProcessorPackage=3, RelationGroup=4, RelationProcessorDie=5, RelationNumaNodeEx=6, RelationProcessorModule=7, RelationAll=0xffff
    DWORD Size;                                  // advance by Size, never sizeof
    union { PROCESSOR_RELATIONSHIP Processor; NUMA_NODE_RELATIONSHIP NumaNode;
            CACHE_RELATIONSHIP Cache; GROUP_RELATIONSHIP Group; };
} SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX;

BOOL GetLogicalProcessorInformationEx(LOGICAL_PROCESSOR_RELATIONSHIP rel,
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX buf, PDWORD len); // call once with NULL for size (ERROR_INSUFFICIENT_BUFFER)
```

- One record per physical core. `GroupMask[0].Mask` holds its logical CPUs (2 bits when SMT).
- `RelationCache` gives `CACHE_RELATIONSHIP{BYTE Level; BYTE Associativity; WORD LineSize; DWORD CacheSize; PROCESSOR_CACHE_TYPE Type; BYTE Reserved[18]; WORD GroupCount; GROUP_AFFINITY GroupMask...}`. The Level 3 records give the CCX / LLC domains. **Intel LP-E cores (Meteor Lake SoC tile, Arrow Lake-H, Lunar Lake E-cluster, Panther Lake LP-E) are in no L3 record.** Use that to tell LP-E from normal E-cores. **[C]**
- `RelationProcessorModule` (Win11) groups the cores that share an L2. An Intel E-core module is 4 cores. **[C]**

### 1.2 `GetSystemCpuSetInformation` **[V]**

```c
typedef enum _CPU_SET_INFORMATION_TYPE { CpuSetInformation } CPU_SET_INFORMATION_TYPE;
typedef struct _SYSTEM_CPU_SET_INFORMATION {
    DWORD Size;                       // advance by Size
    CPU_SET_INFORMATION_TYPE Type;
    union { struct {
        DWORD Id;                     // CPU set ID (opaque; observed 0x100 + index, do not assume)
        WORD  Group;
        BYTE  LogicalProcessorIndex;  // within group
        BYTE  CoreIndex;              // == LP index of first SMT sibling
        BYTE  LastLevelCacheIndex;    // LLC domain id => CCX on AMD
        BYTE  NumaNodeIndex;
        BYTE  EfficiencyClass;
        union { BYTE AllFlags; struct {
            BYTE Parked : 1;          // live core-parking state
            BYTE Allocated : 1;       // reserved by system (e.g. Game Mode / exclusive)
            BYTE AllocatedToTargetProcess : 1;
            BYTE RealTime : 1;
            BYTE ReservedFlags : 4; }; };
        union { DWORD Reserved; BYTE SchedulingClass; }; // Win11 SDK names it; higher = more preferred
        DWORD64 AllocationTag;
    } CpuSet; };
} SYSTEM_CPU_SET_INFORMATION, *PSYSTEM_CPU_SET_INFORMATION;

BOOL GetSystemCpuSetInformation(PSYSTEM_CPU_SET_INFORMATION info, ULONG len, PULONG retLen,
                                HANDLE process /*NULL or target for AllocatedToTargetProcess*/, ULONG flags /*0*/);
```

- It is cheap (one syscall, a few µs), so it can be polled at loop rate to watch `Parked`.
- `SchedulingClass` has finer granularity than EfficiencyClass. On Meteor Lake it has 4 distinct values over 22 LPs, and the **2 LP-E cores carry the lowest value**. EfficiencyClass does not tell LP-E from E there. **[V: Intel community thread]**
- mingw-w64 declares this in `processthreadsapi.h`/`winnt.h` with `_WIN32_WINNT>=0x0A00`. Older headers lack `SchedulingClass`, so read byte offset `Reserved`.

### 1.3 CPUID (run pinned to each LP via `SetThreadGroupAffinity`, then `__cpuidex`)

| Leaf | Field | Meaning | Src |
|---|---|---|---|
| 0x07.0 EDX bit 15 | Hybrid | Part has >1 core type (Intel) | [V] Linux `X86_FEATURE_HYBRID_CPU` (18*32+15) |
| 0x1A.0 EAX[31:24] | Core type | 0x20 = Intel Atom (E / LP-E), 0x40 = Intel Core (P) | [V] Linux `intel_type` |
| 0x1A.0 EAX[23:0] | Native model ID | Microarchitecture sub-id. LP-E and E are both 0x20 type, and the model ID is not a reliable way to separate them | [V]/[U] |
| 0x06 EAX bit 19 | HFI | Hardware Feedback Interface (kernel-only table) | [V] |
| 0x06 EAX bit 23 | ITD | Thread Director classes (kernel-only) | [C] |
| 0x80000021 EAX bit 22 | AMD Workload Classification | AMD equivalent of the ITD classes | [V] Linux scattered.c |
| 0x80000026.0 EAX bit 30 | AMD heterogeneous core topology | If set, EBX carries the core type | [V] Linux scattered.c |
| 0x80000026.0 EBX[31:28] | AMD core type | 0 = performance (Zen5), 1 = efficiency (Zen5c/Zen4c), 2 = low-power | [V] Linux `enum amd_cpu_type` |
| 0x80000026.0 EBX[27:24] | AMD native model id | | [V] |
| 0x80000026.0 EBX[23:16] | AMD power-efficiency ranking | Higher = more efficient **[U semantics]** | [V] field |
| 0x80000026.0 EBX[15:0] | num logical processors at level | | [V] |

- Phoenix2 (Zen4c) predates leaf 0x80000026 core type. **[U]** For that chip, use CPPC highest_perf: `MSR 0xC00102B0` bits [31:24] per thread. It needs the MSR driver. The Zen4c/Zen5c cores report a lower highest_perf. **[V field / C behaviour]**
- AMD MSRs involved (from Linux `msr-index.h`) **[V]**: `CPPC_CAP1 0xC00102B0` (lowest[7:0], lownonlin[15:8], nominal[23:16], highest[31:24]), `CPPC_ENABLE 0xC00102B1`, `CPPC_REQ 0xC00102B3` (max[7:0], min[15:8], des[23:16], EPP[31:24]; per-thread), `CPPC_REQ2 0xC00102B5` (floor[7:0]), `WORKLOAD_CLASS_CONFIG 0xC0000500`, `WORKLOAD_CLASS_ID 0xC0000501`, `WORKLOAD_HRST 0xC0000502`.
- Intel per-LP HWP **[C]**: `IA32_HWP_REQUEST 0x774` (min[7:0], max[15:8], desired[23:16], EPP[31:24], window[41:32], pkg-ctl[42]). This one is **per logical CPU**, so P-cores and E-cores can get different max/EPP. `IA32_HWP_REQUEST_PKG 0x772`. `IA32_HWP_CAPABILITIES 0x771` (highest[7:0], guaranteed[15:8], most-eff[23:16], lowest[31:24]). HFI: `IA32_HW_FEEDBACK_PTR 0x17D0`, `IA32_HW_FEEDBACK_CONFIG 0x17D1`. Only the OS uses these; do not touch them.

### 1.4 Platform matrix

| Platform | Cores | Windows EfficiencyClass | LLC | Notes |
|---|---|---|---|---|
| Intel ADL/RPL (12–14th gen) | P (Golden/Raptor Cove, SMT) + E (Gracemont, 4-core modules) | P=1, E=0 [C] | shared ring L3 | ITD on Win11 only |
| Intel Meteor Lake / Arrow Lake-H | P + E (compute tile) + 2 LP-E (SoC tile) | P=1, E=0, LP-E=0 [V] | LP-E have **no L3** | use SchedulingClass (lowest) or the no-L3 test for LP-E |
| Intel Lunar Lake | 4 P (Lion Cove, no SMT) + 4 LP-E (Skymont, "low power island") | P=1, E=0 [C] | P share 12 MB L3; E-cluster has 4 MB L2, no L3 | E-cores are LP-E. Keep games on P. Background can run on E so the P-cores sleep |
| Intel Arrow Lake-S | P (no SMT) + E | P=1, E=0 | shared L3 | |
| Intel Panther Lake | P + E + LP-E | [U] expected P=1, E/LP-E=0 | LP-E no L3 [U] | |
| AMD Phoenix2 (Z1, 8540U/8440U) | 2 Zen4 + 4 Zen4c, single CCX, 16 MB L3 | **likely all 0** [U] | one LLC | differ in max freq only; same ISA/IPC |
| AMD Strix Point (HX 370 etc.) | 4 Zen5 (CCX0, 16 MB L3) + 8 Zen5c (CCX1, 8 MB L3) | [U]. Check at runtime | **two LLC indices** | cross-CCX traffic is costly. Keep the game within one CCX |
| AMD Z2 Extreme (Strix-derived) | 3 Zen5 + 5 Zen5c | [U] | [U] likely 2 CCX | |
| AMD Krackan Point | 4 Zen5 + 4 Zen5c | [U] | [U] 2 CCX, 8 MB L3 each | |
| AMD Strix Halo / dual-CCD desktop | homogeneous Zen5, 2 CCX/CCD | 0 | 2 LLC | treat as a "cluster" choice, not a P/E choice. X3D: the cache CCD is chosen by the amd3dvcache driver + Game Bar [C] |

### 1.5 Detection algorithm (C)

```c
typedef enum { CT_P = 0, CT_E = 1, CT_LPE = 2 } core_type_t;
typedef struct {
    DWORD cpuset_id; WORD group; BYTE lp, core, llc, eff, sched, parked;
    core_type_t type; BYTE smt_sibling; BYTE cppc_highest; // 0 if unknown
} lp_info_t;

// 1) enumerate CPU sets -> lp_info_t[] (eff, sched, llc, parked)
// 2) nEff = distinct(eff). If nEff > 1: type = (eff == max_eff) ? CT_P : CT_E
// 3) else if CPUID.80000021? / 80000026.EAX[30]: pin to each LP, type from EBX[31:28] (0 P,1 E,2 LPE)
// 4) else if Intel CPUID.07.EDX[15]: pin to each LP, CPUID.1A EAX[31:24] 0x40 -> P, 0x20 -> E
// 5) else if AMD and MSR driver available: read CPPC_CAP1[31:24]; cores < (max_highest - 8) -> E  [threshold U]
// 6) Intel LP-E refinement: an E-core whose LPs are in no RelationCache Level-3 mask -> CT_LPE
//    (alt: SchedulingClass == global min and count <= 4)
// 7) Cluster map: group LPs by LastLevelCacheIndex -> clusters[]; mark "P-cluster" = cluster containing max-ranked core
```

Pin for CPUID: `SetThreadGroupAffinity(GetCurrentThread(), &ga, &old)`, then `Sleep(0)` (or re-check `GetCurrentProcessorNumberEx`), then `__cpuidex(r, leaf, sub)`, then restore. This runs once at startup (~1 ms total).

---

## 2. Windows scheduler and parking power settings

Subgroup `GUID_PROCESSOR_SETTINGS_SUBGROUP` = `54533251-82be-4824-96c1-47b60b740d00` (alias `SUB_PROCESSOR`). All setting GUIDs below are **[V]**. They are cross-checked against microsoft/BaselineManagement `PowerOptions.ps1` and mkht/DSCR_PowerPlan `GUID_LIST_SETTING`. Many settings are hidden (`ATTRIBUTE_HIDE`). They can still be written, and powercfg can show them with `powercfg -attributes SUB_PROCESSOR <alias> -ATTRIB_HIDE`.

"Class 1" = EfficiencyClass 1 (Intel P-cores). A base setting without a class-1 twin applies to all cores. A base setting that has a twin applies to class 0 on hybrid systems. **[C]**

### 2.1 Heterogeneous scheduling

| Alias | GUID | Values | Notes |
|---|---|---|---|
| HETEROPOLICY | `7f2f5cfa-f10c-4823-b5e1-e93ae85f46b5` | index 0–4 | Selects which of 5 "possible value" slots is live for HETERO*THRESHOLD/TIME. It does not choose a policy by name. OEMs provision the slots. [V/C] |
| SCHEDPOLICY | `93b8b6dc-0698-4d1c-9ee4-0644e900c85d` | 0 All processors, 1 Performant only, 2 Prefer performant, 3 Efficient only, 4 Prefer efficient, 5 Automatic | Covers long-running threads. Win11 default is Automatic (5): the OS decides by QoS / Thread Director. [V values] |
| SHORTSCHEDPOLICY | `bae08b81-2d5e-4688-ad6a-13243356654b` | same 0–5 | Covers short-running threads. |
| HETEROINCREASETHRESHOLD | `b000397d-9b0b-483d-98c9-692a6060cfbf` | packed BINARY, 4 bytes = thresholds (%) to unpark the 1st..4th+ class-1 core, relative to class-0 perf | Default ≈ unpark a P-core when E-cores are at 90% of base. Set with `powercfg /SetPossibleValue SUB_PROCESSOR HETEROINCREASETHRESHOLD <idx> BINARY 0x14144650`. [V MS doc via search] |
| HETERODECREASETHRESHOLD | `f8861c27-95e7-475c-865b-13c0cb3f9d6b` | packed, same layout | Default ≈ park at 50%. |
| HETEROINCREASETIME | `4009efa7-e72d-4cba-9edf-91084ea8cbc3` | count of perf-check intervals | [C] |
| HETERODECREASETIME | `7f2492b6-60b1-45e5-ae55-773f8cd5caec` | count of perf-check intervals | [C] |
| HETEROCLASS1INITIALPERF | `1facfc65-a930-4bc5-9f38-504ec097bbc0` | % | Initial perf of a class-1 core when it unparks. |
| HETEROCLASS0FLOORPERF | `fddc842b-8364-4edc-94cf-c17f60de1c80` | % | Perf floor for class 0 while any class-1 core is unparked. |

What these mean in practice: with SCHEDPOLICY=Automatic on Win11, Windows keeps P-cores parked until E-core utilisation crosses the HETERO thresholds. That behaviour belongs to the "Balanced" / battery overlays and is why games stutter when they start on E-cores. For gaming, set **SCHEDPOLICY=2 (prefer performant) and SHORTSCHEDPOLICY=2**, or use CPU sets (section 3). For "background only" behaviour, use 4.

### 2.2 Core parking

| Alias | GUID | Units / values | Default (Balanced, typ.) |
|---|---|---|---|
| CPMINCORES | `0cc5b647-c1df-4637-891a-dec35c318583` | % of cores that must stay unparked (class 0 on hybrid) | varies, often 0–10 on laptop AC [U] |
| CPMINCORES1 | `0cc5b647-c1df-4637-891a-dec35c318584` | % for class 1 (P) | |
| CPMAXCORES | `ea062031-0e34-4ff1-9b6d-eb1059334028` | % max unparked (class 0 on hybrid) | 100 |
| CPMAXCORES1 | `ea062031-0e34-4ff1-9b6d-eb1059334029` | % max unparked, class 1 | 100 |
| CPCONCURRENCY | `2430ab6f-a520-44a2-9601-f7f23b5134b1` | % concurrency threshold, used to work out how many cores to unpark | 95 [C] |
| CPHEADROOM | `f735a673-2066-4f80-a0c5-ddee0cf1bf5d` | % headroom threshold | 50 [C] |
| CPDISTRIBUTION | `4bdaf4e9-d103-46d7-a5f0-6280121616ef` | % utilisation used for distribution concurrency | 90 [C] |
| DISTRIBUTEUTIL | `e0007330-f589-42ed-a401-5ddb10e785d3` | 0/1: move the utility of parked cores onto unparked ones for perf-state selection | [C] |
| CPOVERUTIL | `943c8cb6-6f93-4227-ad87-e9a3feec08d1` | % busy above which a core counts as overutilised | 60 [C] |
| CPINCREASEPOL | `c7be0679-2817-4d69-9d02-519a537ed0c6` | 0 Ideal, 1 Single, 2 Rocket (unpark all) | 0 |
| CPDECREASEPOL | `71021b41-c749-4d21-be74-a00f335d582b` | 0 Ideal, 1 Single, 2 Rocket | 0 |
| CPINCREASETIME | `2ddd5a84-5a71-437e-912a-db0b8c788732` | perf-check intervals before unparking [C] | 1 |
| CPDECREASETIME | `dfd10d17-d5eb-45dd-877a-9a34ddd15c82` | intervals before parking | 1–3 |
| CPINCREASE / CPDECREASE | `df142941-20f3-4edf-9a4a-9c83d3d717d1` / `68dd2f27-a4ce-4e11-8487-3794e4135dfa` | legacy increase/decrease thresholds (%) | |
| CPPERF / CPPERF1 | `447235c7-6a8d-4cc0-8e24-9eaf70b96e2b` / `...2c` | perf state of parked cores: 0 no pref, 1 deepest perf state, 2 lightest | |
| LATENCYHINTUNPARK / 1 | `616cdaa5-695e-4545-97ad-97dc2d1bdd88` / `...89` | % of cores unparked while a latency-sensitivity hint is active (input, app launch, touch) | 100 AC [C] |
| LATENCYHINTPERF / 1 | `619b7505-003b-4e82-b7a6-4dd29c300971` / `...72` | % perf while the hint is active | 99–100 [C] |

**For AutoTDP, turn LATENCYHINTUNPARK/PERF down** (for example 0 / 0) while AutoTDP runs. Otherwise every touch on the overlay or every stick flick ramps all cores to max for about 1 s and blows the power budget. Restore the old values on exit.

### 2.3 Related per-class perf settings (context: EPP/clock doc)

| Alias | GUID | Units |
|---|---|---|
| PERFEPP / PERFEPP1 | `36687f9e-e3a5-4dbf-b1dc-15eb381c6863` / `...64` | 0–100 %, maps to HWP/CPPC EPP ≈ v*255/100 |
| PROCFREQMAX / PROCFREQMAX1 | `75b0ae3f-bce0-45a7-8c89-c9611c25e100` / `...101` | MHz, 0 = no cap |
| PROCTHROTTLEMAX / 1 | `bc5038f7-23e0-4960-96da-33abaf5935ec` / `...ed` | % |
| PROCTHROTTLEMIN / 1 | `893dee8e-2bef-41e0-89c6-b55d0929964c` / `...4d` | % |
| PERFBOOSTMODE | `be337238-0d82-4146-a960-4f3749d470c7` | 0 off, 1 enabled, 2 aggressive, 3 efficient enabled, 4 efficient aggressive, 5 aggr. at guaranteed, 6 eff. aggr. at guaranteed |
| PERFAUTONOMOUS | `8baa4a8a-14c6-4451-8e8b-14bdbd197537` | 0/1 HWP autonomous |
| PERFAUTONOMOUSWINDOW | `cfeda3d0-7697-4566-a922-a9086cd49dfa` | µs |
| PERFCHECK | `4d2b0152-7d5c-498b-88e2-34345392a2c5` | ms, PPM evaluation interval (typ. 15–30) |
| PERFINC/DECTHRESHOLD(1), PERFINC/DECTIME(1), PERFINC/DECPOL(1) | `06cadf0e-...eb35d`/`e`, `12a0ab44-...60a6`/`7`, `984cf492-...f5aa`/`ab`, `d8edeb9b-...93c8`/`9`, `465e1f50-...c418`/`9`, `40fbefc7-...bac6`/`7` | legacy (non-autonomous) governor |
| IDLEDISABLE | `5d76a2ca-e8c0-402f-a133-2158492d58ad` | 0/1. Never set it |

### 2.4 Win10 vs Win11 **[C]**

- EfficiencyClass, CPU sets and the `Parked` flag exist since Win10 1709 (added for Lakefield, 20H1). Class-1 settings exist in Win10 too.
- Thread Director (HFI/ITD) feedback and QoS-aware SCHEDPOLICY=Automatic work **only on Win11**. On Win10, ADL+ scheduling is essentially EfficiencyClass + parking, and the class-1 settings still apply.
- EcoQoS (E-core steering + efficient frequency) is **Win11 21H2+**. On Win10 1709+, `PROCESS_POWER_THROTTLING_EXECUTION_SPEED` only lowers frequency ("LowQoS").
- `SchedulingClass` shows up in the Win11 SDK. It may read 0 on Win10. [U]
- Win11 24H2 made AMD preferred-core scheduling and parking more aggressive on dual-CCD parts. [C]

### 2.5 Writing settings (C)

```c
#include <powrprof.h>   // -lpowrprof
DWORD PowerGetActiveScheme(HKEY root /*NULL*/, GUID **active);            // LocalFree(*active)
DWORD PowerReadACValueIndex(HKEY, const GUID *scheme, const GUID *sub, const GUID *set, LPDWORD v);
DWORD PowerWriteACValueIndex(HKEY, const GUID *scheme, const GUID *sub, const GUID *set, DWORD v);
DWORD PowerWriteDCValueIndex(HKEY, const GUID *scheme, const GUID *sub, const GUID *set, DWORD v);
DWORD PowerSetActiveScheme(HKEY, const GUID *scheme);                     // re-apply => takes effect
DWORD PowerWriteSettingAttributes(const GUID *sub, const GUID *set, DWORD attr); // 0 = unhide
DWORD PowerDuplicateScheme(HKEY, const GUID *src, GUID **dst);
// Optional overlay (undocumented, exported by powrprof on Win10 1709+) [U]:
// DWORD PowerSetActiveOverlayScheme(GUID overlay); DWORD PowerGetActualOverlayScheme(GUID*);
```

Rules:
- Write AC and DC values (a handheld flips on plug/unplug). Then call `PowerSetActiveScheme(NULL, active)` once per batch.
- Work on a **duplicated "Phawx" scheme** so the user's plan is never mutated. Activate it while Phawx runs and restore the previous scheme on exit or crash. Store the previous GUID in the app config so it can be recovered on the next start.
- Each write is a registry write under `HKLM\SYSTEM\CurrentControlSet\Control\Power\User\PowerSchemes\...` plus an RPC to the Power service. `PowerSetActiveScheme` also broadcasts `WM_POWERBROADCAST/PBT_POWERSETTINGCHANGE` to every registered listener.
- **Latency [U, measure with QPC]:** API return is roughly 1–10 ms per batch. The kernel applies the new policy at the next PPM check, about one PERFCHECK interval (15–30 ms). Parking responds after CPINCREASETIME/CPDECREASETIME intervals. Budget **≈50–100 ms end-to-end** for parking to change.
- **At 10–20 Hz** this means registry churn, notification storms to other apps (Game Bar, OEM services, Armoury Crate / Legion Space listen and may fight back), and power-service CPU time. **Rate-limit plan writes to ≤1 per 500 ms, with a 2 s dwell for parking.**

---

## 3. Per-process and per-thread steering

### 3.1 CPU sets (soft, group-agnostic, instant) **[V]**

```c
BOOL SetProcessDefaultCpuSets(HANDLE hProc /*PROCESS_SET_LIMITED_INFORMATION*/, const ULONG *ids, ULONG count); // count=0 clears
BOOL GetProcessDefaultCpuSets(HANDLE, PULONG ids, ULONG cap, PULONG req);
BOOL SetThreadSelectedCpuSets(HANDLE hThr /*THREAD_SET_LIMITED_INFORMATION*/, const ULONG *ids, ULONG count);
BOOL SetProcessDefaultCpuSetMasks(HANDLE, PGROUP_AFFINITY, USHORT count);   // Win11
BOOL SetThreadSelectedCpuSetMasks(HANDLE, PGROUP_AFFINITY, USHORT count);   // Win11
```

- Soft: threads are scheduled only on the listed sets **unless** the sets are all parked or otherwise unavailable, in which case the scheduler can go outside them. Thread-selected sets override process defaults.
- A thread's hard affinity (SetThreadAffinityMask) takes precedence. If a game hard-pins its threads, CPU sets have limited effect. [C]
- The effect applies at the thread's next dispatch, well under 1 ms. Cost is one syscall. **This is the tool for real-time steering.**
- Needs `PROCESS_SET_LIMITED_INFORMATION`. Elevated processes can set it on most games. Anti-cheat protected processes (PPL, EAC/BattlEye) may refuse `OpenProcess`, so treat failure as a no-op.
- It does not inherit to child processes. Re-apply when a new process appears (poll the foreground PID).

### 3.2 Hard affinity **[V]**

`SetProcessAffinityMask(HANDLE, DWORD_PTR)` / `SetThreadGroupAffinity`. Children inherit it. It only covers a single group (≤64 LP). It can starve a game that assumes all cores are available (for example a thread count fixed to `GetActiveProcessorCount`). Use it only as an opt-in "hard" mode. Process Lasso offers both and defaults to CPU sets as the "soft" option. [C]

### 3.3 Power throttling / EcoQoS **[V]**

```c
// processthreadsapi.h (define if mingw lacks)
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION     1
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED     0x1
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4   // Win11
typedef struct { ULONG Version, ControlMask, StateMask; } PROCESS_POWER_THROTTLING_STATE;
// PROCESS_INFORMATION_CLASS: ProcessMemoryPriority=0, ProcessMemoryExhaustionInfo=1, ProcessAppMemoryInfo=2,
//   ProcessInPrivateInfo=3, ProcessPowerThrottling=4, ProcessReservedValue1=5, ProcessTelemetryCoverageInfo=6,
//   ProcessProtectionLevelInfo=7, ProcessLeapSecondInfo=8, ProcessMachineTypeInfo=9
BOOL SetProcessInformation(HANDLE /*PROCESS_SET_INFORMATION*/, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);

#define THREAD_POWER_THROTTLING_CURRENT_VERSION 1
#define THREAD_POWER_THROTTLING_EXECUTION_SPEED 0x1
typedef struct { ULONG Version, ControlMask, StateMask; } THREAD_POWER_THROTTLING_STATE;
// THREAD_INFORMATION_CLASS: ThreadMemoryPriority=0, ThreadAbsoluteCpuPriority=1, ThreadDynamicCodePolicy=2, ThreadPowerThrottling=3
BOOL SetThreadInformation(HANDLE, THREAD_INFORMATION_CLASS, LPVOID, DWORD);
```

| ControlMask | StateMask | Effect |
|---|---|---|
| EXEC_SPEED | EXEC_SPEED | **EcoQoS on.** Win11: E-core preference + efficient frequency. Win10: LowQoS frequency only |
| EXEC_SPEED | 0 | **Opt out.** The process is never throttled, even in the background. Use this on the game |
| 0 | 0 | The system decides (default heuristics) |

Task Manager's "Efficiency mode" = EcoQoS + `IDLE_PRIORITY_CLASS`.

### 3.4 Windows QoS levels (Win11) **[C]**

The scheduler derives them from process state: **High** (foreground window with focus, or audio), **Medium** (visible, not focused), **Low** (minimised / not visible), **Utility** (background services), **Eco** (EcoQoS opt-in), **Media** and **Deadline** (multimedia/MMCSS). SCHEDPOLICY=Automatic maps High → P-cores and Low/Eco/Utility → E-cores. Implication: a borderless game whose focus is stolen by our overlay can drop to Medium. **Create the overlay window `WS_EX_NOACTIVATE`** so the game keeps focus and High QoS. When gamepad input requires focus, keep the overlay's time in focus short.

### 3.5 Priority

`SetPriorityClass(hGame, ABOVE_NORMAL_PRIORITY_CLASS or HIGH_PRIORITY_CLASS)` is optional. Never use REALTIME. Leave background processes alone except for EcoQoS.

### 3.6 How others do it **[C]**

- **Process Lasso:** per-rule CPU sets (soft) or affinity (hard), an "efficiency mode" toggle (EcoQoS), and ParkControl, which edits CPMINCORES/CPMAXCORES(1) in the plan.
- **Intel APO (Application Optimization):** per-game profiles driven through the Intel DTT framework. It effectively restricts or steers game threads across P-cores and one E-core per module. It is proprietary, whitelisted, and **not reproducible** by user-mode APIs beyond CPU sets.
- **AMD 3D V-Cache Optimizer (`amd3dvcache` driver + Game Bar):** when a game is detected, it parks the non-preferred CCD via PPM provisioning. Registry `HKLM\SYSTEM\CurrentControlSet\Services\amd3dvcache\Preferences` `DefaultType` 0 = frequency, 1 = cache. [U key names]
- **Games:** some engines (UE5, id Tech) query EfficiencyClass through `GetLogicalProcessorInformationEx` and size their job systems to the P-core count. Intel's game-dev guide recommends CPU sets and EfficiencyClass-aware thread pools.

---

## 4. Real-time P/E design for the Phawx ON AutoTDP loop

### 4.1 Principles

1. **Frequency is the fast actuator, and core topology is the slow one.** An idle core in C6 / CC6 costs almost nothing, so parking an already-idle core saves little. Parking pays off only when it stops the scheduler from spreading a lightly threaded load across more cores or raising a shared rail. Examples: Intel client parts share one P-core voltage rail, E-cores share the ring rail, and AMD Strix uses one VDDCR_CPU.
2. **Keep game threads on the fastest cluster that fits them, and push everything else to the efficient cluster.** Do this with CPU sets (instant). Parking changes only on sustained state changes.
3. **Hysteresis and dwell everywhere.** A frame-time spike must not cause a parking flip.
4. **Never park the cores the game is using. Never let a game run on Intel LP-E.** LP-E cores have no L3 and add large latency.

### 4.2 Tiers

| Tier | Period | Actuators | Latency to effect |
|---|---|---|---|
| T0 fast | every frame-time sample (8–33 ms) aggregated at 50 ms | Per-class max frequency and EPP: Intel `IA32_HWP_REQUEST` per LP (P max vs E max), AMD `CPPC_REQ` per thread; fallback PROCFREQMAX/PROCFREQMAX1 through the plan at ≤2 Hz. GPU clock | MSR: next HWP/CPPC window (<1 ms) |
| T1 medium | 250–500 ms | CPU sets for the foreground game and heavy background processes; EcoQoS toggles | next dispatch |
| T2 slow | ≥2 s dwell, ≥1 s between writes | CPMAXCORES / CPMAXCORES1 / CPMINCORES(1), SCHEDPOLICY, LATENCYHINT* via the Phawx scheme | ~50–100 ms after write [U] |

**MSR clobber pitfall [C]:** Windows (`intelppm` / `amdppm`) rewrites `IA32_HWP_REQUEST` / `CPPC_REQ` on every policy change and on some idle exits, which can erase our per-LP values. Approach: first set the plan (PERFEPP/PROCFREQMAX) to agree with the MSR targets, then re-assert the MSRs each T0 tick only if a read-back differs (a read costs about 1–3 µs per LP through the driver).

### 4.3 Core-topology states (T2)

```
S_FULL     : all classes unparked. Game CPU set = P ∪ E (not LP-E). Used when CPU-bound and below target.
S_PFOCUS   : P unparked. Game CPU set = P only. E: CPMAXCORES=100 but background steered to E via CPU sets/EcoQoS.
S_PREDUCED : game CPU set = P; CPMAXCORES1 = ceil(100*needP/nP) (park surplus P-cores). E unparked for background.
S_EONLY    : (light/2D/old games, big GPU headroom) game CPU set = E cluster (Strix: Zen5c CCX1; Intel: E modules),
             CPMAXCORES1=0 (park P). Lowest power on Intel. On AMD shared rail, gain mostly comes from lower f.  [U gain, measure]
```

Choosing `needP`: use the count of game threads with >50% utilisation over the window. Get it from per-thread cycle time (`QueryThreadCycleTime` on the game's threads through a `Toolhelp32` snapshot every 1 s, which is cheap), or estimate it as the busy-LP count within the game's CPU set (`NtQuerySystemInformation(SystemProcessorPerformanceInformation)`, per-LP idle/kernel/user 100 ns ticks).

### 4.4 Controller pseudocode

```c
// inputs every 50 ms: fps_avg, ft_p99, target_fps, gpu_busy%, per-LP util, game_pid
// state: tier0 (fP_max, fE_max, gpu_clk), topo_state, dwell timers

void tick_50ms(void) {
    float err = target_fps - fps_avg;           // >0 => below target
    bool  cpu_bound = gpu_busy < 85 && max_util(game_cpus) > 90;

    // T0: fast clocks (asymmetric: raise fast, lower slow)
    if (err > 1.0f || ft_p99 > 1.25f * (1000.0f / target_fps)) {
        if (cpu_bound) fP_max = min(fP_max + step_up_cpu(err), fP_hw_max);
        else           gpu_clk = min(gpu_clk + step_up_gpu(err), gpu_hw_max);
        stable_ticks = 0;
    } else if (err < -2.0f && ++stable_ticks >= 10) {     // 500 ms stable with headroom
        if (!cpu_bound && fP_max > fP_floor) fP_max -= step_dn_cpu;
        else if (gpu_clk > gpu_floor)          gpu_clk -= step_dn_gpu;
        stable_ticks = 5;                                   // partial reset -> ~250 ms cadence
    }
    fE_max = min(fE_max_cfg, fP_max);           // background never outruns the game
    apply_msrs(fP_max, fE_max, epp_game /*0 in "max" mode*/);

    // T1: steering every 500 ms
    if (tick % 10 == 0) steer_processes();

    // T2: topology every 2 s with dwell
    if (tick % 40 == 0 && now - last_topo_change >= 4000) {
        topo_t want = choose_topo(cpu_bound, err, needP, fP_max);
        if (want != topo_state && (want > topo_state /*more cores: act now*/ || headroom_for(8000))) {
            apply_topology(want); last_topo_change = now; topo_state = want;
        }
    }
}
```

Emergency path: if `err > 20%` of target (a frame-rate cliff), jump straight to S_FULL plus max P frequency. Skip the dwell for that step only. Relax the dwell for the next step-down by ×2.

### 4.5 Steering (T1)

```c
static void steer_processes(void) {
    DWORD fg = foreground_pid();                     // GetForegroundWindow -> GetWindowThreadProcessId
    if (fg != game_pid) { restore_game(game_pid); game_pid = is_game(fg) ? fg : 0; }
    if (!game_pid) return;
    HANDLE h = OpenProcess(PROCESS_SET_LIMITED_INFORMATION | PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, game_pid);
    if (!h) return;                                   // protected: give up quietly
    SetProcessDefaultCpuSets(h, game_set_ids, game_set_n);   // P or P∪E or E per topo_state
    PROCESS_POWER_THROTTLING_STATE s = {1, PROCESS_POWER_THROTTLING_EXECUTION_SPEED, 0}; // opt out of EcoQoS
    SetProcessInformation(h, ProcessPowerThrottling, &s, sizeof s);
    CloseHandle(h);
    // Background: only processes using >2% CPU over last window, not system-critical, not our own.
    // SetProcessDefaultCpuSets(bg, e_ids, n) + EcoQoS on; remember and restore on game exit.
}
```

- `is_game`: fullscreen or borderless window covering the monitor, **or** a D3D/Vulkan swapchain present seen in the FPS source (PresentMon ETW), **or** a user allowlist.
- Restore everything (CPU sets count=0, throttling ControlMask=0) when the game exits and when Phawx exits. Keep a small table of modified PIDs.
- Never touch: `csrss`, `dwm`, `audiodg`, `System`, `smss`, `wininit`, `lsass`, anti-cheat services, or our own process. DWM must never go to LP-E or EcoQoS, because it paces composition.

### 4.6 Topology application (T2)

```c
static void apply_topology(topo_t t) {
    DWORD v_e_max = 100, v_p_max = 100, v_e_min = 0, v_p_min = 0;
    switch (t) {
    case S_FULL:     v_p_min = 100; break;                       // no P parking
    case S_PFOCUS:   v_p_min = 100; break;
    case S_PREDUCED: v_p_max = pct(needP + 1, nP); break;        // +1 spare for OS/DPC
    case S_EONLY:    v_p_max = 0; v_e_min = 100; break;          // Windows keeps >=1 core; verify via Parked bits
    }
    plan_write(CPMAXCORES,  v_e_max); plan_write(CPMINCORES,  v_e_min);   // class 0 (E)
    plan_write(CPMAXCORES1, v_p_max); plan_write(CPMINCORES1, v_p_min);   // class 1 (P)
    plan_write(SCHEDPOLICY,      t == S_EONLY ? 4 : 2);
    plan_write(SHORTSCHEDPOLICY, t == S_EONLY ? 4 : 2);
    plan_commit();                    // PowerSetActiveScheme(NULL,&phawx); QPC-time it
    rebuild_game_cpuset(t);           // exclude cores we expect parked from game set
    verify_parked_after(150 /*ms*/);  // GetSystemCpuSetInformation; if mismatch, fall back to CPU-set-only
}
```

On homogeneous systems (all EfficiencyClass 0), the class-1 settings do nothing. Park by cluster using CPU sets only: point the game's set at one CCX and let idle cores sleep. CPMAXCORES then parks by count, but Windows picks *which* cores. On AMD it uses the CPPC preferred-core order, which usually spares the fast cores. [C]

### 4.7 AMD cluster rules

- **Strix Point / Z2E / Krackan:** CCX0 (Zen5, bigger L3) runs the game's main and render threads. CCX1 (Zen5c) takes background work and extra job-system workers only in S_FULL. Avoid a game set that spans both CCXs while the game uses 4 or fewer heavy threads, because cross-CCX coherence goes through the fabric (roughly 100+ ns vs about 20 ns inside a CCX). [C]
- **Phoenix2:** single CCX with shared L3, so steering is only about frequency. Zen4c tops out around 3.3 GHz and Zen4 around 5.1 GHz. Once AutoTDP has capped fP_max below the Zen4c peak, the two core types are equivalent, and the "P" distinction can be ignored.
- **Dual-CCD X3D / Strix Halo:** keep the game on the LLC with more cache (X3D) or on CCD0 (Halo). Park the other CCD with CPU sets plus background steering. Do not fight the amd3dvcache driver if it is present: detect the service and back off topology control.

### 4.8 Intel rules

- **ADL/RPL/ARL-S:** games prefer P. E-cores are useful for background work and for shader compile or streaming threads. S_PFOCUS is the default gaming state.
- **MTL/ARL-H/PTL:** the LP-E cores (SoC tile) are **always excluded** from the game set. Background can go there, which lets the compute tile power-gate when the game is idle or in menus.
- **Lunar Lake:** 4P + 4 LP-E. The game goes on P. At low targets (30–40 fps), light titles can run in S_EONLY on Skymont with the P-cluster parked. That is a large package-power win because the compute cluster can gate. [C/U, measure]

### 4.9 Pitfalls checklist

- Parked ≠ offline. Interrupts, DPCs and hard-affinitised threads still wake parked cores. Check `Parked` via CPU-set info, not by assuming it.
- Windows keeps at least one core unparked and may override the settings under load (LATENCYHINT*, heterogeneous thresholds).
- `PowerSetActiveScheme` fires notifications. OEM services (Armoury Crate, Legion Space, MyASUS, Intel DTT, AMD PMF) may revert plan values. Re-check every T2 tick and flag conflicts in the UI.
- AMD PMF / Intel DTT can change EPP / PL limits underneath us. See the EPP/TDP research docs.
- Changing plans while plugged vs unplugged: write both AC and DC indices.
- Anti-cheat: `OpenProcess` on the game may fail or be logged. Keep the requested access minimal (`PROCESS_SET_LIMITED_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION`) and never touch its threads individually.
- Processor groups (>64 LP): CPU sets handle them natively, affinity masks do not.
- Crash safety: write the "restore" record (original scheme GUID, original values) **before** modifying anything, and apply it at the next start if a dirty flag is set.
- Cost budget: `GetSystemCpuSetInformation` a few µs. `NtQuerySystemInformation` per-LP perf about 10 µs. `SetProcessDefaultCpuSets` a few µs. The plan-write batch is ms-scale, so keep it to T2 only. MSR access through the driver is about 1–3 µs per op. The whole loop should stay well under 0.1% of one core at 20 Hz.

---

## 5. Sources

- Linux kernel: `arch/x86/include/asm/processor.h` (hw_cpu_type union: CPUID.1A EAX[31:24]/[23:0], 0x80000026 EBX fields), `arch/x86/include/asm/topology.h` (`enum amd_cpu_type`), `arch/x86/kernel/cpu/topology_common.c`, `topology_amd.c`, `scattered.c` (0x80000026 EAX[30] HTR_CORES, 0x80000021 EAX[22] WORKLOAD_CLASS), `cpufeatures.h` (HYBRID_CPU 18*32+15, HFI), `msr-index.h` (AMD CPPC 0xC00102B0–B5, WORKLOAD_CLASS 0xC0000500–502, HW_FEEDBACK 0x17D0/1). https://github.com/torvalds/linux
- microsoft/BaselineManagement `src/Parsers/GPO/PowerOptions.ps1` (power setting GUIDs). https://github.com/microsoft/BaselineManagement/blob/main/src/Parsers/GPO/PowerOptions.ps1
- mkht/DSCR_PowerPlan `GUID_LIST_SETTING` (SCHEDPOLICY, SHORTSCHEDPOLICY, PERFEPP1, PROCFREQMAX1, LATENCYHINT*). https://github.com/mkht/DSCR_PowerPlan/blob/master/DSCResources/cPowerPlanSetting/DATA/GUID_LIST_SETTING
- MS Learn, Hetero power scheduling: HeteroIncreaseThreshold, HeteroClass1InitialPerf, HeteroClass0FloorPerf, ShortSchedulingPolicy, "static configuration options for heterogeneous power scheduling". https://learn.microsoft.com/en-us/windows-hardware/customize/power-settings/configuration-for-hetero-power-scheduling-heteroincreasethreshold
- MS Learn API docs: GetSystemCpuSetInformation, SYSTEM_CPU_SET_INFORMATION, SetProcessDefaultCpuSets, SetThreadSelectedCpuSets, PROCESSOR_RELATIONSHIP, SetProcessInformation / PROCESS_POWER_THROTTLING_STATE, "Quality of Service" (Win11 QoS levels).
- Intel community: "Detecting LP E-Cores on Meteor Lake in software" (EfficiencyClass doesn't split LP-E; SchedulingClass does). https://community.intel.com/t5/Mobile-and-Desktop-Processors/Detecting-LP-E-Cores-on-Meteor-Lake-in-software/td-p/1577956
- Intel "Game Dev Guide for 12th Gen Intel Core Hybrid Architecture" and "Optimizing Software for Intel Performance Hybrid Architecture" (348851). https://www.intel.com/content/www/us/en/developer/articles/guide/12th-gen-intel-core-processor-gamedev-guide.html
- Bitsum: "How To Keep Processes Off E-Cores", ParkControl. https://bitsum.com/docs/how-to-keep-processes-off-e-cores/
- Chips and Cheese: Strix Point (Zen5 4-core 16 MB CCX + Zen5c 8-core 8 MB CCX), Meteor Lake Crestmont.
