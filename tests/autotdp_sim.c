/* AutoTDP against a simulated handheld.

   The machine has P-cores (and E-cores on a hybrid CPU), a GPU, clock caps, Windows
   core parking and EPP. The game has a main thread, worker threads and GPU work
   per frame, all wobbling a little like a real scene. Power comes from a battery
   that updates every few seconds and from a package reading, both noisy.
   autotdp.c is compiled in unchanged; only the operating system around it is fake
   and runs on its own clock, so ten minutes of play take a moment.

   Each scenario checks that the frame rate holds, that the power ends close to the
   best the machine could do (found by trying every setting), that the core floor
   holds, that it settles instead of churning, and that every setting is back to
   Windows' own once the game quits and once AutoTDP stops.

   make sim     (runs it under wine off Windows)
   autotdp_sim.exe [-v] [scenario number] */

#include "phawx.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>

static uint64_t T = 1000000, T_end, T0;
uint64_t ph_ms(void) { return T; }

static DWORD sim_wait(HANDLE h, DWORD ms);
static FARPROC sim_gpa(HMODULE m, LPCSTR name);
static HANDLE sim_thread(LPSECURITY_ATTRIBUTES a, SIZE_T s, LPTHREAD_START_ROUTINE f, LPVOID p, DWORD fl, LPDWORD id);
static BOOL sim_close(HANDLE h);

#define WaitForSingleObject sim_wait
#define GetProcAddress sim_gpa
#define CreateThread sim_thread
#define CloseHandle sim_close
#include "../src/autotdp.c"
#undef WaitForSingleObject
#undef GetProcAddress
#undef CreateThread
#undef CloseHandle

static HANDLE const fake_thr = (HANDLE)(intptr_t)0x5157;

/* ---------- the rest of Phawx ON, as little as autotdp.c needs ---------- */

ph_platform g_plat;
HWND g_main;
const GUID GUID_SUB_PROCESSOR_ = { 0x54533251, 0x82be, 0x4824, { 0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00 } };

static int verbose, seed;

void ph_log(const char *fmt, ...)
{
    if (!verbose) return;
    va_list ap;
    va_start(ap, fmt);
    printf("  %7.1f  ", (double)(T - T0) / 1000.0);
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
}

int ph_swprintf(wchar_t *b, int n, const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = _vsnwprintf(b, n, fmt, ap);
    va_end(ap);
    if (n > 0) b[n - 1] = 0;
    return r;
}

void fmt_onoff(const ph_ctl *c, int32_t v, wchar_t *b, int n) { (void)c; lstrcpynW(b, v ? L"On" : L"Off", n); }
void fmt_pct(const ph_ctl *c, int32_t v, wchar_t *b, int n) { (void)c; ph_swprintf(b, n, L"%d%%", v); }
void fmt_mhz(const ph_ctl *c, int32_t v, wchar_t *b, int n) { (void)c; ph_swprintf(b, n, L"%d MHz", v); }
void ph_register_ctls(ph_ctl *c, int n) { for (int i = 0; i < n; i++) c[i].val = c[i].def; }
int ph_ctl_count(void) { return 0; }
ph_ctl *ph_ctl_at(int i) { (void)i; return NULL; }
int ph_ctl_apply(ph_ctl *c, int32_t v) { (void)c; (void)v; return 0; }
int ui_visible(void) { return 0; }
void cfg_set_int(const char *s, const char *k, int v) { (void)s; (void)k; (void)v; }
int cfg_get_str(const char *s, const char *k, char *o, int n) { (void)s; (void)k; (void)o; (void)n; return 0; }
int cfg_set_str(const char *s, const char *k, const char *v) { (void)s; (void)k; (void)v; return 0; }
int fps_start(void) { return 0; }
void fps_stop(void) {}
int fps_status(void) { return 2; }
int display_refresh(void) { return 60; }
void wp_commit(void) {}

static int cap_cpu, cap_gpu;

static int cpu_set_max(ph_clk *d, int mhz) { (void)d; cap_cpu = mhz; return 0; }
static int cpu_reset(ph_clk *d) { cap_cpu = d->max_mhz; return 0; }
static int gpu_set_max(ph_clk *d, int mhz) { (void)d; cap_gpu = mhz; return 0; }
static int gpu_reset(ph_clk *d) { cap_gpu = d->max_mhz; return 0; }

static ph_clk cpu_clk = { .name = L"CPU", .min_mhz = 400, .max_mhz = 4800, .step_mhz = 100, .set_max = cpu_set_max,
                          .reset = cpu_reset };
static ph_clk gpu_clk = { .name = L"GPU", .min_mhz = 300, .max_mhz = 2000, .step_mhz = 50, .set_max = gpu_set_max,
                          .reset = gpu_reset };

ph_clk *ph_cpu_clk(void) { return &cpu_clk; }
ph_clk *ph_gpu_clk(void) { return &gpu_clk; }
int ph_gpu_clk_count(void) { return 1; }
ph_clk *ph_gpu_clk_at(int i) { (void)i; return &gpu_clk; }
void ph_select_gpu_clk(int i) { (void)i; }

/* ---------- Windows power settings ---------- */

typedef struct { GUID g; DWORD ac, dc; } wset;
static wset ws[16];
static int nws, ws_writes;

static wset *ws_find(const GUID *g, int add)
{
    for (int i = 0; i < nws; i++)
        if (!memcmp(&ws[i].g, g, sizeof *g)) return &ws[i];
    if (!add || nws >= 16) return NULL;
    ws[nws].g = *g;
    return &ws[nws++];
}

static int is(const GUID *a, const GUID *b) { return !memcmp(a, b, sizeof *a); }

int wp_read(const GUID *sub, const GUID *set, int dc, DWORD *v)
{
    (void)sub;
    wset *w = ws_find(set, 0);
    if (w) { *v = dc ? w->dc : w->ac; return 0; }
    /* Windows' own: balanced EPP, parking allowed, a few cores kept */
    if (is(set, &G_EPP0) || is(set, &G_EPP1)) *v = 33;
    else if (is(set, &G_CPMAX0) || is(set, &G_CPMAX1)) *v = 100;
    else if (is(set, &G_CPMIN0) || is(set, &G_CPMIN1)) *v = 5;
    else return -1;
    return 0;
}

int wp_write(const GUID *sub, const GUID *set, DWORD ac, DWORD dc)
{
    (void)sub;
    wset *w = ws_find(set, 1);
    if (!w) return -1;
    if (w->ac != ac || w->dc != dc) ws_writes++;
    w->ac = ac;
    w->dc = dc;
    return 0;
}

int wp_set_epp(int cls, int pct)
{
    if (cls != 1) wp_write(NULL, &G_EPP0, (DWORD)pct, (DWORD)pct);
    if (cls != 0) wp_write(NULL, &G_EPP1, (DWORD)pct, (DWORD)pct);
    return 0;
}

int wp_set_cores(int cls, int mn, int mx)
{
    const GUID *gmn = cls == 1 ? &G_CPMIN1 : &G_CPMIN0, *gmx = cls == 1 ? &G_CPMAX1 : &G_CPMAX0;
    if (mx >= 0) wp_write(NULL, gmx, (DWORD)mx, (DWORD)mx);
    if (mn >= 0) {
        if (mx >= 0 && mn > mx) mn = mx;
        wp_write(NULL, gmn, (DWORD)mn, (DWORD)mn);
    }
    return 0;
}

static DWORD ws_get(const GUID *g)
{
    DWORD v = 0;
    wp_read(NULL, g, 0, &v);
    return v;
}

/* ---------- the machine ---------- */

typedef struct {
    const char *name;
    int   hybrid;               /* 4 P-cores (2 threads) + 8 E-cores; else 8 cores (2 threads) */
    float w_main, w_par, w_gpu; /* megacycles per frame */
    int   threads;              /* worker threads */
    int   limit;                /* the game's frame limiter, 0 = none */
    int   target;
    int   bat_upd;              /* ms between battery updates, 0 = on AC */
    int   parts;                /* a package power reading */
    int   round_up;             /* Windows rounds CPMAXCORES up to whole cores */
    int   change_s;             /* at this second the GPU work per frame... */
    float change_gpu;           /* ...changes by this factor */
    int   quit_s;               /* the game quits (0 = never) */
    int   dur_s;
    int   reachable;
    int   crash;                /* end with the crash handler's release instead of a stop */
    int   user_pmax;            /* the user's own "P-core max unparked" %, 0 = Windows' 100 */
    int   pause_s;              /* no frames for 5 s from this second (alt-tab), 0 = never */
} scen;

static const scen *S;
static int NP, NE;              /* cores per type */
static double gscale = 1.0, wob_c, wob_g, fps_avg, fps_fast, low1_avg, frame_acc, e_acc;
static double m_tc, m_tg, m_ft;
static uint32_t frames;
static int bat_val = -1, game_on;
static uint64_t bat_t, rng = 88172645463325252ull;
static uint64_t stat_t0;
static double stat_p, stat_n, stat_short, stat_all;
static int min_p = 99, min_e = 99, min_total = 99;
static int churn, last_np, last_ne, last_epp, max_p;
static int pause_restored = -1, pre_cores = -1, post_cores = -1, pre_trial;

static double urand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (double)(rng >> 11) / 9007199254740992.0;
}

static double nrand(void) { return urand() + urand() + urand() + urand() - 2.0; }   /* sd ~0.58 */

static int allowed(int cls, int n)
{
    if (n <= 0) return 0;
    DWORD pct = ws_get(cls ? &G_CPMAX1 : &G_CPMAX0);
    int k = S->round_up ? (int)((pct * (DWORD)n + 99) / 100) : (int)(pct * (DWORD)n / 100);
    return PH_CLAMP(k, 1, n);
}

static int epp_now(void) { return (int)ws_get(g_plat.hybrid ? &G_EPP1 : &G_EPP0); }

typedef struct { double ft, fps, low1, t_cpu, t_gpu, bm, bw, p_cpu, p_gpu, p_sys, f_run; } pv;

static double vcpu(double f) { return 0.70 + (f > 1400 ? (f - 1400) * 0.00018 : 0); }
static double vgpu(double f) { return 0.65 + (f > 800 ? (f - 800) * 0.0004 : 0); }

/* one core: switching, leakage while it runs, and what it costs unparked but idle */
static double core_w(int p, double fg, double v, double b)
{
    return p ? 0.97 * fg * v * v * b + 0.5 * b + 0.15 * (1 - b) : 0.30 * fg * v * v * b + 0.15 * b + 0.05 * (1 - b);
}

static void eval(int ccap, int gcap, int np, int ne, int epp, double wc, double wg, pv *o)
{
    const double sp = 1.0, se = NE ? 0.6 : 1.0;
    int main_p = np > 0;
    double s_main = main_p ? sp : se;
    /* workers: one thread on each of the fastest cores left */
    int fp = main_p ? np - 1 : 0, fe = main_p ? ne : ne - 1;
    int wp_ = IMIN(fp, S->threads), we = IMIN(fe, S->threads - wp_);
    double capw = wp_ * sp + we * se;
    double wm = S->w_main * wc, wpar = S->w_par * wc, g = S->w_gpu * wg * gscale;
    double ftmin = S->limit ? 1000.0 / S->limit : 0.0, f = ccap;
    for (int pass = 0; pass < 2; pass++) {
        double tm = wm * 1000.0 / (f * s_main), tp = capw > 0 ? wpar * 1000.0 / (f * capw) : 0.0;
        if (capw <= 0) tm += wpar * 1000.0 / (f * s_main);
        o->t_cpu = tm > tp ? tm : tp;
        o->t_gpu = g * 1000.0 / gcap;
        o->ft = fmax(fmax(o->t_cpu, o->t_gpu), ftmin);
        o->bm = tm / o->ft;
        o->bw = tp / o->ft;
        if (!pass) {
            /* with time to spare, a higher EPP lets the CPU run slower for longer */
            double k = 1.0 - epp / 100.0 * (1.0 - o->t_cpu / o->ft);
            f = ccap * (k < 0.5 ? 0.5 : k);
        }
    }
    o->f_run = f;
    o->fps = 1000.0 / o->ft;
    o->low1 = o->fps * (0.93 - 0.3 * fmax(0.0, o->bm - 0.85)) * (1.0 - 0.12 * epp / 100.0 * o->bm);
    double fg = f / 1000.0, v = vcpu(f), pc = 1.2 + 0.25 * fg;
    pc += core_w(main_p, fg, v, o->bm);
    for (int i = 0; i < fp; i++) pc += core_w(1, fg, v, i < wp_ ? o->bw : 0.0);
    for (int i = 0; i < fe; i++) pc += core_w(0, fg, v, i < we ? o->bw : 0.0);
    pc += 0.01 * ((NP - np) + (NE - ne));
    double gb = o->t_gpu / o->ft, vg = vgpu(gcap);
    o->p_gpu = 5.9 * gcap / 1000.0 * vg * vg * gb + 0.3 + 0.9 * gb;
    o->p_cpu = pc;
    o->p_sys = (pc + o->p_gpu) * 1.1 + 3.5;
}

static int cur_np(void) { return NE ? allowed(1, NP) : allowed(0, NP); }
static int cur_ne(void) { return NE ? allowed(0, NE) : 0; }

static pv now_pv;

static void plant(double dt)
{
    T += (uint64_t)dt;
    double tsec = (double)(T - T0) / 1000.0;
    if (S->change_s && tsec >= S->change_s) gscale = S->change_gpu;
    int paused = S->pause_s && tsec >= S->pause_s && tsec < S->pause_s + 5;
    game_on = (!S->quit_s || tsec < S->quit_s) && !paused;
    /* the scene wobbles: about 3% either way, over a couple of seconds */
    double a = dt / 2000.0;
    wob_c += -wob_c * a + nrand() * sqrt(a) * 0.05;
    wob_g += -wob_g * a + nrand() * sqrt(a) * 0.05;
    int np = cur_np(), ne = cur_ne();
    eval(cap_cpu, cap_gpu, np, ne, epp_now(), 1.0 + wob_c, 1.0 + wob_g, &now_pv);
    pv *o = &now_pv;
    double fps = game_on ? o->fps * (1.0 + nrand() * 0.01) : 0.0;
    fps_avg += (fps - fps_avg) * (1.0 - exp(-dt / 500.0));
    fps_fast += (fps - fps_fast) * (1.0 - exp(-dt / 150.0));
    low1_avg += (o->low1 - low1_avg) * (1.0 - exp(-dt / 1500.0));
    m_tc = o->t_cpu;
    m_tg = o->t_gpu;
    m_ft = o->ft;
    frame_acc += fps * dt / 1000.0;
    while (frame_acc >= 1.0) { frames++; frame_acc -= 1.0; }
    double p = game_on ? o->p_sys : 5.0;
    /* the battery reports the average since its last update */
    e_acc += p * dt;
    if (S->bat_upd && T - bat_t >= (uint64_t)S->bat_upd) {
        bat_val = (int)(e_acc / (double)(T - bat_t) * 100.0 + 0.5) * 10;
        e_acc = 0;
        bat_t = T;
    }
    if (tsec > 60 && game_on && (!S->change_s || tsec < S->change_s || tsec > S->change_s + 30)) {
        stat_all += dt;
        if (o->fps < S->target * 0.95) stat_short += dt;
    }
    if (T >= stat_t0 && game_on) { stat_p += o->p_sys * dt; stat_n += dt; }
    int total = np + ne;
    if (S->pause_s) {
        /* just before the pause, near its end (everything back to Windows'), and a
           few seconds after it (where the search was) */
        if (tsec < S->pause_s) { pre_cores = total; pre_trial = tr.kind; }
        if (paused && tsec >= S->pause_s + 4 && pause_restored < 0)
            pause_restored = ws_get(&G_EPP1) == 33 && ws_get(&G_CPMAX0) == 100 && ws_get(&G_CPMAX1) == 100;
        if (tsec >= S->pause_s + 8 && post_cores < 0) post_cores = total;
    }
    if (game_on) {
        max_p = IMAX(max_p, np);
        min_p = IMIN(min_p, np);
        min_e = IMIN(min_e, ne);
        min_total = IMIN(min_total, total);
    }
    int epp = epp_now();
    if (tsec > S->dur_s * 2 / 3 && (np != last_np || ne != last_ne || epp != last_epp)) churn++;
    last_np = np;
    last_ne = ne;
    last_epp = epp;
}

static DWORD sim_wait(HANDLE h, DWORD ms)
{
    if (h == fake_thr) return WAIT_OBJECT_0;
    if (h != stop_ev) return WaitForSingleObject(h, ms);
    for (DWORD t = 0; t < ms; t += 50) plant(50);
    return T >= T_end ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}

static HANDLE sim_thread(LPSECURITY_ATTRIBUTES a, SIZE_T s, LPTHREAD_START_ROUTINE f, LPVOID p, DWORD fl, LPDWORD id)
{
    (void)a; (void)s; (void)f; (void)p; (void)fl; (void)id;
    return fake_thr;
}

static BOOL sim_close(HANDLE h) { return h == fake_thr ? TRUE : CloseHandle(h); }

int fps_sample(ph_fps *o)
{
    if (!game_on) return 0;
    memset(o, 0, sizeof *o);
    o->pid = 1234;
    o->fps = (float)fps_avg;
    o->fps_fast = (float)fps_fast;
    o->low1 = (float)low1_avg;
    o->frametime_ms = (float)m_ft;
    o->cpu_busy_ms = (float)m_tc;
    o->gpu_busy_ms = (float)m_tg;
    o->frames = frames;
    o->source = 1;
    lstrcpynW(o->exe, L"game.exe", 64);
    return 1;
}

int ph_power_read(ph_power *p)
{
    double parts = game_on ? now_pv.p_cpu + now_pv.p_gpu : 1.2;
    p->sys_mw = S->bat_upd ? bat_val : -1;
    p->pkg_mw = S->parts ? (int)(parts * 1000.0 * (1.0 + nrand() * 0.02)) : -1;
    p->gpu_mw = -1;
    p->parts_mw = p->pkg_mw;
    return p->sys_mw >= 0 || p->parts_mw >= 0 ? 0 : -1;
}

/* the CPU's time per logical CPU, as NtQuerySystemInformation(8) gives it */
static LONG NTAPI sim_nqsi(ULONG cls, PVOID buf, ULONG len, PULONG ret)
{
    static sppi acc[MAXLP];
    static uint64_t last;
    if (cls != 8) return -1;
    int n = g_plat.nlogical;
    uint64_t dt = last ? (T - last) * 10000 : 0;
    last = T;
    int np = cur_np(), ne = cur_ne(), main_p = np > 0;
    int fp = main_p ? np - 1 : 0, wp_ = IMIN(fp, S->threads), we = IMIN(main_p ? ne : ne - 1, S->threads - wp_);
    for (int lp = 0; lp < n; lp++) {
        double b = 0.02;
        int core = g_plat.core_of[lp], first = lp == 0 || g_plat.core_of[lp - 1] != core;
        int pcore = core < NP, idx = pcore ? core : core - NP;
        int parked = pcore ? idx >= np : idx >= ne;
        if (parked) b = 0.0;
        else if (game_on && first) {
            if (pcore && main_p && idx == 0) b = now_pv.bm;
            else if (!pcore && !main_p && idx == 0) b = now_pv.bm;
            else if (pcore && idx - 1 < wp_) b = now_pv.bw;
            else if (!pcore && idx - (main_p ? 0 : 1) < we) b = now_pv.bw;
        }
        b = PH_CLAMP(b, 0.0, 1.0);
        uint64_t busy = (uint64_t)((double)dt * b);
        acc[lp].idle.QuadPart += (LONGLONG)(dt - busy);
        acc[lp].kernel.QuadPart += (LONGLONG)(dt - busy);
        acc[lp].user.QuadPart += (LONGLONG)busy;
    }
    ULONG need = (ULONG)(n * sizeof(sppi));
    if (len < need) return -1;
    memcpy(buf, acc, need);
    if (ret) *ret = need;
    return 0;
}

static BOOL WINAPI sim_cpusets(PSYSTEM_CPU_SET_INFORMATION info, ULONG len, PULONG ret, HANDLE proc, ULONG fl)
{
    (void)proc; (void)fl;
    int n = g_plat.nlogical, np = cur_np(), ne = cur_ne();
    ULONG need = (ULONG)(n * sizeof(SYSTEM_CPU_SET_INFORMATION));
    if (ret) *ret = need;
    if (len < need) return FALSE;
    for (int lp = 0; lp < n; lp++) {
        SYSTEM_CPU_SET_INFORMATION *e = &info[lp];
        memset(e, 0, sizeof *e);
        e->Size = sizeof *e;
        e->Type = CpuSetInformation;
        e->CpuSet.Id = 0x100 + lp;
        e->CpuSet.LogicalProcessorIndex = (BYTE)lp;
        int core = g_plat.core_of[lp], pcore = core < NP, idx = pcore ? core : core - NP;
        e->CpuSet.CoreIndex = (BYTE)core;
        e->CpuSet.EfficiencyClass = g_plat.cls[lp];
        if (pcore ? idx >= np : idx >= ne) e->CpuSet.AllFlags = SYSTEM_CPU_SET_INFORMATION_PARKED;
    }
    return TRUE;
}

static FARPROC sim_gpa(HMODULE m, LPCSTR name)
{
    if (!strcmp(name, "NtQuerySystemInformation")) return (FARPROC)(void *)sim_nqsi;
    if (!strcmp(name, "GetSystemCpuSetInformation")) return (FARPROC)(void *)sim_cpusets;
    return GetProcAddress(m, name);
}

/* ---------- the best the machine can do ---------- */

typedef struct { double p; int c, g, np, ne, epp; } best;

static best oracle(void)
{
    best b = { 1e9, 0, 0, 0, 0, 0 };
    int pmin = NE ? 2 : 2, emin = NE ? 1 : 0;
    for (int c = 1500; c <= 4800; c += 100)
        for (int g = 750; g <= 2000; g += 50)
            for (int np = pmin; np <= NP; np++)
                for (int ne = emin; ne <= NE; ne++)
                    for (int e = 0; e <= 60; e += 20) {
                        pv o;
                        eval(c, g, np, ne, e, 1.0, 1.0, &o);
                        if (o.fps < S->target * 0.975 || o.p_sys >= b.p) continue;
                        b = (best){ o.p_sys, c, g, np, ne, e };
                    }
    return b;
}

/* the same scene with every limit off: what the machine draws without AutoTDP */
static double stock(void)
{
    pv o;
    eval(4800, 2000, NP, NE, 33, 1.0, 1.0, &o);
    return o.p_sys;
}

/* ---------- scenarios ---------- */

static const scen scens[] = {
    { "GPU-bound, game limits 60, battery + package, hybrid", 1, 6, 12, 18, 4, 60, 60, 2000, 1, 1, 0, 1, 0, 600, 1 },
    { "same, Windows rounds parking down", 1, 6, 12, 18, 4, 60, 60, 2000, 1, 0, 0, 1, 0, 600, 1 },
    { "CPU-heavy, uncapped, target 60, 8 cores", 0, 26, 70, 20, 6, 0, 60, 2000, 1, 1, 0, 1, 0, 600, 1 },
    { "light, target 30, battery only (5 s updates), hybrid", 1, 8, 10, 10, 3, 0, 30, 5000, 0, 1, 0, 1, 0, 900, 1 },
    { "scene change: GPU work x1.6 at 300 s, 8 cores", 0, 6, 16, 15, 4, 0, 60, 2000, 1, 1, 300, 1.6f, 0, 600, 1 },
    { "target out of reach, 8 cores", 0, 6, 16, 45, 4, 0, 60, 2000, 1, 1, 0, 1, 0, 400, 0 },
    { "no power reading, GPU-bound, hybrid", 1, 6, 12, 18, 4, 60, 60, 0, 0, 1, 0, 1, 0, 500, 1 },
    { "game quits at 300 s, hybrid", 1, 6, 12, 18, 4, 60, 60, 2000, 1, 1, 0, 1, 300, 360, 1 },
    { "main thread heavy, uncapped, target 60, hybrid", 1, 40, 20, 14, 4, 0, 60, 2000, 1, 1, 0, 1, 0, 600, 1 },
    { "many busy threads (parking costs frames), hybrid", 1, 8, 160, 14, 11, 0, 60, 2000, 1, 1, 0, 1, 0, 600, 1 },
    { "crash while running, 8 cores", 0, 6, 12, 18, 4, 60, 60, 2000, 1, 1, 0, 1, 0, 200, 1, 1 },
    { "user limits P-cores to 50%, hybrid", 1, 6, 12, 18, 4, 60, 60, 2000, 1, 1, 0, 1, 0, 500, 1, 0, 50 },
    { "no frames for 5 s at 400 s (alt-tab), hybrid", 1, 6, 12, 18, 4, 60, 60, 2000, 1, 1, 0, 1, 0, 600, 1, 0, 0, 400 },
};

static void machine(int hybrid)
{
    memset(&g_plat, 0, sizeof g_plat);
    g_plat.vendor = hybrid ? VENDOR_INTEL : VENDOR_AMD;
    g_plat.epp = 1;
    NP = hybrid ? 4 : 8;
    NE = hybrid ? 8 : 0;
    int lp = 0;
    for (int c = 0; c < NP; c++)
        for (int t = 0; t < 2; t++) { g_plat.cls[lp] = (uint8_t)(hybrid ? 1 : 0); g_plat.core_of[lp++] = (uint8_t)c; }
    for (int c = 0; c < NE; c++) { g_plat.cls[lp] = 0; g_plat.core_of[lp++] = (uint8_t)(NP + c); }
    g_plat.nlogical = lp;
    g_plat.ncores = NP + NE;
    g_plat.hybrid = hybrid;
    g_plat.max_class = hybrid ? 1 : 0;
    g_plat.n_class1 = hybrid ? NP * 2 : 0;
    g_plat.n_class0 = hybrid ? NE : NP * 2;
}

static int restored(const char *when, int epp)
{
    DWORD pmax = S->user_pmax ? (DWORD)S->user_pmax : 100;
    int ok = ws_get(&G_EPP0) == (DWORD)epp && ws_get(&G_EPP1) == (DWORD)epp && ws_get(&G_CPMAX0) == 100 && ws_get(&G_CPMAX1) == pmax &&
             ws_get(&G_CPMIN0) == 5 && ws_get(&G_CPMIN1) == 5;
    if (!ok)
        printf("    FAIL settings not restored %s: EPP %lu/%lu CPMAX %lu/%lu CPMIN %lu/%lu\n", when, ws_get(&G_EPP0),
               ws_get(&G_EPP1), ws_get(&G_CPMAX0), ws_get(&G_CPMAX1), ws_get(&G_CPMIN0), ws_get(&G_CPMIN1));
    return ok;
}

/* mode: 0 print and check, 1 the clocks-only baseline (no checks), 2 check quietly */
enum { RUN_SHOW, RUN_BASE, RUN_QUIET };

static int run(int idx, int cores, int epp_mode, double *power, int mode)
{
    const scen *s = &scens[idx];
    S = s;
    machine(s->hybrid);
    nws = ws_writes = 0;
    gscale = 1.0;
    wob_c = wob_g = fps_avg = fps_fast = low1_avg = frame_acc = e_acc = 0;
    frames = 0;
    bat_val = -1;
    rng = 88172645463325252ull + (uint64_t)idx * 7919 + (uint64_t)seed * 104729;
    T += 100000;
    T0 = bat_t = T;
    T_end = T0 + (uint64_t)s->dur_s * 1000;
    stat_t0 = T0 + (uint64_t)(s->quit_s ? s->quit_s - 60 : s->dur_s - 60) * 1000;
    stat_p = stat_n = stat_short = stat_all = 0;
    min_p = min_e = min_total = 99;
    max_p = 0;
    pause_restored = pre_cores = post_cores = -1;
    churn = 0;
    cap_cpu = cpu_clk.max_mhz;
    cap_gpu = gpu_clk.max_mhz;
    game_on = 1;

    static int registered;
    if (!registered) { autotdp_register_ctls(); registered = 1; }
    ctls[K_TARGET].val = 2;
    ctls[K_FPS].val = s->target;
    ctls[K_CORES].val = cores;
    ctls[K_EPP].val = epp_mode;
    if (s->user_pmax) wp_write(NULL, &G_CPMAX1, (DWORD)s->user_pmax, (DWORD)s->user_pmax);
    for (int i = 0; i < 20; i++) plant(50);

    if (start_locked()) { printf("    FAIL AutoTDP did not start\n"); return 0; }
    worker(NULL);
    int ok = 1;
    /* "EPP 0 while active" keeps it at 0 without a game too */
    if (s->quit_s) ok &= restored("once the game quit", epp_mode == EPP_ZERO ? 0 : 33);
    wchar_t st1[128], st2[128];
    fmt_status(NULL, 0, st1, 128);
    fmt_power(NULL, 0, st2, 128);
    int end_c = cap_cpu, end_g = cap_gpu, end_np = cur_np(), end_ne = cur_ne(), end_epp = epp_now();
    if (s->crash) {
        autotdp_crash_release(GetCurrentThreadId());
        ok &= restored("after a crash", 33);
        if (autotdp_running()) { printf("    FAIL still running after a crash\n"); ok = 0; }
    }
    autotdp_stop();
    ok &= restored("after stop", 33);
    if (cap_cpu != cpu_clk.max_mhz || cap_gpu != gpu_clk.max_mhz) {
        printf("    FAIL clock caps left on after stop\n");
        ok = 0;
    }
    double p = stat_n > 0 ? stat_p / stat_n : 0;
    *power = p;
    if (mode == RUN_BASE) return ok;

    best b = oracle();
    double shortf = stat_all > 0 ? stat_short / stat_all : 0;
    int floor_ok = s->hybrid ? min_p >= 2 && min_e >= 1 : min_total >= 2;
    int f_frames = s->reachable && shortf > 0.03, f_power = s->reachable && !s->quit_s && p > b.p * 1.12;
    int f_churn = s->dur_s >= 400 && churn > s->dur_s / 40;   /* a short run is still searching */
    int f_user = s->user_pmax && max_p > (s->user_pmax * NP + 99) / 100;
    /* a trial running when the frames stopped is abandoned, so only then may the cores differ */
    int f_pause = s->pause_s && (pause_restored != 1 || (post_cores != pre_cores && !pre_trial));
    ok &= !f_frames && !f_power && floor_ok && !f_churn && !f_user && !f_pause;
    if (mode == RUN_QUIET) {
        if (f_frames || f_power || !floor_ok || f_churn || f_user || f_pause)
            printf("    seed %d: frames short %.1f%%, power +%.1f%%, fewest cores %d, changes late %d%s%s\n", seed,
                   shortf * 100, (p / b.p - 1) * 100, min_total, churn, f_user ? ", too many P-cores" : "",
                   f_pause ? ", pause not handled" : "");
        return ok;
    }
    printf("  end: CPU %d GPU %d, %d P + %d E cores, EPP %d   [%ls | %ls]\n", end_c, end_g, end_np, end_ne, end_epp, st1, st2);
    if (b.c) {
        printf("  best: CPU %d GPU %d, %d P + %d E cores, EPP %d\n", b.c, b.g, b.np, b.ne, b.epp);
        printf("  power %.2f W, best %.2f W (+%.1f%%), ", p, b.p, (p / b.p - 1) * 100);
    } else {
        printf("  best: the target is out of reach\n  power %.2f W, ", p);
    }
    printf("without AutoTDP %.2f W; frames short %.1f%% of the time; fewest cores %d (%d P, %d E); changes late %d\n",
           stock(), shortf * 100, min_total, min_p, min_e, churn);
    if (f_frames) printf("    FAIL frame rate held only %.1f%% of the time\n", 100 - shortf * 100);
    if (f_power) printf("    FAIL power %.1f%% above the best\n", (p / b.p - 1) * 100);
    if (!floor_ok) printf("    FAIL went below the core floor\n");
    if (f_churn) printf("    FAIL still changing settings late: %d\n", churn);
    if (s->user_pmax) printf("  most P-cores used %d (the user allows %d)\n", max_p, (s->user_pmax * NP + 99) / 100);
    if (f_user) printf("    FAIL used more P-cores than the user allows\n");
    if (s->pause_s)
        printf("  pause: settings back to Windows' %s; cores %d before, %d after%s\n", pause_restored == 1 ? "yes" : "no",
               pre_cores, post_cores, pre_trial ? " (a trial was running)" : "");
    if (f_pause) printf("    FAIL the pause was not handled\n");
    return ok;
}

int main(int argc, char **argv)
{
    int only = -1, seeds = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) seeds = atoi(argv[++i]);
        else only = atoi(argv[i]);
    }
    if (seeds > 1) {
        /* the same scenarios with other scenes: only the failures and the spread */
        int fails = 0;
        for (int i = 0; i < (int)PH_ARRAY(scens); i++) {
            if (only >= 0 && i != only) continue;
            double lo = 1e9, hi = 0, sum = 0;
            int bad = 0;
            for (seed = 1; seed <= seeds; seed++) {
                double p;
                if (!run(i, 1, EPP_TUNE, &p, RUN_QUIET)) { bad++; printf("  [%d] seed %d failed\n", i, seed); }
                best b = oracle();
                double r = b.c ? p / b.p : 1.0;
                lo = r < lo ? r : lo;
                hi = r > hi ? r : hi;
                sum += r;
            }
            printf("[%d] %-55s %d/%d ok, power vs best +%.1f%% .. +%.1f%% (mean +%.1f%%)\n", i, scens[i].name,
                   seeds - bad, seeds, (lo - 1) * 100, (hi - 1) * 100, (sum / seeds - 1) * 100);
            fails += bad;
        }
        return fails ? 1 : 0;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    int fails = 0;
    for (int i = 0; i < (int)PH_ARRAY(scens); i++) {
        if (only >= 0 && i != only) continue;
        printf("[%d] %s\n", i, scens[i].name);
        double p, p_old;
        int ok = run(i, 1, EPP_TUNE, &p, RUN_SHOW);
        /* the same with only the clocks, as AutoTDP was before */
        int v = verbose;
        verbose = 0;
        run(i, 0, EPP_ZERO, &p_old, RUN_BASE);
        verbose = v;
        printf("  clocks only: %.2f W, so %.1f%% saved by cores and EPP\n", p_old, (1 - p / p_old) * 100);
        printf("  %s\n", ok ? "ok" : "FAILED");
        fails += !ok;
    }
    printf(fails ? "%d scenario(s) failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
