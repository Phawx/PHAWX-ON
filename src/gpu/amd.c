#include "phawx.h"
#include <setupapi.h>

/* ---------- ADLX (amdadlx64.dll) ---------- */
typedef int ADLX_RESULT;
enum { ADLX_OK = 0, ADLX_BAD_VER = 5 };
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
typedef struct IADLX3DSettingsServices IADLX3DSettingsServices;
typedef struct IADLX3DAntiLag IADLX3DAntiLag;
typedef struct IADLX3DChill IADLX3DChill;
typedef struct IADLX3DBoost IADLX3DBoost;
typedef struct IADLX3DImageSharpening IADLX3DImageSharpening;
typedef struct IADLX3DFrameRateTargetControl IADLX3DFrameRateTargetControl;
typedef struct IADLX3DRadeonSuperResolution IADLX3DRadeonSuperResolution;

typedef ADLX_RESULT (__cdecl *PFN_ADLXInitialize)(uint64_t version, IADLXSystem **ppSystem);
typedef ADLX_RESULT (__cdecl *PFN_ADLXTerminate)(void);

#define ADLX_IUNKNOWN(T) \
    adlx_long   (__stdcall *Acquire)(T *); \
    adlx_long   (__stdcall *Release)(T *); \
    ADLX_RESULT (__stdcall *QueryInterface)(T *, const wchar_t *iid, void **pp);

typedef struct { ADLX_IUNKNOWN(IADLXInterface) } IADLXInterfaceVtbl;
struct IADLXInterface { const IADLXInterfaceVtbl *pVtbl; };

typedef struct {
    ADLX_RESULT (__stdcall *GetHybridGraphicsType)(IADLXSystem *, int *);
    ADLX_RESULT (__stdcall *GetGPUs)(IADLXSystem *, IADLXGPUList **);
    ADLX_RESULT (__stdcall *QueryInterface)(IADLXSystem *, const wchar_t *, void **);
    ADLX_RESULT (__stdcall *GetDisplaysServices)(IADLXSystem *, void **);
    ADLX_RESULT (__stdcall *GetDesktopsServices)(IADLXSystem *, void **);
    ADLX_RESULT (__stdcall *GetGPUsChangedHandling)(IADLXSystem *, void **);
    ADLX_RESULT (__stdcall *EnableLog)(IADLXSystem *, int, int, void *, const wchar_t *);
    ADLX_RESULT (__stdcall *Get3DSettingsServices)(IADLXSystem *, IADLX3DSettingsServices **);
    ADLX_RESULT (__stdcall *GetGPUTuningServices)(IADLXSystem *, IADLXGPUTuningServices **);
    ADLX_RESULT (__stdcall *GetPerformanceMonitoringServices)(IADLXSystem *, IADLXPerformanceMonitoringServices **);
} IADLXSystemVtbl;
struct IADLXSystem { const IADLXSystemVtbl *pVtbl; };

typedef struct {
    ADLX_IUNKNOWN(IADLXGPU)
    ADLX_RESULT (__stdcall *VendorId)(IADLXGPU *, const char **);
    ADLX_RESULT (__stdcall *ASICFamilyType)(IADLXGPU *, int *);
    ADLX_RESULT (__stdcall *Type)(IADLXGPU *, int *);
    ADLX_RESULT (__stdcall *IsExternal)(IADLXGPU *, adlx_bool *);
    ADLX_RESULT (__stdcall *Name)(IADLXGPU *, const char **);
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
} IADLXManualGraphicsTuning2_1Vtbl;
struct IADLXManualGraphicsTuning2_1 { const IADLXManualGraphicsTuning2_1Vtbl *pVtbl; };

typedef struct {
    ADLX_IUNKNOWN(IADLXManualPowerTuning)
    ADLX_RESULT (__stdcall *GetPowerLimitRange)(IADLXManualPowerTuning *, ADLX_IntRange *);
    ADLX_RESULT (__stdcall *GetPowerLimit)(IADLXManualPowerTuning *, adlx_int *);
    ADLX_RESULT (__stdcall *SetPowerLimit)(IADLXManualPowerTuning *, adlx_int);
} IADLXManualPowerTuningVtbl;
struct IADLXManualPowerTuning { const IADLXManualPowerTuningVtbl *pVtbl; };

typedef struct {
    ADLX_IUNKNOWN(IADLXPerformanceMonitoringServices)
    ADLX_RESULT (__stdcall *GetSamplingIntervalRange)(IADLXPerformanceMonitoringServices *, ADLX_IntRange *);
    ADLX_RESULT (__stdcall *SetSamplingInterval)(IADLXPerformanceMonitoringServices *, adlx_int);
    ADLX_RESULT (__stdcall *GetSamplingInterval)(IADLXPerformanceMonitoringServices *, adlx_int *);
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
} IADLXPerformanceMonitoringServicesVtbl;
struct IADLXPerformanceMonitoringServices { const IADLXPerformanceMonitoringServicesVtbl *pVtbl; };

typedef struct {
    ADLX_IUNKNOWN(IADLXGPUMetrics)
    ADLX_RESULT (__stdcall *TimeStamp)(IADLXGPUMetrics *, int64_t *);
    ADLX_RESULT (__stdcall *GPUUsage)(IADLXGPUMetrics *, double *);
    ADLX_RESULT (__stdcall *GPUClockSpeed)(IADLXGPUMetrics *, adlx_int *);
    ADLX_RESULT (__stdcall *GPUVRAMClockSpeed)(IADLXGPUMetrics *, adlx_int *);
    ADLX_RESULT (__stdcall *GPUTemperature)(IADLXGPUMetrics *, double *);
    ADLX_RESULT (__stdcall *GPUHotspotTemperature)(IADLXGPUMetrics *, double *);
    ADLX_RESULT (__stdcall *GPUPower)(IADLXGPUMetrics *, double *);
    ADLX_RESULT (__stdcall *GPUTotalBoardPower)(IADLXGPUMetrics *, double *);
} IADLXGPUMetricsVtbl;
struct IADLXGPUMetrics { const IADLXGPUMetricsVtbl *pVtbl; };

#define S3D_GET(n, T) ADLX_RESULT (__stdcall *n)(IADLX3DSettingsServices *, IADLXGPU *, T **);
typedef struct {
    ADLX_IUNKNOWN(IADLX3DSettingsServices)
    S3D_GET(GetAntiLag, IADLX3DAntiLag)
    S3D_GET(GetChill, IADLX3DChill)
    S3D_GET(GetBoost, IADLX3DBoost)
    S3D_GET(GetImageSharpening, IADLX3DImageSharpening)
    S3D_GET(GetEnhancedSync, IADLXInterface)
    S3D_GET(GetWaitForVerticalRefresh, IADLXInterface)
    S3D_GET(GetFrameRateTargetControl, IADLX3DFrameRateTargetControl)
    S3D_GET(GetAntiAliasing, IADLXInterface)
    S3D_GET(GetMorphologicalAntiAliasing, IADLXInterface)
    S3D_GET(GetAnisotropicFiltering, IADLXInterface)
    S3D_GET(GetTessellation, IADLXInterface)
    ADLX_RESULT (__stdcall *GetRadeonSuperResolution)(IADLX3DSettingsServices *, IADLX3DRadeonSuperResolution **);
} IADLX3DSettingsServicesVtbl;
struct IADLX3DSettingsServices { const IADLX3DSettingsServicesVtbl *pVtbl; };

#define S3D_HEAD(T) \
    ADLX_IUNKNOWN(T) \
    ADLX_RESULT (__stdcall *IsSupported)(T *, adlx_bool *); \
    ADLX_RESULT (__stdcall *IsEnabled)(T *, adlx_bool *);

typedef struct {
    S3D_HEAD(IADLX3DAntiLag)
    ADLX_RESULT (__stdcall *SetEnabled)(IADLX3DAntiLag *, adlx_bool);
} IADLX3DAntiLagVtbl;
struct IADLX3DAntiLag { const IADLX3DAntiLagVtbl *pVtbl; };

typedef struct {
    S3D_HEAD(IADLX3DChill)
    ADLX_RESULT (__stdcall *GetFPSRange)(IADLX3DChill *, ADLX_IntRange *);
    ADLX_RESULT (__stdcall *GetMinFPS)(IADLX3DChill *, adlx_int *);
    ADLX_RESULT (__stdcall *GetMaxFPS)(IADLX3DChill *, adlx_int *);
    ADLX_RESULT (__stdcall *SetEnabled)(IADLX3DChill *, adlx_bool);
    ADLX_RESULT (__stdcall *SetMinFPS)(IADLX3DChill *, adlx_int);
    ADLX_RESULT (__stdcall *SetMaxFPS)(IADLX3DChill *, adlx_int);
} IADLX3DChillVtbl;
struct IADLX3DChill { const IADLX3DChillVtbl *pVtbl; };

typedef struct {
    S3D_HEAD(IADLX3DBoost)
    ADLX_RESULT (__stdcall *GetResolutionRange)(IADLX3DBoost *, ADLX_IntRange *);
    ADLX_RESULT (__stdcall *GetResolution)(IADLX3DBoost *, adlx_int *);
    ADLX_RESULT (__stdcall *SetEnabled)(IADLX3DBoost *, adlx_bool);
    ADLX_RESULT (__stdcall *SetResolution)(IADLX3DBoost *, adlx_int);
} IADLX3DBoostVtbl;
struct IADLX3DBoost { const IADLX3DBoostVtbl *pVtbl; };

typedef struct {
    S3D_HEAD(IADLX3DImageSharpening)
    ADLX_RESULT (__stdcall *GetSharpnessRange)(IADLX3DImageSharpening *, ADLX_IntRange *);
    ADLX_RESULT (__stdcall *GetSharpness)(IADLX3DImageSharpening *, adlx_int *);
    ADLX_RESULT (__stdcall *SetEnabled)(IADLX3DImageSharpening *, adlx_bool);
    ADLX_RESULT (__stdcall *SetSharpness)(IADLX3DImageSharpening *, adlx_int);
} IADLX3DImageSharpeningVtbl;
struct IADLX3DImageSharpening { const IADLX3DImageSharpeningVtbl *pVtbl; };

typedef struct {
    S3D_HEAD(IADLX3DFrameRateTargetControl)
    ADLX_RESULT (__stdcall *GetFPSRange)(IADLX3DFrameRateTargetControl *, ADLX_IntRange *);
    ADLX_RESULT (__stdcall *GetFPS)(IADLX3DFrameRateTargetControl *, adlx_int *);
    ADLX_RESULT (__stdcall *SetEnabled)(IADLX3DFrameRateTargetControl *, adlx_bool);
    ADLX_RESULT (__stdcall *SetFPS)(IADLX3DFrameRateTargetControl *, adlx_int);
} IADLX3DFrameRateTargetControlVtbl;
struct IADLX3DFrameRateTargetControl { const IADLX3DFrameRateTargetControlVtbl *pVtbl; };

typedef struct {
    S3D_HEAD(IADLX3DRadeonSuperResolution)
    ADLX_RESULT (__stdcall *SetEnabled)(IADLX3DRadeonSuperResolution *, adlx_bool);
} IADLX3DRadeonSuperResolutionVtbl;
struct IADLX3DRadeonSuperResolution { const IADLX3DRadeonSuperResolutionVtbl *pVtbl; };

#define REL(p) do { if (p) { (p)->pVtbl->Release(p); (p) = NULL; } } while (0)

/* ---------- ADL (atiadlxx.dll) ---------- */
typedef void *ADL_CONTEXT_HANDLE;
typedef void *(__stdcall *ADL_MAIN_MALLOC_CALLBACK)(int);
#define ADL_OK 0
#define ADL_MAX_PATH 256
#define ADL_PMLOG_MAX_SENSORS 256
#define OD8_COUNT 77
enum { OD8_GFXCLK_FMAX = 0, OD8_GFXCLK_FMIN = 1, OD8_GFXCLK_FREQ1 = 2, OD8_GFXCLK_FREQ3 = 6,
       OD8_UCLK_FMAX = 8, OD8_POWER_PERCENTAGE = 9, OD8_OPTIMZED_POWER_MODE = 36 };
#define ADL_OD8_GFXCLK_LIMITS            (1 << 0)
#define ADL_OD8_GFXCLK_CURVE             (1 << 1)
#define ADL_OD8_POWER_LIMIT              (1 << 3)
#define ADL_OD8_OPTIMIZED_GPU_POWER_MODE (1 << 16)
#define ADL_ASIC_INTEGRATED              (1 << 1)
enum { ADL_PMLOG_CLK_GFXCLK = 1, ADL_PMLOG_TEMPERATURE_EDGE = 8, ADL_PMLOG_INFO_ACTIVITY_GFX = 19,
       ADL_PMLOG_ASIC_POWER = 23, ADL_PMLOG_BOARD_POWER = 73 };
typedef struct {
    int iSize, iAdapterIndex; char strUDID[ADL_MAX_PATH];
    int iBusNumber, iDeviceNumber, iFunctionNumber, iVendorID;
    char strAdapterName[ADL_MAX_PATH], strDisplayName[ADL_MAX_PATH];
    int iPresent, iExist;
    char strDriverPath[ADL_MAX_PATH], strDriverPathExt[ADL_MAX_PATH], strPNPString[ADL_MAX_PATH];
    int iOSDisplayIndex;
} AdapterInfo;
typedef struct { int featureID, minValue, maxValue, defaultValue; } ADLOD8SingleInitSetting;
typedef struct { int count; int overdrive8Capabilities; ADLOD8SingleInitSetting od8SettingTable[OD8_COUNT]; } ADLOD8InitSetting;
typedef struct { int count; int Od8SettingTable[OD8_COUNT]; } ADLOD8CurrentSetting;
typedef struct { int value, requested, reset; } ADLOD8SingleSetSetting;
typedef struct { int count; ADLOD8SingleSetSetting od8SettingTable[OD8_COUNT]; } ADLOD8SetSetting;
typedef struct { int supported; int value; } ADLSingleSensorData;
typedef struct { int size; ADLSingleSensorData sensors[ADL_PMLOG_MAX_SENSORS]; } ADLPMLogDataOutput;

typedef int (*PFN_ADL2_Main_Control_Create)(ADL_MAIN_MALLOC_CALLBACK, int, ADL_CONTEXT_HANDLE *);
typedef int (*PFN_ADL2_Main_Control_Destroy)(ADL_CONTEXT_HANDLE);
typedef int (*PFN_ADL2_Adapter_NumberOfAdapters_Get)(ADL_CONTEXT_HANDLE, int *);
typedef int (*PFN_ADL2_Adapter_AdapterInfo_Get)(ADL_CONTEXT_HANDLE, AdapterInfo *, int);
typedef int (*PFN_ADL2_Adapter_ASICFamilyType_Get)(ADL_CONTEXT_HANDLE, int, int *, int *);
typedef int (*PFN_ADL2_Overdrive_Caps)(ADL_CONTEXT_HANDLE, int, int *, int *, int *);
typedef int (*PFN_ADL2_Overdrive8_Init_Setting_Get)(ADL_CONTEXT_HANDLE, int, ADLOD8InitSetting *);
typedef int (*PFN_ADL2_Overdrive8_Current_Setting_Get)(ADL_CONTEXT_HANDLE, int, ADLOD8CurrentSetting *);
typedef int (*PFN_ADL2_Overdrive8_Setting_Set)(ADL_CONTEXT_HANDLE, int, ADLOD8SetSetting *, ADLOD8CurrentSetting *);
typedef int (*PFN_ADL2_New_QueryPMLogData_Get)(ADL_CONTEXT_HANDLE, int, ADLPMLogDataOutput *);

/* ---------- state ---------- */
enum { M_NONE, M_ADLX, M_ADL };

#define MAX_GPUS 4
#define MAX_KEYS 4
#define CLK_GAP  100

static CRITICAL_SECTION cs;
static int cs_ok;

static struct {
    HMODULE lib;
    PFN_ADLXTerminate term;
    IADLXSystem *sys;
    IADLXGPU *gpu[MAX_GPUS];
    int ngpu;
    IADLXGPU *dgpu;
    IADLXGPUTuningServices *tun;
    IADLXManualGraphicsTuning2 *mgt;
    IADLXManualPowerTuning *mpt;
    IADLXPerformanceMonitoringServices *perf;
    IADLX3DSettingsServices *s3d;
} x;

static struct {
    HMODULE lib;
    ADL_CONTEXT_HANDLE ctx;
    int idx, caps;
    ADLOD8InitSetting init;
    PFN_ADL2_Main_Control_Destroy destroy;
    PFN_ADL2_Overdrive8_Current_Setting_Get cur;
    PFN_ADL2_Overdrive8_Setting_Set set;
    PFN_ADL2_New_QueryPMLogData_Get pmlog;
} a;

static int mode;
static int have_clk, have_plim;
static int max_lo, max_hi, min_lo, min_hi, def_min, def_max;
static int plim_lo, plim_hi, plim_orig, plim_written;
static int user_min, user_max, clk_min, clk_max;
static int app_min = -1, app_max = -1, applied, pmode_set;
static uint64_t reopen_at;
static wchar_t dev_name[64];

static wchar_t keys[MAX_KEYS][128];
static int nkeys;

static int s_mhz = -1, s_load = -1, s_mw = -1, s_temp = -1;
static uint64_t s_ms;

enum { C_HDR, C_NAME, C_MHZ, C_LOAD, C_PWR, C_TEMP, C_MAX, C_MIN, C_PLIM,
       C_RHDR, C_CHILL, C_CHILLMIN, C_CHILLMAX, C_FRTC, C_FRTCFPS, C_ANTILAG, C_BOOST, C_BOOSTRES,
       C_RSR, C_RIS, C_SHARP, C_RESTART, C_N };

static ph_ctl ctls[C_N];

static void lk(void) { EnterCriticalSection(&cs); }
static void ulk(void) { LeaveCriticalSection(&cs); }

/* ---------- vendor open/close ---------- */
static void adlx_close(void)
{
    REL(x.mgt);
    REL(x.mpt);
    REL(x.tun);
    REL(x.perf);
    REL(x.s3d);
    for (int i = 0; i < x.ngpu; i++) REL(x.gpu[i]);
    x.ngpu = 0;
    x.dgpu = NULL;
    if (x.sys && x.term) x.term();
    x.sys = NULL;
    if (x.lib) FreeLibrary(x.lib);
    memset(&x, 0, sizeof x);
}

static void adl_close(void)
{
    if (a.ctx && a.destroy) a.destroy(a.ctx);
    if (a.lib) FreeLibrary(a.lib);
    memset(&a, 0, sizeof a);
}

static void set_name(const char *s)
{
    if (!s || !*s || !MultiByteToWideChar(CP_UTF8, 0, s, -1, dev_name, PH_ARRAY(dev_name)))
        lstrcpynW(dev_name, L"AMD Radeon GPU", PH_ARRAY(dev_name));
}

static int adlx_open(void)
{
    x.lib = LoadLibraryExW(L"amdadlx64.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!x.lib) return -1;
    PFN_ADLXInitialize init = (PFN_ADLXInitialize)(void *)GetProcAddress(x.lib, "ADLXInitialize");
    PFN_ADLXInitialize init_old = (PFN_ADLXInitialize)(void *)GetProcAddress(x.lib, "ADLXInitializeWithIncompatibleDriver");
    x.term = (PFN_ADLXTerminate)(void *)GetProcAddress(x.lib, "ADLXTerminate");
    if (!init || !x.term) { adlx_close(); return -1; }
    ADLX_RESULT r = init(ADLX_FULL_VERSION, &x.sys);
    if (r == ADLX_BAD_VER && init_old) r = init_old(ADLX_FULL_VERSION, &x.sys);
    if (r != ADLX_OK || !x.sys) { x.sys = NULL; adlx_close(); return -1; }

    IADLXGPUList *list = NULL;
    if (x.sys->pVtbl->GetGPUs(x.sys, &list) == ADLX_OK && list) {
        adlx_uint b = list->pVtbl->Begin(list), e = list->pVtbl->End(list);
        for (adlx_uint i = b; i != e && x.ngpu < MAX_GPUS; i++) {
            IADLXGPU *g = NULL;
            if (list->pVtbl->At_GPUList(list, i, &g) != ADLX_OK || !g) continue;
            int t = 0;
            g->pVtbl->Type(g, &t);
            x.gpu[x.ngpu++] = g;
            if (t == 2 && !x.dgpu) x.dgpu = g;
        }
        REL(list);
    }
    if (!x.ngpu) { adlx_close(); return -1; }
    if (x.sys->pVtbl->Get3DSettingsServices(x.sys, &x.s3d) != ADLX_OK) x.s3d = NULL;
    if (!x.dgpu) return 0;

    const char *nm = NULL;
    if (x.dgpu->pVtbl->Name(x.dgpu, &nm) == ADLX_OK) set_name(nm);
    if (x.sys->pVtbl->GetPerformanceMonitoringServices(x.sys, &x.perf) != ADLX_OK) x.perf = NULL;
    if (x.sys->pVtbl->GetGPUTuningServices(x.sys, &x.tun) != ADLX_OK || !x.tun) { x.tun = NULL; return 0; }

    adlx_bool ok = 0;
    IADLXInterface *itf = NULL;
    if (x.tun->pVtbl->IsSupportedManualGFXTuning(x.tun, x.dgpu, &ok) == ADLX_OK && ok &&
        x.tun->pVtbl->GetManualGFXTuning(x.tun, x.dgpu, &itf) == ADLX_OK && itf) {
        IADLXManualGraphicsTuning2_1 *m21 = NULL;
        if (itf->pVtbl->QueryInterface(itf, L"IADLXManualGraphicsTuning2", (void **)&x.mgt) != ADLX_OK) x.mgt = NULL;
        if (x.mgt) {
            ADLX_IntRange rmin = { 0 }, rmax = { 0 };
            adlx_int cmin = 0, cmax = 0, dmin = 0, dmax = 0;
            if (x.mgt->pVtbl->GetGPUMinFrequencyRange(x.mgt, &rmin) == ADLX_OK &&
                x.mgt->pVtbl->GetGPUMaxFrequencyRange(x.mgt, &rmax) == ADLX_OK &&
                rmax.maxValue > rmax.minValue && rmin.maxValue > rmin.minValue) {
                x.mgt->pVtbl->GetGPUMinFrequency(x.mgt, &cmin);
                x.mgt->pVtbl->GetGPUMaxFrequency(x.mgt, &cmax);
                if (itf->pVtbl->QueryInterface(itf, L"IADLXManualGraphicsTuning2_1", (void **)&m21) == ADLX_OK && m21) {
                    m21->pVtbl->GetGPUMinFrequencyDefault(m21, &dmin);
                    m21->pVtbl->GetGPUMaxFrequencyDefault(m21, &dmax);
                    REL(m21);
                }
                if (!def_max) {
                    def_max = dmax > 0 ? dmax : cmax;
                    def_min = dmin > 0 ? dmin : cmin;
                }
                max_lo = rmax.minValue; max_hi = rmax.maxValue;
                min_lo = rmin.minValue; min_hi = rmin.maxValue;
                if (def_max <= 0 || def_max > max_hi) def_max = max_hi;
                if (def_min < min_lo || def_min > min_hi) def_min = min_lo;
                have_clk = 1;
            } else {
                REL(x.mgt);
            }
        }
        REL(itf);
    }

    ok = 0;
    if (x.tun->pVtbl->IsSupportedManualPowerTuning(x.tun, x.dgpu, &ok) == ADLX_OK && ok &&
        x.tun->pVtbl->GetManualPowerTuning(x.tun, x.dgpu, &itf) == ADLX_OK && itf) {
        if (itf->pVtbl->QueryInterface(itf, L"IADLXManualPowerTuning", (void **)&x.mpt) != ADLX_OK) x.mpt = NULL;
        REL(itf);
        ADLX_IntRange r = { 0 };
        adlx_int cur = 0;
        if (x.mpt && x.mpt->pVtbl->GetPowerLimitRange(x.mpt, &r) == ADLX_OK && r.maxValue > r.minValue &&
            x.mpt->pVtbl->GetPowerLimit(x.mpt, &cur) == ADLX_OK) {
            plim_lo = r.minValue;
            plim_hi = r.maxValue;
            if (!plim_written) plim_orig = cur;
            have_plim = 1;
        } else {
            REL(x.mpt);
        }
    }
    return 0;
}

static void *__stdcall adl_malloc(int n) { return ph_alloc(n > 0 ? (size_t)n : 1); }

#define ASYM(T, s) ((T)(void *)GetProcAddress(a.lib, s))

static int adl_open(void)
{
    a.lib = LoadLibraryExW(L"atiadlxx.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!a.lib) return -1;
    PFN_ADL2_Main_Control_Create create = ASYM(PFN_ADL2_Main_Control_Create, "ADL2_Main_Control_Create");
    PFN_ADL2_Adapter_NumberOfAdapters_Get num = ASYM(PFN_ADL2_Adapter_NumberOfAdapters_Get, "ADL2_Adapter_NumberOfAdapters_Get");
    PFN_ADL2_Adapter_AdapterInfo_Get info = ASYM(PFN_ADL2_Adapter_AdapterInfo_Get, "ADL2_Adapter_AdapterInfo_Get");
    PFN_ADL2_Adapter_ASICFamilyType_Get asic = ASYM(PFN_ADL2_Adapter_ASICFamilyType_Get, "ADL2_Adapter_ASICFamilyType_Get");
    PFN_ADL2_Overdrive_Caps caps = ASYM(PFN_ADL2_Overdrive_Caps, "ADL2_Overdrive_Caps");
    PFN_ADL2_Overdrive8_Init_Setting_Get initget = ASYM(PFN_ADL2_Overdrive8_Init_Setting_Get, "ADL2_Overdrive8_Init_Setting_Get");
    a.destroy = ASYM(PFN_ADL2_Main_Control_Destroy, "ADL2_Main_Control_Destroy");
    a.cur = ASYM(PFN_ADL2_Overdrive8_Current_Setting_Get, "ADL2_Overdrive8_Current_Setting_Get");
    a.set = ASYM(PFN_ADL2_Overdrive8_Setting_Set, "ADL2_Overdrive8_Setting_Set");
    a.pmlog = ASYM(PFN_ADL2_New_QueryPMLogData_Get, "ADL2_New_QueryPMLogData_Get");
    if (!create || !a.destroy || !num || !info || !caps || !initget || !a.cur || !a.set ||
        create(adl_malloc, 1, &a.ctx) != ADL_OK) {
        a.ctx = NULL;
        adl_close();
        return -1;
    }
    int n = 0;
    if (num(a.ctx, &n) != ADL_OK || n <= 0 || n > 64) { adl_close(); return -1; }
    AdapterInfo *ai = ph_alloc(sizeof *ai * (size_t)n);
    if (!ai) { adl_close(); return -1; }
    int found = -1;
    if (info(a.ctx, ai, (int)sizeof *ai * n) == ADL_OK) {
        for (int i = 0; i < n && found < 0; i++) {
            if (ai[i].iVendorID != 0x1002) continue;
            int dup = 0;
            for (int k = 0; k < i; k++)
                if (ai[k].iBusNumber == ai[i].iBusNumber && ai[k].iDeviceNumber == ai[i].iDeviceNumber &&
                    ai[k].iFunctionNumber == ai[i].iFunctionNumber) { dup = 1; break; }
            if (dup) continue;
            int idx = ai[i].iAdapterIndex, t = 0, valid = 0;
            if (asic && asic(a.ctx, idx, &t, &valid) == ADL_OK && (t & valid & ADL_ASIC_INTEGRATED)) continue;
            int sup = 0, en = 0, ver = 0;
            if (caps(a.ctx, idx, &sup, &en, &ver) != ADL_OK || !sup || ver != 8) continue;
            memset(&a.init, 0, sizeof a.init);
            a.init.count = OD8_COUNT;
            if (initget(a.ctx, idx, &a.init) != ADL_OK) continue;
            a.caps = a.init.overdrive8Capabilities;
            if (!(a.caps & (ADL_OD8_GFXCLK_LIMITS | ADL_OD8_GFXCLK_CURVE | ADL_OD8_POWER_LIMIT))) continue;
            found = idx;
            set_name(ai[i].strAdapterName);
        }
    }
    ph_free(ai);
    if (found < 0) { adl_close(); return -1; }
    a.idx = found;

    ADLOD8SingleInitSetting *fx = &a.init.od8SettingTable[OD8_GFXCLK_FMAX];
    ADLOD8SingleInitSetting *fn = &a.init.od8SettingTable[OD8_GFXCLK_FMIN];
    if ((a.caps & (ADL_OD8_GFXCLK_LIMITS | ADL_OD8_GFXCLK_CURVE)) && fx->maxValue > fx->minValue) {
        max_lo = fx->minValue; max_hi = fx->maxValue;
        min_lo = fn->minValue; min_hi = fn->maxValue > fn->minValue ? fn->maxValue : fx->maxValue;
        def_max = fx->defaultValue > 0 ? fx->defaultValue : max_hi;
        def_min = fn->defaultValue > 0 ? fn->defaultValue : min_lo;
        def_max = PH_CLAMP(def_max, max_lo, max_hi);
        def_min = PH_CLAMP(def_min, min_lo, min_hi);
        have_clk = 1;
    }
    ADLOD8SingleInitSetting *pp = &a.init.od8SettingTable[OD8_POWER_PERCENTAGE];
    if (!have_plim && (a.caps & ADL_OD8_POWER_LIMIT) && pp->maxValue > pp->minValue) {
        plim_lo = pp->minValue;
        plim_hi = pp->maxValue;
        if (!plim_written) plim_orig = pp->defaultValue;
        have_plim = 1;
    }
    return 0;
}

static void vendor_close(void)
{
    adlx_close();
    adl_close();
    mode = M_NONE;
}

static int vendor_open(void)
{
    have_clk = have_plim = 0;
    int xo = adlx_open() == 0;
    if (xo) mode = M_ADLX;
    if (xo && (!x.dgpu || have_clk)) return 0;
    if (adl_open() == 0) {
        if (!xo) mode = M_ADL;
        return 0;
    }
    return xo ? 0 : -1;
}

static int use_adl(void) { return a.ctx != NULL; }

/* ---------- OD8 ---------- */
static int od8_write(const int *ids, const int *vals, int n, int reset)
{
    ADLOD8CurrentSetting cur;
    ADLOD8SetSetting s;
    memset(&cur, 0, sizeof cur);
    memset(&s, 0, sizeof s);
    cur.count = OD8_COUNT;
    if (a.cur(a.ctx, a.idx, &cur) != ADL_OK) return -1;
    s.count = OD8_COUNT;
    for (int i = 0; i < OD8_COUNT; i++) s.od8SettingTable[i].value = cur.Od8SettingTable[i];
    for (int k = 0; k < n; k++) {
        int id = ids[k];
        if (id < 0 || id >= OD8_COUNT) continue;
        s.od8SettingTable[id].value = reset ? a.init.od8SettingTable[id].defaultValue : vals[k];
        s.od8SettingTable[id].requested = 1;
        s.od8SettingTable[id].reset = reset;
    }
    return a.set(a.ctx, a.idx, &s, &cur) == ADL_OK ? 0 : -1;
}

static int od8_clocks(int mn, int mx, int reset)
{
    int ids[12], vals[12], n = 0;
    if ((a.caps & ADL_OD8_OPTIMIZED_GPU_POWER_MODE) && (reset ? pmode_set : !pmode_set)) {
        int id = OD8_OPTIMZED_POWER_MODE, v = 3;
        if (od8_write(&id, &v, 1, reset) == 0) pmode_set = !reset;
    }
    ids[n] = OD8_GFXCLK_FMAX; vals[n++] = mx;
    ids[n] = OD8_GFXCLK_FMIN; vals[n++] = mn;
    if ((a.caps & ADL_OD8_GFXCLK_CURVE) && !(a.caps & ADL_OD8_GFXCLK_LIMITS) && reset) {
        ids[n] = OD8_GFXCLK_FREQ1; vals[n++] = mn;
        ids[n] = OD8_GFXCLK_FREQ3; vals[n++] = mx;
    } else if ((a.caps & ADL_OD8_GFXCLK_CURVE) && !(a.caps & ADL_OD8_GFXCLK_LIMITS)) {
        ADLOD8CurrentSetting cur;
        memset(&cur, 0, sizeof cur);
        cur.count = OD8_COUNT;
        if (a.cur(a.ctx, a.idx, &cur) != ADL_OK) return -1;
        for (int id = OD8_GFXCLK_FREQ1; id <= OD8_UCLK_FMAX; id++) {
            ids[n] = id;
            vals[n++] = id == OD8_GFXCLK_FREQ1 ? mn : id == OD8_GFXCLK_FREQ3 ? mx : cur.Od8SettingTable[id];
        }
    }
    return od8_write(ids, vals, n, reset);
}

/* ---------- clock application ---------- */
static int hw_clocks(int mn, int mx, int reset)
{
    if (mode == M_ADLX && x.mgt) {
        int r = 0;
        if (app_min < 0 || app_max < 0) {
            adlx_int cmn = 0, cmx = 0;
            x.mgt->pVtbl->GetGPUMinFrequency(x.mgt, &cmn);
            x.mgt->pVtbl->GetGPUMaxFrequency(x.mgt, &cmx);
            app_min = cmn;
            app_max = cmx;
        }
        if (mx < app_min) {
            if (x.mgt->pVtbl->SetGPUMinFrequency(x.mgt, mn) != ADLX_OK) r = -1;
            if (x.mgt->pVtbl->SetGPUMaxFrequency(x.mgt, mx) != ADLX_OK) r = -1;
        } else {
            if (mx != app_max && x.mgt->pVtbl->SetGPUMaxFrequency(x.mgt, mx) != ADLX_OK) r = -1;
            if (mn != app_min && x.mgt->pVtbl->SetGPUMinFrequency(x.mgt, mn) != ADLX_OK) r = -1;
        }
        return r;
    }
    if (use_adl()) return od8_clocks(mn, mx, reset);
    return -1;
}

static int apply_clk(void);
static int hw_plim(int v, int reset);

static void ensure_open(void)
{
    if (mode == M_NONE && reopen_at && ph_ms() >= reopen_at) {
        reopen_at = 0;
        if (vendor_open() != 0) { reopen_at = ph_ms() + 5000; return; }
        if (applied) { app_min = app_max = -1; apply_clk(); }
        if (plim_written && ctls[C_PLIM].active) hw_plim(ctls[C_PLIM].val, 0);
    }
}

static int apply_clk(void)
{
    ensure_open();
    if (!have_clk) return -1;
    int mx = 0, mn;
    if (user_max > 0) mx = user_max;
    if (clk_max > 0 && (!mx || clk_max < mx)) mx = clk_max;
    mn = clk_min > 0 ? clk_min : user_min;
    if (!mx && !mn) {
        if (!applied) return 0;
        if (hw_clocks(def_min, def_max, 1)) return -1;
        applied = 0;
        app_min = def_min;
        app_max = def_max;
        return 0;
    }
    if (!mx) mx = def_max;
    mx = PH_CLAMP(mx, max_lo, def_max);
    if (!mn) mn = def_min;
    if (mn > mx - CLK_GAP) mn = mx - CLK_GAP;
    mn = PH_CLAMP(mn, min_lo, min_hi);
    if (mn > mx) mn = min_lo;
    if (applied && mn == app_min && mx == app_max) return 0;
    if (hw_clocks(mn, mx, 0)) { app_min = app_max = -1; return -1; }
    applied = 1;
    app_min = mn;
    app_max = mx;
    return 0;
}

static int hw_plim(int v, int reset)
{
    ensure_open();
    if (!have_plim) return -1;
    if (mode == M_ADLX && x.mpt) return x.mpt->pVtbl->SetPowerLimit(x.mpt, v) == ADLX_OK ? 0 : -1;
    if (use_adl()) {
        int id = OD8_POWER_PERCENTAGE;
        return od8_write(&id, &v, 1, reset);
    }
    return -1;
}

/* ---------- telemetry ---------- */
static void sample(int max_age)
{
    uint64_t now = ph_ms();
    if (s_ms && now - s_ms < (uint64_t)max_age) return;
    s_ms = now;
    s_mhz = s_load = s_mw = s_temp = -1;
    if (mode == M_ADLX && x.perf && x.dgpu) {
        IADLXGPUMetrics *m = NULL;
        if (x.perf->pVtbl->GetCurrentGPUMetrics(x.perf, x.dgpu, &m) == ADLX_OK && m) {
            adlx_int mhz = 0;
            double d = 0;
            if (m->pVtbl->GPUClockSpeed(m, &mhz) == ADLX_OK) s_mhz = mhz;
            if (m->pVtbl->GPUUsage(m, &d) == ADLX_OK) s_load = PH_CLAMP((int)(d + 0.5), 0, 100);
            if (m->pVtbl->GPUTotalBoardPower(m, &d) == ADLX_OK && d > 0) s_mw = (int)(d * 1000.0);
            else if (m->pVtbl->GPUPower(m, &d) == ADLX_OK && d > 0) s_mw = (int)(d * 1000.0);
            if (m->pVtbl->GPUTemperature(m, &d) == ADLX_OK && d > 0) s_temp = (int)(d + 0.5);
            REL(m);
        }
    } else if (use_adl() && a.pmlog) {
        ADLPMLogDataOutput *o = ph_alloc(sizeof *o);
        if (o) {
            if (a.pmlog(a.ctx, a.idx, o) == ADL_OK) {
                ADLSingleSensorData *s = o->sensors;
                if (s[ADL_PMLOG_CLK_GFXCLK].supported) s_mhz = s[ADL_PMLOG_CLK_GFXCLK].value;
                if (s[ADL_PMLOG_INFO_ACTIVITY_GFX].supported) s_load = PH_CLAMP(s[ADL_PMLOG_INFO_ACTIVITY_GFX].value, 0, 100);
                if (s[ADL_PMLOG_BOARD_POWER].supported && s[ADL_PMLOG_BOARD_POWER].value > 0) s_mw = s[ADL_PMLOG_BOARD_POWER].value * 1000;
                else if (s[ADL_PMLOG_ASIC_POWER].supported) s_mw = s[ADL_PMLOG_ASIC_POWER].value * 1000;
                if (s[ADL_PMLOG_TEMPERATURE_EDGE].supported) s_temp = s[ADL_PMLOG_TEMPERATURE_EDGE].value;
            }
            ph_free(o);
        }
    }
}

static int get_s(int *v, int32_t *o) { lk(); sample(500); *o = *v; ulk(); return *o < 0 ? -1 : 0; }
static int get_mhz(ph_ctl *c, int32_t *o) { return get_s(&s_mhz, o); }
static int get_load(ph_ctl *c, int32_t *o) { return get_s(&s_load, o); }
static int get_mw(ph_ctl *c, int32_t *o) { return get_s(&s_mw, o); }
static int get_temp(ph_ctl *c, int32_t *o) { return get_s(&s_temp, o); }

static void fmt_temp(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    if (v < 0) lstrcpynW(b, L"--", n);
    else ph_swprintf(b, n, L"%d \x00B0" L"C", v);
}

static void fmt_name(const ph_ctl *c, int32_t v, wchar_t *b, int n) { lstrcpynW(b, dev_name, n); }

static void fmt_plim(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    ph_swprintf(b, n, v > 0 ? L"+%d%%" : L"%d%%", v);
}

static int set_clk(ph_ctl *c, int32_t v)
{
    lk();
    int om = user_min, ox = user_max;
    if (c->arg) user_max = v; else user_min = v;
    int r = apply_clk();
    if (r) { user_min = om; user_max = ox; }
    ulk();
    return r;
}

static int set_plim(ph_ctl *c, int32_t v)
{
    lk();
    int r = hw_plim(v, 0);
    if (!r) plim_written = 1;
    ulk();
    return r;
}

/* ---------- Radeon driver settings ---------- */
enum { RS_CHILL, RS_CHILLMIN, RS_CHILLMAX, RS_FRTC, RS_FRTCFPS, RS_ANTILAG, RS_BOOST, RS_BOOSTRES,
       RS_RSR, RS_RIS, RS_SHARP };

static const wchar_t *const rs_val[] = {
    [RS_CHILL] = L"KMD_ChillEnabled", [RS_CHILLMIN] = L"KMD_ChillMinFps", [RS_CHILLMAX] = L"KMD_ChillMaxFps",
    [RS_FRTC] = L"KMD_FRTEnabled", [RS_FRTCFPS] = L"KMD_MaxFrameRateRequested",
    [RS_ANTILAG] = L"KMD_DeLagEnabled", [RS_BOOST] = L"KMD_RadeonBoostEnabled",
    [RS_BOOSTRES] = L"KMD_RadeonBoostMinResolution", [RS_RSR] = L"Gmx_RSREnabled",
    [RS_RIS] = L"KMD_USUEnable", [RS_SHARP] = L"KMD_USUSharpeningDegree",
};

static const int boost_res[] = { 50, 67, 84 };
static const wchar_t *const boost_choices[] = { L"50%", L"67%", L"84%", NULL };
static int restart_pending;

static DWORD rs_encode(int id, int32_t v)
{
    switch (id) {
    case RS_RSR: return v ? 3 : 0;
    case RS_BOOSTRES: return (DWORD)boost_res[PH_CLAMP(v, 0, 2)];
    case RS_SHARP: {
        float f = (float)PH_CLAMP(v, 0, 100) / 100.0f;
        DWORD d;
        memcpy(&d, &f, sizeof d);
        return d;
    }
    default: return (DWORD)v;
    }
}

static int32_t rs_decode(int id, DWORD d)
{
    switch (id) {
    case RS_RSR: return d ? 1 : 0;
    case RS_BOOSTRES: return d <= 58 ? 0 : d <= 75 ? 1 : 2;
    case RS_SHARP: {
        float f;
        memcpy(&f, &d, sizeof f);
        if (!(f >= 0.0f && f <= 1.0f)) return 50;
        return (int32_t)(f * 100.0f + 0.5f);
    }
    case RS_CHILL: case RS_FRTC: case RS_ANTILAG: case RS_BOOST: case RS_RIS: return d ? 1 : 0;
    default: return (int32_t)d;
    }
}

static int clamp_range(const ADLX_IntRange *r, int v)
{
    if (r->maxValue > r->minValue) return PH_CLAMP(v, r->minValue, r->maxValue);
    return v;
}

static int rs_live_gpu(IADLXGPU *g, int id, int32_t v)
{
    IADLX3DSettingsServices *s = x.s3d;
    adlx_bool sup = 0;
    ADLX_IntRange rg = { 0 };
    int ok = 0;
    switch (id) {
    case RS_ANTILAG: {
        IADLX3DAntiLag *p = NULL;
        if (s->pVtbl->GetAntiLag(s, g, &p) != ADLX_OK || !p) return 0;
        if (p->pVtbl->IsSupported(p, &sup) == ADLX_OK && sup) ok = p->pVtbl->SetEnabled(p, v != 0) == ADLX_OK;
        REL(p);
        break;
    }
    case RS_CHILL: case RS_CHILLMIN: case RS_CHILLMAX: {
        IADLX3DChill *p = NULL;
        if (s->pVtbl->GetChill(s, g, &p) != ADLX_OK || !p) return 0;
        if (p->pVtbl->IsSupported(p, &sup) == ADLX_OK && sup) {
            p->pVtbl->GetFPSRange(p, &rg);
            if (id == RS_CHILL) ok = p->pVtbl->SetEnabled(p, v != 0) == ADLX_OK;
            else if (id == RS_CHILLMIN) ok = p->pVtbl->SetMinFPS(p, clamp_range(&rg, v)) == ADLX_OK;
            else ok = p->pVtbl->SetMaxFPS(p, clamp_range(&rg, v)) == ADLX_OK;
        }
        REL(p);
        break;
    }
    case RS_FRTC: case RS_FRTCFPS: {
        IADLX3DFrameRateTargetControl *p = NULL;
        if (s->pVtbl->GetFrameRateTargetControl(s, g, &p) != ADLX_OK || !p) return 0;
        if (p->pVtbl->IsSupported(p, &sup) == ADLX_OK && sup) {
            p->pVtbl->GetFPSRange(p, &rg);
            if (id == RS_FRTC) ok = p->pVtbl->SetEnabled(p, v != 0) == ADLX_OK;
            else ok = p->pVtbl->SetFPS(p, clamp_range(&rg, v)) == ADLX_OK;
        }
        REL(p);
        break;
    }
    case RS_BOOST: case RS_BOOSTRES: {
        IADLX3DBoost *p = NULL;
        if (s->pVtbl->GetBoost(s, g, &p) != ADLX_OK || !p) return 0;
        if (p->pVtbl->IsSupported(p, &sup) == ADLX_OK && sup) {
            p->pVtbl->GetResolutionRange(p, &rg);
            if (id == RS_BOOST) ok = p->pVtbl->SetEnabled(p, v != 0) == ADLX_OK;
            else ok = p->pVtbl->SetResolution(p, clamp_range(&rg, boost_res[PH_CLAMP(v, 0, 2)])) == ADLX_OK;
        }
        REL(p);
        break;
    }
    case RS_RIS: case RS_SHARP: {
        IADLX3DImageSharpening *p = NULL;
        if (s->pVtbl->GetImageSharpening(s, g, &p) != ADLX_OK || !p) return 0;
        if (p->pVtbl->IsSupported(p, &sup) == ADLX_OK && sup) {
            p->pVtbl->GetSharpnessRange(p, &rg);
            if (id == RS_RIS) ok = p->pVtbl->SetEnabled(p, v != 0) == ADLX_OK;
            else ok = p->pVtbl->SetSharpness(p, clamp_range(&rg, v)) == ADLX_OK;
        }
        REL(p);
        break;
    }
    }
    return ok;
}

static int rs_live(int id, int32_t v)
{
    ensure_open();
    if (mode != M_ADLX || !x.s3d) return 0;
    int ok = 0;
    if (id == RS_RSR) {
        IADLX3DRadeonSuperResolution *p = NULL;
        adlx_bool sup = 0;
        if (x.s3d->pVtbl->GetRadeonSuperResolution(x.s3d, &p) == ADLX_OK && p) {
            if (p->pVtbl->IsSupported(p, &sup) == ADLX_OK && sup) ok = p->pVtbl->SetEnabled(p, v != 0) == ADLX_OK;
            REL(p);
        }
        return ok;
    }
    for (int i = 0; i < x.ngpu; i++) ok |= rs_live_gpu(x.gpu[i], id, v);
    return ok;
}

static int rs_get(ph_ctl *c, int32_t *o)
{
    DWORD d;
    for (int i = 0; i < nkeys; i++)
        if (reg_get_dword(HKEY_LOCAL_MACHINE, keys[i], rs_val[c->arg], &d) == 0) {
            *o = rs_decode(c->arg, d);
            return 0;
        }
    return -1;
}

static int rs_set(ph_ctl *c, int32_t v)
{
    int n = 0;
    DWORD d = rs_encode(c->arg, v);
    for (int i = 0; i < nkeys; i++) {
        DWORD old;
        int same = reg_get_dword(HKEY_LOCAL_MACHINE, keys[i], rs_val[c->arg], &old) == 0 && old == d;
        if (same || reg_set_dword(HKEY_LOCAL_MACHINE, keys[i], rs_val[c->arg], d) == 0) {
            n++;
            if (!same && c->arg == RS_FRTC && v)
                reg_set_dword(HKEY_LOCAL_MACHINE, keys[i], L"KMD_FRTCInitialStatus", 1);
            if (!same) restart_pending = 1;
        }
    }
    lk();
    int live = rs_live(c->arg, v);
    ulk();
    if (live) restart_pending = 0;
    return n || live ? 0 : -1;
}

static const GUID guid_display = { 0x4d36e968, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };

static int act_restart(ph_ctl *c, int32_t v)
{
    HDEVINFO h = SetupDiGetClassDevsW(&guid_display, NULL, NULL, DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE) return -1;
    lk();
    vendor_close();
    reopen_at = 0;
    ulk();
    int n = 0;
    SP_DEVINFO_DATA d;
    d.cbSize = sizeof d;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(h, i, &d); i++) {
        wchar_t hw[512];
        memset(hw, 0, sizeof hw);
        if (!SetupDiGetDeviceRegistryPropertyW(h, &d, SPDRP_HARDWAREID, NULL, (BYTE *)hw, sizeof hw - 2 * sizeof(wchar_t), NULL))
            continue;
        CharUpperW(hw);
        if (!wcsstr(hw, L"VEN_1002")) continue;
        SP_PROPCHANGE_PARAMS p;
        memset(&p, 0, sizeof p);
        p.ClassInstallHeader.cbSize = sizeof p.ClassInstallHeader;
        p.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        p.StateChange = DICS_PROPCHANGE;
        p.Scope = DICS_FLAG_GLOBAL;
        if (SetupDiSetClassInstallParamsW(h, &d, &p.ClassInstallHeader, sizeof p) &&
            SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, h, &d))
            n++;
    }
    SetupDiDestroyDeviceInfoList(h);
    lk();
    app_min = app_max = -1;
    pmode_set = 0;
    s_ms = 0;
    reopen_at = ph_ms() + 4000;
    ulk();
    if (n) restart_pending = 0;
    return n ? 0 : -1;
}

static void fmt_restart(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    lstrcpynW(b, restart_pending ? L"Needed" : L"", n);
}

/* ---------- controls ---------- */
#define GF (CF_OPTIONAL | CF_PROFILE)
#define RF (CF_OPTIONAL | CF_PROFILE)

static ph_ctl ctls[C_N] = {
    [C_HDR] = { .label = L"AMD Radeon GPU", .type = CT_HEADER, .page = PG_GPU, .order = 100 },
    [C_NAME] = { .label = L"Adapter", .type = CT_INFO, .page = PG_GPU, .fmt = fmt_name, .order = 101 },
    [C_MHZ] = { .label = L"GPU clock", .type = CT_INFO, .page = PG_GPU, .get = get_mhz, .fmt = fmt_mhz, .order = 102 },
    [C_LOAD] = { .label = L"GPU load", .type = CT_INFO, .page = PG_GPU, .get = get_load, .fmt = fmt_pct, .order = 103 },
    [C_PWR] = { .label = L"GPU power", .type = CT_INFO, .page = PG_GPU, .get = get_mw, .fmt = fmt_watts_mw, .order = 104 },
    [C_TEMP] = { .label = L"GPU temperature", .type = CT_INFO, .page = PG_GPU, .get = get_temp, .fmt = fmt_temp, .order = 105 },
    [C_MAX] = { .key = "amdgpu.maxclk", .label = L"Max GPU clock", .type = CT_SLIDER, .page = PG_GPU,
                .flags = GF | CF_AUTOTDP, .step = 25, .unit = L"MHz", .set = set_clk, .fmt = fmt_mhz,
                .arg = 1, .order = 106 },
    [C_MIN] = { .key = "amdgpu.minclk", .label = L"Min GPU clock", .type = CT_SLIDER, .page = PG_GPU,
                .flags = GF | CF_AUTOTDP | CF_ADVANCED, .step = 25, .unit = L"MHz", .set = set_clk,
                .fmt = fmt_mhz, .arg = 0, .order = 107 },
    [C_PLIM] = { .key = "amdgpu.power", .label = L"GPU power limit", .type = CT_SLIDER, .page = PG_GPU,
                 .flags = GF | CF_SIGNED, .step = 1, .unit = L"%", .set = set_plim, .fmt = fmt_plim, .order = 108 },

    [C_RHDR] = { .label = L"Radeon settings", .type = CT_HEADER, .page = PG_GPU, .order = 150 },
    [C_CHILL] = { .key = "radeon.chill", .label = L"Radeon Chill", .type = CT_TOGGLE, .page = PG_GPU,
                  .flags = RF, .get = rs_get, .set = rs_set, .fmt = fmt_onoff, .arg = RS_CHILL, .order = 151 },
    [C_CHILLMIN] = { .key = "radeon.chillmin", .label = L"Chill min FPS", .type = CT_SLIDER, .page = PG_GPU,
                     .flags = RF, .min = 30, .max = 300, .step = 1, .def = 30, .unit = L"FPS",
                     .get = rs_get, .set = rs_set, .arg = RS_CHILLMIN, .order = 152 },
    [C_CHILLMAX] = { .key = "radeon.chillmax", .label = L"Chill max FPS", .type = CT_SLIDER, .page = PG_GPU,
                     .flags = RF, .min = 30, .max = 300, .step = 1, .def = 60, .unit = L"FPS",
                     .get = rs_get, .set = rs_set, .arg = RS_CHILLMAX, .order = 153 },
    [C_FRTC] = { .key = "radeon.frtc", .label = L"Frame rate target", .type = CT_TOGGLE, .page = PG_GPU,
                 .flags = RF, .get = rs_get, .set = rs_set, .fmt = fmt_onoff, .arg = RS_FRTC, .order = 154 },
    [C_FRTCFPS] = { .key = "radeon.frtcfps", .label = L"Target FPS", .type = CT_SLIDER, .page = PG_GPU,
                    .flags = RF, .min = 30, .max = 300, .step = 1, .def = 60, .unit = L"FPS",
                    .get = rs_get, .set = rs_set, .arg = RS_FRTCFPS, .order = 155 },
    [C_ANTILAG] = { .key = "radeon.antilag", .label = L"Radeon Anti-Lag", .type = CT_TOGGLE, .page = PG_GPU,
                    .flags = RF, .get = rs_get, .set = rs_set, .fmt = fmt_onoff, .arg = RS_ANTILAG, .order = 156 },
    [C_BOOST] = { .key = "radeon.boost", .label = L"Radeon Boost", .type = CT_TOGGLE, .page = PG_GPU,
                  .flags = RF, .get = rs_get, .set = rs_set, .fmt = fmt_onoff, .arg = RS_BOOST, .order = 157 },
    [C_BOOSTRES] = { .key = "radeon.boostres", .label = L"Boost min resolution", .type = CT_CHOICE, .page = PG_GPU,
                     .flags = RF | CF_ADVANCED, .choices = boost_choices, .def = 0,
                     .get = rs_get, .set = rs_set, .arg = RS_BOOSTRES, .order = 158 },
    [C_RSR] = { .key = "radeon.rsr", .label = L"Radeon Super Resolution", .type = CT_TOGGLE, .page = PG_GPU,
                .flags = RF, .get = rs_get, .set = rs_set, .fmt = fmt_onoff, .arg = RS_RSR, .order = 159 },
    [C_RIS] = { .key = "radeon.ris", .label = L"Image Sharpening", .type = CT_TOGGLE, .page = PG_GPU,
                .flags = RF, .get = rs_get, .set = rs_set, .fmt = fmt_onoff, .arg = RS_RIS, .order = 160 },
    [C_SHARP] = { .key = "radeon.sharpness", .label = L"Sharpness", .type = CT_SLIDER, .page = PG_GPU,
                  .flags = RF | CF_ADVANCED, .min = 10, .max = 100, .step = 10, .def = 50, .unit = L"%",
                  .get = rs_get, .set = rs_set, .fmt = fmt_pct, .arg = RS_SHARP, .order = 161 },
    [C_RESTART] = { .key = "radeon.restart", .label = L"Restart GPU driver", .type = CT_ACTION, .page = PG_GPU,
                    .flags = CF_NOSAVE | CF_DANGER | CF_ADVANCED, .set = act_restart, .fmt = fmt_restart, .order = 162 },
};

/* ---------- clock domain ---------- */
static int clk_set_max(ph_clk *d, int mhz)
{
    lk();
    int o = clk_max;
    clk_max = mhz;
    int r = apply_clk();
    if (r) clk_max = o;
    ulk();
    return r;
}

static int clk_set_min(ph_clk *d, int mhz)
{
    lk();
    int o = clk_min;
    clk_min = mhz;
    int r = apply_clk();
    if (r) clk_min = o;
    ulk();
    return r;
}

static int clk_reset(ph_clk *d)
{
    lk();
    clk_max = clk_min = 0;
    int r = apply_clk();
    ulk();
    return r;
}

static int clk_cur(ph_clk *d, int *mhz)
{
    lk();
    sample(250);
    int v = s_mhz;
    ulk();
    if (v < 0) return -1;
    *mhz = v;
    return 0;
}

static int clk_util(ph_clk *d, int *pct)
{
    lk();
    sample(250);
    int v = s_load;
    ulk();
    if (v < 0) return -1;
    *pct = v;
    return 0;
}

static ph_clk clk = {
    L"AMD Radeon", 0, 0, 25, clk_set_max, clk_set_min, clk_reset, clk_cur, clk_util, NULL, 30
};

/* ---------- backend ---------- */
static int amd_probe(void)
{
    nkeys = gpu_class_keys(L"VEN_1002", keys, MAX_KEYS);
    return nkeys > 0;
}

static int amd_init(void)
{
    if (!cs_ok) { InitializeCriticalSection(&cs); cs_ok = 1; }
    lstrcpynW(dev_name, L"AMD Radeon GPU", PH_ARRAY(dev_name));
    vendor_open();

    int dgpu = (mode == M_ADLX && x.dgpu) || use_adl();
    if (!dgpu) {
        for (int i = C_HDR; i <= C_PLIM; i++) ctls[i].flags |= CF_HIDDEN;
    } else {
        if (have_clk) {
            ctls[C_MAX].min = max_lo;
            ctls[C_MAX].max = def_max;
            ctls[C_MAX].def = def_max;
            ctls[C_MIN].min = min_lo;
            ctls[C_MIN].max = PH_CLAMP(def_max - CLK_GAP, min_lo, min_hi);
            ctls[C_MIN].def = PH_CLAMP(def_min, ctls[C_MIN].min, ctls[C_MIN].max);
            clk.min_mhz = max_lo;
            clk.max_mhz = def_max;
            ph_register_gpu_clk(&clk);
        } else {
            ctls[C_MAX].flags |= CF_HIDDEN;
            ctls[C_MIN].flags |= CF_HIDDEN;
        }
        if (have_plim) {
            ctls[C_PLIM].min = plim_lo;
            ctls[C_PLIM].max = plim_hi;
            ctls[C_PLIM].def = PH_CLAMP(plim_orig, plim_lo, plim_hi);
        } else {
            ctls[C_PLIM].flags |= CF_HIDDEN;
        }
        sample(0);
        if (s_mhz < 0) ctls[C_MHZ].flags |= CF_HIDDEN;
        if (s_load < 0) ctls[C_LOAD].flags |= CF_HIDDEN;
        if (s_mw < 0) ctls[C_PWR].flags |= CF_HIDDEN;
        if (s_temp < 0) ctls[C_TEMP].flags |= CF_HIDDEN;
    }
    if (!nkeys)
        for (int i = C_RHDR; i < C_N; i++) ctls[i].flags |= CF_HIDDEN;
    ph_register_ctls(ctls, C_N);
    return 0;
}

static void amd_shutdown(void)
{
    if (!cs_ok) return;
    lk();
    user_min = user_max = clk_min = clk_max = 0;
    if (applied) apply_clk();
    if (plim_written && hw_plim(plim_orig, 1) == 0) plim_written = 0;
    vendor_close();
    ulk();
}

static void amd_resume(void)
{
    if (!cs_ok) return;
    lk();
    s_ms = 0;
    if (applied) {
        app_min = app_max = -1;
        pmode_set = 0;
        apply_clk();
    }
    if (plim_written && ctls[C_PLIM].active) hw_plim(ctls[C_PLIM].val, 0);
    ulk();
}

static void amd_tick(void)
{
    if (!cs_ok) return;
    lk();
    ensure_open();
    int changed = 0;
    if (user_max && !ctls[C_MAX].active) { user_max = 0; changed = 1; }
    if (user_min && !ctls[C_MIN].active) { user_min = 0; changed = 1; }
    if (changed) apply_clk();
    if (plim_written && !ctls[C_PLIM].active && hw_plim(plim_orig, 1) == 0) plim_written = 0;
    ulk();
}

ph_backend bk_amdgpu = {
    "amdgpu",
    L"AMD Radeon GPU",
    BK_GPU,
    amd_probe,
    amd_init,
    amd_shutdown,
    amd_resume,
    amd_tick,
    0
};
