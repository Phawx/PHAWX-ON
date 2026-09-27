# GPU clock control and telemetry from plain C on Windows (AMD / NVIDIA / Intel)

Scope: an x64 mingw-w64 C11 process running elevated. Every vendor library is loaded at runtime with `LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_SYSTEM32)` and `GetProcAddress`. Nothing is linked at build time and no SDK is needed at runtime. All declarations below were checked against the vendor headers (fetched 2026-09-26). Where noted, sizes and offsets were also checked by compiling with `x86_64-w64-mingw32-gcc 13` next to the original headers.

On x64, `__cdecl`, `__stdcall` and default calling conventions are the same ABI. The macros are kept below only for documentation and for a possible 32-bit build.

---

## 0. Summary and recommended backend order

| Vendor / part | Clock cap (the AutoTDP knob) | Telemetry (clock, busy %, power) | Library |
|---|---|---|---|
| NVIDIA dGPU (Turing+ GeForce, Volta+ pro) | `nvmlDeviceSetGpuLockedClocks(min,max)`, reset with `nvmlDeviceResetGpuLockedClocks` | `nvmlDeviceGetClockInfo`, `nvmlDeviceGetUtilizationRates`, `nvmlDeviceGetPowerUsage` | `nvml.dll` |
| NVIDIA (fallback, offsets only) | NvAPI `SetPstates20` P0 GPC clock offset (kHz). This is an offset, not a cap | NvAPI `GetAllClockFrequencies`, `GetDynamicPstatesInfoEx` | `nvapi64.dll` |
| AMD dGPU RDNA2+ | ADLX `IADLXManualGraphicsTuning2::SetGPUMaxFrequency` (MHz) | ADLX `GetCurrentGPUMetrics` | `amdadlx64.dll` |
| AMD dGPU pre-Navi21 / old driver | ADL `ADL2_Overdrive8_Setting_Set` (OD8_GFXCLK_FMAX) | `ADL2_New_QueryPMLogData_Get` | `atiadlxx.dll` |
| AMD APU iGPU | Usually no Overdrive. Use SMU: RyzenAdj-style PSMU msg `0x89` "set_gfx_clk" (forced clock, Renoir..Strix Halo). See §2.5 | ADLX metrics work on APUs; PM table `gfx_clk` | SMU via hw layer |
| Intel Arc dGPU / Xe iGPU (driver IGCL present) | IGCL `ctlFrequencySetRange` on the `CTL_FREQ_DOMAIN_GPU` handle when `canControl` is true | `ctlFrequencyGetState` (`actual`) and `ctlPowerTelemetryGet` (activity counters) | `ControlLib.dll` |
| Intel iGPU Gen9..Gen12 (MCHBAR method) | MCHBAR+0x5994 byte 0 = `GEN6_RP_STATE_LIMITS` RP0 cap, in 50 MHz units | MCHBAR+0x5948 `GEN6_GT_PERF_STATUS` | physical MMIO via kernel driver |
| Any vendor | none | `D3DKMTQueryStatistics` per-node RunningTime (cheap) | `gdi32.dll` |

For AutoTDP, the lowest usable clock is a **max-frequency cap** (NVML locked clocks with max = target, ADLX SetGPUMaxFrequency, IGCL range max, Intel RP0 cap). Only AMD APU `set_gfx_clk` is a *forced* clock (min = max). That still works as a knob, but the GPU is pinned even when idle, so release it (see §2.5) when not gaming.

---

## 1. NVIDIA

### 1.1 NVML (nvml.dll)

- **Location.** DCH drivers (every driver since about 2019) place `nvml.dll` in `C:\Windows\System32`. Legacy drivers used `C:\Program Files\NVIDIA Corporation\NVSMI\nvml.dll`. Try System32 first (`LOAD_LIBRARY_SEARCH_SYSTEM32`), then the NVSMI path via `%ProgramW6432%`.
- **API version.** Header API version 13 (NVML 13.x, 2026). Always resolve the `_v2` symbol names (`nvmlInit_v2`, `nvmlDeviceGetCount_v2`, `nvmlDeviceGetHandleByIndex_v2`). The unsuffixed names are legacy.
- **Admin.** `SetGpuLockedClocks`, `ResetGpuLockedClocks`, `SetMemoryLockedClocks` and `SetPowerManagementLimit` document "Requires root/admin permissions" and return `NVML_ERROR_NO_PERMISSION` (4) otherwise.
- **Support.** Locked clocks are documented for "Volta or newer fully supported devices". On Windows, `nvidia-smi -lgc` (which uses the same NVML call) works on GeForce Turing/Ampere/Ada/Blackwell when run as admin. That is community-verified, not an NVIDIA guarantee. Maxwell/Pascal GeForce often returns `NVML_ERROR_NOT_SUPPORTED` (3), so probe at runtime. Memory locked clocks are Ampere+ and flaky on GeForce.
- **Persistence.** Locked clocks survive process exit and last until reset, reboot or driver reload. **Always call `nvmlDeviceResetGpuLockedClocks` on exit, and also from a crash handler / `SetUnhandledExceptionFilter`.** Otherwise the user's GPU stays capped.
- **Symbolic limits.** `SetGpuLockedClocks(dev, NVML_CLOCK_LIMIT_ID_UNLIMITED, NVML_CLOCK_LIMIT_ID_UNLIMITED)` is equivalent to reset. Mixing symbolic and numeric values returns `INVALID_ARGUMENT`.
- **Choosing min.** Use the lowest entry of `nvmlDeviceGetSupportedGraphicsClocks` (call `nvmlDeviceGetSupportedMemoryClocks` first because it takes a memory clock argument). If that is not supported, use about 210 MHz. Set max to the AutoTDP target. The driver snaps to the nearest valid V/F point.
- **Units.**
  - Clocks: MHz.
  - Power: milliwatts. Ampere+ (except GA100) returns a 1 s average. For faster feedback use field values `NVML_FI_DEV_POWER_INSTANT` via `nvmlDeviceGetFieldValues`, or energy deltas from `nvmlDeviceGetTotalEnergyConsumption` (mJ).
  - Utilization: percent over the last sample period, which is 1/6 s to 1 s depending on product.
- **Latency.** Queries are about 0.1–1 ms, and a set takes effect within tens of ms. These figures come from community measurements, not a spec, so measure on target hardware.
- **Optimus pitfall.** Any NVML call on a laptop dGPU that is in D3cold wakes it and keeps it awake, which costs watts on battery. Only initialise and poll NVML while the foreground game is on the dGPU. Check with DXGI (adapter LUID of the swapchain) or with the D3DKMT per-adapter RunningTime delta, which does not wake the device.
- **Temperature.** `nvmlDeviceGetTemperature` is marked DEPRECATED(13.0) but still exported. Prefer `nvmlDeviceGetTemperatureV` when present.

```c
typedef struct nvmlDevice_st *nvmlDevice_t;
typedef int nvmlReturn_t;
enum { NVML_SUCCESS = 0, NVML_ERROR_UNINITIALIZED = 1, NVML_ERROR_INVALID_ARGUMENT = 2,
       NVML_ERROR_NOT_SUPPORTED = 3, NVML_ERROR_NO_PERMISSION = 4, NVML_ERROR_ALREADY_INITIALIZED = 5,
       NVML_ERROR_NOT_FOUND = 6, NVML_ERROR_INSUFFICIENT_SIZE = 7, NVML_ERROR_DRIVER_NOT_LOADED = 9,
       NVML_ERROR_FUNCTION_NOT_FOUND = 13, NVML_ERROR_GPU_IS_LOST = 15, NVML_ERROR_FREQ_NOT_SUPPORTED = 24,
       NVML_ERROR_DEPRECATED = 26, NVML_ERROR_UNKNOWN = 999 };
typedef enum { NVML_CLOCK_GRAPHICS = 0, NVML_CLOCK_SM = 1, NVML_CLOCK_MEM = 2, NVML_CLOCK_VIDEO = 3 } nvmlClockType_t;
typedef enum { NVML_CLOCK_ID_CURRENT = 0, NVML_CLOCK_ID_APP_CLOCK_TARGET = 1,
               NVML_CLOCK_ID_APP_CLOCK_DEFAULT = 2, NVML_CLOCK_ID_CUSTOMER_BOOST_MAX = 3 } nvmlClockId_t;
typedef enum { NVML_TEMPERATURE_GPU = 0 } nvmlTemperatureSensors_t;
typedef enum { NVML_PSTATE_0 = 0, NVML_PSTATE_15 = 15, NVML_PSTATE_UNKNOWN = 32 } nvmlPstates_t;
#define NVML_CLOCK_LIMIT_ID_TDP       0xffffff01u
#define NVML_CLOCK_LIMIT_ID_UNLIMITED 0xffffff02u
#define NVML_DEVICE_NAME_V2_BUFFER_SIZE 96
typedef struct { unsigned int gpu, memory; } nvmlUtilization_t;
typedef struct { unsigned int version; nvmlTemperatureSensors_t sensorType; int temperature; } nvmlTemperature_v1_t;
#define nvmlTemperature_v1 ((unsigned int)(sizeof(nvmlTemperature_v1_t) | (1u << 24)))
typedef struct { unsigned int version; nvmlClockType_t type; nvmlPstates_t pstate;
                 int clockOffsetMHz, minClockOffsetMHz, maxClockOffsetMHz; } nvmlClockOffset_v1_t;
#define nvmlClockOffset_v1 ((unsigned int)(sizeof(nvmlClockOffset_v1_t) | (1u << 24)))
#define nvmlClocksEventReasonGpuIdle            0x1ull
#define nvmlClocksEventReasonSwPowerCap         0x4ull
#define nvmlClocksEventReasonSwThermalSlowdown  0x20ull
#define nvmlClocksEventReasonHwSlowdown         0x8ull

typedef nvmlReturn_t (*PFN_nvmlInit_v2)(void);
typedef nvmlReturn_t (*PFN_nvmlShutdown)(void);
typedef const char * (*PFN_nvmlErrorString)(nvmlReturn_t);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetCount_v2)(unsigned int *);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetHandleByIndex_v2)(unsigned int, nvmlDevice_t *);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetName)(nvmlDevice_t, char *, unsigned int);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetClockInfo)(nvmlDevice_t, nvmlClockType_t, unsigned int *);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetMaxClockInfo)(nvmlDevice_t, nvmlClockType_t, unsigned int *);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetClock)(nvmlDevice_t, nvmlClockType_t, nvmlClockId_t, unsigned int *);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetSupportedMemoryClocks)(nvmlDevice_t, unsigned int *count, unsigned int *mhz);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetSupportedGraphicsClocks)(nvmlDevice_t, unsigned int memMHz, unsigned int *count, unsigned int *mhz);
typedef nvmlReturn_t (*PFN_nvmlDeviceSetGpuLockedClocks)(nvmlDevice_t, unsigned int minMHz, unsigned int maxMHz);
typedef nvmlReturn_t (*PFN_nvmlDeviceResetGpuLockedClocks)(nvmlDevice_t);
typedef nvmlReturn_t (*PFN_nvmlDeviceSetMemoryLockedClocks)(nvmlDevice_t, unsigned int minMHz, unsigned int maxMHz);
typedef nvmlReturn_t (*PFN_nvmlDeviceResetMemoryLockedClocks)(nvmlDevice_t);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetUtilizationRates)(nvmlDevice_t, nvmlUtilization_t *);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetPowerUsage)(nvmlDevice_t, unsigned int *mW);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetTotalEnergyConsumption)(nvmlDevice_t, unsigned long long *mJ);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetPowerManagementLimitConstraints)(nvmlDevice_t, unsigned int *minmW, unsigned int *maxmW);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetPowerManagementLimit)(nvmlDevice_t, unsigned int *mW);
typedef nvmlReturn_t (*PFN_nvmlDeviceSetPowerManagementLimit)(nvmlDevice_t, unsigned int mW);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetEnforcedPowerLimit)(nvmlDevice_t, unsigned int *mW);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetTemperature)(nvmlDevice_t, nvmlTemperatureSensors_t, unsigned int *);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetTemperatureV)(nvmlDevice_t, nvmlTemperature_v1_t *);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetPerformanceState)(nvmlDevice_t, nvmlPstates_t *);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetCurrentClocksEventReasons)(nvmlDevice_t, unsigned long long *);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetClockOffsets)(nvmlDevice_t, nvmlClockOffset_v1_t *);
typedef nvmlReturn_t (*PFN_nvmlDeviceSetClockOffsets)(nvmlDevice_t, nvmlClockOffset_v1_t *);
```

Sequence: `nvmlInit_v2` → `GetCount_v2` → `GetHandleByIndex_v2(i)` → `GetMaxClockInfo(GRAPHICS)` for the upper bound → AutoTDP loop calls `SetGpuLockedClocks(dev, minSupported, target)` only when the target changes by at least one step (15 MHz) → `ResetGpuLockedClocks` + `nvmlShutdown` on exit.
`nvmlDeviceSetClockOffsets` (NVML ≥ 12.5 era, replaces the deprecated `nvmlDeviceSetGpcClkVfOffset`) sets the V/F offset per P-state and is the NVML equivalent of the Afterburner core offset.

### 1.2 NvAPI (nvapi64.dll): fallback and extra telemetry

- **Export.** `nvapi64.dll` (System32) exports one function, `nvapi_QueryInterface(unsigned int id) → void*`. Every function is obtained by ID. IDs come from `nvapi_interface.h` in github.com/NVIDIA/nvapi. `SetPstates20` is not in the public list; its ID comes from reverse-engineered tools.
- **Struct versioning.** `version = sizeof(struct) | (ver << 16)`. The layouts and version constants below were compiled and match the official header (for example `NV_GPU_PERF_PSTATES20_INFO_VER1 = 0x11C94`, `VER3 = 0x31CF8`, `NV_GPU_CLOCK_FREQUENCIES_VER_3 = 0x30108`, `NV_GPU_DYNAMIC_PSTATES_INFO_EX_VER = 0x10048`).
- **Clock units.** kHz.
- **GetDynamicPstatesInfoEx.** `utilization[0]` is GPU (graphics engine), `[1]` FB, `[2]` VID, `[3]` BUS.
- **SetPstates20 offset.** Fill a V1 struct: `numPstates=1, numClocks=1, numBaseVoltages=0, pstates[0].pstateId=0 (P0), pstates[0].clocks[0].domainId=NVAPI_GPU_PUBLIC_CLOCK_GRAPHICS, freqDelta_kHz.value=offset_kHz`. Negative offsets shift the whole V/F curve down. That is not a cap, so it is only a coarse fallback for GPUs where NVML locked clocks are unsupported. The allowed range is `freqDelta_kHz.valueRange` from `GetPstates20`, and it is often limited on mobile GPUs. Admin is required. Offsets do not persist across reboot.
- **License.** The NVAPI SDK headers are MIT (SPDX header in the repo), so they can be vendored. The minimal redeclaration below avoids that need.

```c
typedef int32_t NvAPI_Status;
#define NVAPI_OK 0
typedef struct NvPhysicalGpuHandle__ *NvPhysicalGpuHandle;
#define NVAPI_MAX_PHYSICAL_GPUS 64
#define NV_MAKE_VER(sz, v) ((uint32_t)((sz) | ((v) << 16)))
#define NVID_Initialize                  0x0150E828u
#define NVID_Unload                      0xD22BDD7Eu
#define NVID_EnumPhysicalGPUs            0xE5AC921Fu
#define NVID_GPU_GetFullName             0xCEEE8E9Fu
#define NVID_GPU_GetAllClockFrequencies  0xDCB616C3u
#define NVID_GPU_GetDynamicPstatesInfoEx 0x60DED2EDu
#define NVID_GPU_GetPstates20            0x6FF81213u
#define NVID_GPU_SetPstates20            0x0F4DAE6Bu
#define NVID_GPU_GetCurrentPstate        0x927DA4F6u
#define NVID_GPU_GetThermalSettings      0xE3640A56u
#define NVID_GPU_GetPerfDecreaseInfo     0x7F7F4600u
typedef void *(__cdecl *PFN_nvapi_QueryInterface)(uint32_t id);
typedef NvAPI_Status (__cdecl *PFN_NvAPI_Initialize)(void);
typedef NvAPI_Status (__cdecl *PFN_NvAPI_Unload)(void);
typedef NvAPI_Status (__cdecl *PFN_NvAPI_EnumPhysicalGPUs)(NvPhysicalGpuHandle h[NVAPI_MAX_PHYSICAL_GPUS], uint32_t *count);
typedef NvAPI_Status (__cdecl *PFN_NvAPI_GPU_GetFullName)(NvPhysicalGpuHandle, char name[64]);
enum { NVAPI_GPU_PUBLIC_CLOCK_GRAPHICS = 0, NVAPI_GPU_PUBLIC_CLOCK_MEMORY = 4,
       NVAPI_GPU_PUBLIC_CLOCK_PROCESSOR = 7, NVAPI_GPU_PUBLIC_CLOCK_VIDEO = 8 };
enum { NV_GPU_CLOCK_FREQUENCIES_CURRENT_FREQ = 0, NV_GPU_CLOCK_FREQUENCIES_BASE_CLOCK = 1,
       NV_GPU_CLOCK_FREQUENCIES_BOOST_CLOCK = 2 };
typedef struct { uint32_t bIsPresent : 1; uint32_t reserved : 31; uint32_t frequency; } NV_CLK_DOMAIN;
typedef struct {
    uint32_t version;
    uint32_t ClockType : 4; uint32_t reserved : 20; uint32_t reserved1 : 8;
    NV_CLK_DOMAIN domain[32];
} NV_GPU_CLOCK_FREQUENCIES_V2;                                  /* 0x108 */
#define NV_GPU_CLOCK_FREQUENCIES_VER_3 NV_MAKE_VER(sizeof(NV_GPU_CLOCK_FREQUENCIES_V2), 3)
typedef struct {
    uint32_t version; uint32_t flags;
    struct { uint32_t bIsPresent : 1; uint32_t percentage; } utilization[8];
} NV_GPU_DYNAMIC_PSTATES_INFO_EX;                               /* 0x48 */
#define NV_GPU_DYNAMIC_PSTATES_INFO_EX_VER NV_MAKE_VER(sizeof(NV_GPU_DYNAMIC_PSTATES_INFO_EX), 1)
typedef struct { int32_t value; struct { int32_t min, max; } valueRange; } NV_PSTATES20_PARAM_DELTA;
typedef struct {
    int32_t domainId; int32_t typeId;
    uint32_t bIsEditable : 1; uint32_t reserved : 31;
    NV_PSTATES20_PARAM_DELTA freqDelta_kHz;
    union {
        struct { uint32_t freq_kHz; } single;
        struct { uint32_t minFreq_kHz, maxFreq_kHz; int32_t domainId; uint32_t minVoltage_uV, maxVoltage_uV; } range;
    } data;
} NV_PSTATE20_CLOCK_ENTRY_V1;                                   /* 0x2C */
typedef struct {
    int32_t domainId; uint32_t bIsEditable : 1; uint32_t reserved : 31;
    uint32_t volt_uV; NV_PSTATES20_PARAM_DELTA voltDelta_uV;
} NV_PSTATE20_BASE_VOLTAGE_ENTRY_V1;
typedef struct {
    uint32_t version; uint32_t bIsEditable : 1; uint32_t reserved : 31;
    uint32_t numPstates, numClocks, numBaseVoltages;
    struct {
        int32_t pstateId; uint32_t bIsEditable : 1; uint32_t reserved : 31;
        NV_PSTATE20_CLOCK_ENTRY_V1 clocks[8];
        NV_PSTATE20_BASE_VOLTAGE_ENTRY_V1 baseVoltages[4];
    } pstates[16];
} NV_GPU_PERF_PSTATES20_INFO_V1;                                /* 0x1C94 */
typedef struct {
    uint32_t version; uint32_t bIsEditable : 1; uint32_t reserved : 31;
    uint32_t numPstates, numClocks, numBaseVoltages;
    struct {
        int32_t pstateId; uint32_t bIsEditable : 1; uint32_t reserved : 31;
        NV_PSTATE20_CLOCK_ENTRY_V1 clocks[8];
        NV_PSTATE20_BASE_VOLTAGE_ENTRY_V1 baseVoltages[4];
    } pstates[16];
    struct { uint32_t numVoltages; NV_PSTATE20_BASE_VOLTAGE_ENTRY_V1 voltages[4]; } ov;
} NV_GPU_PERF_PSTATES20_INFO_V2;                                /* 0x1CF8 */
#define NV_GPU_PERF_PSTATES20_INFO_VER1 NV_MAKE_VER(sizeof(NV_GPU_PERF_PSTATES20_INFO_V1), 1)
#define NV_GPU_PERF_PSTATES20_INFO_VER3 NV_MAKE_VER(sizeof(NV_GPU_PERF_PSTATES20_INFO_V2), 3)
typedef NvAPI_Status (__cdecl *PFN_NvAPI_GPU_GetAllClockFrequencies)(NvPhysicalGpuHandle, NV_GPU_CLOCK_FREQUENCIES_V2 *);
typedef NvAPI_Status (__cdecl *PFN_NvAPI_GPU_GetDynamicPstatesInfoEx)(NvPhysicalGpuHandle, NV_GPU_DYNAMIC_PSTATES_INFO_EX *);
typedef NvAPI_Status (__cdecl *PFN_NvAPI_GPU_GetPstates20)(NvPhysicalGpuHandle, NV_GPU_PERF_PSTATES20_INFO_V2 *);
typedef NvAPI_Status (__cdecl *PFN_NvAPI_GPU_SetPstates20)(NvPhysicalGpuHandle, NV_GPU_PERF_PSTATES20_INFO_V1 *);
```

`GetAllClockFrequencies`: set `version = VER_3` and `ClockType = CURRENT_FREQ` (0). The value is in `domain[NVAPI_GPU_PUBLIC_CLOCK_GRAPHICS].frequency` (kHz).

---

## 2. AMD

### 2.1 ADLX (amdadlx64.dll): primary path

- **Installation.** Ships with Adrenalin (System32 `amdadlx64.dll`; `amdadlx32.dll` for 32-bit). SDK: github.com/GPUOpen-LibrariesAndSDKs/ADLX. Current header version is 2.0.0.125.
- **Pure C interface: yes.** Every header has `#else //__cplusplus` blocks that define `XxxVtbl` structs of function pointers, and objects are `struct { const XxxVtbl *pVtbl; }`. The methods are `ADLX_STD_CALL` (`__stdcall`) and the C functions are `ADLX_CDECL_CALL`. `adlx_bool` is `uint8_t` in C. Interface IDs are **wide strings** equal to the interface name (e.g. `L"IADLXManualGraphicsTuning2"`) and are passed to `QueryInterface`.
- **License.** The "ADLX SDK License Agreement.pdf" in the repo is a custom AMD license, **not MIT**. Rather than vendoring AMD headers, redeclare the few vtables below. The vtable order is an ABI fact. It is uncertain whether the PDF allows header redistribution, and redeclaring avoids the question.
- **Refcounting.** Every interface except `IADLXSystem` starts with `Acquire, Release, QueryInterface`. `IADLXSystem` is not refcounted and its first slot is `GetHybridGraphicsType`. Release every object you get, and release objects before `ADLXTerminate`, or it returns `ADLX_ORPHAN_OBJECTS`.
- **Exports.** `ADLXQueryFullVersion`, `ADLXQueryVersion`, `ADLXInitialize`, `ADLXInitializeWithIncompatibleDriver`, `ADLXInitialize2`, `ADLXInitializeWithCallerAdl`, `ADLXTerminate`.
  - Version arg: `ADLX_FULL_VERSION = (2ull<<48)|(0ull<<32)|(0ull<<16)|125`.
  - If `ADLXInitialize` returns `ADLX_BAD_VER` (5) on an older driver, retry with `ADLXInitializeWithIncompatibleDriver`. Features the driver lacks then return `ADLX_NOT_SUPPORTED` (12).
- **Manual GFX tuning.**
  - `IsSupportedManualGFXTuning(gpu)` is true on RDNA2+ dGPUs (Navi21+). These use `IADLXManualGraphicsTuning2`: Min/Max frequency in MHz plus a voltage offset.
  - Pre-Navi21 GPUs use `IADLXManualGraphicsTuning1` (state list).
  - **APU iGPUs (680M/780M/880M/890M, Z1/Z2) normally report false.** Use §2.5.
  - Setting max frequency applies a driver Overdrive profile. To return to stock use `SetGPUMaxFrequency(GetGPUMaxFrequencyDefault())`, available on the `Tuning2_1` interface, or `ResetToFactory(gpu)` on the services object.
  - Apply latency has not been measured. It is expected to be tens to hundreds of ms because it goes through the KMD/SMU. Rate-limit to about 4 Hz or less.
- **Metrics.** `GetCurrentGPUMetrics(gpu)` returns a snapshot. `GPUClockSpeed` (MHz), `GPUUsage` (%), `GPUPower` (W, double), `GPUTotalBoardPower` (W) and `GPUTemperature` (°C). Check support first with `GetSupportedGPUMetrics`. `SetSamplingInterval` (ms) controls the driver's internal sampling, and the range comes from `GetSamplingIntervalRange` (typically 100–1000 ms). This is too slow for a sub-100 ms AutoTDP loop, so use D3DKMT (§4) for busy %. `GetCurrentFPS` exists (AMD overlay FPS) but only reports while the driver metrics pipeline is tracking a game, and it is AMD-only.

```c
#include <stdint.h>
#include <wchar.h>
typedef int ADLX_RESULT;
enum { ADLX_OK = 0, ADLX_ALREADY_ENABLED, ADLX_ALREADY_INITIALIZED, ADLX_FAIL, ADLX_INVALID_ARGS,
       ADLX_BAD_VER, ADLX_UNKNOWN_INTERFACE, ADLX_TERMINATED, ADLX_ADL_INIT_ERROR, ADLX_NOT_FOUND,
       ADLX_INVALID_OBJECT, ADLX_ORPHAN_OBJECTS, ADLX_NOT_SUPPORTED, ADLX_PENDING_OPERATION,
       ADLX_GPU_INACTIVE, ADLX_GPU_IN_USE, ADLX_TIMEOUT_OPERATION, ADLX_NOT_ACTIVE, ADLX_RESET_NEEDED };
#define ADLX_SUCCEEDED(x) ((x) == ADLX_OK || (x) == ADLX_ALREADY_ENABLED || (x) == ADLX_ALREADY_INITIALIZED)
typedef uint8_t adlx_bool;
typedef int32_t adlx_int;
typedef uint32_t adlx_uint;
typedef long adlx_long;
#define ADLX_FULL_VERSION ((2ull << 48) | (0ull << 32) | (0ull << 16) | 125ull)
typedef struct { adlx_int minValue, maxValue, step; } ADLX_IntRange;

typedef struct IADLXInterface IADLXInterface;
typedef struct IADLXSystem IADLXSystem;
typedef struct IADLXGPU IADLXGPU;
typedef struct IADLXGPUList IADLXGPUList;
typedef struct IADLXGPUTuningServices IADLXGPUTuningServices;
typedef struct IADLXManualGraphicsTuning2 IADLXManualGraphicsTuning2;
typedef struct IADLXManualGraphicsTuning2_1 IADLXManualGraphicsTuning2_1;
typedef struct IADLXManualPowerTuning IADLXManualPowerTuning;
typedef struct IADLXPerformanceMonitoringServices IADLXPerformanceMonitoringServices;
typedef struct IADLXGPUMetrics IADLXGPUMetrics;
typedef struct IADLXGPUMetricsSupport IADLXGPUMetricsSupport;
typedef struct IADLX3DSettingsServices IADLX3DSettingsServices;
typedef struct IADLX3DChill IADLX3DChill;
typedef struct IADLX3DFrameRateTargetControl IADLX3DFrameRateTargetControl;

typedef ADLX_RESULT (__cdecl *PFN_ADLXQueryFullVersion)(uint64_t *fullVersion);
typedef ADLX_RESULT (__cdecl *PFN_ADLXInitialize)(uint64_t version, IADLXSystem **ppSystem);
typedef ADLX_RESULT (__cdecl *PFN_ADLXTerminate)(void);

#define ADLX_IUNKNOWN(T) \
    adlx_long   (__stdcall *Acquire)(T *); \
    adlx_long   (__stdcall *Release)(T *); \
    ADLX_RESULT (__stdcall *QueryInterface)(T *, const wchar_t *iid, void **pp);

typedef struct { ADLX_IUNKNOWN(IADLXInterface) } IADLXInterfaceVtbl;
struct IADLXInterface { const IADLXInterfaceVtbl *pVtbl; };

typedef struct {
    ADLX_RESULT (__stdcall *GetHybridGraphicsType)(IADLXSystem *, int *hgType);
    ADLX_RESULT (__stdcall *GetGPUs)(IADLXSystem *, IADLXGPUList **);
    ADLX_RESULT (__stdcall *QueryInterface)(IADLXSystem *, const wchar_t *, void **);
    ADLX_RESULT (__stdcall *GetDisplaysServices)(IADLXSystem *, void **);
    ADLX_RESULT (__stdcall *GetDesktopsServices)(IADLXSystem *, void **);
    ADLX_RESULT (__stdcall *GetGPUsChangedHandling)(IADLXSystem *, void **);
    ADLX_RESULT (__stdcall *EnableLog)(IADLXSystem *, int mode, int severity, void *logger, const wchar_t *file);
    ADLX_RESULT (__stdcall *Get3DSettingsServices)(IADLXSystem *, IADLX3DSettingsServices **);
    ADLX_RESULT (__stdcall *GetGPUTuningServices)(IADLXSystem *, IADLXGPUTuningServices **);
    ADLX_RESULT (__stdcall *GetPerformanceMonitoringServices)(IADLXSystem *, IADLXPerformanceMonitoringServices **);
    ADLX_RESULT (__stdcall *TotalSystemRAM)(IADLXSystem *, adlx_uint *ramMB);
    ADLX_RESULT (__stdcall *GetI2C)(IADLXSystem *, IADLXGPU *, void **);
} IADLXSystemVtbl;
struct IADLXSystem { const IADLXSystemVtbl *pVtbl; };

typedef struct {
    ADLX_IUNKNOWN(IADLXGPU)
    ADLX_RESULT (__stdcall *VendorId)(IADLXGPU *, const char **);
    ADLX_RESULT (__stdcall *ASICFamilyType)(IADLXGPU *, int *);
    ADLX_RESULT (__stdcall *Type)(IADLXGPU *, int *);             /* 0 undefined, 1 integrated, 2 discrete */
    ADLX_RESULT (__stdcall *IsExternal)(IADLXGPU *, adlx_bool *);
    ADLX_RESULT (__stdcall *Name)(IADLXGPU *, const char **);
    ADLX_RESULT (__stdcall *DriverPath)(IADLXGPU *, const char **);
    ADLX_RESULT (__stdcall *PNPString)(IADLXGPU *, const char **);
    ADLX_RESULT (__stdcall *HasDesktops)(IADLXGPU *, adlx_bool *);
    ADLX_RESULT (__stdcall *TotalVRAM)(IADLXGPU *, adlx_uint *);
    ADLX_RESULT (__stdcall *VRAMType)(IADLXGPU *, const char **);
    ADLX_RESULT (__stdcall *BIOSInfo)(IADLXGPU *, const char **, const char **, const char **);
    ADLX_RESULT (__stdcall *DeviceId)(IADLXGPU *, const char **);
    ADLX_RESULT (__stdcall *RevisionId)(IADLXGPU *, const char **);
    ADLX_RESULT (__stdcall *SubSystemId)(IADLXGPU *, const char **);
    ADLX_RESULT (__stdcall *SubSystemVendorId)(IADLXGPU *, const char **);
    ADLX_RESULT (__stdcall *UniqueId)(IADLXGPU *, adlx_int *);
} IADLXGPUVtbl;
struct IADLXGPU { const IADLXGPUVtbl *pVtbl; };

typedef struct {
    ADLX_IUNKNOWN(IADLXGPUList)
    adlx_uint   (__stdcall *Size)(IADLXGPUList *);
    adlx_bool   (__stdcall *Empty)(IADLXGPUList *);
    adlx_uint   (__stdcall *Begin)(IADLXGPUList *);
    adlx_uint   (__stdcall *End)(IADLXGPUList *);
    ADLX_RESULT (__stdcall *At)(IADLXGPUList *, adlx_uint, IADLXInterface **);
    ADLX_RESULT (__stdcall *Clear)(IADLXGPUList *);
    ADLX_RESULT (__stdcall *Remove_Back)(IADLXGPUList *);
    ADLX_RESULT (__stdcall *Add_Back)(IADLXGPUList *, IADLXInterface *);
    ADLX_RESULT (__stdcall *At_GPUList)(IADLXGPUList *, adlx_uint, IADLXGPU **);
    ADLX_RESULT (__stdcall *Add_Back_GPUList)(IADLXGPUList *, IADLXGPU *);
} IADLXGPUListVtbl;
struct IADLXGPUList { const IADLXGPUListVtbl *pVtbl; };

typedef struct {
    ADLX_IUNKNOWN(IADLXGPUTuningServices)
    ADLX_RESULT (__stdcall *GetGPUTuningChangedHandling)(IADLXGPUTuningServices *, void **);
    ADLX_RESULT (__stdcall *IsAtFactory)(IADLXGPUTuningServices *, IADLXGPU *, adlx_bool *);
    ADLX_RESULT (__stdcall *ResetToFactory)(IADLXGPUTuningServices *, IADLXGPU *);
    ADLX_RESULT (__stdcall *IsSupportedAutoTuning)(IADLXGPUTuningServices *, IADLXGPU *, adlx_bool *);
    ADLX_RESULT (__stdcall *IsSupportedPresetTuning)(IADLXGPUTuningServices *, IADLXGPU *, adlx_bool *);
    ADLX_RESULT (__stdcall *IsSupportedManualGFXTuning)(IADLXGPUTuningServices *, IADLXGPU *, adlx_bool *);
    ADLX_RESULT (__stdcall *IsSupportedManualVRAMTuning)(IADLXGPUTuningServices *, IADLXGPU *, adlx_bool *);
    ADLX_RESULT (__stdcall *IsSupportedManualFanTuning)(IADLXGPUTuningServices *, IADLXGPU *, adlx_bool *);
    ADLX_RESULT (__stdcall *IsSupportedManualPowerTuning)(IADLXGPUTuningServices *, IADLXGPU *, adlx_bool *);
    ADLX_RESULT (__stdcall *GetAutoTuning)(IADLXGPUTuningServices *, IADLXGPU *, IADLXInterface **);
    ADLX_RESULT (__stdcall *GetPresetTuning)(IADLXGPUTuningServices *, IADLXGPU *, IADLXInterface **);
    ADLX_RESULT (__stdcall *GetManualGFXTuning)(IADLXGPUTuningServices *, IADLXGPU *, IADLXInterface **);
    ADLX_RESULT (__stdcall *GetManualVRAMTuning)(IADLXGPUTuningServices *, IADLXGPU *, IADLXInterface **);
    ADLX_RESULT (__stdcall *GetManualFanTuning)(IADLXGPUTuningServices *, IADLXGPU *, IADLXInterface **);
    ADLX_RESULT (__stdcall *GetManualPowerTuning)(IADLXGPUTuningServices *, IADLXGPU *, IADLXInterface **);
} IADLXGPUTuningServicesVtbl;
struct IADLXGPUTuningServices { const IADLXGPUTuningServicesVtbl *pVtbl; };

#define ADLX_MGT2_BODY(T) \
    ADLX_IUNKNOWN(T) \
    ADLX_RESULT (__stdcall *GetGPUMinFrequencyRange)(T *, ADLX_IntRange *); \
    ADLX_RESULT (__stdcall *GetGPUMinFrequency)(T *, adlx_int *); \
    ADLX_RESULT (__stdcall *SetGPUMinFrequency)(T *, adlx_int); \
    ADLX_RESULT (__stdcall *GetGPUMaxFrequencyRange)(T *, ADLX_IntRange *); \
    ADLX_RESULT (__stdcall *GetGPUMaxFrequency)(T *, adlx_int *); \
    ADLX_RESULT (__stdcall *SetGPUMaxFrequency)(T *, adlx_int); \
    ADLX_RESULT (__stdcall *GetGPUVoltageRange)(T *, ADLX_IntRange *); \
    ADLX_RESULT (__stdcall *GetGPUVoltage)(T *, adlx_int *); \
    ADLX_RESULT (__stdcall *SetGPUVoltage)(T *, adlx_int);
typedef struct { ADLX_MGT2_BODY(IADLXManualGraphicsTuning2) } IADLXManualGraphicsTuning2Vtbl;
struct IADLXManualGraphicsTuning2 { const IADLXManualGraphicsTuning2Vtbl *pVtbl; };
typedef struct {
    ADLX_MGT2_BODY(IADLXManualGraphicsTuning2_1)
    ADLX_RESULT (__stdcall *GetGPUMinFrequencyDefault)(IADLXManualGraphicsTuning2_1 *, adlx_int *);
    ADLX_RESULT (__stdcall *GetGPUMaxFrequencyDefault)(IADLXManualGraphicsTuning2_1 *, adlx_int *);
    ADLX_RESULT (__stdcall *GetGPUVoltageDefault)(IADLXManualGraphicsTuning2_1 *, adlx_int *);
} IADLXManualGraphicsTuning2_1Vtbl;
struct IADLXManualGraphicsTuning2_1 { const IADLXManualGraphicsTuning2_1Vtbl *pVtbl; };

typedef struct {
    ADLX_IUNKNOWN(IADLXManualPowerTuning)
    ADLX_RESULT (__stdcall *GetPowerLimitRange)(IADLXManualPowerTuning *, ADLX_IntRange *);  /* % offset, e.g. -10..+15 */
    ADLX_RESULT (__stdcall *GetPowerLimit)(IADLXManualPowerTuning *, adlx_int *);
    ADLX_RESULT (__stdcall *SetPowerLimit)(IADLXManualPowerTuning *, adlx_int);
    ADLX_RESULT (__stdcall *IsSupportedTDCLimit)(IADLXManualPowerTuning *, adlx_bool *);
    ADLX_RESULT (__stdcall *GetTDCLimitRange)(IADLXManualPowerTuning *, ADLX_IntRange *);
    ADLX_RESULT (__stdcall *GetTDCLimit)(IADLXManualPowerTuning *, adlx_int *);
    ADLX_RESULT (__stdcall *SetTDCLimit)(IADLXManualPowerTuning *, adlx_int);
} IADLXManualPowerTuningVtbl;
struct IADLXManualPowerTuning { const IADLXManualPowerTuningVtbl *pVtbl; };

typedef struct {
    ADLX_IUNKNOWN(IADLXPerformanceMonitoringServices)
    ADLX_RESULT (__stdcall *GetSamplingIntervalRange)(IADLXPerformanceMonitoringServices *, ADLX_IntRange *);
    ADLX_RESULT (__stdcall *SetSamplingInterval)(IADLXPerformanceMonitoringServices *, adlx_int ms);
    ADLX_RESULT (__stdcall *GetSamplingInterval)(IADLXPerformanceMonitoringServices *, adlx_int *ms);
    ADLX_RESULT (__stdcall *GetMaxPerformanceMetricsHistorySizeRange)(IADLXPerformanceMonitoringServices *, ADLX_IntRange *);
    ADLX_RESULT (__stdcall *SetMaxPerformanceMetricsHistorySize)(IADLXPerformanceMonitoringServices *, adlx_int);
    ADLX_RESULT (__stdcall *GetMaxPerformanceMetricsHistorySize)(IADLXPerformanceMonitoringServices *, adlx_int *);
    ADLX_RESULT (__stdcall *ClearPerformanceMetricsHistory)(IADLXPerformanceMonitoringServices *);
    ADLX_RESULT (__stdcall *GetCurrentPerformanceMetricsHistorySize)(IADLXPerformanceMonitoringServices *, adlx_int *);
    ADLX_RESULT (__stdcall *StartPerformanceMetricsTracking)(IADLXPerformanceMonitoringServices *);
    ADLX_RESULT (__stdcall *StopPerformanceMetricsTracking)(IADLXPerformanceMonitoringServices *);
    ADLX_RESULT (__stdcall *GetAllMetricsHistory)(IADLXPerformanceMonitoringServices *, adlx_int, adlx_int, void **);
    ADLX_RESULT (__stdcall *GetGPUMetricsHistory)(IADLXPerformanceMonitoringServices *, IADLXGPU *, adlx_int, adlx_int, void **);
    ADLX_RESULT (__stdcall *GetSystemMetricsHistory)(IADLXPerformanceMonitoringServices *, adlx_int, adlx_int, void **);
    ADLX_RESULT (__stdcall *GetFPSHistory)(IADLXPerformanceMonitoringServices *, adlx_int, adlx_int, void **);
    ADLX_RESULT (__stdcall *GetCurrentAllMetrics)(IADLXPerformanceMonitoringServices *, void **);
    ADLX_RESULT (__stdcall *GetCurrentGPUMetrics)(IADLXPerformanceMonitoringServices *, IADLXGPU *, IADLXGPUMetrics **);
    ADLX_RESULT (__stdcall *GetCurrentSystemMetrics)(IADLXPerformanceMonitoringServices *, void **);
    ADLX_RESULT (__stdcall *GetCurrentFPS)(IADLXPerformanceMonitoringServices *, void **ppFPS);
    ADLX_RESULT (__stdcall *GetSupportedGPUMetrics)(IADLXPerformanceMonitoringServices *, IADLXGPU *, IADLXGPUMetricsSupport **);
    ADLX_RESULT (__stdcall *GetSupportedSystemMetrics)(IADLXPerformanceMonitoringServices *, void **);
} IADLXPerformanceMonitoringServicesVtbl;
struct IADLXPerformanceMonitoringServices { const IADLXPerformanceMonitoringServicesVtbl *pVtbl; };

typedef struct {
    ADLX_IUNKNOWN(IADLXGPUMetrics)
    ADLX_RESULT (__stdcall *TimeStamp)(IADLXGPUMetrics *, int64_t *ms);
    ADLX_RESULT (__stdcall *GPUUsage)(IADLXGPUMetrics *, double *pct);
    ADLX_RESULT (__stdcall *GPUClockSpeed)(IADLXGPUMetrics *, adlx_int *mhz);
    ADLX_RESULT (__stdcall *GPUVRAMClockSpeed)(IADLXGPUMetrics *, adlx_int *mhz);
    ADLX_RESULT (__stdcall *GPUTemperature)(IADLXGPUMetrics *, double *c);
    ADLX_RESULT (__stdcall *GPUHotspotTemperature)(IADLXGPUMetrics *, double *c);
    ADLX_RESULT (__stdcall *GPUPower)(IADLXGPUMetrics *, double *w);
    ADLX_RESULT (__stdcall *GPUTotalBoardPower)(IADLXGPUMetrics *, double *w);
    ADLX_RESULT (__stdcall *GPUFanSpeed)(IADLXGPUMetrics *, adlx_int *rpm);
    ADLX_RESULT (__stdcall *GPUVRAM)(IADLXGPUMetrics *, adlx_int *mb);
    ADLX_RESULT (__stdcall *GPUVoltage)(IADLXGPUMetrics *, adlx_int *mv);
    ADLX_RESULT (__stdcall *GPUIntakeTemperature)(IADLXGPUMetrics *, double *c);
} IADLXGPUMetricsVtbl;
struct IADLXGPUMetrics { const IADLXGPUMetricsVtbl *pVtbl; };

typedef struct {
    ADLX_IUNKNOWN(IADLXGPUMetricsSupport)
    ADLX_RESULT (__stdcall *IsSupportedGPUUsage)(IADLXGPUMetricsSupport *, adlx_bool *);
    ADLX_RESULT (__stdcall *IsSupportedGPUClockSpeed)(IADLXGPUMetricsSupport *, adlx_bool *);
    ADLX_RESULT (__stdcall *IsSupportedGPUVRAMClockSpeed)(IADLXGPUMetricsSupport *, adlx_bool *);
    ADLX_RESULT (__stdcall *IsSupportedGPUTemperature)(IADLXGPUMetricsSupport *, adlx_bool *);
    ADLX_RESULT (__stdcall *IsSupportedGPUHotspotTemperature)(IADLXGPUMetricsSupport *, adlx_bool *);
    ADLX_RESULT (__stdcall *IsSupportedGPUPower)(IADLXGPUMetricsSupport *, adlx_bool *);
    ADLX_RESULT (__stdcall *IsSupportedGPUTotalBoardPower)(IADLXGPUMetricsSupport *, adlx_bool *);
    /* further slots (FanSpeed, VRAM, Voltage, *Range, Intake) not needed; never call past here */
} IADLXGPUMetricsSupportVtbl;
struct IADLXGPUMetricsSupport { const IADLXGPUMetricsSupportVtbl *pVtbl; };
```

Truncating a vtable struct is safe as long as you never call past the declared slots.

The ADLX 3D settings are the **live** replacement for the Radeon registry keys in §2.4 and need no driver restart. `IADLX3DSettingsServices` vtbl after the IUnknown triple:

| Slot | Method |
|---|---|
| 3 | GetAntiLag(gpu) |
| 4 | GetChill(gpu) |
| 5 | GetBoost(gpu) |
| 6 | GetImageSharpening(gpu) |
| 7 | GetEnhancedSync(gpu) |
| 8 | GetWaitForVerticalRefresh(gpu) |
| 9 | GetFrameRateTargetControl(gpu) |
| 10 | GetAntiAliasing |
| 11 | GetMorphologicalAntiAliasing |
| 12 | GetAnisotropicFiltering |
| 13 | GetTessellation |
| 14 | GetRadeonSuperResolution (no gpu arg) |
| 15 | GetResetShaderCache |
| 16 | Get3DSettingsChangedHandling |

Two of the settings interfaces:

- **`IADLX3DChill`** (after the IUnknown triple): IsSupported, IsEnabled, GetFPSRange, GetMinFPS, GetMaxFPS, SetEnabled, SetMinFPS, SetMaxFPS.
- **`IADLX3DFrameRateTargetControl`** (FRTC): IsSupported, IsEnabled, GetFPSRange, GetFPS, SetEnabled, SetFPS.

Newer features (AFMF, Anti-Lag 2, RSR sharpness) are on `IADLX3DSettingsServices1/2` (obtained with QueryInterface `L"IADLX3DSettingsServices1"`). Fetch `I3DSettings1.h` / `I3DSettings2.h` when implementing them.

Sequence for the clock cap:

```c
sys->pVtbl->GetGPUs(sys, &list);  list->pVtbl->At_GPUList(list, i, &gpu);
sys->pVtbl->GetGPUTuningServices(sys, &tun);
tun->pVtbl->IsSupportedManualGFXTuning(tun, gpu, &ok);
tun->pVtbl->GetManualGFXTuning(tun, gpu, &itf);
itf->pVtbl->QueryInterface(itf, L"IADLXManualGraphicsTuning2", (void **)&mgt2);   /* fails on pre-Navi21 -> Tuning1 */
mgt2->pVtbl->GetGPUMaxFrequencyRange(mgt2, &r);  mgt2->pVtbl->SetGPUMaxFrequency(mgt2, clamp(target, r.minValue, r.maxValue));
itf->pVtbl->Release(itf);
```

### 2.2 Legacy ADL (atiadlxx.dll): Overdrive8 and PMLog

- **Files and license.** `atiadlxx.dll` (64-bit, System32; `atiadlxy.dll` is 32-bit). Headers: github.com/GPUOpen-LibrariesAndSDKs/display-library `include/` (**MIT**; vendoring is fine). All exports are `__cdecl`. The malloc callback is `__stdcall`.
- **Duplicate adapters.** `ADL2_Adapter_AdapterInfo_Get` returns one entry per display path. Several entries share a GPU, so dedupe by `(iBusNumber, iDeviceNumber, iFunctionNumber)` and keep `iVendorID == 1002`. Use `ADL2_Overdrive_Caps(..., &iVersion)`, which returns 8 for OD8.
- **OD8 generations.**
  - **Navi21+ ("OD8 Plus")** reports caps with `ADL_OD8_GFXCLK_LIMITS` set and `ADL_OD8_GFXCLK_CURVE` clear. Sequence:
    1. If caps has `ADL_OD8_OPTIMIZED_GPU_POWER_MODE`, first set `OD8_OPTIMZED_POWER_MODE` (index 36) to value 3 (manual).
    2. Set `OD8_GFXCLK_FMAX` (0) and `OD8_GFXCLK_FMIN` (1) together, with `requested=1` on both and the unchanged one filled from the current settings.
  - **Vega/Navi1x (curve)**: setting FMAX must also set `OD8_GFXCLK_FREQ3` to the same value, and FMIN must also set `FREQ1`. Send the whole block FREQ1..UCLK_FMAX with `requested=1`.
- **`OD8_COUNT` pitfall.** The value is 77 in the current header and grows between SDK versions. Always set `count = OD8_COUNT` and zero the struct. `ADL2_Overdrive8_Init_SettingX2_Get` / `Current_SettingX2_Get` return the driver's actual feature count and ADL-allocated arrays (freed with your free, since ADL uses your malloc callback). Use them to detect a mismatch.
- **Reset.** `ADLOD8SingleSetSetting.reset = 1` with `value = defaultValue`. AMD's sample also writes `ADL2_Adapter_RegValueInt_Set(ctx, idx, 1, NULL, "IsAutoDefault", 0)` after a manual set.
- **APUs.** `ADL2_Overdrive_Caps` usually reports not supported on APUs. Use §2.5.

```c
typedef void *ADL_CONTEXT_HANDLE;
typedef void *(__stdcall *ADL_MAIN_MALLOC_CALLBACK)(int);
#define ADL_OK 0
#define ADL_ERR (-1)
#define ADL_ERR_NOT_SUPPORTED (-8)
#define ADL_MAX_PATH 256
#define ADL_PMLOG_MAX_SENSORS 256
#define OD8_COUNT 77
enum { OD8_GFXCLK_FMAX = 0, OD8_GFXCLK_FMIN = 1, OD8_GFXCLK_FREQ1 = 2, OD8_GFXCLK_VOLTAGE1 = 3,
       OD8_GFXCLK_FREQ2 = 4, OD8_GFXCLK_VOLTAGE2 = 5, OD8_GFXCLK_FREQ3 = 6, OD8_GFXCLK_VOLTAGE3 = 7,
       OD8_UCLK_FMAX = 8, OD8_POWER_PERCENTAGE = 9, OD8_UCLK_FMIN = 34, OD8_OPTIMZED_POWER_MODE = 36,
       OD8_TDC_PERCENTAGE = 47 };
#define ADL_OD8_GFXCLK_LIMITS            (1 << 0)
#define ADL_OD8_GFXCLK_CURVE             (1 << 1)
#define ADL_OD8_UCLK_MAX                 (1 << 2)
#define ADL_OD8_POWER_LIMIT              (1 << 3)
#define ADL_OD8_OPTIMIZED_GPU_POWER_MODE (1 << 16)
enum { ADL_PMLOG_CLK_GFXCLK = 1, ADL_PMLOG_CLK_MEMCLK = 2, ADL_PMLOG_CLK_SOCCLK = 3,
       ADL_PMLOG_TEMPERATURE_EDGE = 8, ADL_PMLOG_SOC_POWER = 17, ADL_PMLOG_INFO_ACTIVITY_GFX = 19,
       ADL_PMLOG_INFO_ACTIVITY_MEM = 20, ADL_PMLOG_GFX_VOLTAGE = 21, ADL_PMLOG_ASIC_POWER = 23,
       ADL_PMLOG_TEMPERATURE_HOTSPOT = 27, ADL_PMLOG_TEMPERATURE_GFX = 28, ADL_PMLOG_GFX_POWER = 30,
       ADL_PMLOG_TEMPERATURE_CPU = 32, ADL_PMLOG_CPU_POWER = 33, ADL_PMLOG_CLK_CPUCLK = 34,
       ADL_PMLOG_THROTTLER_STATUS = 35, ADL_PMLOG_CLK_FCLK = 44, ADL_PMLOG_BOARD_POWER = 73 };
typedef struct {
    int iSize, iAdapterIndex; char strUDID[ADL_MAX_PATH];
    int iBusNumber, iDeviceNumber, iFunctionNumber, iVendorID;
    char strAdapterName[ADL_MAX_PATH], strDisplayName[ADL_MAX_PATH];
    int iPresent, iExist;
    char strDriverPath[ADL_MAX_PATH], strDriverPathExt[ADL_MAX_PATH], strPNPString[ADL_MAX_PATH];
    int iOSDisplayIndex;
} AdapterInfo;                                   /* Windows layout */
typedef struct { int featureID, minValue, maxValue, defaultValue; } ADLOD8SingleInitSetting;
typedef struct { int count; int overdrive8Capabilities; ADLOD8SingleInitSetting od8SettingTable[OD8_COUNT]; } ADLOD8InitSetting;
typedef struct { int count; int Od8SettingTable[OD8_COUNT]; } ADLOD8CurrentSetting;
typedef struct { int value, requested, reset; } ADLOD8SingleSetSetting;
typedef struct { int count; ADLOD8SingleSetSetting od8SettingTable[OD8_COUNT]; } ADLOD8SetSetting;
typedef struct { int supported; int value; } ADLSingleSensorData;
typedef struct { int size; ADLSingleSensorData sensors[ADL_PMLOG_MAX_SENSORS]; } ADLPMLogDataOutput;

typedef int (*PFN_ADL2_Main_Control_Create)(ADL_MAIN_MALLOC_CALLBACK, int iEnumConnectedAdapters, ADL_CONTEXT_HANDLE *);
typedef int (*PFN_ADL2_Main_Control_Destroy)(ADL_CONTEXT_HANDLE);
typedef int (*PFN_ADL2_Adapter_NumberOfAdapters_Get)(ADL_CONTEXT_HANDLE, int *);
typedef int (*PFN_ADL2_Adapter_AdapterInfo_Get)(ADL_CONTEXT_HANDLE, AdapterInfo *, int iInputSize);
typedef int (*PFN_ADL2_Adapter_Active_Get)(ADL_CONTEXT_HANDLE, int, int *);
typedef int (*PFN_ADL2_Overdrive_Caps)(ADL_CONTEXT_HANDLE, int, int *supported, int *enabled, int *version);
typedef int (*PFN_ADL2_Overdrive8_Init_Setting_Get)(ADL_CONTEXT_HANDLE, int, ADLOD8InitSetting *);
typedef int (*PFN_ADL2_Overdrive8_Current_Setting_Get)(ADL_CONTEXT_HANDLE, int, ADLOD8CurrentSetting *);
typedef int (*PFN_ADL2_Overdrive8_Setting_Set)(ADL_CONTEXT_HANDLE, int, ADLOD8SetSetting *, ADLOD8CurrentSetting *);
typedef int (*PFN_ADL2_Overdrive8_Init_SettingX2_Get)(ADL_CONTEXT_HANDLE, int, int *caps, int *nFeatures, ADLOD8SingleInitSetting **);
typedef int (*PFN_ADL2_Overdrive8_Current_SettingX2_Get)(ADL_CONTEXT_HANDLE, int, int *nFeatures, int **);
typedef int (*PFN_ADL2_New_QueryPMLogData_Get)(ADL_CONTEXT_HANDLE, int, ADLPMLogDataOutput *);
typedef int (*PFN_ADL2_Adapter_RegValueInt_Set)(ADL_CONTEXT_HANDLE, int, int iDriverPathOption, char *subKey, char *keyName, int value);
```

`ADL2_New_QueryPMLogData_Get`: zero the struct, call it, then read `sensors[ADL_PMLOG_CLK_GFXCLK].value` (MHz), `INFO_ACTIVITY_GFX` (%) and `ASIC_POWER` (W, integer), each guarded by `.supported`. LibreHardwareMonitor uses this exact prototype. On APUs, `CPU_POWER` / `CLK_CPUCLK` / `SOC_POWER` are often populated too. The cheaper polling path is `ADL2_Overdrive8_PMLog_ShareMemory_*`, which is more complex and not needed.

### 2.3 AMD APU GPU clocks via SMU

- **Static clock.** `ryzenadj --gfx-clk=N` (MHz). It is also replayed on resume.
- **Hotkeys.** ±50 MHz within 400..2700, reading the current clock from PM-table offset **0x0354** of `--dump-table`. That offset is hard-coded and only valid for one PM-table version. RyzenAdj's own `get_gfx_clk` uses per-`table_ver` offsets: e.g. 0x5B4 (0x37000x), 0x5D0 (0x370005), 0x60C (0x400001), 0x624 (0x400002). Always switch on `table_ver`.
- **Steam Deck.** `--vrmgfx-current` is a GFX VRM current limit (mA), not a power value.

### 2.4 Radeon registry settings

- **Key.** `HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\NNNN`. Find NNNN from `HKLM\SYSTEM\CurrentControlSet\Enum\PCI\VEN_1002&...\<inst>` value `Driver` = `{4d36e968-...}\NNNN`. In C, `SetupDiGetClassDevs(&GUID_DEVCLASS_DISPLAY)` + `SetupDiOpenDevRegKey(..., DIREG_DRV, KEY_READ|KEY_WRITE)` is cleaner.
- **Apply.** The KMD reads these at driver start. Follow up with `pnputil /restart-device <instance>` (in C: `SetupDiSetClassInstallParams` + `DIF_PROPERTYCHANGE` / `DICS_PROPCHANGE`, or `CM_Disable_DevNode` + `CM_Enable_DevNode`). That flashes the screen and can kill running D3D apps. **Prefer ADLX 3D settings (§2.1), which apply live.** Use the registry only as a fallback.

| Value (subkey) | Type | Meaning / encoding |
|---|---|---|
| `KMD_ChillEnabled` | DWORD | 0/1 |
| `KMD_ChillMinFps`, `KMD_ChillMaxFps` | DWORD | FPS |
| `KMD_FRTEnabled` | DWORD | FRTC 0/1 |
| `KMD_MaxFrameRateRequested` | DWORD | FRTC FPS |
| `KMD_FRTCInitialStatus` | DWORD | 1 |
| `KMD_DeLagEnabled` | DWORD | Anti-Lag 0/1 |
| `KMD_RadeonBoostEnabled` | DWORD | 0/1 |
| `KMD_RadeonBoostMinResolution` | DWORD | % (50/67/84) |
| `KMD_USUEnable` | DWORD | RIS 0/1 |
| `KMD_USUSharpeningDegree` | DWORD | IEEE-754 float bits: 0x3F800000=1.0, 0x3F000000=0.5, 0x3DCCCCCD=0.1 |
| `Gmx_RSREnabled` | DWORD | RSR: 0 off, 3 on |
| `DrvFrameGenEnabled` | BINARY | AFMF: 4-byte LE 00000000 / 01000000 |
| `UMD\VSyncControl`, `UMD\Tessellation_OPTION`, `UMD\tfq`, `UMD\EnableTripleBuffering`, `UMD\SurfaceFormatReplacements`, `UMD\MLF`, `UMD\AntiStuttering` | BINARY | **UTF-16LE ASCII digits, no terminator**: bytes `30 00` = "0", `31 00` = "1", `32 00` = "2" |
| `UMD\Tessellation` | BINARY | UTF-16LE digits, e.g. `36 00 34 00` = "64" |
| `DAL3_DATA\power_v1\abmuserenable` | BINARY | 1 byte 00/01 (Vari-Bright on/off) |
| `DAL3_DATA\power_v1\abmlevel` | BINARY | 4-byte LE 0..4 |
| `DAL3_DATA\common\LINK_0\vsr` | BINARY | 4-byte LE 1/2 (VSR) |

The UMD values are UTF-16 digit strings (e.g. 3000↔3100); per-digit meanings are empirical, not from AMD docs.

### 2.5 AMD APU iGPU clock (no Overdrive)

- **Forced clock.** RyzenAdj `set_gfx_clk(value MHz)` sends **PSMU message 0x89**, argument MHz, on Renoir, Lucienne, Cezanne, Van Gogh, Rembrandt, Mendocino, Phoenix, Hawk Point, Krackan, Strix Point and Strix Halo (per RyzenAdj api.c; Krackan/Strix are marked "debug"). It is a **forced** GFX clock (min = max). The PSMU mailbox addresses are family-specific (e.g. `0x3B10A20/0x3B10A80/0x3B10A88`, or `0x3B10524/0x3B10570/0x3B10A40` on newer families). This belongs in the SMU layer shared with CPU TDP; see RyzenAdj `nb_smu_ops.c`.
- **Releasing the forced clock.** No "unforce" message is known. The practical release is to force the highest clock the APU supports, or to reboot / resume from sleep. After S3/S0ix the forced clock is lost, so it must be reapplied after resume. This is uncertain, so test on hardware.
- **Soft cap alternative (unverified on Windows).** Linux amdgpu caps APU GFX with MP1 messages `SetSoftMaxGfxClk` / `SetHardMinGfxClk` / `SetSoftMinGfxclk` (argument MHz):

  | SMU IP | Used for | SetSoftMinGfxclk | SetSoftMaxGfxClk | SetHardMinGfxClk |
  |---|---|---|---|---|
  | smu_v11_5 | Van Gogh | 0x0C | 0x1F | 0x20 |
  | smu_v12_0 | Renoir/Cezanne | n/a | 0x30 | 0x31 |
  | smu_v13_0_1 | Rembrandt-class | 0x09 | 0x1B | 0x1C |
  | smu_v13_0_4 | Phoenix-class | 0x09 | 0x1B | 0x1C |
  | smu_v13_0_5 | Mendocino | 8 | 20 | 21 |
  | smu_v14_0_0 | Strix/Krackan | 0x09 | 0x1B | 0x1C |

  The mapping of SMU IP versions to product names is approximate. On Windows the Adrenalin KMD also drives MP1, so these messages may be overridden and may race the driver's own mailbox use. Treat this as an experimental backend and verify with PM-table `gfx_clk`.
- **Old APUs.** Raven, Picasso, Dali and Lucienne have `set_max_gfxclk_freq` (0x46) and `set_min_gfxclk_freq` (0x47) via RyzenAdj, which is a real cap.

---

## 3. Intel

### 3.1 IGCL (ControlLib.dll)

- **Files.** `ControlLib.dll` is installed in System32 by the Intel graphics driver (Xe/Arc driver 30.0.101.1xxx and later). Header: github.com/intel/drivers.gpu.control-library `include/igcl_api.h` (v1-r1, CTL_IMPL 1.1). The static wrapper `Source/cApiWrapper.cpp` only does `LoadLibraryExW(L"ControlLib.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32)` and `GetProcAddress("ctlXxx")`, so resolving the same names yourself is equivalent.
- **License.** The "Intel Software License Agreement" (**not MIT**) allows use and redistribution of the headers **"solely for use on Intel platforms"**, with the notice kept. Redeclaring the minimal structs below avoids vendoring.
- **C pitfall.** `igcl_api.h` uses `bool` without including `<stdbool.h>`. In C, include `<stdbool.h>` first. `bool` is 1 byte.
- **Struct headers.** Every struct begins `uint32_t Size; uint8_t Version;`. Set `Size = sizeof`. Version is 0 unless noted.
  - `ctl_power_telemetry_t` with `Version = 1` fills the extra fields (`gpuEffectiveClock`, `gpuVrTemp`, ...). Version 0 fills the base set.
  - `ctlPowerTelemetryGet` is **rate-limited to 50 ms**. Calls within 50 ms return identical data.
- **Init.** `Size=sizeof, Version=0, AppVersion=CTL_MAKE_VERSION(1,1), flags=CTL_INIT_FLAG_USE_LEVEL_ZERO`, `ApplicationUID` zeroed. `USE_LEVEL_ZERO` is "usually required for telemetry". Leave out `CTL_INIT_FLAG_IGSC_FUL`, which is for firmware update. Then `ctlEnumerateDevices` (call it twice: count, then handles) and `ctlGetDeviceProperties`. Keep `pci_vendor_id == 0x8086` and `device_type == CTL_DEVICE_TYPE_GRAPHICS`. `graphics_adapter_properties & CTL_ADAPTER_PROPERTIES_FLAG_INTEGRATED` identifies an iGPU. `pDeviceID` can point at a `LUID` with `device_id_size = sizeof(LUID)` to get the adapter LUID, which D3DKMT correlation needs.
- **Frequency.**
  - `ctlEnumFrequencyDomains` → for each handle `ctlFrequencyGetProperties`. Keep `type == CTL_FREQ_DOMAIN_GPU` and check **`canControl`**. `min`/`max` are hardware MHz, where max is the non-OC max.
  - `ctlFrequencySetRange` with `min`/`max` in MHz (double). Semantics per the header: `min = 0` allows down to the hardware min; `min = -1` means factory min. `max = 0` or a huge value allows up to the hardware max; `max = -1` means factory max. On read, a negative value means no external limit. **To restore defaults write `{-1, -1}`.**
  - Returns `CTL_RESULT_ERROR_INSUFFICIENT_PERMISSIONS` if not admin.
  - `ctlFrequencyGetState`: `actual` = current MHz, `request`, `efficient`, `tdp`, and `throttleReasons` bitmask (bit0 average power cap, bit1 burst power cap, bit2 current, bit3 thermal, bit4 PSU, bit5 SW range, bit6 HW range).
- **iGPU support.**
  - **Arc dGPU:** frequency range, power limits and OC offset are supported.
  - **Iris Xe / Arc iGPU (TGL, ADL, MTL, LNL):** telemetry works. HandheldCompanion reads `ctlPowerTelemetryGet` (clock, global and render activity, energy, temperature) on MSI Claw MTL and LNL.
  - **Frequency set on iGPU is unconfirmed.** `canControl` may be false, or `SetRange` may return `CTL_RESULT_ERROR_UNSUPPORTED_FEATURE` (0x4000000a), depending on driver. Probe at runtime and fall back to §3.2 on pre-MTL parts.
  - `ctlEnumPowerDomains` usually returns 0 domains on iGPUs, because the package PL1/PL2 is the CPU's job.
  - `ctlOverclock*` is Arc dGPU only and requires a prior `ctlOverclockWaiverSet`. Don't use it for AutoTDP.
- **Busy % from telemetry.** `globalActivityCounter` and `renderComputeActivityCounter` are monotonic seconds (double). Busy = Δcounter / Δ`timeStamp` (seconds since epoch, 1 ms accuracy). Power = Δ`gpuEnergyCounter` (J) / Δt. Read each value with `item.type` (usually `CTL_DATA_TYPE_DOUBLE`) and `value.datadouble`.

Minimal declarations. Sizes were checked against the original header with mingw: init_args 0x24, adapter_properties 0x140 (name @0x58, num_xe_cores @0xCC), freq_properties 0x20, freq_range 0x18, freq_state 0x38, power_properties 0x14, power_limits 0x24, oc_telemetry_item 0x18, power_telemetry 0x400.

```c
#include <stdint.h>
#include <stdbool.h>
typedef int32_t ctl_result_t;
#define CTL_RESULT_SUCCESS 0
#define CTL_RESULT_ERROR_INSUFFICIENT_PERMISSIONS 0x40000006
#define CTL_RESULT_ERROR_UNSUPPORTED_FEATURE      0x4000000a
#define CTL_MAKE_VERSION(ma, mi) (((ma) << 16) | ((mi) & 0xffff))
#define CTL_INIT_FLAG_USE_LEVEL_ZERO 1u
#define CTL_ADAPTER_PROPERTIES_FLAG_INTEGRATED 1u
typedef struct _ctl_api_handle_t *ctl_api_handle_t;
typedef struct _ctl_device_adapter_handle_t *ctl_device_adapter_handle_t;
typedef struct _ctl_freq_handle_t *ctl_freq_handle_t;
typedef struct _ctl_pwr_handle_t *ctl_pwr_handle_t;
typedef struct _ctl_engine_handle_t *ctl_engine_handle_t;
typedef enum { CTL_FREQ_DOMAIN_GPU = 0, CTL_FREQ_DOMAIN_MEMORY = 1, CTL_FREQ_DOMAIN_MEDIA = 2 } ctl_freq_domain_t;
typedef enum { CTL_DEVICE_TYPE_GRAPHICS = 1, CTL_DEVICE_TYPE_SYSTEM = 2 } ctl_device_type_t;
typedef enum { CTL_UNITS_FREQUENCY_MHZ = 0, CTL_UNITS_VOLTAGE_VOLTS = 3, CTL_UNITS_POWER_WATTS = 4,
               CTL_UNITS_TEMPERATURE_CELSIUS = 5, CTL_UNITS_ENERGY_JOULES = 6, CTL_UNITS_TIME_SECONDS = 7,
               CTL_UNITS_PERCENT = 11, CTL_UNITS_UNKNOWN = 0x4800FFFF } ctl_units_t;
typedef enum { CTL_DATA_TYPE_INT32 = 4, CTL_DATA_TYPE_UINT32 = 5, CTL_DATA_TYPE_INT64 = 6, CTL_DATA_TYPE_UINT64 = 7,
               CTL_DATA_TYPE_FLOAT = 8, CTL_DATA_TYPE_DOUBLE = 9, CTL_DATA_TYPE_UNKNOWN = 0x4800FFFF } ctl_data_type_t;
typedef enum { CTL_PSU_TYPE_PSU_NONE = 0 } ctl_psu_type_t;
typedef enum { CTL_ENGINE_GROUP_GT = 0, CTL_ENGINE_GROUP_RENDER = 1, CTL_ENGINE_GROUP_MEDIA = 2 } ctl_engine_group_t;
typedef struct { uint32_t Data1; uint16_t Data2; uint16_t Data3; uint8_t Data4[8]; } ctl_application_id_t;
typedef struct {
    uint32_t Size; uint8_t Version; uint32_t AppVersion; uint32_t flags;
    uint32_t SupportedVersion; ctl_application_id_t ApplicationUID;
} ctl_init_args_t;
typedef struct { uint64_t major_version, minor_version, build_number; } ctl_firmware_version_t;
typedef struct { uint8_t bus, device, function; } ctl_adapter_bdf_t;
typedef struct {
    uint32_t Size; uint8_t Version; void *pDeviceID; uint32_t device_id_size;
    ctl_device_type_t device_type; uint32_t supported_subfunction_flags;
    uint64_t driver_version; ctl_firmware_version_t firmware_version;
    uint32_t pci_vendor_id, pci_device_id, rev_id;
    uint32_t num_eus_per_sub_slice, num_sub_slices_per_slice, num_slices;
    char name[100]; uint32_t graphics_adapter_properties; uint32_t Frequency;
    uint16_t pci_subsys_id, pci_subsys_vendor_id; ctl_adapter_bdf_t adapter_bdf;
    uint32_t num_xe_cores; char reserved[108];
} ctl_device_adapter_properties_t;
typedef struct { uint32_t Size; uint8_t Version; ctl_freq_domain_t type; bool canControl; double min, max; } ctl_freq_properties_t;
typedef struct { uint32_t Size; uint8_t Version; double min, max; } ctl_freq_range_t;
typedef struct { uint32_t Size; uint8_t Version; double currentVoltage, request, tdp, efficient, actual; uint32_t throttleReasons; } ctl_freq_state_t;
typedef struct { uint32_t Size; uint8_t Version; bool canControl; int32_t defaultLimit, minLimit, maxLimit; } ctl_power_properties_t;
typedef struct { bool enabled; int32_t power; int32_t interval; } ctl_power_sustained_limit_t;   /* mW, ms */
typedef struct { bool enabled; int32_t power; } ctl_power_burst_limit_t;
typedef struct { int32_t powerAC; int32_t powerDC; } ctl_power_peak_limit_t;
typedef struct { uint32_t Size; uint8_t Version; ctl_power_sustained_limit_t sustainedPowerLimit;
                 ctl_power_burst_limit_t burstPowerLimit; ctl_power_peak_limit_t peakPowerLimits; } ctl_power_limits_t;
typedef struct { uint32_t Size; uint8_t Version; uint64_t energy; uint64_t timestamp; } ctl_power_energy_counter_t;
typedef struct { uint32_t Size; uint8_t Version; ctl_engine_group_t type; } ctl_engine_properties_t;
typedef struct { uint32_t Size; uint8_t Version; uint64_t activeTime; uint64_t timestamp; } ctl_engine_stats_t;
typedef union { int8_t data8; uint8_t datau8; int16_t data16; uint16_t datau16; int32_t data32; uint32_t datau32;
                int64_t data64; uint64_t datau64; float datafloat; double datadouble; } ctl_data_value_t;
typedef struct { bool bSupported; ctl_units_t units; ctl_data_type_t type; ctl_data_value_t value; } ctl_oc_telemetry_item_t;
typedef struct { bool bSupported; ctl_psu_type_t psuType; ctl_oc_telemetry_item_t energyCounter, voltage; } ctl_psu_info_t;
typedef struct {
    uint32_t Size; uint8_t Version;
    ctl_oc_telemetry_item_t timeStamp, gpuEnergyCounter, gpuVoltage, gpuCurrentClockFrequency, gpuCurrentTemperature,
                            globalActivityCounter, renderComputeActivityCounter, mediaActivityCounter;
    bool gpuPowerLimited, gpuTemperatureLimited, gpuCurrentLimited, gpuVoltageLimited, gpuUtilizationLimited;
    ctl_oc_telemetry_item_t vramEnergyCounter, vramVoltage, vramCurrentClockFrequency, vramCurrentEffectiveFrequency,
                            vramReadBandwidthCounter, vramWriteBandwidthCounter, vramCurrentTemperature;
    bool vramPowerLimited, vramTemperatureLimited, vramCurrentLimited, vramVoltageLimited, vramUtilizationLimited;
    ctl_oc_telemetry_item_t totalCardEnergyCounter;
    ctl_psu_info_t psu[5];
    ctl_oc_telemetry_item_t fanSpeed[5];
    ctl_oc_telemetry_item_t gpuVrTemp, vramVrTemp, saVrTemp, gpuEffectiveClock, gpuOverVoltagePercent,
                            gpuPowerPercent, gpuTemperaturePercent, vramReadBandwidth, vramWriteBandwidth;
} ctl_power_telemetry_t;
typedef ctl_result_t (__cdecl *PFN_ctlInit)(ctl_init_args_t *, ctl_api_handle_t *);
typedef ctl_result_t (__cdecl *PFN_ctlClose)(ctl_api_handle_t);
typedef ctl_result_t (__cdecl *PFN_ctlEnumerateDevices)(ctl_api_handle_t, uint32_t *, ctl_device_adapter_handle_t *);
typedef ctl_result_t (__cdecl *PFN_ctlGetDeviceProperties)(ctl_device_adapter_handle_t, ctl_device_adapter_properties_t *);
typedef ctl_result_t (__cdecl *PFN_ctlEnumFrequencyDomains)(ctl_device_adapter_handle_t, uint32_t *, ctl_freq_handle_t *);
typedef ctl_result_t (__cdecl *PFN_ctlFrequencyGetProperties)(ctl_freq_handle_t, ctl_freq_properties_t *);
typedef ctl_result_t (__cdecl *PFN_ctlFrequencyGetRange)(ctl_freq_handle_t, ctl_freq_range_t *);
typedef ctl_result_t (__cdecl *PFN_ctlFrequencySetRange)(ctl_freq_handle_t, const ctl_freq_range_t *);
typedef ctl_result_t (__cdecl *PFN_ctlFrequencyGetState)(ctl_freq_handle_t, ctl_freq_state_t *);
typedef ctl_result_t (__cdecl *PFN_ctlEnumPowerDomains)(ctl_device_adapter_handle_t, uint32_t *, ctl_pwr_handle_t *);
typedef ctl_result_t (__cdecl *PFN_ctlPowerGetProperties)(ctl_pwr_handle_t, ctl_power_properties_t *);
typedef ctl_result_t (__cdecl *PFN_ctlPowerGetEnergyCounter)(ctl_pwr_handle_t, ctl_power_energy_counter_t *);
typedef ctl_result_t (__cdecl *PFN_ctlPowerGetLimits)(ctl_pwr_handle_t, ctl_power_limits_t *);
typedef ctl_result_t (__cdecl *PFN_ctlPowerSetLimits)(ctl_pwr_handle_t, const ctl_power_limits_t *);
typedef ctl_result_t (__cdecl *PFN_ctlPowerTelemetryGet)(ctl_device_adapter_handle_t, ctl_power_telemetry_t *);
typedef ctl_result_t (__cdecl *PFN_ctlOverclockGpuFrequencyOffsetSet)(ctl_device_adapter_handle_t, double mhz);
typedef ctl_result_t (__cdecl *PFN_ctlOverclockGpuFrequencyOffsetGet)(ctl_device_adapter_handle_t, double *mhz);
typedef ctl_result_t (__cdecl *PFN_ctlEnumEngineGroups)(ctl_device_adapter_handle_t, uint32_t *, ctl_engine_handle_t *);
typedef ctl_result_t (__cdecl *PFN_ctlEngineGetProperties)(ctl_engine_handle_t, ctl_engine_properties_t *);
typedef ctl_result_t (__cdecl *PFN_ctlEngineGetActivity)(ctl_engine_handle_t, ctl_engine_stats_t *);
```

`ctlEngineGetActivity`: busy = ΔactiveTime / Δtimestamp. Both are in µs per Level-Zero sysman semantics; the unit is assumed and should be verified. This is a cheaper per-engine alternative to power telemetry.

### 3.2 iGPU cap via MCHBAR+0x5994

- **Register.** From Linux `include/drm/intel/mchbar_regs.h` (the i915 MCHBAR mirror sits at GTTMMADR+0x140000 and aliases MCHBAR+0x0000):

| MCHBAR offset | Name | Fields |
|---|---|---|
| 0x5948 | `GEN6_GT_PERF_STATUS` | current GT ratio (read-only) |
| **0x5994** | **`GEN6_RP_STATE_LIMITS`** | **bits 7:0 = RP0 limit, i.e. the maximum GT frequency ratio the PCU may grant** |
| 0x5998 | `GEN6_RP_STATE_CAP` | bits 7:0 RP0 (max), 15:8 RP1, 23:16 RPn (min) |
| 0x59A0 | `PCU_PACKAGE_RAPL_LIMIT` | PL1/PL2 |

- **Units.** 50 MHz per ratio step on Gen9 big core (SKL/KBL/CFL/CML) and on Gen11/Gen12 (ICL/TGL/ADL/RPL). i915 multiplies by `GEN9_FREQ_SCALER` = 3 to convert to its internal 16.67 MHz unit. Gen9-LP (BXT/GLK/APL) uses a different layout, `BXT_RP_STATE_CAP` in GT MMIO 0x138170.
- **Behaviour.** A single byte write to MCHBAR+0x5994. Default 0xFF means "no cap", because the PCU clamps to RP0. Writing N caps the iGPU at N × 50 MHz; e.g. 0x0C = 600 MHz. Read RP0/RPn from 0x5998 so the UI range is right. It is not reset by the graphics driver in practice (that is why the tool works), but **it is lost on S3/S4**, so re-apply on resume.
- **MCHBAR base.** PCI 0:0.0 config 0x48 (low dword; bit 0 = enable) and 0x4C (high). Mask the low bits (`& ~0x7FFFull`).
- **Meteor Lake / Lunar Lake / Arrow Lake: unverified.** GT frequency moved to GuC SLPC and the GT caps moved to GT MMIO `MTL_RP_STATE_CAP` 0x138000 (RP0 bits 8:0, RPn bits 24:16, units 16.67 MHz). Whether MCHBAR+0x5994 still caps the Xe-LPG/Xe2 iGPU is unknown. Prefer IGCL there, and verify with `ctlFrequencyGetState.actual`.
- **Last resort.** Soft limits in GT MMIO: `GEN6_RP_INTERRUPT_LIMITS` 0xA014 and `GEN6_RPNSWREQ` 0xA008 (ratio in bits 31:23 on gen9+). These need GTTMMADR (BAR0 of 0:2.0) plus forcewake, and the i915-equivalent Windows KMD overwrites them. Do not use.

### 3.3 Intel graphics power-plan setting

The "Intel Graphics Power Plan" powercfg setting is subgroup `44f3beca-a7c0-460e-9df2-bb8b99e0cba6`, setting `3619c3f2-afb2-4afc-b0e9-e7fef372de36`. Values: 0 = Max Battery, 1 = Balanced, 2 = Max Performance. Set it with `PowerWriteACValueIndex` / `PowerWriteDCValueIndex` + `PowerSetActiveScheme` (powrprof.dll).

---

## 4. Vendor-neutral GPU utilization

### 4.1 D3DKMTQueryStatistics (recommended)

- **Cost.** About microseconds per call. It does not wake a D3cold dGPU, needs no admin, and works for every WDDM adapter. This is the method System Informer uses (`plugins/ExtendedTools/gpumon.c`).
- **Exports.** `gdi32.dll` exports `D3DKMTQueryStatistics`, `D3DKMTEnumAdapters2`, `D3DKMTOpenAdapterFromLuid`, `D3DKMTQueryAdapterInfo` and `D3DKMTCloseAdapter`, all returning `NTSTATUS`. mingw-w64 has no `d3dkmthk.h` with these structs, so declare them yourself.
- **Algorithm.**
  1. `D3DKMTEnumAdapters2` (call twice) to get LUIDs.
  2. `QUERYSTATISTICS_ADAPTER` to get `NodeCount`.
  3. For each node, `D3DKMTQueryAdapterInfo(KMTQAITYPE_NODEMETADATA=25)` with `NodeOrdinalAndAdapterIndex = node` to get `EngineType`.
  4. Every tick, `QUERYSTATISTICS_NODE` returns `GlobalInformation.RunningTime` in **100 ns** units, cumulative. util(node) = ΔRunningTime / Δwall(100 ns, from `QueryPerformanceCounter`). GPU busy = **max** over 3D nodes, which matches Task Manager's per-engine view. Clamp to [0, 1].
- **Layout.** Offsets verified by compiling the WDK-derived header with mingw x64: `sizeof(D3DKMT_QUERYSTATISTICS) = 0x328`. A zeroed 0x328-byte blob with fields at these offsets is enough:

```c
#include <windows.h>
typedef LONG NTSTATUS;
typedef UINT D3DKMT_HANDLE;
enum { D3DKMT_QUERYSTATISTICS_ADAPTER = 0, D3DKMT_QUERYSTATISTICS_SEGMENT = 3, D3DKMT_QUERYSTATISTICS_NODE = 5 };
enum { KMTQAITYPE_NODEMETADATA = 25 };
enum { DXGK_ENGINE_TYPE_OTHER = 0, DXGK_ENGINE_TYPE_3D = 1, DXGK_ENGINE_TYPE_VIDEO_DECODE = 2,
       DXGK_ENGINE_TYPE_VIDEO_ENCODE = 3, DXGK_ENGINE_TYPE_VIDEO_PROCESSING = 4, DXGK_ENGINE_TYPE_SCENE_ASSEMBLY = 5,
       DXGK_ENGINE_TYPE_COPY = 6, DXGK_ENGINE_TYPE_OVERLAY = 7, DXGK_ENGINE_TYPE_CRYPTO = 8 };
typedef struct {                      /* x64: 0x328 bytes */
    UINT   Type;                      /* 0x000 */
    LUID   AdapterLuid;               /* 0x004 */
    HANDLE hProcess;                  /* 0x010 */
    BYTE   QueryResult[0x308];        /* 0x018 */
    ULONG  QueryId;                   /* 0x320  NodeId / SegmentId */
    ULONG  pad;
} PX_D3DKMT_QUERYSTATISTICS;
#define PX_QS_ADAPTER_NBSEGMENTS   0x018   /* ULONG, in QueryResult for TYPE_ADAPTER */
#define PX_QS_ADAPTER_NODECOUNT    0x01C   /* ULONG */
#define PX_QS_NODE_RUNNINGTIME     0x018   /* LARGE_INTEGER, GlobalInformation.RunningTime, 100ns */
#define PX_QS_NODE_SYS_RUNNINGTIME 0x128   /* LARGE_INTEGER, SystemInformation.RunningTime */
#pragma pack(push, 1)
typedef struct {                      /* 0x4E bytes, pack(1) as in the WDK */
    UINT  NodeOrdinalAndAdapterIndex;
    UINT  EngineType;
    WCHAR FriendlyName[32];
    UINT  Flags;
    BYTE  GpuMmuSupported, IoMmuSupported;
} PX_D3DKMT_NODEMETADATA;
#pragma pack(pop)
typedef struct { D3DKMT_HANDLE hAdapter; UINT Type; void *pPrivateDriverData; UINT PrivateDriverDataSize; } PX_D3DKMT_QUERYADAPTERINFO;   /* 0x18 */
typedef struct { D3DKMT_HANDLE hAdapter; LUID AdapterLuid; ULONG NumOfSources; BOOL bPrecisePresentRegionsPreferred; } PX_D3DKMT_ADAPTERINFO; /* 0x14 */
typedef struct { ULONG NumAdapters; PX_D3DKMT_ADAPTERINFO *pAdapters; } PX_D3DKMT_ENUMADAPTERS2;       /* 0x10 */
typedef struct { LUID AdapterLuid; D3DKMT_HANDLE hAdapter; } PX_D3DKMT_OPENADAPTERFROMLUID;           /* 0x0C */
typedef struct { D3DKMT_HANDLE hAdapter; } PX_D3DKMT_CLOSEADAPTER;
typedef NTSTATUS (APIENTRY *PFN_D3DKMTQueryStatistics)(const PX_D3DKMT_QUERYSTATISTICS *);
typedef NTSTATUS (APIENTRY *PFN_D3DKMTEnumAdapters2)(PX_D3DKMT_ENUMADAPTERS2 *);
typedef NTSTATUS (APIENTRY *PFN_D3DKMTOpenAdapterFromLuid)(PX_D3DKMT_OPENADAPTERFROMLUID *);
typedef NTSTATUS (APIENTRY *PFN_D3DKMTQueryAdapterInfo)(const PX_D3DKMT_QUERYADAPTERINFO *);
typedef NTSTATUS (APIENTRY *PFN_D3DKMTCloseAdapter)(const PX_D3DKMT_CLOSEADAPTER *);
```

- **Vendor correlation.** Map D3DKMT adapters to vendor handles by PCI VEN (`D3DKMTQueryAdapterInfo KMTQAITYPE_PHYSICALADAPTERDEVICEIDS`) or by LUID. IGCL exposes the LUID through `pDeviceID`. DXGI `IDXGIAdapter1::GetDesc1` gives both LUID and VendorId and is the simplest. Skip Microsoft Basic Render Driver (VEN 1414).

### 4.2 PDH "\GPU Engine(*engtype_3D)\Utilization Percentage" (not recommended for the fast loop)

- **Usage.** `PdhOpenQueryW` → `PdhAddEnglishCounterW(q, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &c)` → `PdhCollectQueryData` twice → `PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, ...)`.
- **Instance names.** Instances look like `pid_1234_luid_0x00000000_0x0000D1F2_phys_0_eng_0_engtype_3D`. Sum per `(luid, eng)` across pids, then take the max over engines.
- **Cost.** The provider enumerates every process × engine on each collect. Expect milliseconds of CPU per sample with many processes, heap churn, and wildcard instances that change as processes come and go. The count is unmeasured here; expect around 1–10 ms. Task Manager uses it at 1 Hz. Only use it if D3DKMT fails.

---

## 5. Pluggable backend interface (extensibility)

One vtable per backend, probed in priority order. Adding new hardware means one new `.c` file and one table entry.

```c
typedef struct px_gpu px_gpu;
typedef struct {
    const char *name;
    int  (*probe)(px_gpu *out, int max);                  /* returns count found */
    int  (*get_range)(px_gpu *, int *min_mhz, int *max_mhz);
    int  (*set_max_clock)(px_gpu *, int mhz);             /* 0 = restore default */
    int  (*set_min_clock)(px_gpu *, int mhz);             /* optional, NULL if n/a */
    int  (*read)(px_gpu *, int *cur_mhz, int *busy_pct_x10, int *power_mw, int *temp_c);
    int  (*set_power_limit)(px_gpu *, int mw_or_pct);     /* optional */
    void (*restore)(px_gpu *);                            /* called on exit / crash / suspend */
    void (*shutdown)(void);
} px_gpu_ops;
struct px_gpu { const px_gpu_ops *ops; LUID luid; uint16_t ven, dev; uint8_t integrated; void *h; int cap_mhz, dflt_max; };
/* order: nvml, nvapi, adlx, adl_od8, amd_smu(apu), igcl, intel_mchbar; telemetry fallback: d3dkmt */
```

AutoTDP notes for GPU control:
- Only call `set_max_clock` when the target moves by at least one hardware step (NVIDIA 15 MHz; AMD/Intel 50 MHz is a sane quantum).
- Rate-limit writes to about 10 Hz for NVML and IGCL, and about 4 Hz for ADLX, ADL and SMU.
- Read busy % from D3DKMT at the loop rate, not from vendor APIs, which average over 100 ms to 1 s.
- Always run `restore()` on exit, on `WM_POWERBROADCAST`/`PBT_APMSUSPEND` and in the crash filter. Re-apply on `PBT_APMRESUMEAUTOMATIC`.

---

## Sources

- NVML header, API v13: https://raw.githubusercontent.com/NVIDIA/nvidia-settings/main/src/nvml.h
- nvidia-smi docs: https://docs.nvidia.com/deploy/nvidia-smi/index.html
- NvAPI (MIT): https://github.com/NVIDIA/nvapi (`nvapi.h`, `nvapi_interface.h`). SetPstates20 ID: https://github.com/tokkenno/nvapi.net/wiki/NvAPI-Functions , https://github.com/Alia5/NvAPI-Overclocking
- ADLX: https://github.com/GPUOpen-LibrariesAndSDKs/ADLX `SDK/Include/{ADLX.h,ADLXDefines.h,ADLXVersion.h,ADLXStructures.h,ISystem.h,ICollections.h,IGPUTuning.h,IGPUManualGFXTuning.h,IGPUManualPowerTuning.h,IPerformanceMonitoring.h,I3DSettings.h}`. License: "ADLX SDK License Agreement.pdf"
- ADL (MIT): https://github.com/GPUOpen-LibrariesAndSDKs/display-library `include/adl_defines.h`, `adl_structures.h`, `adl_sdk.h`, `Sample/Overdrive8/Overdrive8.cpp`
- LibreHardwareMonitor ADL interop: https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/blob/master/LibreHardwareMonitorLib/Interop/AtiAdlxx.cs
- RyzenAdj: https://github.com/FlyGoat/RyzenAdj `lib/api.c` (set_gfx_clk 0x89, get_gfx_clk offsets), `lib/nb_smu_ops.c`
- Linux amdgpu SMU message IDs: https://github.com/torvalds/linux/tree/master/drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if
- Intel IGCL: https://github.com/intel/drivers.gpu.control-library (`include/igcl_api.h`, `Source/cApiWrapper.cpp`, `Samples/Telemetry_Samples/Sample_TelemetryAPP.cpp`, `License.txt`)
- HandheldCompanion (IGCL telemetry on MSI Claw; ADLX usage): https://github.com/Valkirie/HandheldCompanion
- Linux i915 MCHBAR/RPS: https://github.com/torvalds/linux/blob/master/include/drm/intel/mchbar_regs.h , `drivers/gpu/drm/i915/gt/intel_rps.c`, `drivers/gpu/drm/i915/i915_reg.h`
- System Informer GPU monitor and D3DKMT headers: https://github.com/winsiderss/systeminformer `plugins/ExtendedTools/gpumon.c`, `plugins/ExtendedTools/d3dkmt/d3dkmthk.h`
