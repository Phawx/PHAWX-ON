#include "phawx.h"

#define MSR_RAPL_UNIT     0x606
#define MSR_PKG_LIMIT     0x610
#define MSR_PKG_ENERGY    0x611
#define MSR_PKG_INFO      0x614
#define MSR_TEMP_TARGET   0x1A2
#define MSR_PKG_THERM     0x1B1

#define PL_LOCK           (1ull << 63)
#define PL_PWR_MASK       0x7FFFull
#define PL_EN             (1ull << 15)
#define PL_CLAMP          (1ull << 16)
#define PL_TW_SHIFT       17
#define PL_TW_MASK        (0x7Full << PL_TW_SHIFT)
#define PL2_SHIFT         32

static int pwr_unit, energy_unit, time_unit;
static uint64_t orig_limit;
static int wrote;
static int tjmax = 100;
static int pkg_mw = -1, pkg_temp = -1;
static uint32_t last_energy;
static double last_ms;
static int have_energy;

static const int32_t tw_ms[] = {
    2, 10, 50, 100, 250, 500, 1000, 2000, 4000, 8000,
    16000, 28000, 32000, 56000, 64000, 96000, 128000
};
static const wchar_t *const tw_names[] = {
    L"2 ms", L"10 ms", L"50 ms", L"100 ms", L"250 ms", L"500 ms", L"1 s", L"2 s", L"4 s", L"8 s",
    L"16 s", L"28 s", L"32 s", L"56 s", L"64 s", L"96 s", L"128 s", NULL
};

static int32_t raw_to_mw(uint64_t raw)
{
    return (int32_t)(((raw & PL_PWR_MASK) * 1000ull) >> pwr_unit);
}

static uint64_t mw_to_raw(int32_t mw)
{
    uint64_t r = (((uint64_t)(mw > 0 ? mw : 0) << pwr_unit) + 500) / 1000;
    if (r < 1) r = 1;
    return r > PL_PWR_MASK ? PL_PWR_MASK : r;
}

static double tw_decode_ms(uint32_t f)
{
    uint32_t y = f & 0x1F, z = (f >> 5) & 3;
    return (double)(1ull << y) * (1.0 + z / 4.0) * 1000.0 / (double)(1u << time_unit);
}

static uint32_t tw_encode(int32_t ms)
{
    double want = (double)ms, best = 1e300;
    uint32_t bf = 0;
    for (uint32_t y = 0; y < 32; y++)
        for (uint32_t z = 0; z < 4; z++) {
            double t = tw_decode_ms(y | (z << 5));
            double d = t > want ? t / want : want / t;
            if (d < best) { best = d; bf = y | (z << 5); }
        }
    return bf;
}

static int tw_index(double ms)
{
    int bi = 0;
    double best = 1e300;
    for (int i = 0; i < PH_ARRAY(tw_ms); i++) {
        double d = ms > tw_ms[i] ? ms / tw_ms[i] : tw_ms[i] / ms;
        if (d < best) { best = d; bi = i; }
    }
    return bi;
}

static int read_limit(uint64_t *v)
{
    if (drv_rdmsr(MSR_PKG_LIMIT, v)) return -1;
    return (*v & PL_LOCK) ? -1 : 0;
}

static int write_limit(uint64_t v)
{
    v &= ~PL_LOCK;
    if (drv_wrmsr(MSR_PKG_LIMIT, v)) return -1;
    wrote = 1;
    return 0;
}

static int get_pl(ph_ctl *c, int32_t *out)
{
    uint64_t v;
    if (drv_rdmsr(MSR_PKG_LIMIT, &v)) return -1;
    *out = raw_to_mw(v >> (c->arg ? PL2_SHIFT : 0));
    return 0;
}

static int set_pl(ph_ctl *c, int32_t mw)
{
    uint64_t v;
    if (read_limit(&v)) return -1;
    int sh = c->arg ? PL2_SHIFT : 0;
    uint64_t field = PL_PWR_MASK | PL_EN | PL_CLAMP;
    v = (v & ~(field << sh)) | ((mw_to_raw(mw) | PL_EN | PL_CLAMP) << sh);
    if (!c->arg) {
        uint64_t pl2 = (v >> PL2_SHIFT) & PL_PWR_MASK;
        if (pl2 < mw_to_raw(mw))
            v = (v & ~(field << PL2_SHIFT)) | ((mw_to_raw(mw) | PL_EN | PL_CLAMP) << PL2_SHIFT);
    }
    return write_limit(v);
}

static int get_tw(ph_ctl *c, int32_t *out)
{
    uint64_t v;
    if (drv_rdmsr(MSR_PKG_LIMIT, &v)) return -1;
    uint64_t f = (v >> (c->arg ? PL2_SHIFT : 0)) >> PL_TW_SHIFT;
    *out = tw_index(tw_decode_ms((uint32_t)(f & 0x7F)));
    return 0;
}

static int set_tw(ph_ctl *c, int32_t idx)
{
    uint64_t v;
    if (idx < 0 || idx >= PH_ARRAY(tw_ms) || read_limit(&v)) return -1;
    int sh = c->arg ? PL2_SHIFT : 0;
    v = (v & ~(PL_TW_MASK << sh)) | ((uint64_t)tw_encode(tw_ms[idx]) << (PL_TW_SHIFT + sh));
    return write_limit(v);
}

static void sample(void)
{
    uint64_t v;
    if (!drv_rdmsr(MSR_PKG_THERM, &v) && (v & (1ull << 31)))
        pkg_temp = tjmax - (int)((v >> 16) & 0x7F);
    if (!drv_rdmsr(MSR_PKG_ENERGY, &v)) {
        uint32_t e = (uint32_t)v;
        double now = ph_qpc_ms();
        if (have_energy && now - last_ms > 3000.0) have_energy = 0;
        if (have_energy && now - last_ms > 50.0) {
            uint32_t d = e - last_energy;
            double w = (double)d / (double)(1ull << energy_unit) / ((now - last_ms) / 1000.0);
            pkg_mw = (int)(w * 1000.0);
        }
        if (!have_energy || now - last_ms > 50.0) {
            last_energy = e;
            last_ms = now;
            have_energy = 1;
        }
    }
}

static void fmt_power(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    if (pkg_mw < 0) lstrcpynW(b, L"--", n);
    else ph_swprintf(b, n, L"%d.%d W", pkg_mw / 1000, (pkg_mw % 1000) / 100);
}

static void fmt_temp(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    if (pkg_temp < 0) lstrcpynW(b, L"--", n);
    else ph_swprintf(b, n, L"%d \x00B0" L"C", pkg_temp);
}

static void fmt_locked(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    lstrcpynW(b, L"Locked by firmware", n);
}

static int get_power(ph_ctl *c, int32_t *out) { *out = pkg_mw; return pkg_mw < 0 ? -1 : 0; }
static int get_temp(ph_ctl *c, int32_t *out) { *out = pkg_temp; return pkg_temp < 0 ? -1 : 0; }

#define HWF (CF_OPTIONAL | CF_REAPPLY)

static ph_ctl ctls[] = {
    { .key = "cpu.tdp", .label = L"TDP (PL1)", .type = CT_SLIDER, .page = PG_QUICK,
      .flags = HWF | CF_PROFILE, .step = 1000, .unit = L"W", .get = get_pl, .set = set_pl,
      .fmt = fmt_watts_mw, .arg = 0, .order = 20 },
    { .key = "cpu.tdpboost", .label = L"Boost TDP (PL2)", .type = CT_SLIDER, .page = PG_QUICK,
      .flags = HWF | CF_PROFILE, .step = 1000, .unit = L"W", .get = get_pl, .set = set_pl,
      .fmt = fmt_watts_mw, .arg = 1, .order = 21 },
    { .key = NULL, .label = L"Power limits", .type = CT_HEADER, .page = PG_CPU, .order = 100 },
    { .key = NULL, .label = L"Package power", .type = CT_INFO, .page = PG_CPU,
      .get = get_power, .fmt = fmt_power, .order = 101 },
    { .key = NULL, .label = L"Package temperature", .type = CT_INFO, .page = PG_CPU,
      .get = get_temp, .fmt = fmt_temp, .order = 102 },
    { .key = NULL, .label = L"Limit status", .type = CT_INFO, .page = PG_CPU,
      .flags = CF_HIDDEN, .fmt = fmt_locked, .order = 103 },
    { .key = "cpu.pl1time", .label = L"PL1 time window", .type = CT_CHOICE, .page = PG_CPU,
      .flags = HWF | CF_ADVANCED, .choices = tw_names, .get = get_tw, .set = set_tw,
      .arg = 0, .order = 104 },
    { .key = "cpu.pl2time", .label = L"PL2 time window", .type = CT_CHOICE, .page = PG_CPU,
      .flags = HWF | CF_ADVANCED, .choices = tw_names, .get = get_tw, .set = set_tw,
      .arg = 1, .order = 105 },
};

enum { C_PL1, C_PL2, C_HDR, C_PWR, C_TEMP, C_LOCK, C_TW1, C_TW2 };

static int intel_probe(void) { return g_plat.vendor == VENDOR_INTEL; }

static int intel_init(void)
{
    uint64_t u, lim, info = 0, tt;
    if (!drv_ok() || drv_rdmsr(MSR_RAPL_UNIT, &u) || drv_rdmsr(MSR_PKG_LIMIT, &lim)) return 0;
    pwr_unit = (int)(u & 0xF);
    energy_unit = (int)((u >> 8) & 0x1F);
    time_unit = (int)((u >> 16) & 0xF);
    orig_limit = lim;
    wrote = 0;
    if (!drv_rdmsr(MSR_TEMP_TARGET, &tt)) {
        int t = (int)((tt >> 16) & 0xFF);
        if (t >= 60 && t <= 130) tjmax = t;
    }
    drv_rdmsr(MSR_PKG_INFO, &info);

    int32_t pl1 = raw_to_mw(lim), pl2 = raw_to_mw(lim >> PL2_SHIFT);
    int32_t tdp = raw_to_mw(info), pmin = raw_to_mw(info >> 16), pmax = raw_to_mw(info >> 32);
    int32_t lo = pmin >= 1000 ? pmin : 3000;
    int32_t hi = pmax > tdp ? pmax : tdp * 5 / 2;
    if (hi < pl1) hi = pl1;
    if (hi < pl2) hi = pl2;
    if (hi < 30000) hi = 30000;
    if (hi > 250000) hi = 250000;
    lo = lo / 1000 * 1000;
    hi = (hi + 999) / 1000 * 1000;
    if (lo >= hi) lo = 1000;

    for (int i = C_PL1; i <= C_PL2; i++) {
        int32_t cur = i == C_PL1 ? pl1 : pl2;
        ctls[i].min = lo;
        ctls[i].max = hi;
        ctls[i].def = PH_CLAMP((cur + 500) / 1000 * 1000, lo, hi);
    }
    ctls[C_TW1].def = tw_index(tw_decode_ms((uint32_t)((lim >> PL_TW_SHIFT) & 0x7F)));
    ctls[C_TW2].def = tw_index(tw_decode_ms((uint32_t)((lim >> (PL2_SHIFT + PL_TW_SHIFT)) & 0x7F)));

    if (lim & PL_LOCK) {
        ctls[C_PL1].flags |= CF_HIDDEN;
        ctls[C_PL2].flags |= CF_HIDDEN;
        ctls[C_TW1].flags |= CF_HIDDEN;
        ctls[C_TW2].flags |= CF_HIDDEN;
        ctls[C_LOCK].flags &= (uint16_t)~CF_HIDDEN;
    }
    uint64_t t;
    if (drv_rdmsr(MSR_PKG_THERM, &t)) ctls[C_TEMP].flags |= CF_HIDDEN;
    if (drv_rdmsr(MSR_PKG_ENERGY, &t)) ctls[C_PWR].flags |= CF_HIDDEN;

    sample();
    ph_register_ctls(ctls, PH_ARRAY(ctls));
    return 0;
}

static void intel_shutdown(void)
{
    uint64_t v;
    if (!wrote || drv_rdmsr(MSR_PKG_LIMIT, &v) || (v & PL_LOCK)) return;
    drv_wrmsr(MSR_PKG_LIMIT, orig_limit & ~PL_LOCK);
    wrote = 0;
}

static void intel_resume(void)
{
    have_energy = 0;
}

static void intel_tick(void)
{
    sample();
}

ph_backend bk_intel = {
    "intel",
    L"Intel CPU",
    BK_CPU,
    intel_probe,
    intel_init,
    intel_shutdown,
    intel_resume,
    intel_tick,
    0
};
