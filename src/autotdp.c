#include "phawx.h"
#include <string.h>

ph_auto_cfg g_auto = {
    .tolerance_pct = 3, .raise_aggr = 3, .lower_aggr = 2, .settle_ms = 2000, .epp_zero = 1
};

static const GUID G_EPP0 = { 0x36687f9e, 0xe3a5, 0x4dbf, { 0xb1, 0xdc, 0x15, 0xeb, 0x38, 0x1c, 0x68, 0x63 } };
static const GUID G_EPP1 = { 0x36687f9e, 0xe3a5, 0x4dbf, { 0xb1, 0xdc, 0x15, 0xeb, 0x38, 0x1c, 0x68, 0x64 } };
static const GUID G_CPMIN0 = { 0x0cc5b647, 0xc1df, 0x4637, { 0x89, 0x1a, 0xde, 0xc3, 0x5c, 0x31, 0x85, 0x83 } };
static const GUID G_CPMIN1 = { 0x0cc5b647, 0xc1df, 0x4637, { 0x89, 0x1a, 0xde, 0xc3, 0x5c, 0x31, 0x85, 0x84 } };
static const GUID G_CPMAX0 = { 0xea062031, 0x0e34, 0x4ff1, { 0x9b, 0x6d, 0xeb, 0x10, 0x59, 0x33, 0x40, 0x28 } };
static const GUID G_CPMAX1 = { 0xea062031, 0x0e34, 0x4ff1, { 0x9b, 0x6d, 0xeb, 0x10, 0x59, 0x33, 0x40, 0x29 } };

typedef struct { const GUID *g; DWORD ac, dc; int ok; } saved_pw;

static saved_pw sv_epp[2] = { { &G_EPP0 }, { &G_EPP1 } };
static saved_pw sv_park[4] = { { &G_CPMIN0 }, { &G_CPMAX0 }, { &G_CPMIN1 }, { &G_CPMAX1 } };

static void pw_save(saved_pw *s)
{
    s->ok = wp_read(&GUID_SUB_PROCESSOR_, s->g, 0, &s->ac) == 0 &&
            wp_read(&GUID_SUB_PROCESSOR_, s->g, 1, &s->dc) == 0;
}

static void pw_restore(saved_pw *s)
{
    if (s->ok) wp_write(&GUID_SUB_PROCESSOR_, s->g, s->ac, s->dc);
    s->ok = 0;
}

/* ---------- controller state ---------- */

typedef struct {
    ph_clk  *d;
    int      cur, floor, ceil, step;
    int      applied;
    int      touched;
    int      bad, bad_n;
    uint64_t bad_t;
} dom;

static HANDLE thr, stop_ev;
static volatile LONG running;
static CRITICAL_SECTION cs, op;
static INIT_ONCE cs_once = INIT_ONCE_STATIC_INIT;
static ph_auto_state st;
static dom dc, dg;
static int epp_set, park_level;

#define BAD_HOLD_MS  20000
#define CAP_PROBE_MS 30000
#define CAP_PROBE_MAX 300000

static BOOL CALLBACK cs_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&cs);
    InitializeCriticalSection(&op);
    return TRUE;
}

static void LK(void) { InitOnceExecuteOnce(&cs_once, cs_init, NULL, NULL); EnterCriticalSection(&cs); }
static void UL(void) { LeaveCriticalSection(&cs); }

/* ---------- controls ---------- */

enum {
    K_ON, K_TARGET, K_FPS, K_STATUS, K_CPUMAX, K_GPUMAX, K_GPUDOM,
    K_HDR, K_TOL, K_RAISE, K_LOWER, K_SETTLE, K_CPUFLOOR, K_CPUCEIL, K_GPUFLOOR, K_GPUCEIL, K_CORES, K_EPP0,
    K_COUNT
};

static ph_ctl ctls[K_COUNT];

#define V(k) (ctls[k].val)

static void sync_cfg(void)
{
    int mode = V(K_TARGET);
    g_auto.target_fps = mode == 2 ? V(K_FPS) : 0;
    g_auto.half_refresh = mode == 1;
    g_auto.tolerance_pct = V(K_TOL);
    g_auto.raise_aggr = PH_CLAMP(V(K_RAISE), 1, 5);
    g_auto.lower_aggr = PH_CLAMP(V(K_LOWER), 1, 5);
    g_auto.settle_ms = V(K_SETTLE);
    g_auto.cpu_floor_mhz = V(K_CPUFLOOR);
    g_auto.cpu_ceil_mhz = V(K_CPUCEIL);
    g_auto.gpu_floor_mhz = V(K_GPUFLOOR);
    g_auto.gpu_ceil_mhz = V(K_GPUCEIL);
    g_auto.manage_cores = V(K_CORES) && g_plat.hybrid;
    g_auto.epp_zero = V(K_EPP0);
    uint16_t f = ctls[K_FPS].flags;
    uint16_t nf = mode == 2 ? (uint16_t)(f & ~CF_HIDDEN) : (uint16_t)(f | CF_HIDDEN);
    if (nf != f) ctls[K_FPS].flags = nf;
}

/* ---------- domains ---------- */

static int dom_usable(ph_clk *d)
{
    return d && d->set_max && d->max_mhz > d->min_mhz && d->max_mhz > 0;
}

static void dom_limits(dom *x, int floor_cfg, int ceil_cfg)
{
    ph_clk *d = x->d;
    int lo = d->min_mhz > 0 ? d->min_mhz : 100, hi = d->max_mhz;
    x->step = d->step_mhz > 0 ? d->step_mhz : 50;
    x->ceil = ceil_cfg > 0 ? PH_CLAMP(ceil_cfg, lo, hi) : hi;
    int af = lo + (hi - lo) / 4;
    x->floor = floor_cfg > 0 ? floor_cfg : af;
    x->floor = PH_CLAMP(x->floor, lo, x->ceil);
    if (x->cur <= 0) x->cur = x->ceil;
    x->cur = PH_CLAMP(x->cur, x->floor, x->ceil);
}

static int dom_q(dom *x, int v)
{
    int s = x->step > 0 ? x->step : 50;
    v = (v + s / 2) / s * s;
    return PH_CLAMP(v, x->floor, x->ceil);
}

static void dom_apply(dom *x, int force)
{
    if (!x->d) return;
    if (!force && x->cur == x->applied) return;
    if (x->d->set_max(x->d, x->cur) == 0) {
        x->applied = x->cur;
        x->touched = 1;
    } else {
        x->applied = -1;
    }
}

static void dom_release(dom *x)
{
    if (x->d && x->touched) {
        if (!x->d->reset || x->d->reset(x->d)) x->d->set_max(x->d, x->d->max_mhz);
    }
    x->touched = 0;
    x->applied = -1;
}

static int dom_raise(dom *x, float factor, int min_steps)
{
    if (!x->d || x->cur >= x->ceil) return 0;
    int n = (int)((float)x->cur * factor);
    if (n < x->cur + x->step * min_steps) n = x->cur + x->step * min_steps;
    n = dom_q(x, n);
    if (n <= x->cur) n = x->ceil;
    x->cur = n;
    return 1;
}

static int bad_active(dom *x, uint64_t now)
{
    int sh = x->bad_n > 1 ? PH_CLAMP(x->bad_n - 1, 0, 3) : 0;
    return x->bad > 0 && now - x->bad_t < ((uint64_t)BAD_HOLD_MS << sh);
}

static void dom_mark_bad(dom *x, uint64_t now)
{
    int dd = x->cur - x->bad;
    x->bad_n = x->bad > 0 && dd <= x->step && dd >= -x->step ? x->bad_n + 1 : 1;
    x->bad = x->cur;
    x->bad_t = now;
}

static int dom_can_lower(dom *x, uint64_t now)
{
    if (!x->d || x->cur <= x->floor) return 0;
    if (bad_active(x, now) && x->cur - x->step <= x->bad) return 0;
    return 1;
}

/* ---------- CPU utilisation (max per logical CPU) ---------- */

typedef LONG (NTAPI *pfn_nqsi)(ULONG, PVOID, ULONG, PULONG);
typedef struct { LARGE_INTEGER idle, kernel, user, dpc, intr; ULONG cnt; } sppi;

#define MAXLP 64
static pfn_nqsi nqsi;
static sppi cpu_buf[MAXLP];
static uint64_t prev_busy[MAXLP], prev_tot[MAXLP];
static int cpu_primed;

static int cpu_util(int *maxp, int *avgp)
{
    if (!nqsi) {
        HMODULE m = GetModuleHandleW(L"ntdll.dll");
        if (m) nqsi = (pfn_nqsi)(void *)GetProcAddress(m, "NtQuerySystemInformation");
        if (!nqsi) return -1;
    }
    ULONG len = 0;
    if (nqsi(8, cpu_buf, sizeof cpu_buf, &len) < 0) return -1;
    int n = (int)(len / sizeof(sppi)), mx = 0, sum = 0, cnt = 0;
    if (n > MAXLP) n = MAXLP;
    for (int i = 0; i < n; i++) {
        uint64_t tot = (uint64_t)cpu_buf[i].kernel.QuadPart + (uint64_t)cpu_buf[i].user.QuadPart;
        uint64_t busy = tot - (uint64_t)cpu_buf[i].idle.QuadPart;
        uint64_t dt = tot - prev_tot[i], db = busy - prev_busy[i];
        prev_tot[i] = tot;
        prev_busy[i] = busy;
        if (!cpu_primed || !dt || tot < busy) continue;
        int p = db >= dt ? 100 : (int)(db * 100 / dt);
        if (p > mx) mx = p;
        sum += p;
        cnt++;
    }
    int ok = cpu_primed && cnt;
    cpu_primed = 1;
    if (!ok) return -1;
    *maxp = mx;
    *avgp = sum / cnt;
    return 0;
}

/* ---------- core parking ---------- */

static void park_apply(int level)
{
    if (level == park_level) return;
    if (level > 0 && park_level == 0)
        for (int i = 0; i < 4; i++) pw_save(&sv_park[i]);
    int np = g_plat.n_class1, ne = g_plat.n_class0;
    if (level == 0) {
        for (int i = 0; i < 4; i++) pw_restore(&sv_park[i]);
    } else {
        if (np >= 2) wp_set_cores(1, 0, 50);
        if (level >= 2 && ne >= 2) wp_set_cores(0, 0, 50);
    }
    park_level = level;
    LK();
    st.parked_p = level >= 1 && np >= 2 ? 50 : 0;
    st.parked_e = level >= 2 && ne >= 2 ? 50 : 0;
    UL();
}

/* ---------- worker ---------- */

static DWORD WINAPI worker(LPVOID p)
{
    (void)p;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    ph_fps f;
    uint64_t now = ph_ms(), last_tick = now, last_frame_t = 0, last_raise = 0, last_lower = 0;
    uint64_t stable_since = now, last_util = 0, last_refresh_q = 0, last_fps_start = now, last_reapply = now;
    uint64_t last_park = 0, lower_t = 0, below_since = 0, cap_t = 0;
    uint32_t last_frames = 0;
    float ff = 0.0f, gs = -1.0f, cs_ = -1.0f, cap_target = 0.0f;
    int refresh = 60, cmax = -1, cavg = -1, gutil = -1, below_n = 0, park_votes = 0, idle = 1, last_target = 0;
    int lower_prev = 0, bn = 0, alt = 0, probe_ms = CAP_PROBE_MS;
    float last_cap = 0.0f;
    DWORD last_pid = 0;
    dom *lower_dom = NULL;
    DWORD wait = 50;

    while (WaitForSingleObject(stop_ev, wait) == WAIT_TIMEOUT) {
        now = ph_ms();
        int force = now - last_tick > 1500 || now - last_reapply > 15000;
        if (force) last_reapply = now;
        last_tick = now;
        sync_cfg();

        ph_clk *g = ph_gpu_clk();
        if (!dom_usable(g)) g = NULL;
        if (g != dg.d) {
            dom_release(&dg);
            dg.d = g;
            dg.cur = 0;
            dg.bad = 0;
            force = 1;
        }
        if (dc.d) dom_limits(&dc, g_auto.cpu_floor_mhz, g_auto.cpu_ceil_mhz);
        if (dg.d) dom_limits(&dg, g_auto.gpu_floor_mhz, g_auto.gpu_ceil_mhz);

        if (!last_refresh_q || now - last_refresh_q > 2000) {
            int r = display_refresh();
            refresh = r >= 20 && r <= 1000 ? r : 60;
            last_refresh_q = now;
        }
        int target = g_auto.target_fps > 0 ? g_auto.target_fps : g_auto.half_refresh ? refresh / 2 : refresh;
        if (target < 10) target = 10;
        if (target != last_target) { cap_target = 0.0f; last_cap = 0.0f; probe_ms = CAP_PROBE_MS; last_target = target; }

        int ok = fps_sample(&f);
        if (!ok && now - last_fps_start > 10000) { fps_start(); last_fps_start = now; }
        if (ok && f.pid != last_pid) {
            last_pid = f.pid;
            cap_target = last_cap = 0.0f;
            probe_ms = CAP_PROBE_MS;
            dc.bad = dg.bad = dc.bad_n = dg.bad_n = 0;
            lower_dom = NULL;
        }
        if (ok && f.frames != last_frames) { last_frames = f.frames; last_frame_t = now; }
        int game = ok && f.fps > 1.0f && last_frame_t && now - last_frame_t < 1500;

        if (!game) {
            if (!idle) {
                idle = 1;
                ff = 0.0f;
                cap_target = 0.0f;
                lower_dom = NULL;
                park_apply(0);
            }
            if (dc.d) dc.cur = dc.ceil;
            if (dg.d) dg.cur = dg.ceil;
            dom_apply(&dc, force);
            dom_apply(&dg, force);
            LK();
            st.cpu_mhz = dc.d ? dc.cur : 0;
            st.gpu_mhz = dg.d ? dg.cur : 0;
            st.target = target;
            st.fps = 0.0f;
            st.bottleneck = 0;
            UL();
            wait = 250;
            continue;
        }
        if (idle) {
            idle = 0;
            stable_since = now;
            below_n = 0;
        }
        wait = 50;

        float fast = f.fps_fast > 0.0f ? f.fps_fast : f.fps;
        ff = ff > 0.0f ? ff * 0.5f + fast * 0.5f : fast;

        if (now - last_util >= 250) {
            last_util = now;
            if (cpu_util(&cmax, &cavg)) {
                cmax = cavg = -1;
                ph_clk *c = ph_cpu_clk();
                int u;
                if (c && c->util && c->util(c, &u) == 0) cmax = cavg = u;
            }
            gutil = -1;
            if (dg.d && dg.d->util) {
                int u;
                if (dg.d->util(dg.d, &u) == 0) gutil = u;
            }
        }
        float ft = f.frametime_ms > 0.0f ? f.frametime_ms : 1000.0f / f.fps;
        gs = f.gpu_busy_ms > 0.0f && ft > 0.0f ? f.gpu_busy_ms / ft : gutil >= 0 ? (float)gutil / 100.0f : -1.0f;
        cs_ = f.cpu_busy_ms > 0.0f && ft > 0.0f ? f.cpu_busy_ms / ft : -1.0f;
        if (cmax >= 0 && (float)cmax / 100.0f > cs_) cs_ = (float)cmax / 100.0f;
        if (gs > 1.0f) gs = 1.0f;
        if (cs_ > 1.0f) cs_ = 1.0f;
        bn = gs >= 0.88f && gs >= cs_ ? 2 : cs_ >= 0.88f ? 1 : 0;

        float eff = cap_target > 0.0f ? cap_target : (float)target;
        if (cap_target > 0.0f && ff > cap_target * 1.05f) {
            cap_target = 0.0f;
            last_cap = 0.0f;
            probe_ms = CAP_PROBE_MS;
            eff = (float)target;
        }
        if (cap_target > 0.0f && now - cap_t > (uint64_t)probe_ms) { cap_target = 0.0f; eff = (float)target; }
        float thr_lo = eff * (1.0f - (float)g_auto.tolerance_pct / 100.0f);
        int ra = g_auto.raise_aggr, la = g_auto.lower_aggr;
        float big = 0.30f - 0.04f * (float)ra;

        int below = ff < thr_lo;
        if (below) { below_n++; if (!below_since) below_since = now; }
        else { below_n = 0; below_since = 0; }
        float d = below ? (eff - ff) / eff : 0.0f;

        int at_ceil = (!dc.d || dc.cur >= dc.ceil) && (!dg.d || dg.cur >= dg.ceil);
        if (below && at_ceil && cap_target <= 0.0f && below_since && now - below_since > 4000 && f.fps > 5.0f) {
            cap_target = f.fps * 1.02f;
            if (cap_target > (float)target) cap_target = (float)target;
            if (last_cap > 0.0f && cap_target < last_cap * 1.05f && cap_target > last_cap * 0.95f)
                probe_ms = probe_ms * 2 > CAP_PROBE_MAX ? CAP_PROBE_MAX : probe_ms * 2;
            else
                probe_ms = CAP_PROBE_MS;
            last_cap = cap_target;
            cap_t = now;
            below_n = 0;
            below_since = 0;
            stable_since = now;
        }

        if (g_auto.manage_cores && park_level > 0 && (below || bn == 1 || cmax >= 80)) {
            park_apply(0);
            park_votes = 0;
            last_park = now;
        }

        int cooldown = 400 - 60 * ra;
        if (below && (below_n >= 2 || d >= big) && now - last_raise >= (uint64_t)cooldown) {
            int handled = 0;
            if (lower_dom && now - lower_t < 3000 && d < big) {
                dom_mark_bad(lower_dom, now);
                lower_dom->cur = dom_q(lower_dom, lower_prev + lower_dom->step);
                handled = 1;
            }
            lower_dom = NULL;
            if (!handled) {
                float fac = 1.0f + d * 0.6f * (float)ra;
                int ms = (ra + 1) / 2;
                dom *pri = bn == 2 ? &dg : bn == 1 ? &dc : NULL;
                dom *sec = bn == 2 ? &dc : bn == 1 ? &dg : NULL;
                float ss = bn == 2 ? cs_ : gs;
                if (d >= big) {
                    if (pri) {
                        if (pri->d) { pri->bad = 0; pri->cur = pri->ceil; }
                        if (sec->d) dom_raise(sec, fac, ms);
                    } else {
                        if (dc.d) { dc.bad = 0; dc.cur = dc.ceil; }
                        if (dg.d) { dg.bad = 0; dg.cur = dg.ceil; }
                    }
                } else if (pri) {
                    if (!dom_raise(pri, fac, ms) || ss >= 0.7f) dom_raise(sec, 1.0f + (fac - 1.0f) / 3.0f, 1);
                } else {
                    dom_raise(&dc, fac, ms);
                    dom_raise(&dg, fac, ms);
                }
            }
            last_raise = now;
            stable_since = now;
            below_n = 0;
        } else if (!below) {
            int stable = f.fps >= thr_lo;
            if (!stable) stable_since = now;
            int lw = 1800 - 250 * la;
            if (lw < 500) lw = 500;
            if (stable && now - stable_since >= (uint64_t)g_auto.settle_ms && now - last_lower >= (uint64_t)lw) {
                int cl = dom_can_lower(&dc, now), gl = dom_can_lower(&dg, now);
                dom *x = NULL;
                if (cl && gl) {
                    float cscore = cs_ >= 0.0f ? cs_ : 0.5f, gscore = gs >= 0.0f ? gs : 0.5f;
                    if (cscore < gscore - 0.05f) x = &dc;
                    else if (gscore < cscore - 0.05f) x = &dg;
                    else x = (alt ^= 1) ? &dg : &dc;
                } else if (cl) x = &dc;
                else if (gl) x = &dg;
                if (x) {
                    int s = x->cur * la / 100;
                    if (s < x->step) s = x->step;
                    if (f.fps > eff * 1.08f) s *= 2;
                    int n = dom_q(x, x->cur - s);
                    if (bad_active(x, now) && n <= x->bad) n = dom_q(x, x->bad + x->step);
                    if (n < x->cur) {
                        lower_prev = x->cur;
                        lower_dom = x;
                        lower_t = now;
                        x->cur = n;
                    }
                    last_lower = now;
                }
            }
        }
        if (lower_dom && now - lower_t >= 3000) lower_dom = NULL;

        if (g_auto.manage_cores && now - last_park >= 2000) {
            last_park = now;
            int clear = bn == 2 && gs >= 0.95f && cmax >= 0 && cmax < 55 && !below;
            park_votes = clear ? park_votes + 1 : 0;
            if (park_votes >= 2 && park_level < 2) {
                int nl = park_level + 1;
                if (nl == 2 && (cavg < 0 || cavg >= 20)) nl = park_level;
                if (nl != park_level) park_apply(nl);
                park_votes = 0;
            }
        } else if (!g_auto.manage_cores && park_level) {
            park_apply(0);
        }

        dom_apply(&dc, force);
        dom_apply(&dg, force);

        LK();
        st.cpu_mhz = dc.d ? dc.cur : 0;
        st.gpu_mhz = dg.d ? dg.cur : 0;
        st.target = (int)(eff + 0.5f);
        st.fps = f.fps;
        st.cpu_util = cmax;
        st.gpu_util = gs >= 0.0f ? (int)(gs * 100.0f + 0.5f) : -1;
        st.bottleneck = bn;
        UL();
    }
    return 0;
}

/* ---------- start / stop ---------- */

static void notify(int on)
{
    ctls[K_ON].val = on;
    ctls[K_ON].dirty = 1;
    if (g_main && IsWindow(g_main)) {
        cfg_set_int("global", "autotdp.on", on);
        PostMessageW(g_main, WM_PH_REFRESH, 0, 0);
    }
}

static int start_locked(void)
{
    if (running) return 0;
    sync_cfg();
    ph_clk *c = ph_cpu_clk(), *g = ph_gpu_clk();
    memset(&dc, 0, sizeof dc);
    memset(&dg, 0, sizeof dg);
    dc.applied = dg.applied = -1;
    dc.d = dom_usable(c) ? c : NULL;
    dg.d = dom_usable(g) ? g : NULL;
    if (!dc.d && !dg.d) {
        ph_log("autotdp: no clock domains");
        return -1;
    }
    if (dc.d) dom_limits(&dc, g_auto.cpu_floor_mhz, g_auto.cpu_ceil_mhz);
    if (dg.d) dom_limits(&dg, g_auto.gpu_floor_mhz, g_auto.gpu_ceil_mhz);
    if (!stop_ev) stop_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!stop_ev) return -1;
    ResetEvent(stop_ev);

    epp_set = 0;
    if (g_auto.epp_zero) {
        pw_save(&sv_epp[0]);
        pw_save(&sv_epp[1]);
        for (int i = 0; i < 2; i++)
            if (sv_epp[i].ok && wp_set_epp(i, 0)) sv_epp[i].ok = 0;
        epp_set = sv_epp[0].ok || sv_epp[1].ok;
        wp_commit();
    }
    park_level = 0;
    cpu_primed = 0;
    LK();
    memset(&st, 0, sizeof st);
    st.running = 1;
    st.cpu_util = st.gpu_util = -1;
    UL();
    fps_start();
    InterlockedExchange(&running, 1);
    thr = CreateThread(NULL, 64 * 1024, worker, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!thr) {
        InterlockedExchange(&running, 0);
        if (epp_set) { pw_restore(&sv_epp[0]); pw_restore(&sv_epp[1]); wp_commit(); }
        LK();
        st.running = 0;
        UL();
        return -1;
    }
    ph_log("autotdp: started (cpu %ls, gpu %ls)", dc.d ? dc.d->name : L"-", dg.d ? dg.d->name : L"-");
    notify(1);
    return 0;
}

int autotdp_start(void)
{
    InitOnceExecuteOnce(&cs_once, cs_init, NULL, NULL);
    EnterCriticalSection(&op);
    int r = start_locked();
    LeaveCriticalSection(&op);
    return r;
}

void autotdp_stop(void)
{
    InitOnceExecuteOnce(&cs_once, cs_init, NULL, NULL);
    EnterCriticalSection(&op);
    if (!running) { LeaveCriticalSection(&op); return; }
    if (thr) {
        SetEvent(stop_ev);
        WaitForSingleObject(thr, INFINITE);
        CloseHandle(thr);
        thr = NULL;
    }
    dom_release(&dc);
    dom_release(&dg);
    dc.d = dg.d = NULL;
    park_apply(0);
    if (epp_set) {
        pw_restore(&sv_epp[0]);
        pw_restore(&sv_epp[1]);
        epp_set = 0;
    }
    InterlockedExchange(&running, 0);
    LK();
    memset(&st, 0, sizeof st);
    UL();
    wp_commit();
    LeaveCriticalSection(&op);
    for (int i = 0, n = ph_ctl_count(); i < n && !running; i++) {
        ph_ctl *k = ph_ctl_at(i);
        if (k && k->key && k->active && (k->flags & CF_AUTOTDP) && !(k->flags & CF_HIDDEN) && k->set)
            ph_ctl_apply(k, k->val);
    }
    wp_commit();
    if (!running && !ui_visible()) fps_stop();
    ph_log("autotdp: stopped");
    notify(0);
}

/* Crash path. autotdp_stop would wait forever if the worker is the thread that
   crashed (it sits in the crash filter), and it re-applies user values through
   callbacks that may be what crashed. Only take the caps off, and only built-in
   ones: the plugin host resets plugin domains itself, without calling a plugin
   that crashed or waiting on a lock the crashed thread holds. */
void autotdp_crash_release(DWORD crashed_tid)
{
    if (!running) return;
    if (stop_ev) SetEvent(stop_ev);
    if (thr && GetThreadId(thr) != crashed_tid) WaitForSingleObject(thr, 1000);
    if (dc.d && !dc.d->plugin) dom_release(&dc);
    if (dg.d && !dg.d->plugin) dom_release(&dg);
    park_apply(0);
    if (epp_set) {
        pw_restore(&sv_epp[0]);
        pw_restore(&sv_epp[1]);
        epp_set = 0;
    }
    InterlockedExchange(&running, 0);
}

int autotdp_running(void) { return running != 0; }

void autotdp_state(ph_auto_state *s)
{
    LK();
    *s = st;
    s->running = running != 0;
    UL();
}

/* ---------- control callbacks ---------- */

static int gpu_sel;

static int set_on(ph_ctl *c, int32_t v)
{
    (void)c;
    if (v) return autotdp_start();
    autotdp_stop();
    return 0;
}

static int get_on(ph_ctl *c, int32_t *out) { (void)c; *out = autotdp_running(); return 0; }

static int set_store(ph_ctl *c, int32_t v) { (void)c; (void)v; return 0; }

static int set_target(ph_ctl *c, int32_t v)
{
    (void)c;
    if (v == 2) ctls[K_FPS].flags &= (uint16_t)~CF_HIDDEN;
    else ctls[K_FPS].flags |= CF_HIDDEN;
    return 0;
}

static int cap_clk(ph_clk *d, int32_t v)
{
    if (!d) return -1;
    if (v <= 0) return d->reset ? d->reset(d) : 0;
    v = PH_CLAMP(v, d->min_mhz, d->max_mhz);
    return d->set_max(d, v);
}

static int set_cpumax(ph_ctl *c, int32_t v)
{
    (void)c;
    if (running) return 0;
    return cap_clk(ph_cpu_clk(), v);
}

static int set_gpumax(ph_ctl *c, int32_t v)
{
    (void)c;
    if (running) return 0;
    return cap_clk(ph_gpu_clk(), v);
}

static void gpu_ranges(ph_clk *d)
{
    ph_ctl *m = &ctls[K_GPUMAX];
    if (!d) return;
    m->max = d->max_mhz;
    m->step = d->step_mhz > 0 ? d->step_mhz : 50;
    if (m->val > m->max) m->val = m->max;
}

/* auto.gpudomain is an index into a list sorted by priority, which a plugin with
   a higher one shifts; the name is saved too and wins when it is still there */
#define GPUDOM_NAME "auto.gpudomain_name"

static int set_gpudom(ph_ctl *c, int32_t v)
{
    (void)c;
    if (v < 0 || v >= ph_gpu_clk_count()) return -1;
    if (v == gpu_sel) return 0;
    ph_clk *old = ph_gpu_clk();
    ph_ctl *m = &ctls[K_GPUMAX];
    if (!running && m->active && m->val > 0 && old && old->reset) old->reset(old);
    ph_select_gpu_clk(v);
    gpu_sel = v;
    ph_clk *nw = ph_gpu_clk();
    gpu_ranges(nw);
    if (!running && m->active && m->val > 0) cap_clk(nw, m->val);
    char name[64];
    if (nw && nw->name && WideCharToMultiByte(CP_UTF8, 0, nw->name, -1, name, sizeof name, NULL, NULL))
        cfg_set_str("global", GPUDOM_NAME, name);
    return 0;
}

void autotdp_cfg_loaded(void)
{
    ph_ctl *c = &ctls[K_GPUDOM];
    char name[64];
    wchar_t w[64];
    if (!c->active || !cfg_get_str("global", GPUDOM_NAME, name, sizeof name) || !name[0] ||
        !MultiByteToWideChar(CP_UTF8, 0, name, -1, w, PH_ARRAY(w)))
        return;
    for (int i = 0; i < ph_gpu_clk_count(); i++) {
        ph_clk *d = ph_gpu_clk_at(i);
        if (d->name && !lstrcmpW(d->name, w)) { c->val = i; return; }
    }
}

static void fmt_fps(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c;
    ph_swprintf(b, n, L"%d FPS", v);
}

static void fmt_ms(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c;
    ph_swprintf(b, n, L"%d.%d s", v / 1000, (v % 1000) / 100);
}

static void fmt_lvl(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    static const wchar_t *const nm[] = { L"Gentle", L"Low", L"Medium", L"High", L"Max" };
    (void)c;
    lstrcpynW(b, nm[PH_CLAMP(v, 1, 5) - 1], n);
}

static void fmt_status(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c; (void)v;
    ph_auto_state s;
    autotdp_state(&s);
    int pm = fps_status();
    const wchar_t *pmmsg = pm == 0 ? L"PresentMon not installed (no FPS)" : pm == 1 ? L"PresentMon service not reachable" : NULL;
    if (!s.running) {
        if (!dom_usable(ph_cpu_clk()) && !dom_usable(ph_gpu_clk())) lstrcpynW(b, L"Off - no CPU/GPU clock control found", n);
        else if (pmmsg) ph_swprintf(b, n, L"Off - %s", pmmsg);
        else lstrcpynW(b, L"Off", n);
        return;
    }
    if (s.fps <= 0.0f) { lstrcpynW(b, pmmsg ? pmmsg : L"Waiting for game", n); return; }
    wchar_t cb[16], gb[16];
    if (s.cpu_mhz) ph_swprintf(cb, 16, L"%d", s.cpu_mhz); else lstrcpynW(cb, L"-", 16);
    if (s.gpu_mhz) ph_swprintf(gb, 16, L"%d", s.gpu_mhz); else lstrcpynW(gb, L"-", 16);
    ph_swprintf(b, n, L"%d/%d FPS  C%s G%s%s", (int)(s.fps + 0.5f), s.target, cb, gb,
                s.bottleneck == 2 ? L" GPU" : s.bottleneck == 1 ? L" CPU" : L"");
}

static const wchar_t *const target_ch[] = { L"Refresh rate", L"Half refresh", L"Custom", NULL };
static const wchar_t *gpu_ch[9];

#define QK (CF_PROFILE)
#define TUNE (CF_ADVANCED | CF_PROFILE)

static ph_ctl ctls[K_COUNT] = {
    [K_ON] = { .key = "auto.on", .label = L"Phawx ON", .type = CT_TOGGLE, .page = PG_QUICK, .order = 1,
               .flags = CF_NOSAVE, .set = set_on, .get = get_on, .fmt = fmt_onoff },
    [K_TARGET] = { .key = "auto.target", .label = L"Target", .type = CT_CHOICE, .page = PG_QUICK, .order = 2,
                   .flags = QK, .choices = target_ch, .set = set_target },
    [K_FPS] = { .key = "auto.fps", .label = L"Target FPS", .type = CT_SLIDER, .page = PG_QUICK, .order = 3,
                .flags = QK | CF_HIDDEN, .min = 20, .max = 240, .step = 5, .def = 60, .unit = L"FPS",
                .fmt = fmt_fps, .set = set_store },
    [K_STATUS] = { .key = NULL, .label = L"AutoTDP", .type = CT_INFO, .page = PG_QUICK, .order = 4,
                   .fmt = fmt_status },
    [K_CPUMAX] = { .key = "auto.cpumax", .label = L"CPU max clock", .type = CT_SLIDER, .page = PG_QUICK, .order = 30,
                   .flags = CF_AUTOTDP | CF_PROFILE | CF_REAPPLY, .min = 0, .step = 100, .def = 0, .unit = L"MHz",
                   .fmt = fmt_mhz, .set = set_cpumax },
    [K_GPUMAX] = { .key = "auto.gpumax", .label = L"GPU max clock", .type = CT_SLIDER, .page = PG_QUICK, .order = 40,
                   .flags = CF_AUTOTDP | CF_PROFILE | CF_REAPPLY, .min = 0, .step = 50, .def = 0, .unit = L"MHz",
                   .fmt = fmt_mhz, .set = set_gpumax },
    [K_GPUDOM] = { .key = "auto.gpudomain", .label = L"GPU clock control", .type = CT_CHOICE, .page = PG_QUICK,
                   .order = 41, .choices = gpu_ch, .set = set_gpudom },

    [K_HDR] = { .key = NULL, .label = L"AutoTDP tuning", .type = CT_HEADER, .page = PG_CPU, .order = 400,
                .flags = CF_ADVANCED },
    [K_TOL] = { .key = "auto.tolerance", .label = L"FPS tolerance", .type = CT_SLIDER, .page = PG_CPU, .order = 401,
                .flags = TUNE, .min = 1, .max = 15, .step = 1, .def = 3, .unit = L"%", .fmt = fmt_pct, .set = set_store },
    [K_RAISE] = { .key = "auto.raise", .label = L"Raise aggressiveness", .type = CT_SLIDER, .page = PG_CPU,
                  .order = 402, .flags = TUNE, .min = 1, .max = 5, .step = 1, .def = 3, .fmt = fmt_lvl, .set = set_store },
    [K_LOWER] = { .key = "auto.lower", .label = L"Lower aggressiveness", .type = CT_SLIDER, .page = PG_CPU,
                  .order = 403, .flags = TUNE, .min = 1, .max = 5, .step = 1, .def = 2, .fmt = fmt_lvl, .set = set_store },
    [K_SETTLE] = { .key = "auto.settle", .label = L"Settle time", .type = CT_SLIDER, .page = PG_CPU, .order = 404,
                   .flags = TUNE, .min = 500, .max = 10000, .step = 250, .def = 2000, .fmt = fmt_ms, .set = set_store },
    [K_CPUFLOOR] = { .key = "auto.cpufloor", .label = L"AutoTDP CPU floor", .type = CT_SLIDER, .page = PG_CPU,
                     .order = 405, .flags = TUNE, .min = 0, .step = 100, .unit = L"MHz", .fmt = fmt_mhz, .set = set_store },
    [K_CPUCEIL] = { .key = "auto.cpuceil", .label = L"AutoTDP CPU ceiling", .type = CT_SLIDER, .page = PG_CPU,
                    .order = 406, .flags = TUNE, .min = 0, .step = 100, .unit = L"MHz", .fmt = fmt_mhz, .set = set_store },
    [K_GPUFLOOR] = { .key = "auto.gpufloor", .label = L"AutoTDP GPU floor", .type = CT_SLIDER, .page = PG_CPU,
                     .order = 407, .flags = TUNE, .min = 0, .step = 50, .unit = L"MHz", .fmt = fmt_mhz, .set = set_store },
    [K_GPUCEIL] = { .key = "auto.gpuceil", .label = L"AutoTDP GPU ceiling", .type = CT_SLIDER, .page = PG_CPU,
                    .order = 408, .flags = TUNE, .min = 0, .step = 50, .unit = L"MHz", .fmt = fmt_mhz, .set = set_store },
    [K_CORES] = { .key = "auto.cores", .label = L"Dynamic core parking", .type = CT_TOGGLE, .page = PG_CPU,
                  .order = 409, .flags = TUNE, .def = 0, .fmt = fmt_onoff, .set = set_store },
    [K_EPP0] = { .key = "auto.epp0", .label = L"Force EPP 0 while active", .type = CT_TOGGLE, .page = PG_CPU,
                 .order = 410, .flags = CF_ADVANCED, .def = 1, .fmt = fmt_onoff, .set = set_store },
};

void autotdp_register_ctls(void)
{
    InitOnceExecuteOnce(&cs_once, cs_init, NULL, NULL);
    ph_clk *c = ph_cpu_clk();
    int ng = ph_gpu_clk_count(), gmax = 0, gstep = 50;
    if (ng > PH_ARRAY(gpu_ch) - 1) ng = PH_ARRAY(gpu_ch) - 1;
    for (int i = 0; i < ng; i++) {
        ph_clk *g = ph_gpu_clk_at(i);
        gpu_ch[i] = g->name ? g->name : L"GPU";
        if (g->max_mhz > gmax) { gmax = g->max_mhz; gstep = g->step_mhz > 0 ? g->step_mhz : 50; }
    }
    gpu_ch[ng] = NULL;

    if (dom_usable(c)) {
        int st_ = c->step_mhz > 0 ? c->step_mhz : 100;
        ph_ctl *k[] = { &ctls[K_CPUMAX], &ctls[K_CPUFLOOR], &ctls[K_CPUCEIL] };
        for (int i = 0; i < 3; i++) { k[i]->max = c->max_mhz; k[i]->step = st_; }
    } else {
        ctls[K_CPUMAX].flags |= CF_HIDDEN;
        ctls[K_CPUFLOOR].flags |= CF_HIDDEN;
        ctls[K_CPUCEIL].flags |= CF_HIDDEN;
        ctls[K_CPUMAX].max = ctls[K_CPUFLOOR].max = ctls[K_CPUCEIL].max = 6000;
    }
    if (gmax > 0) {
        ph_ctl *k[] = { &ctls[K_GPUFLOOR], &ctls[K_GPUCEIL] };
        for (int i = 0; i < 2; i++) { k[i]->max = gmax; k[i]->step = gstep; }
        ctls[K_GPUMAX].max = gmax;
        gpu_ranges(ph_gpu_clk());
    } else {
        ctls[K_GPUMAX].flags |= CF_HIDDEN;
        ctls[K_GPUFLOOR].flags |= CF_HIDDEN;
        ctls[K_GPUCEIL].flags |= CF_HIDDEN;
        ctls[K_GPUMAX].max = ctls[K_GPUFLOOR].max = ctls[K_GPUCEIL].max = 3000;
    }
    if (ng < 2) {
        ctls[K_GPUDOM].flags |= CF_HIDDEN;
        if (ng == 0) { gpu_ch[0] = L"None"; gpu_ch[1] = NULL; }
    }
    if (!g_plat.hybrid) ctls[K_CORES].flags |= CF_HIDDEN;
    if (!g_plat.epp) ctls[K_EPP0].flags |= CF_HIDDEN;
    ctls[K_ON].flags &= (uint16_t)~CF_HIDDEN;
    ctls[K_TARGET].flags &= (uint16_t)~CF_HIDDEN;
    ctls[K_STATUS].flags &= (uint16_t)~CF_HIDDEN;
    ph_register_ctls(ctls, K_COUNT);
}
