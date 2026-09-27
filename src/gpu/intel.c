#include "phawx.h"
#include <stdbool.h>

typedef int32_t ctl_result_t;
#define CTL_OK 0
#define CTL_MAKE_VERSION(ma, mi) (((ma) << 16) | ((mi) & 0xffff))
#define CTL_INIT_FLAG_USE_LEVEL_ZERO 1u
#define CTL_ADAPTER_PROPERTIES_FLAG_INTEGRATED 1u
typedef struct _ctl_api_handle_t *ctl_api_handle_t;
typedef struct _ctl_device_adapter_handle_t *ctl_device_adapter_handle_t;
typedef struct _ctl_freq_handle_t *ctl_freq_handle_t;
typedef struct _ctl_pwr_handle_t *ctl_pwr_handle_t;
typedef enum { CTL_FREQ_DOMAIN_GPU = 0, CTL_FREQ_DOMAIN_MEMORY = 1 } ctl_freq_domain_t;
typedef enum { CTL_DEVICE_TYPE_GRAPHICS = 1, CTL_DEVICE_TYPE_SYSTEM = 2 } ctl_device_type_t;
typedef enum { CTL_UNITS_FREQUENCY_MHZ = 0, CTL_UNITS_UNKNOWN = 0x4800FFFF } ctl_units_t;
typedef enum { CTL_DATA_TYPE_INT32 = 4, CTL_DATA_TYPE_UINT32 = 5, CTL_DATA_TYPE_INT64 = 6, CTL_DATA_TYPE_UINT64 = 7,
               CTL_DATA_TYPE_FLOAT = 8, CTL_DATA_TYPE_DOUBLE = 9, CTL_DATA_TYPE_UNKNOWN = 0x4800FFFF } ctl_data_type_t;
typedef enum { CTL_PSU_TYPE_PSU_NONE = 0 } ctl_psu_type_t;
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
typedef struct { bool enabled; int32_t power; int32_t interval; } ctl_power_sustained_limit_t;
typedef struct { bool enabled; int32_t power; } ctl_power_burst_limit_t;
typedef struct { int32_t powerAC; int32_t powerDC; } ctl_power_peak_limit_t;
typedef struct { uint32_t Size; uint8_t Version; ctl_power_sustained_limit_t sustainedPowerLimit;
                 ctl_power_burst_limit_t burstPowerLimit; ctl_power_peak_limit_t peakPowerLimits; } ctl_power_limits_t;
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

_Static_assert(sizeof(ctl_init_args_t) == 0x24, "init_args");
_Static_assert(sizeof(ctl_device_adapter_properties_t) == 0x140, "adapter_properties");
_Static_assert(sizeof(ctl_freq_properties_t) == 0x20, "freq_properties");
_Static_assert(sizeof(ctl_freq_range_t) == 0x18, "freq_range");
_Static_assert(sizeof(ctl_freq_state_t) == 0x38, "freq_state");
_Static_assert(sizeof(ctl_power_properties_t) == 0x14, "power_properties");
_Static_assert(sizeof(ctl_power_limits_t) == 0x24, "power_limits");
_Static_assert(sizeof(ctl_oc_telemetry_item_t) == 0x18, "telemetry_item");
_Static_assert(sizeof(ctl_power_telemetry_t) == 0x400, "power_telemetry");

typedef ctl_result_t (*PFN_ctlInit)(ctl_init_args_t *, ctl_api_handle_t *);
typedef ctl_result_t (*PFN_ctlClose)(ctl_api_handle_t);
typedef ctl_result_t (*PFN_ctlEnumerateDevices)(ctl_api_handle_t, uint32_t *, ctl_device_adapter_handle_t *);
typedef ctl_result_t (*PFN_ctlGetDeviceProperties)(ctl_device_adapter_handle_t, ctl_device_adapter_properties_t *);
typedef ctl_result_t (*PFN_ctlEnumFrequencyDomains)(ctl_device_adapter_handle_t, uint32_t *, ctl_freq_handle_t *);
typedef ctl_result_t (*PFN_ctlFrequencyGetProperties)(ctl_freq_handle_t, ctl_freq_properties_t *);
typedef ctl_result_t (*PFN_ctlFrequencyGetRange)(ctl_freq_handle_t, ctl_freq_range_t *);
typedef ctl_result_t (*PFN_ctlFrequencySetRange)(ctl_freq_handle_t, const ctl_freq_range_t *);
typedef ctl_result_t (*PFN_ctlFrequencyGetState)(ctl_freq_handle_t, ctl_freq_state_t *);
typedef ctl_result_t (*PFN_ctlEnumPowerDomains)(ctl_device_adapter_handle_t, uint32_t *, ctl_pwr_handle_t *);
typedef ctl_result_t (*PFN_ctlPowerGetProperties)(ctl_pwr_handle_t, ctl_power_properties_t *);
typedef ctl_result_t (*PFN_ctlPowerGetLimits)(ctl_pwr_handle_t, ctl_power_limits_t *);
typedef ctl_result_t (*PFN_ctlPowerSetLimits)(ctl_pwr_handle_t, const ctl_power_limits_t *);
typedef ctl_result_t (*PFN_ctlPowerTelemetryGet)(ctl_device_adapter_handle_t, ctl_power_telemetry_t *);

static struct {
    PFN_ctlInit init;
    PFN_ctlClose close;
    PFN_ctlEnumerateDevices enumdev;
    PFN_ctlGetDeviceProperties devprops;
    PFN_ctlEnumFrequencyDomains enumfreq;
    PFN_ctlFrequencyGetProperties freqprops;
    PFN_ctlFrequencyGetRange getrange;
    PFN_ctlFrequencySetRange setrange;
    PFN_ctlFrequencyGetState freqstate;
    PFN_ctlEnumPowerDomains enumpwr;
    PFN_ctlPowerGetProperties pwrprops;
    PFN_ctlPowerGetLimits getlimits;
    PFN_ctlPowerSetLimits setlimits;
    PFN_ctlPowerTelemetryGet telemetry;
} igcl;

typedef struct {
    double t, act, energy;
    int have, sup_load, sup_mw;
    int load, mw, temp;
    uint64_t ms;
} tsample;

enum { C_HDR, C_NAME, C_MHZ, C_LOAD, C_PWR, C_TEMP, C_MAX, C_MIN, C_PL1, C_PL2, C_N };

typedef struct {
    ctl_device_adapter_handle_t dev;
    ctl_freq_handle_t freq;
    ctl_pwr_handle_t pwr;
    int integrated;
    int can_freq;
    int hw_min, hw_max;
    ctl_freq_range_t orig_range;
    int have_orig_range, range_written;
    ctl_power_limits_t orig_limits;
    int limits_written;
    int user_min, user_max, clk_min, clk_max;
    int cur_min, cur_max;
    wchar_t name[64];
    tsample ui, at;
    ph_ctl ctls[C_N];
    ph_clk clk;
} igpu;

#define MAX_GPU 2

static HMODULE lib;
static ctl_api_handle_t api;
static igpu *gpus[MAX_GPU];
static int ngpu;
static CRITICAL_SECTION cs;

static void lk(void) { EnterCriticalSection(&cs); }
static void ulk(void) { LeaveCriticalSection(&cs); }

static int apply_range(igpu *g)
{
    int mx = 0, mn;
    if (g->user_max > 0) mx = g->user_max;
    if (g->clk_max > 0 && (!mx || g->clk_max < mx)) mx = g->clk_max;
    mn = g->clk_min > 0 ? g->clk_min : g->user_min;
    ctl_freq_range_t r = { sizeof r, 0, -1.0, -1.0 };
    if (!mx && !mn) {
        if (!g->range_written) return 0;
        if (g->have_orig_range) r = g->orig_range;
        r.Size = sizeof r;
        r.Version = 0;
        if (igcl.setrange(g->freq, &r) != CTL_OK) return -1;
        g->range_written = 0;
        g->cur_min = g->cur_max = 0;
        return 0;
    }
    if (mx) mx = PH_CLAMP(mx, g->hw_min, g->hw_max);
    if (mn) mn = PH_CLAMP(mn, g->hw_min, mx ? mx : g->hw_max);
    if (g->range_written && mn == g->cur_min && mx == g->cur_max) return 0;
    r.min = mn ? (double)mn : -1.0;
    r.max = mx ? (double)mx : -1.0;
    if (igcl.setrange(g->freq, &r) != CTL_OK) return -1;
    g->range_written = 1;
    g->cur_min = mn;
    g->cur_max = mx;
    return 0;
}

static double item_val(const ctl_oc_telemetry_item_t *it, int *ok)
{
    *ok = it->bSupported;
    if (!it->bSupported) return 0;
    switch (it->type) {
    case CTL_DATA_TYPE_DOUBLE: return it->value.datadouble;
    case CTL_DATA_TYPE_FLOAT:  return it->value.datafloat;
    case CTL_DATA_TYPE_INT32:  return it->value.data32;
    case CTL_DATA_TYPE_UINT32: return it->value.datau32;
    case CTL_DATA_TYPE_INT64:  return (double)it->value.data64;
    case CTL_DATA_TYPE_UINT64: return (double)it->value.datau64;
    default: *ok = 0; return 0;
    }
}

static void tsample_update(igpu *g, tsample *s, int min_ms)
{
    uint64_t now = ph_ms();
    if (s->ms && now - s->ms < (uint64_t)min_ms) return;
    s->ms = now;
    ctl_power_telemetry_t tb, *t = &tb;
    memset(t, 0, sizeof *t);
    t->Size = sizeof *t;
    if (igcl.telemetry(g->dev, t) != CTL_OK) {
        s->load = s->mw = s->temp = -1;
        s->have = 0;
        return;
    }
    int okt, oka, oke, okc;
    double ts = item_val(&t->timeStamp, &okt);
    double act = item_val(&t->globalActivityCounter, &oka);
    double en = item_val(&t->gpuEnergyCounter, &oke);
    double tc = item_val(&t->gpuCurrentTemperature, &okc);
    s->temp = okc && tc > 0 && tc < 150 ? (int)tc : -1;
    s->sup_load = okt && oka;
    s->sup_mw = okt && oke;
    if (!okt) { s->have = 0; s->load = s->mw = -1; return; }
    if (s->have) {
        double dt = ts - s->t;
        if (dt > 0.001) {
            if (oka) {
                int l = (int)((act - s->act) / dt * 100.0 + 0.5);
                s->load = PH_CLAMP(l, 0, 100);
            }
            if (oke && en >= s->energy) s->mw = (int)((en - s->energy) / dt * 1000.0);
        }
    }
    if (!oka) s->load = -1;
    if (!oke) s->mw = -1;
    s->t = ts;
    s->act = act;
    s->energy = en;
    s->have = 1;
}

static igpu *of(ph_ctl *c) { return (igpu *)c->ctx; }

static int get_mhz(ph_ctl *c, int32_t *o)
{
    ctl_freq_state_t st = { sizeof st, 0 };
    if (!of(c)->freq || !igcl.freqstate || igcl.freqstate(of(c)->freq, &st) != CTL_OK || st.actual <= 0) return -1;
    *o = (int32_t)(st.actual + 0.5);
    return 0;
}

static int get_load(ph_ctl *c, int32_t *o)
{
    if (!igcl.telemetry) return -1;
    tsample_update(of(c), &of(c)->ui, 500);
    *o = of(c)->ui.load;
    return *o < 0 ? -1 : 0;
}

static int get_mw(ph_ctl *c, int32_t *o)
{
    if (!igcl.telemetry) return -1;
    tsample_update(of(c), &of(c)->ui, 500);
    *o = of(c)->ui.mw;
    return *o < 0 ? -1 : 0;
}

static int get_temp(ph_ctl *c, int32_t *o)
{
    if (!igcl.telemetry) return -1;
    tsample_update(of(c), &of(c)->ui, 500);
    *o = of(c)->ui.temp;
    return *o < 0 ? -1 : 0;
}

static void fmt_temp(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    if (v < 0) lstrcpynW(b, L"--", n);
    else ph_swprintf(b, n, L"%d \x00B0" L"C", v);
}

static void fmt_name(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    lstrcpynW(b, ((igpu *)c->ctx)->name, n);
}

static int set_clk(ph_ctl *c, int32_t v)
{
    igpu *g = of(c);
    lk();
    int om = g->user_min, ox = g->user_max;
    if (c->arg) g->user_max = v; else g->user_min = v;
    int r = apply_range(g);
    if (r) { g->user_min = om; g->user_max = ox; }
    ulk();
    return r;
}

static int get_pl(ph_ctl *c, int32_t *o)
{
    ctl_power_limits_t l = { sizeof l, 0 };
    if (!of(c)->pwr || igcl.getlimits(of(c)->pwr, &l) != CTL_OK) return -1;
    *o = c->arg ? l.burstPowerLimit.power : l.sustainedPowerLimit.power;
    return *o > 0 ? 0 : -1;
}

static int set_pl(ph_ctl *c, int32_t v)
{
    igpu *g = of(c);
    ctl_power_limits_t l = { sizeof l, 0 };
    if (igcl.getlimits(g->pwr, &l) != CTL_OK) return -1;
    l.Size = sizeof l;
    l.Version = 0;
    if (c->arg) {
        l.burstPowerLimit.enabled = true;
        l.burstPowerLimit.power = v;
    } else {
        l.sustainedPowerLimit.enabled = true;
        l.sustainedPowerLimit.power = v;
        if (l.burstPowerLimit.enabled && l.burstPowerLimit.power < v) l.burstPowerLimit.power = v;
    }
    if (igcl.setlimits(g->pwr, &l) != CTL_OK) return -1;
    g->limits_written = 1;
    return 0;
}

static int restore_limits(igpu *g)
{
    ctl_power_limits_t l = g->orig_limits;
    l.Size = sizeof l;
    l.Version = 0;
    if (igcl.setlimits(g->pwr, &l) != CTL_OK) return -1;
    g->limits_written = 0;
    return 0;
}

#define GF (CF_OPTIONAL | CF_PROFILE)

static const ph_ctl tmpl[C_N] = {
    { .key = NULL, .type = CT_HEADER, .page = PG_GPU },
    { .key = NULL, .label = L"Adapter", .type = CT_INFO, .page = PG_GPU, .fmt = fmt_name },
    { .key = NULL, .label = L"GPU clock", .type = CT_INFO, .page = PG_GPU, .get = get_mhz, .fmt = fmt_mhz },
    { .key = NULL, .label = L"GPU load", .type = CT_INFO, .page = PG_GPU, .get = get_load, .fmt = fmt_pct },
    { .key = NULL, .label = L"GPU power", .type = CT_INFO, .page = PG_GPU, .get = get_mw, .fmt = fmt_watts_mw },
    { .key = NULL, .label = L"GPU temperature", .type = CT_INFO, .page = PG_GPU, .get = get_temp, .fmt = fmt_temp },
    { .label = L"Max GPU clock", .type = CT_SLIDER, .page = PG_GPU, .flags = GF | CF_AUTOTDP,
      .step = 50, .unit = L"MHz", .set = set_clk, .fmt = fmt_mhz, .arg = 1 },
    { .label = L"Min GPU clock", .type = CT_SLIDER, .page = PG_GPU,
      .flags = GF | CF_AUTOTDP | CF_ADVANCED, .step = 50, .unit = L"MHz", .set = set_clk,
      .fmt = fmt_mhz, .arg = 0 },
    { .label = L"GPU power limit", .type = CT_SLIDER, .page = PG_GPU, .flags = GF | CF_REAPPLY,
      .step = 1000, .unit = L"W", .get = get_pl, .set = set_pl, .fmt = fmt_watts_mw, .arg = 0 },
    { .label = L"GPU boost power limit", .type = CT_SLIDER, .page = PG_GPU,
      .flags = GF | CF_REAPPLY | CF_ADVANCED, .step = 1000, .unit = L"W", .get = get_pl, .set = set_pl,
      .fmt = fmt_watts_mw, .arg = 1 },
};

static const char *const keys_igpu[C_N] = {
    [C_MAX] = "intelgpu.maxclk", [C_MIN] = "intelgpu.minclk",
    [C_PL1] = "intelgpu.power", [C_PL2] = "intelgpu.powerboost"
};
static const char *const keys_arc[C_N] = {
    [C_MAX] = "arc.maxclk", [C_MIN] = "arc.minclk",
    [C_PL1] = "arc.power", [C_PL2] = "arc.powerboost"
};

static igpu *clk_gpu(ph_clk *d) { return (igpu *)d->ctx; }

static int clk_set_max(ph_clk *d, int mhz)
{
    igpu *g = clk_gpu(d);
    lk();
    int o = g->clk_max;
    g->clk_max = mhz;
    int r = apply_range(g);
    if (r) g->clk_max = o;
    ulk();
    return r;
}

static int clk_set_min(ph_clk *d, int mhz)
{
    igpu *g = clk_gpu(d);
    lk();
    int o = g->clk_min;
    g->clk_min = mhz;
    int r = apply_range(g);
    if (r) g->clk_min = o;
    ulk();
    return r;
}

static int clk_reset(ph_clk *d)
{
    igpu *g = clk_gpu(d);
    lk();
    g->clk_min = g->clk_max = 0;
    int r = apply_range(g);
    ulk();
    return r;
}

static int clk_cur(ph_clk *d, int *mhz)
{
    ctl_freq_state_t st = { sizeof st, 0 };
    if (igcl.freqstate(clk_gpu(d)->freq, &st) != CTL_OK || st.actual <= 0) return -1;
    *mhz = (int)(st.actual + 0.5);
    return 0;
}

static int clk_util(ph_clk *d, int *pct)
{
    igpu *g = clk_gpu(d);
    tsample_update(g, &g->at, 60);
    if (g->at.load < 0) return -1;
    *pct = g->at.load;
    return 0;
}

#define SYM(f, s) (igcl.f = (__typeof__(igcl.f))(void *)GetProcAddress(lib, s))

static int load_syms(void)
{
    SYM(init, "ctlInit");
    SYM(close, "ctlClose");
    SYM(enumdev, "ctlEnumerateDevices");
    SYM(devprops, "ctlGetDeviceProperties");
    SYM(enumfreq, "ctlEnumFrequencyDomains");
    SYM(freqprops, "ctlFrequencyGetProperties");
    SYM(getrange, "ctlFrequencyGetRange");
    SYM(setrange, "ctlFrequencySetRange");
    SYM(freqstate, "ctlFrequencyGetState");
    SYM(enumpwr, "ctlEnumPowerDomains");
    SYM(pwrprops, "ctlPowerGetProperties");
    SYM(getlimits, "ctlPowerGetLimits");
    SYM(setlimits, "ctlPowerSetLimits");
    SYM(telemetry, "ctlPowerTelemetryGet");
    return igcl.init && igcl.close && igcl.enumdev && igcl.devprops;
}

static void find_freq(igpu *g)
{
    if (!igcl.enumfreq || !igcl.freqprops) return;
    uint32_t n = 0;
    if (igcl.enumfreq(g->dev, &n, NULL) != CTL_OK || !n) return;
    if (n > 8) n = 8;
    ctl_freq_handle_t h[8];
    if (igcl.enumfreq(g->dev, &n, h) != CTL_OK) return;
    for (uint32_t i = 0; i < n && i < 8; i++) {
        ctl_freq_properties_t p = { sizeof p, 0 };
        if (igcl.freqprops(h[i], &p) != CTL_OK || p.type != CTL_FREQ_DOMAIN_GPU) continue;
        g->freq = h[i];
        if (p.canControl && igcl.setrange && p.max > p.min && p.min >= 0 && p.max < 10000) {
            g->can_freq = 1;
            g->hw_min = (int)(p.min + 0.5);
            g->hw_max = (int)(p.max + 0.5);
        }
        break;
    }
    if (g->can_freq && igcl.getrange) {
        ctl_freq_range_t r = { sizeof r, 0 };
        if (igcl.getrange(g->freq, &r) == CTL_OK) {
            g->orig_range = r;
            g->have_orig_range = 1;
        }
    }
}

static void find_power(igpu *g, int *lo, int *hi, int *def)
{
    if (!igcl.enumpwr || !igcl.pwrprops || !igcl.getlimits || !igcl.setlimits) return;
    uint32_t n = 0;
    if (igcl.enumpwr(g->dev, &n, NULL) != CTL_OK || !n) return;
    if (n > 4) n = 4;
    ctl_pwr_handle_t h[4];
    if (igcl.enumpwr(g->dev, &n, h) != CTL_OK) return;
    for (uint32_t i = 0; i < n && i < 4; i++) {
        ctl_power_properties_t p = { sizeof p, 0 };
        if (igcl.pwrprops(h[i], &p) != CTL_OK || !p.canControl) continue;
        ctl_power_limits_t l = { sizeof l, 0 };
        if (igcl.getlimits(h[i], &l) != CTL_OK) continue;
        if (p.maxLimit <= p.minLimit + 1000 || p.minLimit < 0) continue;
        g->pwr = h[i];
        g->orig_limits = l;
        *lo = (p.minLimit + 999) / 1000 * 1000;
        *hi = p.maxLimit / 1000 * 1000;
        if (*lo >= *hi) { *lo = p.minLimit; *hi = p.maxLimit; }
        *def = l.sustainedPowerLimit.power > 0 ? l.sustainedPowerLimit.power : p.defaultLimit;
        return;
    }
}

static void setup(igpu *g)
{
    const char *const *keys = g->integrated ? keys_igpu : keys_arc;
    int base = g->integrated ? 300 : 320;
    if (igcl.telemetry) tsample_update(g, &g->ui, 0);
    memcpy(g->ctls, tmpl, sizeof tmpl);
    for (int i = 0; i < C_N; i++) {
        g->ctls[i].ctx = g;
        g->ctls[i].order = (int16_t)(base + i);
        if (keys[i]) g->ctls[i].key = keys[i];
    }
    g->ctls[C_HDR].label = g->integrated ? L"Intel integrated GPU" : L"Intel Arc GPU";

    if (g->can_freq) {
        for (int i = C_MAX; i <= C_MIN; i++) {
            g->ctls[i].min = g->hw_min;
            g->ctls[i].max = g->hw_max;
        }
        g->ctls[C_MAX].def = g->hw_max;
        g->ctls[C_MIN].def = g->hw_min;
        g->clk.name = g->integrated ? L"Intel iGPU" : L"Intel Arc";
        g->clk.min_mhz = g->hw_min;
        g->clk.max_mhz = g->hw_max;
        g->clk.step_mhz = 50;
        g->clk.set_max = clk_set_max;
        g->clk.set_min = clk_set_min;
        g->clk.reset = clk_reset;
        g->clk.cur = igcl.freqstate ? clk_cur : NULL;
        g->clk.util = igcl.telemetry && g->ui.sup_load ? clk_util : NULL;
        g->clk.ctx = g;
        g->clk.prio = g->integrated ? 25 : 30;
        ph_register_gpu_clk(&g->clk);
    } else {
        g->ctls[C_MAX].flags |= CF_HIDDEN;
        g->ctls[C_MIN].flags |= CF_HIDDEN;
    }

    int lo = 0, hi = 0, def = 0;
    find_power(g, &lo, &hi, &def);
    if (g->pwr) {
        for (int i = C_PL1; i <= C_PL2; i++) {
            g->ctls[i].min = lo;
            g->ctls[i].max = hi;
        }
        g->ctls[C_PL1].def = PH_CLAMP(def, lo, hi);
        int b = g->orig_limits.burstPowerLimit.power;
        g->ctls[C_PL2].def = PH_CLAMP(b > 0 ? b : hi, lo, hi);
        if (!g->orig_limits.burstPowerLimit.enabled) g->ctls[C_PL2].flags |= CF_HIDDEN;
    } else {
        g->ctls[C_PL1].flags |= CF_HIDDEN;
        g->ctls[C_PL2].flags |= CF_HIDDEN;
    }

    if (!g->freq || !igcl.freqstate) g->ctls[C_MHZ].flags |= CF_HIDDEN;
    if (!g->ui.sup_load) g->ctls[C_LOAD].flags |= CF_HIDDEN;
    if (!g->ui.sup_mw) g->ctls[C_PWR].flags |= CF_HIDDEN;
    if (g->ui.temp < 0) g->ctls[C_TEMP].flags |= CF_HIDDEN;
    ph_register_ctls(g->ctls, C_N);
}

static int ig_probe(void)
{
    if (!lib) lib = LoadLibraryExW(L"ControlLib.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return lib != NULL;
}

static void unload(void)
{
    if (!api) {
        for (int i = 0; i < ngpu; i++) ph_free(gpus[i]);
        ngpu = 0;
    }
    if (api && igcl.close) igcl.close(api);
    api = NULL;
    if (lib) FreeLibrary(lib);
    lib = NULL;
}

static int ig_init(void)
{
    if (!lib) return 0;
    InitializeCriticalSection(&cs);
    if (!load_syms()) { unload(); return 0; }
    ctl_init_args_t a = { sizeof a, 0, CTL_MAKE_VERSION(1, 1), CTL_INIT_FLAG_USE_LEVEL_ZERO, 0, { 0 } };
    if (igcl.init(&a, &api) != CTL_OK || !api) { api = NULL; unload(); return 0; }

    uint32_t n = 0;
    if (igcl.enumdev(api, &n, NULL) != CTL_OK || !n) { unload(); return 0; }
    if (n > 8) n = 8;
    ctl_device_adapter_handle_t devs[8];
    if (igcl.enumdev(api, &n, devs) != CTL_OK) { unload(); return 0; }

    int have_i = 0, have_d = 0;
    for (uint32_t i = 0; i < n && i < 8 && ngpu < MAX_GPU; i++) {
        ctl_device_adapter_properties_t p;
        LUID luid;
        memset(&p, 0, sizeof p);
        p.Size = sizeof p;
        p.pDeviceID = &luid;
        p.device_id_size = sizeof luid;
        if (igcl.devprops(devs[i], &p) != CTL_OK) continue;
        if (p.pci_vendor_id != 0x8086 || p.device_type != CTL_DEVICE_TYPE_GRAPHICS) continue;
        int integ = (p.graphics_adapter_properties & CTL_ADAPTER_PROPERTIES_FLAG_INTEGRATED) != 0;
        if (integ ? have_i : have_d) continue;
        igpu *g = ph_alloc(sizeof *g);
        if (!g) break;
        g->dev = devs[i];
        g->integrated = integ;
        g->ui.load = g->ui.mw = g->ui.temp = -1;
        g->at.load = g->at.mw = g->at.temp = -1;
        p.name[sizeof p.name - 1] = 0;
        if (!p.name[0] || !MultiByteToWideChar(CP_UTF8, 0, p.name, -1, g->name, PH_ARRAY(g->name)))
            lstrcpynW(g->name, integ ? L"Intel Graphics" : L"Intel Arc", PH_ARRAY(g->name));
        find_freq(g);
        gpus[ngpu++] = g;
        if (integ) have_i = 1; else have_d = 1;
    }
    if (!ngpu) { unload(); return 0; }
    for (int i = 0; i < ngpu; i++) setup(gpus[i]);
    return 0;
}

static void ig_shutdown(void)
{
    if (!api) { unload(); return; }
    lk();
    for (int i = 0; i < ngpu; i++) {
        igpu *g = gpus[i];
        g->user_min = g->user_max = g->clk_min = g->clk_max = 0;
        if (g->can_freq) apply_range(g);
        if (g->limits_written) restore_limits(g);
    }
    ulk();
    unload();
}

static void ig_resume(void)
{
    if (!api) return;
    lk();
    for (int i = 0; i < ngpu; i++) {
        igpu *g = gpus[i];
        g->ui.have = g->at.have = 0;
        g->ui.ms = g->at.ms = 0;
        if (g->range_written) {
            g->range_written = 0;
            apply_range(g);
        }
    }
    ulk();
}

static void ig_tick(void)
{
    if (!api) return;
    lk();
    for (int i = 0; i < ngpu; i++) {
        igpu *g = gpus[i];
        int changed = 0;
        if (g->user_max && !g->ctls[C_MAX].active) { g->user_max = 0; changed = 1; }
        if (g->user_min && !g->ctls[C_MIN].active) { g->user_min = 0; changed = 1; }
        if (changed) apply_range(g);
        if (g->limits_written && !g->ctls[C_PL1].active && !g->ctls[C_PL2].active) restore_limits(g);
    }
    ulk();
}

ph_backend bk_intelgpu = {
    "intelgpu",
    L"Intel GPU",
    BK_GPU,
    ig_probe,
    ig_init,
    ig_shutdown,
    ig_resume,
    ig_tick,
    0
};
