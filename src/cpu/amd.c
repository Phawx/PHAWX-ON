#include "phawx.h"
#include "smu.h"

#define MSR_HWCR      0xC0010015
#define HWCR_CPBDIS   (1ull << 25)
#define MSR_PSTATE0   0xC0010064
#define MSR_CPPC_CAP1 0xC00102B0
#define MSR_CPPC_EN   0xC00102B1
#define MSR_CPPC_REQ  0xC00102B3
#define MSR_CSTATE    0xC0010296    /* CSTATE_CONFIG: CCR0..2_CC6EN, as ZenStates sets and clears them */
#define CC6_BITS      ((1ull << 22) | (1ull << 14) | (1ull << 6))

#define HWF (CF_OPTIONAL | CF_REAPPLY)
#define GFX_LO 200

static int fam = AF_UNKNOWN;
static int32_t orig[S_COUNT];
static uint32_t wrote;
static int32_t pwr_mw = -1, temp_c = -1;
static int gfx_hi = 1600;

static int cpb_orig = -1, cpb_wrote;
static uint64_t cc6_orig;
static int cc6_wrote;

typedef struct { uint8_t lo, hi, omin, omax, saved; } cppc_core;
static cppc_core *cores;
static int ncores;
static int nom_perf, nom_mhz;
static CRITICAL_SECTION ccs;
static int ccs_ready;

enum {
    C_TDP, C_BOOST, C_HDR, C_PWR, C_TEMP, C_TCTL, C_SLOW, C_APUSLOW, C_STAPMT, C_SLOWT,
    C_SMUMODE, C_CLKHDR, C_CPB, C_CC6, C_GHDR, C_GMIN, C_GMAX, C_N
};
static ph_ctl ctls[C_N];

static int smu_apply(int s, int32_t v)
{
    int r = smu_set(s, (uint32_t)v);
    if (r == 0) wrote |= 1u << s;
    return r;
}

static int fail(ph_ctl *c, int r)
{
    if (r == 1) c->flags |= CF_HIDDEN;
    return r ? -1 : 0;
}

static int pm_mw(int field, int32_t *out)
{
    float f;
    if (smu_pm_get(field, &f) || f < 0.5f || f > 400.0f) return -1;
    *out = (int32_t)(f * 1000.0f + 0.5f);
    return 0;
}

static int get_tdp(ph_ctl *c, int32_t *out)   { return pm_mw(PM_STAPM_LIM, out); }
static int get_boost(ph_ctl *c, int32_t *out) { return pm_mw(PM_FAST_LIM, out); }
static int get_slow(ph_ctl *c, int32_t *out)  { return pm_mw(PM_SLOW_LIM, out); }
static int get_apuslow(ph_ctl *c, int32_t *out) { return pm_mw(PM_APU_SLOW_LIM, out); }

static int get_tctl(ph_ctl *c, int32_t *out)
{
    float f;
    if (smu_pm_get(PM_TCTL_LIM, &f) || f < 40.0f || f > 125.0f) return -1;
    *out = (int32_t)(f + 0.5f);
    return 0;
}

static int set_tdp(ph_ctl *c, int32_t mw)
{
    ph_ctl *b = &ctls[C_BOOST], *sl = &ctls[C_SLOW];
    int32_t fast = -1, cur;
    if (b->active) fast = b->val > mw ? b->val : mw;
    /* a fast limit that cannot be read may be below the new one (Van Gogh ships 15/15/15 W) */
    else if (pm_mw(PM_FAST_LIM, &cur) || cur < mw) fast = mw;
    if (fast > 0 && smu_has(S_FAST_LIMIT)) smu_apply(S_FAST_LIMIT, fast);
    if (!sl->active && smu_has(S_SLOW_LIMIT)) smu_apply(S_SLOW_LIMIT, mw);
    return fail(c, smu_apply(S_STAPM_LIMIT, mw));
}

static int set_boost(ph_ctl *c, int32_t mw)
{
    ph_ctl *t = &ctls[C_TDP];
    if (t->active && t->val > mw) mw = t->val;
    return fail(c, smu_apply(S_FAST_LIMIT, mw));
}

static int set_smu(ph_ctl *c, int32_t v)
{
    return fail(c, smu_apply(c->arg, v));
}

static int gclk_max(ph_clk *d, int mhz)  { return smu_apply(S_MAX_GFXCLK, PH_CLAMP(mhz, GFX_LO, gfx_hi)) ? -1 : 0; }
static int gclk_min(ph_clk *d, int mhz)  { return smu_apply(S_MIN_GFXCLK, PH_CLAMP(mhz, GFX_LO, gfx_hi)) ? -1 : 0; }
static int gclk_reset(ph_clk *d)
{
    ph_ctl *m = &ctls[C_GMAX];
    return gclk_max(d, m->active ? m->val : gfx_hi);
}

static ph_clk gpu_clk = {
    .name = L"iGPU (SMU)", .min_mhz = GFX_LO, .max_mhz = 1600, .step_mhz = 50,
    .set_max = gclk_max, .set_min = gclk_min, .reset = gclk_reset, .prio = 20
};

static int set_gmax(ph_ctl *c, int32_t v) { return gclk_max(&gpu_clk, v); }
static int set_gmin(ph_ctl *c, int32_t v) { return gclk_min(&gpu_clk, v); }

static int get_cpb(ph_ctl *c, int32_t *out)
{
    uint64_t v;
    if (drv_rdmsr(MSR_HWCR, &v)) return -1;
    *out = !(v & HWCR_CPBDIS);
    return 0;
}

static int set_cpb(ph_ctl *c, int32_t on)
{
    if (drv_rmw_all(MSR_HWCR, HWCR_CPBDIS, on ? 0 : HWCR_CPBDIS)) return -1;
    cpb_wrote = 1;
    return 0;
}

/* core C6 on every core; restored from core 0's original bits */
static int get_cc6(ph_ctl *c, int32_t *out)
{
    uint64_t v;
    if (drv_rdmsr(MSR_CSTATE, &v)) return -1;
    *out = (v & CC6_BITS) == CC6_BITS;
    return 0;
}

static int set_cc6(ph_ctl *c, int32_t on)
{
    if (drv_rmw_all(MSR_CSTATE, CC6_BITS, on ? CC6_BITS : 0)) return -1;
    cc6_wrote = 1;
    return 0;
}

static void cc6_restore(void)
{
    if (cc6_wrote && drv_rmw_all(MSR_CSTATE, CC6_BITS, cc6_orig) == 0) cc6_wrote = 0;
}

static void release_cc6(ph_ctl *c) { cc6_restore(); }

static int sub_cc6(const ph_ctl *c, wchar_t *b, int n)
{
    lstrcpynW(b, L"On: lower idle power. Off: less latency, more idle power", n);
    return TONE_DIM;
}

/* the firmware's power-saving or max-performance profile (SMU) */
static const wchar_t *const smumode_ch[] = { L"Power saving", L"Max performance", NULL };

static int set_smumode(ph_ctl *c, int32_t v)
{
    return fail(c, smu_apply(v ? S_MAX_PERF : S_POWER_SAVING, 0));
}

/* nothing reads the profile back: Default goes to the one ALIB picks for the power source */
static void smumode_restore(void)
{
    uint32_t m = (1u << S_MAX_PERF) | (1u << S_POWER_SAVING);
    if (!(wrote & m)) return;
    smu_set(platform_on_ac() ? S_MAX_PERF : S_POWER_SAVING, 0);
    wrote &= ~m;
}

static void release_smumode(ph_ctl *c) { smumode_restore(); }

static int perf_of(int mhz) { return (mhz * nom_perf + nom_mhz / 2) / nom_mhz; }
static int mhz_of(int perf) { return perf * nom_mhz / nom_perf; }

static int cppc_apply(int perf)
{
    int ok = 0;
    EnterCriticalSection(&ccs);
    for (int i = 0; i < ncores; i++) {
        cppc_core *k = &cores[i];
        uint64_t v;
        int mx, mn;
        if (!k->hi || drv_rdmsr_cpu(MSR_CPPC_REQ, i, &v)) continue;
        if (perf < 0) {
            if (!k->saved) { ok++; continue; }
            mx = k->omax;
            mn = k->omin;
        } else {
            if (!k->saved) { k->omax = (uint8_t)v; k->omin = (uint8_t)(v >> 8); k->saved = 1; }
            mx = PH_CLAMP(perf, k->lo, k->hi);
            mn = (int)((v >> 8) & 0xFF);
            if (mn > mx) mn = mx;
        }
        uint64_t nv = (v & ~0xFFFFull) | (uint64_t)mx | ((uint64_t)mn << 8);
        if (nv == v || drv_wrmsr_cpu(MSR_CPPC_REQ, i, nv) == 0) {
            ok++;
            if (perf < 0) k->saved = 0;
        }
    }
    LeaveCriticalSection(&ccs);
    return ok ? 0 : -1;
}

static int cclk_max(ph_clk *d, int mhz) { return cppc_apply(perf_of(mhz)); }
static int cclk_reset(ph_clk *d)        { return cppc_apply(-1); }

static ph_clk cpu_clk = {
    .name = L"CPU (CPPC)", .step_mhz = 100, .set_max = cclk_max, .reset = cclk_reset, .prio = 30
};

static int nominal_mhz(void)
{
    uint64_t p;
    int mhz = 0;
    if (drv_rdmsr(MSR_PSTATE0, &p) == 0 && (p >> 63)) {
        if (g_plat.family >= 0x1A) mhz = (int)(p & 0xFFF) * 5;
        else {
            int fid = (int)(p & 0xFF), did = (int)((p >> 8) & 0x3F);
            if (did) mhz = fid * 200 / did;
        }
    }
    if (mhz < 400 || mhz > 6000) {
        DWORD d = 0;
        mhz = reg_get_dword(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                            L"~MHz", &d) == 0 ? (int)d : 0;
    }
    return (mhz >= 400 && mhz <= 6000) ? mhz : 0;
}

static void cppc_init(void)
{
    uint64_t cap, en, req;
    if (!g_plat.cppc || drv_rdmsr_cpu(MSR_CPPC_CAP1, 0, &cap) || drv_rdmsr_cpu(MSR_CPPC_EN, 0, &en) ||
        !(en & 1) || drv_rdmsr_cpu(MSR_CPPC_REQ, 0, &req) || drv_wrmsr_cpu(MSR_CPPC_REQ, 0, req))
        return;
    nom_perf = (int)((cap >> 16) & 0xFF);
    nom_mhz = nominal_mhz();
    if (!nom_perf || !nom_mhz) return;
    ncores = g_plat.nlogical > 0 ? g_plat.nlogical : (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (ncores <= 0 || !(cores = ph_alloc(sizeof(cppc_core) * (size_t)ncores))) return;
    int hi = 0, lnl = 255;
    for (int i = 0; i < ncores; i++) {
        uint64_t c;
        if (drv_rdmsr_cpu(MSR_CPPC_CAP1, i, &c)) continue;
        cores[i].lo = (uint8_t)c;
        cores[i].hi = (uint8_t)(c >> 24);
        if (cores[i].hi > hi) hi = cores[i].hi;
        int n = (int)((c >> 8) & 0xFF);
        if (n && n < lnl) lnl = n;
    }
    if (!hi || lnl > hi) { ph_free(cores); cores = NULL; ncores = 0; return; }
    if (!ccs_ready) { InitializeCriticalSection(&ccs); ccs_ready = 1; }
    cpu_clk.min_mhz = mhz_of(lnl) / 100 * 100;
    cpu_clk.max_mhz = mhz_of(hi);
    if (cpu_clk.max_mhz > 6000) cpu_clk.max_mhz = 6000;
    cpu_clk.max_mhz = (cpu_clk.max_mhz + 99) / 100 * 100;
    if (cpu_clk.min_mhz < 400) cpu_clk.min_mhz = 400;
    if (cpu_clk.min_mhz >= cpu_clk.max_mhz) return;
    ph_register_cpu_clk(&cpu_clk);
    ph_log("amd: cppc nominal %d perf = %d MHz, range %d-%d MHz", nom_perf, nom_mhz,
           cpu_clk.min_mhz, cpu_clk.max_mhz);
}

/* also for fan curves, which read it while the overlay is closed */
static int read_temp(int *c)
{
    float f;
    if (smu_pm_ok() && !smu_pm_refresh() && !smu_pm_get(PM_TCTL_VAL, &f) && f > 0.0f && f < 125.0f) {
        *c = (int)(f + 0.5f);
        return 0;
    }
    return smu_tctl(c);     /* no PM table (Van Gogh, Mendocino...) */
}

static void sample(void)
{
    float f;
    int t;
    temp_c = read_temp(&t) == 0 ? t : -1;
    if (!smu_pm_ok()) return;
    smu_pm_refresh();
    pwr_mw = smu_pm_get(PM_FAST_VAL, &f) == 0 && f >= 0.0f && f < 400.0f ? (int32_t)(f * 1000.0f) : -1;
}

static void fmt_power(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    if (pwr_mw < 0) lstrcpynW(b, L"--", n);
    else ph_swprintf(b, n, L"%d.%d W", pwr_mw / 1000, (pwr_mw % 1000) / 100);
}

static void fmt_temp(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    if (temp_c < 0) lstrcpynW(b, L"--", n);
    else ph_swprintf(b, n, L"%d \x00B0" L"C", temp_c);
}

static void fmt_c(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    ph_swprintf(b, n, L"%d \x00B0" L"C", v);
}

static void fmt_s(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    ph_swprintf(b, n, L"%d s", v);
}

/* for AutoTDP: APU power (CPU and iGPU together) from the PM table */
static int read_power(int *mw)
{
    float f;
    if (!smu_pm_ok() || smu_pm_refresh() || smu_pm_get(PM_FAST_VAL, &f) || f < 0.0f || f >= 400.0f) return -1;
    *mw = (int)(f * 1000.0f);
    return 0;
}

/* Hardware info's per-core rows: the PM table's arrays where the table is known,
   in the SMU's core order (cores it reports as 0 °C are absent and left out),
   else APERF/MPERF clocks without temperatures */
static uint32_t pm_cores;   /* the table's cores that are there, found on the first read */

static int core_read(int *mhz, int *celsius, int max)
{
    float g[PH_MAX_CORES], t[PH_MAX_CORES];
    int n = smu_pm_cores(g, t, PH_MAX_CORES);
    if (n > 0) {
        if (!pm_cores)
            for (int i = 0; i < n; i++) if (t[i] > 1.0f && t[i] < 125.0f) pm_cores |= 1u << i;
        int k = 0;
        for (int i = 0; i < n && k < max; i++) {
            if (!(pm_cores & (1u << i))) continue;
            celsius[k] = t[i] > 1.0f && t[i] < 125.0f ? (int)(t[i] + 0.5f) : 0;
            mhz[k] = g[i] > 0.05f && g[i] < 7.0f ? (int)(g[i] * 1000.0f + 0.5f) : 0;
            k++;
        }
        if (k) return k;
    }
    return ph_core_aperf(mhz, max);
}

static int get_power(ph_ctl *c, int32_t *out) { *out = pwr_mw; return pwr_mw < 0 ? -1 : 0; }
static int get_temp(ph_ctl *c, int32_t *out)  { *out = temp_c; return temp_c < 0 ? -1 : 0; }

static ph_ctl ctls[C_N] = {
    [C_TDP] = { .key = "cpu.tdp", .label = L"TDP (sustained)", .type = CT_SLIDER, .page = PG_QUICK,
      .flags = HWF | CF_PROFILE, .step = 1000, .unit = L"W", .get = get_tdp, .set = set_tdp,
      .fmt = fmt_watts_mw, .order = 20 },
    [C_BOOST] = { .key = "cpu.tdpboost", .label = L"Boost TDP (fast)", .type = CT_SLIDER, .page = PG_QUICK,
      .flags = HWF | CF_PROFILE, .step = 1000, .unit = L"W", .get = get_boost, .set = set_boost,
      .fmt = fmt_watts_mw, .order = 21 },
    [C_HDR] = { .label = L"Power limits", .type = CT_HEADER, .page = PG_CPU, .order = 100 },
    [C_PWR] = { .label = L"APU power", .type = CT_INFO, .page = PG_CPU,
      .get = get_power, .fmt = fmt_power, .order = 101 },
    [C_TEMP] = { .label = L"Temperature", .type = CT_INFO, .page = PG_CPU,
      .get = get_temp, .fmt = fmt_temp, .order = 102 },
    [C_TCTL] = { .key = "cpu.tctl", .label = L"Temperature limit", .type = CT_SLIDER, .page = PG_CPU,
      .flags = HWF, .min = 60, .max = 105, .step = 1, .def = 95, .unit = L"\x00B0" L"C",
      .get = get_tctl, .set = set_smu, .fmt = fmt_c, .arg = S_TCTL_TEMP, .order = 103 },
    [C_SLOW] = { .key = "cpu.slow", .label = L"Slow limit", .type = CT_SLIDER, .page = PG_CPU,
      .flags = HWF | CF_ADVANCED | CF_PROFILE, .step = 1000, .unit = L"W", .get = get_slow,
      .set = set_smu, .fmt = fmt_watts_mw, .arg = S_SLOW_LIMIT, .order = 104 },
    [C_APUSLOW] = { .key = "cpu.apuslow", .label = L"APU slow limit", .type = CT_SLIDER, .page = PG_CPU,
      .flags = HWF | CF_ADVANCED | CF_PROFILE, .step = 1000, .unit = L"W", .get = get_apuslow,
      .set = set_smu, .fmt = fmt_watts_mw, .arg = S_APU_SLOW_LIMIT, .order = 105 },
    [C_STAPMT] = { .key = "cpu.stapmtime", .label = L"STAPM time", .type = CT_SLIDER, .page = PG_CPU,
      .flags = HWF | CF_ADVANCED, .min = 10, .max = 500, .step = 10, .def = 200, .unit = L"s",
      .set = set_smu, .fmt = fmt_s, .arg = S_STAPM_TIME, .order = 106 },
    [C_SLOWT] = { .key = "cpu.slowtime", .label = L"Slow time", .type = CT_SLIDER, .page = PG_CPU,
      .flags = HWF | CF_ADVANCED, .min = 1, .max = 60, .step = 1, .def = 5, .unit = L"s",
      .set = set_smu, .fmt = fmt_s, .arg = S_SLOW_TIME, .order = 107 },
    [C_SMUMODE] = { .key = "cpu.smumode", .label = L"Firmware power profile", .type = CT_CHOICE, .page = PG_CPU,
      .flags = HWF | CF_ADVANCED, .choices = smumode_ch, .max = 1, .def = 1, .set = set_smumode,
      .release = release_smumode, .order = 108 },
    [C_CLKHDR] = { .label = L"CPU clocks", .type = CT_HEADER, .page = PG_CPU, .order = 110 },
    [C_CPB] = { .key = "cpu.cpb", .label = L"Core Performance Boost", .type = CT_TOGGLE, .page = PG_CPU,
      .flags = HWF, .def = 1, .get = get_cpb, .set = set_cpb, .fmt = fmt_onoff, .order = 111 },
    [C_CC6] = { .key = "cpu.cc6", .label = L"Deep core sleep (CC6)", .type = CT_TOGGLE, .page = PG_CPU,
      .flags = HWF | CF_ADVANCED, .max = 1, .def = 1, .get = get_cc6, .set = set_cc6, .fmt = fmt_onoff,
      .sub = sub_cc6, .release = release_cc6, .order = 112 },
    [C_GHDR] = { .label = L"Integrated GPU (SMU)", .type = CT_HEADER, .page = PG_GPU, .order = 300 },
    [C_GMIN] = { .key = "gpu.igpumin", .label = L"iGPU min clock", .type = CT_SLIDER, .page = PG_GPU,
      .flags = HWF | CF_PROFILE, .min = GFX_LO, .step = 50, .def = GFX_LO, .unit = L"MHz",
      .set = set_gmin, .fmt = fmt_mhz, .order = 301 },
    [C_GMAX] = { .key = "gpu.igpumax", .label = L"iGPU max clock", .type = CT_SLIDER, .page = PG_GPU,
      .flags = HWF | CF_PROFILE | CF_AUTOTDP, .min = GFX_LO, .step = 50, .unit = L"MHz",
      .set = set_gmax, .fmt = fmt_mhz, .order = 302 },
};

static void hide(int i) { ctls[i].flags |= CF_HIDDEN; }

static void limits_init(void)
{
    int32_t lo = 3000, hi = 65000, v;
    uint32_t f = fam >= 0 ? 1u << fam : 0;
    if (f & ((1u << AF_VANGOGH) | (1u << AF_MENDOCINO))) hi = 30000;
    else if (fam == AF_STRIXHALO) hi = 140000;
    else if (f & ((1u << AF_DRAGON) | (1u << AF_FIRE))) hi = 180000;

    static const struct { int c, s, pm; int32_t def; } lim[] = {
        { C_TDP, S_STAPM_LIMIT, PM_STAPM_LIM, 15000 },
        { C_BOOST, S_FAST_LIMIT, PM_FAST_LIM, 25000 },
        { C_SLOW, S_SLOW_LIMIT, PM_SLOW_LIM, 20000 },
        { C_APUSLOW, S_APU_SLOW_LIMIT, PM_APU_SLOW_LIM, 20000 },
    };
    for (int i = 0; i < PH_ARRAY(lim); i++) {
        ph_ctl *c = &ctls[lim[i].c];
        int32_t d = lim[i].def;
        if (pm_mw(lim[i].pm, &v) == 0) { orig[lim[i].s] = v; d = v; if (v > hi) hi = (v + 999) / 1000 * 1000; }
        c->min = lo;
        c->max = hi;
        c->def = PH_CLAMP((d + 500) / 1000 * 1000, lo, hi);
        if (!smu_has(lim[i].s)) hide(lim[i].c);
    }
    if (get_tctl(NULL, &v) == 0) {
        orig[S_TCTL_TEMP] = v;
        ctls[C_TCTL].def = PH_CLAMP(v, ctls[C_TCTL].min, ctls[C_TCTL].max);
    }
    if (!smu_has(S_TCTL_TEMP)) hide(C_TCTL);
    if (!smu_has(S_STAPM_TIME)) hide(C_STAPMT);
    if (!smu_has(S_SLOW_TIME)) hide(C_SLOWT);
    if (!smu_has(S_MAX_PERF) || !smu_has(S_POWER_SAVING)) hide(C_SMUMODE);
    else ctls[C_SMUMODE].def = platform_on_ac() ? 1 : 0;
    int t;
    if (!smu_pm_ok()) hide(C_PWR);
    if (read_temp(&t)) hide(C_TEMP);
    if (!smu_has(S_STAPM_LIMIT) && !smu_has(S_TCTL_TEMP) && !smu_pm_ok() && (ctls[C_TEMP].flags & CF_HIDDEN)) hide(C_HDR);

    gfx_hi = fam == AF_LUCIENNE ? 2000 : 1600;
    gpu_clk.max_mhz = gfx_hi;
    ctls[C_GMIN].max = ctls[C_GMAX].max = ctls[C_GMAX].def = gfx_hi;
    if (smu_has(S_MAX_GFXCLK)) ph_register_gpu_clk(&gpu_clk);
    else { hide(C_GHDR); hide(C_GMAX); }
    if (!smu_has(S_MIN_GFXCLK)) { hide(C_GMIN); gpu_clk.set_min = NULL; }
}

static void cpb_init(void)
{
    uint64_t v;
    if (drv_rdmsr_cpu(MSR_HWCR, 0, &v) || drv_wrmsr_cpu(MSR_HWCR, 0, v)) {
        hide(C_CPB);
        return;
    }
    cpb_orig = !(v & HWCR_CPBDIS);
    ctls[C_CPB].def = cpb_orig;
}

static void cc6_init(void)
{
    uint64_t v;
    if (drv_rdmsr_cpu(MSR_CSTATE, 0, &v) || drv_wrmsr_cpu(MSR_CSTATE, 0, v)) {
        hide(C_CC6);
        return;
    }
    cc6_orig = v & CC6_BITS;
    ctls[C_CC6].def = cc6_orig == CC6_BITS;
}

static int amd_probe(void) { return g_plat.vendor == VENDOR_AMD; }

static int amd_init(void)
{
    for (int i = 0; i < S_COUNT; i++) orig[i] = -1;
    wrote = 0;
    cpb_wrote = 0;
    cc6_wrote = 0;
    fam = smu_detect();
    if (!drv_ok()) return 0;
    if (fam != AF_UNKNOWN) smu_init();
    limits_init();
    cpb_init();
    cc6_init();
    cppc_init();
    if ((ctls[C_CPB].flags & CF_HIDDEN) && (ctls[C_CC6].flags & CF_HIDDEN)) hide(C_CLKHDR);
    sample();
    int t;
    if (read_temp(&t) == 0) ph_set_cpu_temp_reader(read_temp);
    if (smu_pm_ok()) ph_set_power_reader(PWR_PKG, read_power);
    pm_cores = 0;
    ph_set_core_reader(core_read);
    ph_register_ctls(ctls, C_N);
    return 0;
}

static void amd_shutdown(void)
{
    static const int order[] = { S_FAST_LIMIT, S_SLOW_LIMIT, S_APU_SLOW_LIMIT, S_STAPM_LIMIT, S_TCTL_TEMP };
    for (int i = 0; i < PH_ARRAY(order); i++) {
        int s = order[i];
        if ((wrote & (1u << s)) && orig[s] > 0) smu_set(s, (uint32_t)orig[s]);
    }
    if (wrote & (1u << S_MAX_GFXCLK)) smu_set(S_MAX_GFXCLK, (uint32_t)gfx_hi);
    if (wrote & (1u << S_MIN_GFXCLK)) smu_set(S_MIN_GFXCLK, GFX_LO);
    smumode_restore();
    wrote = 0;
    if (cores) cppc_apply(-1);
    if (cpb_wrote && cpb_orig >= 0) drv_rmw_all(MSR_HWCR, HWCR_CPBDIS, cpb_orig ? 0 : HWCR_CPBDIS);
    cpb_wrote = 0;
    cc6_restore();
}

static void amd_resume(void)
{
    pwr_mw = temp_c = -1;
}

static void amd_tick(void)
{
    sample();
}

ph_backend bk_amd = {
    "amd",
    L"AMD CPU",
    BK_CPU,
    amd_probe,
    amd_init,
    amd_shutdown,
    amd_resume,
    amd_tick,
    0
};
