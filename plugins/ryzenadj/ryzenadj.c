/* RyzenAdj adapter for Phawx ON.
 *
 * Uses libryzenadj.dll from RyzenAdj's Windows release (github.com/FlyGoat/RyzenAdj)
 * for AMD APU power limits on machines where Phawx ON's own SMU control (through
 * PawnIO) is not available. Put libryzenadj.dll and WinRing0x64.dll/.sys from that
 * release in plugins\ryzenadj\ next to this DLL, and inpoutx64.dll too for "Read power
 * table". libryzenadj talks to the hardware through those drivers, not PawnIO.
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
typedef void  (__stdcall *fn_void)(void);

/* enum ryzen_family values from RyzenAdj's public API. 0..10 are the same in every
   release; later ones were renumbered, so power ranges come from CPUID instead. */
enum { FAM_RAVEN = 0, FAM_PICASSO = 1, FAM_DALI = 4, FAM_LUCIENNE = 5 };

/* the CPUs libryzenadj 0.19 knows (lib/cpuid.c), with the top of the range the
   built-in AMD control offers them, in W */
static const struct { uint8_t fam, model, w; } cpus[] = {
    { 0x17, 0x11, 65 }, { 0x17, 0x18, 65 }, { 0x17, 0x20, 65 }, { 0x17, 0x60, 65 }, { 0x17, 0x68, 65 },
    { 0x17, 0x90, 30 }, { 0x17, 0x91, 30 }, { 0x17, 0xA0, 30 },
    { 0x19, 0x40, 65 }, { 0x19, 0x44, 65 }, { 0x19, 0x50, 65 }, { 0x19, 0x61, 180 },
    { 0x19, 0x74, 65 }, { 0x19, 0x75, 65 }, { 0x19, 0x78, 65 },
    { 0x1A, 0x20, 65 }, { 0x1A, 0x24, 65 }, { 0x1A, 0x44, 180 }, { 0x1A, 0x60, 65 }, { 0x1A, 0x70, 140 },
};

static struct {
    HMODULE lib;
    ryzen_access ry;
    fn_init init;
    fn_cleanup cleanup;
    fn_family family;
    fn_table init_table, refresh_table;
    fn_set set_stapm, set_fast, set_slow, set_tctl, set_gfxmax;
    fn_get stapm_lim, fast_lim, fast_val, slow_lim, tctl_lim, tctl_val;
    fn_void ols_deinit;
} ra;

static const phx_host *H;
static phx_plugin *SELF;
static HANDLE pci_mx;
static HMODULE inpout;
static int fam = -1, have_table, gfx_top, gfx_touched;
static int table_start, table_saved;   /* "Read power table" when Phawx ON started, and now */
static DWORD last_refresh;

enum { L_STAPM, L_FAST, L_SLOW, L_TCTL, L_N };
static int32_t orig[L_N] = { -1, -1, -1, -1 };   /* mW, or degrees C for Tctl */
static unsigned wrote;

static int valid(float f) { return f == f && f > 0.0f && f < 1000.0f; }
static int32_t watts_mw(float w) { return (int32_t)(w * 1000.0f + 0.5f); }

/* ---------- the worker ----------
   libryzenadj waits for the SMU's answer without a timeout, so every call into it
   runs on this one thread while the caller waits a bounded time. A call that does
   not come back leaves busy set: from then on nothing calls or waits for the
   library again, and the worker, the library and this DLL are left alone. */

#define CALL_MS 1000
#define OPEN_MS 5000

static HANDLE wk, wk_go, wk_done;
static volatile LONG busy;
static int stuck;
static int (*job)(void);
static int job_rc;
static fn_set job_set;
static fn_get job_get;
static uint32_t job_v;
static float job_f;

static DWORD WINAPI worker(void *arg)
{
    (void)arg;
    for (;;) {
        WaitForSingleObject(wk_go, INFINITE);
        if (!job) return 0;
        job_rc = job();
        SetEvent(wk_done);
    }
}

static int run(int (*fn)(void), DWORD ms)
{
    if (!wk || InterlockedCompareExchange(&busy, 1, 0)) return -1;
    job = fn;
    SetEvent(wk_go);
    if (WaitForSingleObject(wk_done, ms) != WAIT_OBJECT_0) {
        stuck = 1;
        H->set_status(SELF, L"RyzenAdj stopped responding");
        phx_logf(H, SELF, "libryzenadj did not return within %lu ms, no longer calling it", ms);
        return -1;
    }
    InterlockedExchange(&busy, 0);
    return job_rc;
}

/* The SMU mailbox has no owner. Tools that share it take this mutex around each
   transaction, and so does Phawx ON's built-in SMU code. A mutex belongs to the
   thread that took it, so only the worker takes it. */
static int lock(void)
{
    if (!pci_mx) return 0;
    DWORD w = WaitForSingleObject(pci_mx, 500);
    return w == WAIT_OBJECT_0 || w == WAIT_ABANDONED ? 0 : -1;
}
static void unlock(void) { if (pci_mx) ReleaseMutex(pci_mx); }

/* jobs, run on the worker */

static int j_open(void)
{
    if (lock()) return -1;
    ra.ry = ra.init();
    if (ra.ry) {
        fam = ra.family(ra.ry);
        have_table = inpout && ra.init_table && ra.refresh_table && ra.init_table(ra.ry) == 0;
        if (have_table) last_refresh = GetTickCount();   /* init_table has just transferred it */
    }
    unlock();
    return 0;
}

static int j_set(void)
{
    if (!ra.ry || lock()) return -1;
    int r = job_set(ra.ry, job_v);
    unlock();
    return r;
}

/* the power table is refreshed at most twice a second; the SMU rejects faster
   transfers. The getters only read the copy refresh_table made. */
static int j_get(void)
{
    DWORD now = GetTickCount();
    if (!ra.ry) return -1;
    if (now - last_refresh >= 500) {
        last_refresh = now;
        if (lock() == 0) {
            ra.refresh_table(ra.ry);
            unlock();
        }
    }
    job_f = job_get(ra.ry);
    return 0;
}

/* cleanup_ryzenadj also unmaps the power table, and since libryzenadj 0.17 it
   crashes when the table was never mapped. Then only WinRing0 is closed, as cleanup
   would have done first (that also removes its service); the rest is left to the
   process exit. 1 = cleaned up. */
static int j_close(void)
{
    ryzen_access ry = ra.ry;
    ra.ry = NULL;
    if (have_table) { ra.cleanup(ry); return 1; }
    if (ra.ols_deinit) ra.ols_deinit();
    return 0;
}

static int set_hw(fn_set f, uint32_t v)
{
    if (!f) return -1;
    job_set = f;
    job_v = v;
    return run(j_set, CALL_MS);
}

static int get_hw(fn_get f, float *out)
{
    if (!have_table || !f) return -1;
    job_get = f;
    if (run(j_get, CALL_MS) || !valid(job_f)) return -1;
    *out = job_f;
    return 0;
}

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

/* the readouts need the power table */
static void PHX_CALL fmt_pwr(const phx_control *c, int32_t v, wchar_t *b, int n)
{
    if (table_start) fmt_w(c, v, b, n);
    else lstrcpynW(b, L"Needs Read power table", n);
}

static void PHX_CALL fmt_temp(const phx_control *c, int32_t v, wchar_t *b, int n)
{
    if (table_start) fmt_c(c, v, b, n);
    else lstrcpynW(b, L"Needs Read power table", n);
}

static int PHX_CALL get_power(phx_control *c, int32_t *out)
{
    (void)c;
    float f;
    if (get_hw(ra.fast_val, &f)) return -1;
    *out = watts_mw(f);
    return 0;
}

static int PHX_CALL get_temp(phx_control *c, int32_t *out)
{
    (void)c;
    float f;
    if (get_hw(ra.tctl_val, &f)) return -1;
    *out = (int32_t)(f + 0.5f);
    return 0;
}

static int PHX_CALL get_limit(phx_control *c, int32_t *out)
{
    static fn_get *const g[L_N] = { &ra.stapm_lim, &ra.fast_lim, &ra.slow_lim, &ra.tctl_lim };
    int k = (int)(intptr_t)c->user;
    float f;
    if (get_hw(*g[k], &f)) return -1;
    *out = k == L_TCTL ? (int32_t)(f + 0.5f) : watts_mw(f);
    return 0;
}

enum { C_HDR, C_PWR, C_TEMP, C_STAPM, C_FAST, C_SLOW, C_TCTL, C_TABLE, C_N };
static phx_control ctl[C_N];

static int apply(int k, int32_t v)
{
    static fn_set *const s[L_N] = { &ra.set_stapm, &ra.set_fast, &ra.set_slow, &ra.set_tctl };
    if (set_hw(*s[k], (uint32_t)v)) return -1;
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
    /* a fast limit that cannot be read may be below the new one (Van Gogh ships 15/15/15 W) */
    if (fast < 0 && (get_limit(&ctl[C_FAST], &cur) || cur < mw)) fast = mw;
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

/* kept in [plugin:ryzenadj] and read at start: inpoutx64.dll is loaded, or not,
   before anything else runs */
static int PHX_CALL get_table(phx_control *c, int32_t *out) { (void)c; *out = table_saved; return 0; }

static int PHX_CALL set_table(phx_control *c, int32_t v)
{
    (void)c;
    v = v != 0;
    if (v == table_saved) return 0;
    table_saved = v;
    H->cfg_set_int(SELF, "table", v);
    if (v != table_start) H->toast(SELF, L"Restart Phawx ON to apply");
    return 0;
}

#define HW (PHX_F_OPTIONAL | PHX_F_REAPPLY)

static phx_control ctl[C_N] = {
    [C_HDR] = { .size = sizeof(phx_control), .type = PHX_HEADER, .label = L"RyzenAdj", .page = PHX_PAGE_CPU, .order = 0 },
    [C_PWR] = { .size = sizeof(phx_control), .type = PHX_INFO, .label = L"APU power", .page = PHX_PAGE_CPU,
                .order = 1, .def = -1, .get = get_power, .fmt = fmt_pwr },
    [C_TEMP] = { .size = sizeof(phx_control), .type = PHX_INFO, .label = L"Temperature", .page = PHX_PAGE_CPU,
                 .order = 2, .def = -1, .get = get_temp, .fmt = fmt_temp },
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
    /* libryzenadj reads the table through InpOut, whose DLL installs its driver as an
       auto-start service that stays after Phawx ON exits */
    [C_TABLE] = { .size = sizeof(phx_control), .type = PHX_TOGGLE, .key = "table", .label = L"Read power table",
                  .page = PHX_PAGE_CPU, .order = 7, .flags = PHX_F_DANGER | PHX_F_NOSAVE, .def = 0,
                  .get = get_table, .set = set_table,
                  .desc = L"Reads the power table through the InpOut driver. Its DLL installs the driver as a Windows "
                          L"service (inpoutx64) that stays after Phawx ON exits. To remove it: sc delete inpoutx64, "
                          L"restart, then delete C:\\Windows\\System32\\drivers\\inpoutx64.sys. Takes effect after a restart." },
};

/* ---------- iGPU clock for AutoTDP (Raven, Picasso, Dali, Lucienne only) ---------- */

static int PHX_CALL gfx_max(phx_clock *d, int mhz)
{
    (void)d;
    if (mhz < 200) mhz = 200;
    if (mhz > gfx_top) mhz = gfx_top;
    if (set_hw(ra.set_gfxmax, (uint32_t)mhz)) return -1;
    gfx_touched = mhz != gfx_top;
    return 0;
}

static int PHX_CALL gfx_reset(phx_clock *d) { return gfx_max(d, gfx_top); }

static phx_clock gfx = {
    .size = sizeof(phx_clock), .kind = PHX_CLOCK_GPU, .name = L"iGPU (RyzenAdj)", .min_mhz = 200, .max_mhz = 1600,
    .step_mhz = 50, .prio = 20, .set_max = gfx_max, .reset = gfx_reset,
};

/* ---------- lifetime ---------- */

/* undoes init when it fails; the host then unloads this DLL */
static void unload(void)
{
    HMODULE self;
    int keep = ra.ry && run(j_close, CALL_MS) != 1;   /* libryzenadj stays loaded unless it cleaned up */
    if (wk && !busy) {
        job = NULL;
        SetEvent(wk_go);
        if (WaitForSingleObject(wk, CALL_MS) == WAIT_OBJECT_0) {
            CloseHandle(wk);
            wk = NULL;
        }
    }
    if (wk) {
        /* still inside libryzenadj, so this DLL must stay */
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           (LPCWSTR)(void *)&ra, &self);
        return;
    }
    if (wk_go) { CloseHandle(wk_go); wk_go = NULL; }
    if (wk_done) { CloseHandle(wk_done); wk_done = NULL; }
    if (!keep) {
        if (ra.lib) FreeLibrary(ra.lib);
        if (inpout) FreeLibrary(inpout);
    }
    ra.lib = NULL;
    inpout = NULL;
    if (pci_mx) { CloseHandle(pci_mx); pci_mx = NULL; }
}

/* Phawx ON is exiting: the worker and the DLLs stay until the process ends */
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
    if (ra.ry) run(j_close, CALL_MS);
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
    int32_t dummy, hi = 0;
    H = host;
    SELF = self;
    if (host->version < PHX_API_VERSION) return PHX_E_VERSION;
    const phx_platform *pf = host->platform;
    if (pf->vendor != 2) { host->set_status(self, L"needs an AMD Ryzen APU"); return PHX_UNSUPPORTED; }
    /* Phawx ON's own SMU control is the safer path; two tools on one limit would fight */
    if (host->ctl_get(self, "cpu.tdp", &dummy, NULL) == 0) {
        host->set_status(self, L"not needed, Phawx ON controls this APU itself");
        return PHX_UNSUPPORTED;
    }
    for (int i = 0; i < (int)(sizeof cpus / sizeof cpus[0]); i++)
        if (cpus[i].fam == pf->family && cpus[i].model == pf->model) hi = cpus[i].w * 1000;
    if (!hi) { host->set_status(self, L"RyzenAdj does not support this CPU"); return PHX_UNSUPPORTED; }

    table_start = table_saved = host->cfg_get_int(self, "table", 0) != 0;
    lstrcpynW(dir, host->plugin_dir(self), MAX_PATH);
    /* libryzenadj loads inpoutx64.DLL by bare name, for the power table only. Loading
       it first from here by full path makes that find this copy instead of searching
       the exe folder and PATH; without it, init_table is never called. */
    if (table_start) {
        _snwprintf(path, MAX_PATH, L"%s\\inpoutx64.dll", dir);
        path[MAX_PATH - 1] = 0;
        inpout = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
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
    /* libryzenadj links WinRing0x64.dll */
    HMODULE ols = GetModuleHandleW(L"WinRing0x64.dll");
    if (ols) ra.ols_deinit = (fn_void)GetProcAddress(ols, "DeinitializeOls");

    pci_mx = CreateMutexW(NULL, FALSE, L"Global\\Access_PCI");
    if (!pci_mx) pci_mx = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, L"Global\\Access_PCI");
    wk_go = CreateEventW(NULL, FALSE, FALSE, NULL);
    wk_done = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (wk_go && wk_done) wk = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (!wk) return fail(PHX_ERROR, L"could not start its worker thread");
    if (run(j_open, OPEN_MS))
        return fail(PHX_ERROR, stuck ? L"RyzenAdj stopped responding" : L"another tool is holding the SMU");
    if (!ra.ry) return fail(PHX_ERROR, L"RyzenAdj could not open its driver (Windows may block WinRing0)");
    if (fam < 0) return fail(PHX_UNSUPPORTED, L"RyzenAdj does not support this CPU");

    /* ranges like the built-in control; start from the firmware's own values */
    for (int k = 0; k < L_N; k++) {
        phx_control *c = &ctl[C_STAPM + k];
        int32_t v;
        if (get_limit(c, &v)) continue;
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
    if (table_start && !have_table) {
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
    phx_logf(host, self, "cpu family %02Xh model %02Xh, libryzenadj family %d, up to %d W, power table %s%s",
             pf->family, pf->model, fam, hi / 1000, have_table ? "readable" : table_start ? "not readable" : "off",
             table_start && !inpout ? " (inpoutx64.dll could not be loaded)" : "");
    /* short enough for the Plugins page */
    if (have_table) host->set_status(self, L"InpOut driver stays installed after exit");
    else if (table_start && !inpout) host->set_status(self, L"no inpoutx64.dll, limits are not restored");
    else if (table_start) host->set_status(self, L"no power table, limits are not restored");
    else host->set_status(self, L"limits not read back or restored on exit");
    *out = &info;
    return PHX_OK;
}
