/* RyzenAdj adapter for Phawx ON.
 *
 * Uses libryzenadj.dll from RyzenAdj's Windows release (github.com/FlyGoat/RyzenAdj)
 * for AMD APU power limits on machines where Phawx ON's own SMU control (through
 * PawnIO) is not available. Put libryzenadj.dll and the files that come with it
 * (WinRing0x64.dll, WinRing0x64.sys, inpoutx64.dll) in plugins\ryzenadj\ next to
 * this DLL. libryzenadj talks to the hardware through those drivers, not PawnIO.
 *
 * Only the entry points below are used, resolved at run time; no RyzenAdj code or
 * header is included, so this file stays MIT like the rest of Phawx ON.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "phawx_plugin.h"

typedef void *ryzen_access;
typedef ryzen_access (__stdcall *fn_init)(void);
typedef void  (__stdcall *fn_cleanup)(ryzen_access);
typedef int   (*fn_family)(ryzen_access);
typedef int   (__stdcall *fn_table)(ryzen_access);
typedef int   (__stdcall *fn_set)(ryzen_access, uint32_t);
typedef float (__stdcall *fn_get)(ryzen_access);

/* enum ryzen_family values from RyzenAdj's public API */
enum { FAM_RAVEN = 0, FAM_PICASSO = 1, FAM_DALI = 4, FAM_LUCIENNE = 5, FAM_VANGOGH = 6,
       FAM_MENDOCINO = 8, FAM_DRAGONRANGE = 11, FAM_STRIXHALO = 14, FAM_FIRERANGE = 15 };

static struct {
    HMODULE lib;
    ryzen_access ry;
    fn_init init;
    fn_cleanup cleanup;
    fn_family family;
    fn_table init_table, refresh_table;
    fn_set set_stapm, set_fast, set_slow, set_tctl, set_gfxmax;
    fn_get stapm_lim, fast_lim, fast_val, slow_lim, tctl_lim, tctl_val;
} ra;

static const phx_host *H;
static phx_plugin *SELF;
static HANDLE pci_mx;
static int fam = -1, have_table, gfx_top, gfx_touched;
static DWORD last_refresh;

enum { L_STAPM, L_FAST, L_SLOW, L_TCTL, L_N };
static int32_t orig[L_N] = { -1, -1, -1, -1 };   /* mW, or degrees C for Tctl */
static unsigned wrote;

/* The SMU mailbox has no owner. Tools that share it take this mutex around each
   transaction, and so does Phawx ON's built-in SMU code. */
static int lock(void)
{
    if (!pci_mx) return 0;
    DWORD w = WaitForSingleObject(pci_mx, 500);
    return w == WAIT_OBJECT_0 || w == WAIT_ABANDONED ? 0 : -1;
}
static void unlock(void) { if (pci_mx) ReleaseMutex(pci_mx); }

static int valid(float f) { return f == f && f > 0.0f && f < 1000.0f; }

/* the power table is refreshed at most twice a second; the SMU rejects faster transfers */
static void refresh(void)
{
    DWORD now = GetTickCount();
    if (!have_table || now - last_refresh < 500) return;
    last_refresh = now;
    if (lock() == 0) {
        ra.refresh_table(ra.ry);
        unlock();
    }
}

static int set_locked(fn_set f, uint32_t v)
{
    if (!f || lock()) return -1;
    int r = f(ra.ry, v);
    unlock();
    return r;
}

static float get_locked(fn_get f)
{
    float v = -1.0f;
    if (!f || lock()) return v;
    v = f(ra.ry);
    unlock();
    return v;
}

static int32_t watts_mw(float w) { return (int32_t)(w * 1000.0f + 0.5f); }

/* ---------- controls ---------- */

static void PHX_CALL fmt_w(const phx_control *c, int32_t v, wchar_t *b, int n)
{
    (void)c;
    if (v < 0) { lstrcpynW(b, L"--", n); return; }
    if (v % 1000) _snwprintf(b, (size_t)n, L"%d.%d W", v / 1000, (v % 1000) / 100);
    else _snwprintf(b, (size_t)n, L"%d W", v / 1000);
}

static void PHX_CALL fmt_c(const phx_control *c, int32_t v, wchar_t *b, int n)
{
    (void)c;
    if (v < 0) lstrcpynW(b, L"--", n);
    else _snwprintf(b, (size_t)n, L"%d \x00B0" L"C", v);
}

static int PHX_CALL get_power(phx_control *c, int32_t *out)
{
    (void)c;
    refresh();
    float f = get_locked(ra.fast_val);
    if (!valid(f)) return -1;
    *out = watts_mw(f);
    return 0;
}

static int PHX_CALL get_temp(phx_control *c, int32_t *out)
{
    (void)c;
    refresh();
    float f = get_locked(ra.tctl_val);
    if (!valid(f)) return -1;
    *out = (int32_t)(f + 0.5f);
    return 0;
}

static int PHX_CALL get_limit(phx_control *c, int32_t *out)
{
    static fn_get *const g[L_N] = { &ra.stapm_lim, &ra.fast_lim, &ra.slow_lim, &ra.tctl_lim };
    int k = (int)(intptr_t)c->user;
    refresh();
    float f = get_locked(*g[k]);
    if (!valid(f)) return -1;
    *out = k == L_TCTL ? (int32_t)(f + 0.5f) : watts_mw(f);
    return 0;
}

enum { C_HDR, C_PWR, C_TEMP, C_STAPM, C_FAST, C_SLOW, C_TCTL, C_N };
static phx_control ctl[C_N];

static int apply(int k, int32_t v)
{
    static fn_set *const s[L_N] = { &ra.set_stapm, &ra.set_fast, &ra.set_slow, &ra.set_tctl };
    if (set_locked(*s[k], (uint32_t)v)) return -1;
    wrote |= 1u << k;
    return 0;
}

/* same rules as the built-in AMD control: the fast limit is never below the
   sustained one, and the slow limit follows the sustained one unless it is set */
static int PHX_CALL set_stapm(phx_control *c, int32_t mw)
{
    (void)c;
    int32_t fast = ctl[C_FAST].active ? ctl[C_FAST].value : -1, cur;
    if (fast >= 0 && fast < mw) fast = mw;
    if (fast < 0 && get_limit(&ctl[C_FAST], &cur) == 0 && cur < mw) fast = mw;
    if (fast > 0) apply(L_FAST, fast);
    if (!ctl[C_SLOW].active) apply(L_SLOW, mw);
    return apply(L_STAPM, mw);
}

static int PHX_CALL set_fast(phx_control *c, int32_t mw)
{
    (void)c;
    if (ctl[C_STAPM].active && ctl[C_STAPM].value > mw) mw = ctl[C_STAPM].value;
    return apply(L_FAST, mw);
}

static int PHX_CALL set_slow(phx_control *c, int32_t mw) { (void)c; return apply(L_SLOW, mw); }
static int PHX_CALL set_tctl(phx_control *c, int32_t t) { (void)c; return apply(L_TCTL, t); }

#define HW (PHX_F_OPTIONAL | PHX_F_REAPPLY)

static phx_control ctl[C_N] = {
    [C_HDR] = { .size = sizeof(phx_control), .type = PHX_HEADER, .label = L"RyzenAdj", .page = PHX_PAGE_CPU, .order = 0 },
    [C_PWR] = { .size = sizeof(phx_control), .type = PHX_INFO, .label = L"APU power", .page = PHX_PAGE_CPU,
                .order = 1, .get = get_power, .fmt = fmt_w },
    [C_TEMP] = { .size = sizeof(phx_control), .type = PHX_INFO, .label = L"Temperature", .page = PHX_PAGE_CPU,
                 .order = 2, .get = get_temp, .fmt = fmt_c },
    [C_STAPM] = { .size = sizeof(phx_control), .type = PHX_SLIDER, .key = "stapm", .label = L"TDP (sustained)",
                  .page = PHX_PAGE_CPU, .order = 3, .flags = HW | PHX_F_PROFILE, .min = 3000, .max = 65000,
                  .step = 1000, .def = 15000, .unit = L"W", .get = get_limit, .set = set_stapm, .fmt = fmt_w,
                  .user = (void *)(intptr_t)L_STAPM },
    [C_FAST] = { .size = sizeof(phx_control), .type = PHX_SLIDER, .key = "fast", .label = L"Boost TDP (fast)",
                 .page = PHX_PAGE_CPU, .order = 4, .flags = HW | PHX_F_PROFILE, .min = 3000, .max = 65000,
                 .step = 1000, .def = 25000, .unit = L"W", .get = get_limit, .set = set_fast, .fmt = fmt_w,
                 .user = (void *)(intptr_t)L_FAST },
    [C_SLOW] = { .size = sizeof(phx_control), .type = PHX_SLIDER, .key = "slow", .label = L"Slow limit",
                 .page = PHX_PAGE_CPU, .order = 5, .flags = HW | PHX_F_PROFILE | PHX_F_ADVANCED, .min = 3000,
                 .max = 65000, .step = 1000, .def = 20000, .unit = L"W", .get = get_limit, .set = set_slow,
                 .fmt = fmt_w, .user = (void *)(intptr_t)L_SLOW },
    [C_TCTL] = { .size = sizeof(phx_control), .type = PHX_SLIDER, .key = "tctl", .label = L"Temperature limit",
                 .page = PHX_PAGE_CPU, .order = 6, .flags = HW, .min = 60, .max = 105, .step = 1, .def = 95,
                 .unit = L"\x00B0" L"C", .get = get_limit, .set = set_tctl, .fmt = fmt_c,
                 .user = (void *)(intptr_t)L_TCTL },
};

/* ---------- iGPU clock for AutoTDP (Raven, Picasso, Dali, Lucienne only) ---------- */

static int PHX_CALL gfx_max(phx_clock *d, int mhz)
{
    (void)d;
    if (mhz < 200) mhz = 200;
    if (mhz > gfx_top) mhz = gfx_top;
    if (set_locked(ra.set_gfxmax, (uint32_t)mhz)) return -1;
    gfx_touched = mhz != gfx_top;
    return 0;
}

static int PHX_CALL gfx_reset(phx_clock *d) { return gfx_max(d, gfx_top); }

static phx_clock gfx = {
    .size = sizeof(phx_clock), .kind = PHX_CLOCK_GPU, .name = L"iGPU (RyzenAdj)", .min_mhz = 200, .max_mhz = 1600,
    .step_mhz = 50, .prio = 20, .set_max = gfx_max, .reset = gfx_reset,
};

/* ---------- lifetime ---------- */

static void unload(void)
{
    if (ra.ry && ra.cleanup) ra.cleanup(ra.ry);
    ra.ry = NULL;
    if (ra.lib) FreeLibrary(ra.lib);
    ra.lib = NULL;
    if (pci_mx) { CloseHandle(pci_mx); pci_mx = NULL; }
}

static void PHX_CALL on_shutdown(void)
{
    /* put back what the firmware had, fast first so it never drops below sustained */
    static const int order[] = { L_FAST, L_SLOW, L_STAPM, L_TCTL };
    for (int i = 0; i < 4; i++) {
        int k = order[i];
        if ((wrote & (1u << k)) && orig[k] > 0) apply(k, orig[k]);
    }
    wrote = 0;
    if (gfx_touched) gfx_max(&gfx, gfx_top);
    unload();
}

static const phx_info info = {
    .size = sizeof(phx_info), .name = L"RyzenAdj", .version = L"1.0", .shutdown = on_shutdown,
};

static int fail(int rc, const wchar_t *why)
{
    H->set_status(SELF, why);
    unload();
    return rc;
}

#define SYM(f, name) (ra.f = (__typeof__(ra.f))GetProcAddress(ra.lib, name))

PHX_EXPORT int PHX_CALL phx_plugin_init(const phx_host *host, phx_plugin *self, const phx_info **out)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    int32_t dummy;
    H = host;
    SELF = self;
    if (host->version < PHX_API_VERSION) return PHX_E_VERSION;
    if (host->platform->vendor != 2) { host->set_status(self, L"needs an AMD Ryzen APU"); return PHX_UNSUPPORTED; }
    /* Phawx ON's own SMU control is the safer path; two tools on one limit would fight */
    if (host->ctl_get(self, "cpu.tdp", &dummy, NULL) == 0) {
        host->set_status(self, L"not needed, Phawx ON controls this APU itself");
        return PHX_UNSUPPORTED;
    }

    lstrcpynW(dir, host->plugin_dir(self), MAX_PATH);
    /* libryzenadj loads inpoutx64.DLL by bare name; loading it first from here makes
       that find this copy instead of searching the exe folder and PATH */
    static const wchar_t *const deps[] = { L"inpoutx64.dll", L"WinRing0x64.dll" };
    for (int i = 0; i < 2; i++) {
        _snwprintf(path, MAX_PATH, L"%s\\%s", dir, deps[i]);
        path[MAX_PATH - 1] = 0;
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
            LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    _snwprintf(path, MAX_PATH, L"%s\\libryzenadj.dll", dir);
    path[MAX_PATH - 1] = 0;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        return fail(PHX_ERROR, L"libryzenadj.dll is missing from plugins\\ryzenadj");
    ra.lib = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ra.lib) return fail(PHX_ERROR, L"libryzenadj.dll or a DLL it needs could not be loaded");
    SYM(init, "init_ryzenadj");
    SYM(cleanup, "cleanup_ryzenadj");
    SYM(family, "get_cpu_family");
    SYM(init_table, "init_table");
    SYM(refresh_table, "refresh_table");
    SYM(set_stapm, "set_stapm_limit");
    SYM(set_fast, "set_fast_limit");
    SYM(set_slow, "set_slow_limit");
    SYM(set_tctl, "set_tctl_temp");
    SYM(set_gfxmax, "set_max_gfxclk_freq");
    SYM(stapm_lim, "get_stapm_limit");
    SYM(fast_lim, "get_fast_limit");
    SYM(fast_val, "get_fast_value");
    SYM(slow_lim, "get_slow_limit");
    SYM(tctl_lim, "get_tctl_temp");
    SYM(tctl_val, "get_tctl_temp_value");
    if (!ra.init || !ra.cleanup || !ra.family || !ra.set_stapm || !ra.set_fast || !ra.set_slow || !ra.set_tctl)
        return fail(PHX_ERROR, L"this libryzenadj.dll is too old or not RyzenAdj");

    pci_mx = CreateMutexW(NULL, FALSE, L"Global\\Access_PCI");
    if (!pci_mx) pci_mx = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, L"Global\\Access_PCI");
    if (lock()) return fail(PHX_ERROR, L"another tool is holding the SMU");
    ra.ry = ra.init();
    if (ra.ry) {
        fam = ra.family(ra.ry);
        have_table = ra.init_table && ra.refresh_table && ra.init_table(ra.ry) == 0;
    }
    unlock();
    if (!ra.ry) return fail(PHX_ERROR, L"RyzenAdj could not open its driver (Windows may block WinRing0)");
    if (fam < 0) return fail(PHX_UNSUPPORTED, L"RyzenAdj does not support this CPU");

    /* ranges like the built-in control; start from the firmware's own values */
    int32_t hi = fam == FAM_VANGOGH || fam == FAM_MENDOCINO ? 30000 : fam == FAM_STRIXHALO ? 140000 :
                 fam == FAM_DRAGONRANGE || fam == FAM_FIRERANGE ? 180000 : 65000;
    for (int k = 0; k < L_N; k++) {
        phx_control *c = &ctl[C_STAPM + k];
        int32_t v;
        if (!have_table || get_limit(c, &v)) continue;
        orig[k] = v;
        if (k != L_TCTL && v > hi) hi = (v + 999) / 1000 * 1000;
    }
    for (int k = L_STAPM; k <= L_SLOW; k++) {
        phx_control *c = &ctl[C_STAPM + k];
        c->max = hi;
        if (orig[k] > 0) c->def = (orig[k] + 500) / 1000 * 1000;
        if (c->def > hi) c->def = hi;
    }
    if (orig[L_TCTL] >= 60 && orig[L_TCTL] <= 105) ctl[C_TCTL].def = orig[L_TCTL];
    if (!have_table) {
        ctl[C_PWR].flags |= PHX_F_HIDDEN;
        ctl[C_TEMP].flags |= PHX_F_HIDDEN;
    }
    for (int i = 0; i < C_N; i++)
        if (host->add_control(self, &ctl[i])) return fail(PHX_ERROR, L"could not register its controls");

    if (ra.set_gfxmax && (fam == FAM_RAVEN || fam == FAM_PICASSO || fam == FAM_DALI || fam == FAM_LUCIENNE)) {
        gfx_top = fam == FAM_LUCIENNE ? 2000 : 1600;
        gfx.max_mhz = gfx_top;
        host->add_clock(self, &gfx);
    }
    phx_logf(host, self, "family %d, power table %s", fam, have_table ? "readable" : "not readable");
    if (!have_table) host->set_status(self, L"limits can be set, but not read back");
    *out = &info;
    return PHX_OK;
}
