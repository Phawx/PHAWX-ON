#include "phawx.h"

typedef struct nvmlDevice_st *nvmlDevice_t;
typedef int nvmlReturn_t;
enum { NVML_SUCCESS = 0, NVML_ERROR_INSUFFICIENT_SIZE = 7 };
enum { NVML_CLOCK_GRAPHICS = 0 };
enum { NVML_TEMPERATURE_GPU = 0 };
typedef struct { unsigned int gpu, memory; } nvmlUtilization_t;
typedef struct { unsigned int version; int sensorType; int temperature; } nvmlTemperature_v1_t;
#define nvmlTemperature_v1 ((unsigned int)(sizeof(nvmlTemperature_v1_t) | (1u << 24)))
#define NVML_NAME_LEN 96

typedef nvmlReturn_t (*PFN_init)(void);
typedef nvmlReturn_t (*PFN_shutdown)(void);
typedef nvmlReturn_t (*PFN_count)(unsigned int *);
typedef nvmlReturn_t (*PFN_handle)(unsigned int, nvmlDevice_t *);
typedef nvmlReturn_t (*PFN_name)(nvmlDevice_t, char *, unsigned int);
typedef nvmlReturn_t (*PFN_clock)(nvmlDevice_t, int, unsigned int *);
typedef nvmlReturn_t (*PFN_memclks)(nvmlDevice_t, unsigned int *, unsigned int *);
typedef nvmlReturn_t (*PFN_gfxclks)(nvmlDevice_t, unsigned int, unsigned int *, unsigned int *);
typedef nvmlReturn_t (*PFN_setlock)(nvmlDevice_t, unsigned int, unsigned int);
typedef nvmlReturn_t (*PFN_dev)(nvmlDevice_t);
typedef nvmlReturn_t (*PFN_util)(nvmlDevice_t, nvmlUtilization_t *);
typedef nvmlReturn_t (*PFN_u32)(nvmlDevice_t, unsigned int *);
typedef nvmlReturn_t (*PFN_u32x2)(nvmlDevice_t, unsigned int *, unsigned int *);
typedef nvmlReturn_t (*PFN_setu32)(nvmlDevice_t, unsigned int);
typedef nvmlReturn_t (*PFN_temp)(nvmlDevice_t, int, unsigned int *);
typedef nvmlReturn_t (*PFN_tempv)(nvmlDevice_t, nvmlTemperature_v1_t *);

static struct {
    PFN_init init;
    PFN_shutdown shutdown;
    PFN_count count;
    PFN_handle handle;
    PFN_name name;
    PFN_clock clock, maxclock;
    PFN_memclks memclks;
    PFN_gfxclks gfxclks;
    PFN_setlock setlock;
    PFN_dev resetlock;
    PFN_util util;
    PFN_u32 power, plimit;
    PFN_u32x2 pconstraints;
    PFN_setu32 setplimit;
    PFN_temp temp;
    PFN_tempv tempv;
} nv;

static HMODULE lib;
static int inited;
static nvmlDevice_t dev;
static wchar_t dev_name[64];
static int hw_min = 210, hw_max;
static int user_min, user_max, clk_min, clk_max;
static int lock_min, lock_max, locked;
static unsigned int orig_plimit;
static int plimit_written;
static CRITICAL_SECTION cs;

static int s_mhz = -1, s_load = -1, s_mw = -1, s_temp = -1;
static uint64_t s_ms;
static int a_load = -1;
static uint64_t a_ms;

static void lk(void) { EnterCriticalSection(&cs); }
static void ulk(void) { LeaveCriticalSection(&cs); }

static int apply_lock(void)
{
    int mx = 0, mn;
    if (user_max > 0) mx = user_max;
    if (clk_max > 0 && (!mx || clk_max < mx)) mx = clk_max;
    mn = clk_min > 0 ? clk_min : user_min;
    if (!mx && !mn) {
        if (!locked) return 0;
        if (nv.resetlock(dev) != NVML_SUCCESS) return -1;
        locked = 0;
        lock_min = lock_max = 0;
        return 0;
    }
    if (!mx) mx = hw_max;
    if (!mn) mn = hw_min;
    mx = PH_CLAMP(mx, hw_min, hw_max);
    mn = PH_CLAMP(mn, hw_min, mx);
    if (locked && mn == lock_min && mx == lock_max) return 0;
    if (nv.setlock(dev, (unsigned)mn, (unsigned)mx) != NVML_SUCCESS) return -1;
    locked = 1;
    lock_min = mn;
    lock_max = mx;
    return 0;
}

static void sample(void)
{
    uint64_t now = ph_ms();
    if (s_ms && now - s_ms < 500) return;
    s_ms = now;
    unsigned int v;
    nvmlUtilization_t u;
    s_mhz = nv.clock(dev, NVML_CLOCK_GRAPHICS, &v) == NVML_SUCCESS ? (int)v : -1;
    s_load = nv.util(dev, &u) == NVML_SUCCESS ? (int)u.gpu : -1;
    s_mw = nv.power && nv.power(dev, &v) == NVML_SUCCESS ? (int)v : -1;
    s_temp = -1;
    if (nv.tempv) {
        nvmlTemperature_v1_t t = { nvmlTemperature_v1, NVML_TEMPERATURE_GPU, 0 };
        if (nv.tempv(dev, &t) == NVML_SUCCESS) s_temp = t.temperature;
    }
    if (s_temp < 0 && nv.temp && nv.temp(dev, NVML_TEMPERATURE_GPU, &v) == NVML_SUCCESS) s_temp = (int)v;
}

/* for AutoTDP: an NVIDIA GPU is always discrete, so its power adds to the package's */
static int read_power(int *mw)
{
    unsigned int v;
    if (!nv.power || nv.power(dev, &v) != NVML_SUCCESS) return -1;
    *mw = (int)v;
    return 0;
}

static int get_mhz(ph_ctl *c, int32_t *o) { sample(); *o = s_mhz; return s_mhz < 0 ? -1 : 0; }
static int get_load(ph_ctl *c, int32_t *o) { sample(); *o = s_load; return s_load < 0 ? -1 : 0; }
static int get_mw(ph_ctl *c, int32_t *o) { sample(); *o = s_mw; return s_mw < 0 ? -1 : 0; }
static int get_temp(ph_ctl *c, int32_t *o) { sample(); *o = s_temp; return s_temp < 0 ? -1 : 0; }

static void fmt_temp(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    if (v < 0) lstrcpynW(b, L"--", n);
    else ph_swprintf(b, n, L"%d \x00B0" L"C", v);
}

static void fmt_name(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    lstrcpynW(b, dev_name, n);
}

static int set_clk(ph_ctl *c, int32_t v)
{
    lk();
    int om = user_min, ox = user_max;
    if (c->arg) user_max = v; else user_min = v;
    int r = apply_lock();
    if (r) { user_min = om; user_max = ox; }
    ulk();
    return r;
}

static int get_plimit(ph_ctl *c, int32_t *o)
{
    unsigned int v;
    if (nv.plimit(dev, &v) != NVML_SUCCESS) return -1;
    *o = (int32_t)v;
    return 0;
}

static int set_plimit(ph_ctl *c, int32_t v)
{
    if (nv.setplimit(dev, (unsigned)v) != NVML_SUCCESS) return -1;
    plimit_written = 1;
    return 0;
}

#define NVF (CF_OPTIONAL | CF_PROFILE)

static ph_ctl ctls[] = {
    { .key = NULL, .label = L"NVIDIA GPU", .type = CT_HEADER, .page = PG_GPU, .order = 200 },
    { .key = NULL, .label = L"Adapter", .type = CT_INFO, .page = PG_GPU, .fmt = fmt_name, .order = 201 },
    { .key = NULL, .label = L"GPU clock", .type = CT_INFO, .page = PG_GPU,
      .get = get_mhz, .fmt = fmt_mhz, .order = 202 },
    { .key = NULL, .label = L"GPU load", .type = CT_INFO, .page = PG_GPU,
      .get = get_load, .fmt = fmt_pct, .order = 203 },
    { .key = NULL, .label = L"GPU power", .type = CT_INFO, .page = PG_GPU,
      .get = get_mw, .fmt = fmt_watts_mw, .order = 204 },
    { .key = NULL, .label = L"GPU temperature", .type = CT_INFO, .page = PG_GPU,
      .get = get_temp, .fmt = fmt_temp, .order = 205 },
    { .key = "nvidia.maxclk", .label = L"Max GPU clock", .type = CT_SLIDER, .page = PG_GPU,
      .flags = NVF | CF_AUTOTDP, .step = 15, .unit = L"MHz", .set = set_clk, .fmt = fmt_mhz,
      .arg = 1, .order = 206 },
    { .key = "nvidia.minclk", .label = L"Min GPU clock", .type = CT_SLIDER, .page = PG_GPU,
      .flags = NVF | CF_AUTOTDP | CF_ADVANCED, .step = 15, .unit = L"MHz", .set = set_clk,
      .fmt = fmt_mhz, .arg = 0, .order = 207 },
    { .key = "nvidia.power", .label = L"GPU power limit", .type = CT_SLIDER, .page = PG_GPU,
      .flags = NVF, .step = 1000, .unit = L"W", .get = get_plimit, .set = set_plimit,
      .fmt = fmt_watts_mw, .order = 208 },
};

enum { C_HDR, C_NAME, C_MHZ, C_LOAD, C_PWR, C_TEMP, C_MAX, C_MIN, C_PLIM };

static int clk_set_max(ph_clk *d, int mhz)
{
    lk();
    int o = clk_max;
    clk_max = mhz;
    int r = apply_lock();
    if (r) clk_max = o;
    ulk();
    return r;
}

static int clk_set_min(ph_clk *d, int mhz)
{
    lk();
    int o = clk_min;
    clk_min = mhz;
    int r = apply_lock();
    if (r) clk_min = o;
    ulk();
    return r;
}

static int clk_reset(ph_clk *d)
{
    lk();
    clk_max = clk_min = 0;
    int r = apply_lock();
    ulk();
    return r;
}

static int clk_cur(ph_clk *d, int *mhz)
{
    unsigned int v;
    if (nv.clock(dev, NVML_CLOCK_GRAPHICS, &v) != NVML_SUCCESS) return -1;
    *mhz = (int)v;
    return 0;
}

static int clk_util(ph_clk *d, int *pct)
{
    uint64_t now = ph_ms();
    if (!a_ms || now - a_ms >= 100) {
        nvmlUtilization_t u;
        a_load = nv.util(dev, &u) == NVML_SUCCESS ? (int)u.gpu : -1;
        a_ms = now;
    }
    if (a_load < 0) return -1;
    *pct = a_load;
    return 0;
}

static ph_clk clk = {
    L"NVIDIA GPU", 0, 0, 15, clk_set_max, clk_set_min, clk_reset, clk_cur, clk_util, NULL, 30
};

static HMODULE load_nvml(void)
{
    HMODULE h = LoadLibraryExW(L"nvml.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (h) return h;
    /* older drivers: Program Files from the known folder, since a user can set %ProgramW6432% */
    wchar_t p[MAX_PATH];
    if (ph_program_files(p, MAX_PATH - 40)) return NULL;
    lstrcatW(p, L"\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    return LoadLibraryExW(p, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}

static void unload(void)
{
    ph_set_power_reader(PWR_GPU, NULL);
    nv.power = NULL;
    if (inited && nv.shutdown) nv.shutdown();
    inited = 0;
    if (lib) FreeLibrary(lib);
    lib = NULL;
}

static int nv_probe(void)
{
    if (!lib) lib = load_nvml();
    return lib != NULL;
}

#define SYM(f, s) (nv.f = (__typeof__(nv.f))(void *)GetProcAddress(lib, s))

static void find_min_clock(void)
{
    unsigned int mem[64], nm = PH_ARRAY(mem);
    if (!nv.memclks || !nv.gfxclks || nv.memclks(dev, &nm, mem) != NVML_SUCCESS || !nm) return;
    unsigned int cap = 512;
    unsigned int *g = ph_alloc(cap * sizeof *g);
    if (!g) return;
    unsigned int lo = 0;
    for (unsigned int i = 0; i < nm && i < PH_ARRAY(mem); i++) {
        unsigned int ng = cap;
        if (nv.gfxclks(dev, mem[i], &ng, g) != NVML_SUCCESS) continue;
        for (unsigned int k = 0; k < ng && k < cap; k++)
            if (g[k] && (!lo || g[k] < lo)) lo = g[k];
    }
    ph_free(g);
    if (lo >= 100 && (int)lo < hw_max) hw_min = (int)lo;
}

static int nv_init(void)
{
    if (!lib) return 0;
    InitializeCriticalSection(&cs);
    SYM(init, "nvmlInit_v2");
    SYM(shutdown, "nvmlShutdown");
    SYM(count, "nvmlDeviceGetCount_v2");
    SYM(handle, "nvmlDeviceGetHandleByIndex_v2");
    SYM(name, "nvmlDeviceGetName");
    SYM(clock, "nvmlDeviceGetClockInfo");
    SYM(maxclock, "nvmlDeviceGetMaxClockInfo");
    SYM(memclks, "nvmlDeviceGetSupportedMemoryClocks");
    SYM(gfxclks, "nvmlDeviceGetSupportedGraphicsClocks");
    SYM(setlock, "nvmlDeviceSetGpuLockedClocks");
    SYM(resetlock, "nvmlDeviceResetGpuLockedClocks");
    SYM(util, "nvmlDeviceGetUtilizationRates");
    SYM(power, "nvmlDeviceGetPowerUsage");
    SYM(plimit, "nvmlDeviceGetPowerManagementLimit");
    SYM(pconstraints, "nvmlDeviceGetPowerManagementLimitConstraints");
    SYM(setplimit, "nvmlDeviceSetPowerManagementLimit");
    SYM(temp, "nvmlDeviceGetTemperature");
    SYM(tempv, "nvmlDeviceGetTemperatureV");
    if (!nv.init || !nv.count || !nv.handle || !nv.clock || !nv.util || nv.init() != NVML_SUCCESS) {
        unload();
        return 0;
    }
    inited = 1;
    unsigned int n = 0;
    if (nv.count(&n) != NVML_SUCCESS || !n || nv.handle(0, &dev) != NVML_SUCCESS) {
        unload();
        return 0;
    }

    char nm[NVML_NAME_LEN] = "NVIDIA GPU";
    if (nv.name) nv.name(dev, nm, sizeof nm);
    nm[sizeof nm - 1] = 0;
    if (!MultiByteToWideChar(CP_UTF8, 0, nm, -1, dev_name, PH_ARRAY(dev_name)))
        lstrcpynW(dev_name, L"NVIDIA GPU", PH_ARRAY(dev_name));

    unsigned int mx = 0;
    if (nv.maxclock && nv.maxclock(dev, NVML_CLOCK_GRAPHICS, &mx) == NVML_SUCCESS && mx > 300)
        hw_max = (int)mx;
    if (hw_max) find_min_clock();

    if (hw_max && nv.setlock && nv.resetlock) {
        for (int i = C_MAX; i <= C_MIN; i++) {
            ctls[i].min = hw_min;
            ctls[i].max = hw_max;
        }
        ctls[C_MAX].def = hw_max;
        ctls[C_MIN].def = hw_min;
        clk.min_mhz = hw_min;
        clk.max_mhz = hw_max;
        ph_register_gpu_clk(&clk);
    } else {
        ctls[C_MAX].flags |= CF_HIDDEN;
        ctls[C_MIN].flags |= CF_HIDDEN;
    }

    unsigned int pmin = 0, pmax = 0, pcur = 0;
    if (nv.plimit && nv.pconstraints && nv.setplimit &&
        nv.pconstraints(dev, &pmin, &pmax) == NVML_SUCCESS && pmax > pmin + 1000 &&
        nv.plimit(dev, &pcur) == NVML_SUCCESS) {
        orig_plimit = pcur;
        ctls[C_PLIM].min = (int32_t)((pmin + 999) / 1000 * 1000);
        ctls[C_PLIM].max = (int32_t)(pmax / 1000 * 1000);
        if (ctls[C_PLIM].min >= ctls[C_PLIM].max) {
            ctls[C_PLIM].min = (int32_t)pmin;
            ctls[C_PLIM].max = (int32_t)pmax;
        }
        ctls[C_PLIM].def = PH_CLAMP((int32_t)pcur, ctls[C_PLIM].min, ctls[C_PLIM].max);
    } else {
        ctls[C_PLIM].flags |= CF_HIDDEN;
    }

    unsigned int v;
    if (!nv.power || nv.power(dev, &v) != NVML_SUCCESS) ctls[C_PWR].flags |= CF_HIDDEN;
    else ph_set_power_reader(PWR_GPU, read_power);
    sample();
    if (s_temp < 0) ctls[C_TEMP].flags |= CF_HIDDEN;

    ph_register_ctls(ctls, PH_ARRAY(ctls));
    return 0;
}

static void nv_shutdown(void)
{
    if (!inited) { unload(); return; }
    lk();
    if (locked) nv.resetlock(dev);
    locked = 0;
    user_min = user_max = clk_min = clk_max = 0;
    if (plimit_written) nv.setplimit(dev, orig_plimit);
    plimit_written = 0;
    ulk();
    unload();
}

static void nv_resume(void)
{
    if (!inited) return;
    s_ms = a_ms = 0;
    lk();
    if (locked) {
        locked = 0;
        apply_lock();
    }
    ulk();
    if (plimit_written && ctls[C_PLIM].active) nv.setplimit(dev, (unsigned)ctls[C_PLIM].val);
}

static void nv_tick(void)
{
    if (!inited) return;
    lk();
    int changed = 0;
    if (user_max && !ctls[C_MAX].active) { user_max = 0; changed = 1; }
    if (user_min && !ctls[C_MIN].active) { user_min = 0; changed = 1; }
    if (changed) apply_lock();
    ulk();
    if (plimit_written && !ctls[C_PLIM].active && nv.setplimit(dev, orig_plimit) == NVML_SUCCESS)
        plimit_written = 0;
}

ph_backend bk_nvidia = {
    "nvidia",
    L"NVIDIA GPU",
    BK_GPU,
    nv_probe,
    nv_init,
    nv_shutdown,
    nv_resume,
    nv_tick,
    0
};
