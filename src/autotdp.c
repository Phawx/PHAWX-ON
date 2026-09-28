#include "phawx.h"
#include <string.h>

/* AutoTDP ("Phawx ON"): hold the frame rate PresentMon reports at the target with
   as little power as the machine will do it on.

   What it moves:
   - the CPU and GPU clock caps: up at once when frames fall short, down a step at a
     time while the frame rate holds;
   - how many cores Windows may keep unparked, per core type (CPMAXCORES);
   - EPP, how eagerly the CPU clocks up inside the cap.
   All of them are judged by what the machine then draws: the battery's discharge
   rate (the whole machine) while it runs on battery, the CPU package or APU and a
   discrete GPU otherwise. A lower clock is kept unless the draw goes up. Fewer cores
   or a higher EPP is a trial: the clocks may catch up with it, and it is kept only
   if the machine then draws clearly less at the same frame rate. Only one thing is
   tried at a time, so each change is measured on its own.

   Without a game the caps come off and core parking and a tuned EPP go back to
   Windows' own settings; everything does when AutoTDP stops, and after a crash. */

ph_auto_cfg g_auto = {
    .tolerance_pct = 3, .raise_aggr = 3, .lower_aggr = 2, .settle_ms = 2000, .manage_cores = 1, .epp_mode = EPP_TUNE
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

#define IMIN(a, b) ((a) < (b) ? (a) : (b))
#define IMAX(a, b) ((a) > (b) ? (a) : (b))

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
static uint64_t last_change;        /* when AutoTDP last changed a cap or a setting */

#define BAD_HOLD_MS   20000         /* a value that cost frames is not tried again for this long (doubling) */
#define MISS_HOLD_MS  30000         /* after a trial that saved nothing (doubling) */
#define CAP_PROBE_MS  30000
#define CAP_PROBE_MAX 300000
#define FAIL_MS       3000          /* a drop this soon after a quick clock step is blamed on it */
#define CHECK_MS      1000          /* for Windows to park cores after a new limit */
#define EPP_STEP      20
#define EPP_MAX       60

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
    K_ON, K_TARGET, K_FPS, K_STATUS, K_POWER, K_CPUMAX, K_GPUMAX, K_GPUDOM,
    K_HDR, K_TOL, K_RAISE, K_LOWER, K_SETTLE, K_CPUFLOOR, K_CPUCEIL, K_GPUFLOOR, K_GPUCEIL, K_CORES, K_EPP,
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
    g_auto.manage_cores = V(K_CORES);
    g_auto.epp_mode = PH_CLAMP(V(K_EPP), EPP_LEAVE, EPP_TUNE);
    uint16_t f = ctls[K_FPS].flags;
    uint16_t nf = mode == 2 ? (uint16_t)(f & ~CF_HIDDEN) : (uint16_t)(f | CF_HIDDEN);
    if (nf != f) ctls[K_FPS].flags = nf;
}

/* ---------- clock domains ---------- */

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
    int moved = x->cur != x->applied;
    if (x->d->set_max(x->d, x->cur) == 0) {
        x->applied = x->cur;
        x->touched = 1;
        if (moved) last_change = ph_ms();
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

static void dom_mark_bad(dom *x, int v, uint64_t now)
{
    int dd = v - x->bad;
    x->bad_n = x->bad > 0 && dd <= x->step && dd >= -x->step ? x->bad_n + 1 : 1;
    x->bad = v;
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

/* ---------- unparked cores, as Windows reports them ---------- */

typedef BOOL (WINAPI *pfn_cpusets)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
static pfn_cpusets cpusets;
static int cpusets_tried;
static SYSTEM_CPU_SET_INFORMATION cs_buf[256];
static int act[2] = { -1, -1 };     /* unparked cores per class, latest */
static int8_t act_ring[8][2];       /* the last two seconds */
static int act_n, act_i;
static uint64_t act_t;              /* when act was read */
static int park_up;                 /* Windows rounds CPMAXCORES up to whole cores */

/* physical cores with a logical CPU that is not parked, per class (1 = P on hybrid) */
static int unparked(int out[2])
{
    if (!cpusets && !cpusets_tried) {
        cpusets_tried = 1;
        HMODULE m = GetModuleHandleW(L"kernel32.dll");
        if (m) cpusets = (pfn_cpusets)(void *)GetProcAddress(m, "GetSystemCpuSetInformation");
    }
    ULONG len = 0;
    if (!cpusets || !cpusets(cs_buf, sizeof cs_buf, &len, NULL, 0) || len > sizeof cs_buf) return -1;
    uint8_t seen[4][256];
    memset(seen, 0, sizeof seen);
    int any = 0;
    out[0] = out[1] = 0;
    for (ULONG off = 0; off + 8 <= len;) {
        const SYSTEM_CPU_SET_INFORMATION *e = (const SYSTEM_CPU_SET_INFORMATION *)(const void *)((const BYTE *)cs_buf + off);
        if (e->Size < 24 || off + e->Size > len) break;
        if (e->Type == CpuSetInformation) {
            uint8_t *s = &seen[e->CpuSet.Group & 3][e->CpuSet.CoreIndex];
            any = 1;
            if (!(e->CpuSet.AllFlags & SYSTEM_CPU_SET_INFORMATION_PARKED) && !*s) {
                *s = 1;
                out[g_plat.hybrid && e->CpuSet.EfficiencyClass == g_plat.max_class]++;
            }
        }
        off += e->Size;
    }
    return any ? 0 : -1;
}

static void act_sample(void)
{
    int u[2];
    if (unparked(u)) {
        act[0] = act[1] = -1;
        act_n = 0;
        return;
    }
    act[0] = u[0];
    act[1] = u[1];
    act_t = ph_ms();
    act_ring[act_i][0] = (int8_t)IMIN(u[0], 127);
    act_ring[act_i][1] = (int8_t)IMIN(u[1], 127);
    act_i = (act_i + 1) % 8;
    if (act_n < 8) act_n++;
}

static int act_max(int c)
{
    if (act_n < 4) return -1;
    int m = 0;
    for (int i = 0; i < act_n; i++) m = IMAX(m, act_ring[i][c]);
    return m;
}

/* ---------- power ---------- */

#define PW_N 192                    /* 250 ms apart: 48 s */

typedef struct { uint64_t t; int sys, parts; } pw_s;
typedef struct { int mean, sd, n; } pstat;

static pw_s pw[PW_N];
static int pw_i, pw_n, sys_prev = -1, sys_upd;
static uint64_t pw_t, sys_t;

static void pw_sample(uint64_t now)
{
    if (pw_t && now - pw_t < 250) return;
    pw_t = now;
    ph_power p;
    int sys = -1, parts = -1;
    if (ph_power_read(&p) == 0) {
        sys = p.sys_mw;
        parts = p.parts_mw;
    }
    /* how often the battery's reading changes sets how long a trial waits for it */
    if (sys < 0) sys_t = 0;
    else if (sys != sys_prev) {
        if (sys_t && sys_prev >= 0 && now - sys_t < 60000) {
            int dt = (int)(now - sys_t);
            sys_upd = sys_upd ? (sys_upd * 3 + dt) / 4 : dt;
        }
        sys_t = now;
    }
    sys_prev = sys;
    pw[pw_i] = (pw_s){ now, sys, parts };
    pw_i = (pw_i + 1) % PW_N;
    if (pw_n < PW_N) pw_n++;
}

static int isqrt64(int64_t v)
{
    if (v <= 0) return 0;
    uint64_t x = (uint64_t)v, r = 0, b = (uint64_t)1 << 62;
    while (b > x) b >>= 2;
    while (b) {
        if (x >= r + b) { x -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return (int)r;
}

/* mean and spread of the battery's reading (sys) or of the parts over [a, b] */
static int pw_stat(int sys, uint64_t a, uint64_t b, pstat *o)
{
    int64_t sum = 0, sq = 0;
    int n = 0, all = 0, chg = 0, prev = -1;
    for (int j = 0; j < pw_n; j++) {
        const pw_s *s = &pw[(pw_i - pw_n + j + PW_N) % PW_N];
        if (s->t < a || s->t > b) continue;
        all++;
        int v = sys ? s->sys : s->parts;
        if (v < 0) continue;
        sum += v;
        sq += (int64_t)v * v;
        if (v != prev) chg++;
        prev = v;
        n++;
    }
    if (n < 4 || n * 4 < all * 3) return -1;
    /* the battery's own updates count, not the samples; samples 250 ms apart
       are not independent either */
    o->n = sys ? chg : (n + 1) / 2;
    o->mean = (int)(sum / n);
    o->sd = isqrt64(sq / n - (int64_t)o->mean * o->mean);
    return 0;
}

/* -1 clearly less, 1 clearly more, 0 within the noise */
static int pw_cmp(const pstat *b, const pstat *t)
{
    int thr = IMAX(150, b->mean * 15 / 1000);
    int se = isqrt64((int64_t)b->sd * b->sd / b->n + (int64_t)t->sd * t->sd / t->n);
    thr = IMAX(thr, 2 * se);
    int d = t->mean - b->mean;
    return d < -thr ? -1 : d > thr ? 1 : 0;
}

/* which readings came in over the last two seconds */
static void pw_avail(uint64_t now, int *sys, int *parts)
{
    *sys = *parts = 0;
    for (int j = 0; j < pw_n && j < 8; j++) {
        const pw_s *s = &pw[(pw_i - 1 - j + PW_N) % PW_N];
        if (now - s->t > 2000) break;
        if (s->sys >= 0) *sys = 1;
        if (s->parts >= 0) *parts = 1;
    }
}

static int pw_now(uint64_t now, int *src)
{
    pstat s;
    uint64_t a = now > 2000 ? now - 2000 : 0;
    if (pw_stat(1, a, now, &s) == 0) { *src = PSRC_SYSTEM; return s.mean; }
    if (pw_stat(0, a, now, &s) == 0) { *src = PSRC_PARTS; return s.mean; }
    *src = PSRC_NONE;
    return -1;
}

/* how long a trial lets things settle, then measures. The parts read fast; the
   battery's rate is often an average the firmware updates every few seconds. */
static void trial_times(int sys, int parts, int structural, int *settle, int *meas)
{
    int s = 1000, m = structural ? 3000 : 2500;
    if (sys) {
        int u = PH_CLAMP(sys_upd > 0 ? sys_upd : 2000, 1000, 10000);
        int ss = PH_CLAMP(u * 3 / 2 + 500, 1500, 10000), sm = PH_CLAMP(u * 3, 3000, 15000);
        if (!parts) { s = ss; m = sm; }
        else if (structural) { s = IMAX(s, IMIN(ss, 4000)); m = IMAX(m, IMIN(sm, 8000)); }
    }
    *settle = s;
    *meas = m;
}

/* ---------- Windows settings the search moves ---------- */

enum { KN_CORE0, KN_CORE1, KN_EPP, KN_N };  /* unparked cores of class 0 (E, or all) and 1 (P); EPP */

typedef struct {
    int      on;                        /* the search may move it */
    int      n;                         /* cores in the class */
    int      cur, home, limit, step;    /* it starts at home and goes back there without a game */
    int      applied;                   /* what was written, -1 = Windows' own setting */
    int      saved, broken;
    int      bad, bad_n;                /* a value that cost frames */
    uint64_t bad_t;
    int      miss;                      /* trials in a row that saved nothing */
    uint64_t next_t;                    /* no trial before this */
    int      resume;                    /* where it was when the game stopped showing frames */
} knob;

static knob kn[KN_N];

static int kn_dir(const knob *k) { return k->limit < k->home ? -1 : 1; }

static int kn_bad(const knob *k, int v, uint64_t now)
{
    if (!k->bad_t || now - k->bad_t >= ((uint64_t)BAD_HOLD_MS << PH_CLAMP(k->bad_n - 1, 0, 3))) return 0;
    return kn_dir(k) < 0 ? v <= k->bad : v >= k->bad;
}

static void kn_mark_bad(knob *k, int v, uint64_t now)
{
    k->bad_n = k->bad_t && k->bad == v ? k->bad_n + 1 : 1;
    k->bad = v;
    k->bad_t = now;
    k->next_t = now + BAD_HOLD_MS;
}

static void kn_miss(knob *k, uint64_t now)
{
    k->miss++;
    k->next_t = now + ((uint64_t)MISS_HOLD_MS << PH_CLAMP(k->miss - 1, 0, 3));
}

/* where the next trial goes: a step further, bigger after trials that saved nothing
   (a single core can be too little to measure, or round to no change), and not as
   far as a value that cost frames */
static int kn_target(const knob *k, uint64_t now)
{
    int d = kn_dir(k), v = k->cur + d * k->step * (1 << PH_CLAMP(k->miss, 0, 2));
    if (d < 0 ? v < k->limit : v > k->limit) v = k->limit;
    while (v != k->cur && kn_bad(k, v, now)) v -= d * k->step;
    return v;
}

static int kn_home(void)
{
    for (int i = 0; i < KN_N; i++)
        if (kn[i].cur != kn[i].home) return 0;
    return 1;
}

static void kn_setup(void)
{
    uint8_t seen[256] = { 0 };
    int n[2] = { 0, 0 };
    for (int i = 0; i < g_plat.nlogical && i < 256 && g_plat.ncores > 0; i++) {
        int c = g_plat.core_of[i];
        if (seen[c]) continue;
        seen[c] = 1;
        n[g_plat.hybrid && g_plat.cls[i] == g_plat.max_class]++;
    }
    memset(kn, 0, sizeof kn);
    for (int i = KN_CORE0; i <= KN_CORE1; i++) {
        /* never more cores than the plan already allows (the user's own limit) */
        DWORD ac, dcv;
        int home = n[i];
        const GUID *g = i ? &G_CPMAX1 : &G_CPMAX0;
        if (n[i] && wp_read(&GUID_SUB_PROCESSOR_, g, 0, &ac) == 0 && wp_read(&GUID_SUB_PROCESSOR_, g, 1, &dcv) == 0) {
            DWORD pct = ac < dcv ? ac : dcv;
            if (pct < 100) home = PH_CLAMP((int)((pct * (DWORD)n[i] + 99) / 100), 1, n[i]);
        }
        kn[i].n = n[i];
        kn[i].home = kn[i].cur = kn[i].resume = home;
        kn[i].step = 1;
        kn[i].applied = -1;
    }
    /* at least two cores; on a hybrid CPU two P-cores and an E-core */
    if (g_plat.hybrid) {
        kn[KN_CORE1].limit = IMIN(n[1], 2);
        kn[KN_CORE0].limit = IMIN(n[0], 1);
    } else {
        kn[KN_CORE0].limit = IMIN(n[0], 2);
    }
    kn[KN_EPP].limit = EPP_MAX;
    kn[KN_EPP].step = EPP_STEP;
    kn[KN_EPP].applied = -1;
}

static void kn_sync(void)
{
    for (int i = KN_CORE0; i <= KN_CORE1; i++) kn[i].on = g_auto.manage_cores && kn[i].home > kn[i].limit && !kn[i].broken;
    kn[KN_EPP].on = g_auto.epp_mode == EPP_TUNE && g_plat.epp && !kn[KN_EPP].broken;
}

/* a new game or target: start again from all cores and EPP 0 */
static void kn_reset(void)
{
    for (int i = 0; i < KN_N; i++) {
        knob *k = &kn[i];
        k->cur = k->resume = k->home;
        k->bad = k->bad_n = k->miss = 0;
        k->bad_t = k->next_t = 0;
    }
}

/* want: a CPMAXCORES % or an EPP; -1 = Windows' own setting back */
static void kn_write(int i, int want)
{
    knob *k = &kn[i];
    saved_pw *sv = i == KN_EPP ? sv_epp : &sv_park[i * 2];
    if (k->broken) want = -1;
    if (want == k->applied) return;
    if (want < 0) {
        pw_restore(&sv[0]);
        pw_restore(&sv[1]);
        k->saved = 0;
    } else {
        if (!k->saved) {
            pw_save(&sv[0]);
            pw_save(&sv[1]);
            k->saved = 1;
        }
        int r = -1;
        if (i == KN_EPP) {
            for (int c = 0; c < 2; c++)      /* only what can be put back */
                if (sv[c].ok && wp_set_epp(c, want) == 0) r = 0;
        } else if (sv[0].ok && sv[1].ok) {
            r = wp_set_cores(i, 0, want);
        }
        if (r) {
            ph_log("autotdp: cannot change %s, leaving it to Windows", i == KN_EPP ? "EPP" : "core parking");
            pw_restore(&sv[0]);
            pw_restore(&sv[1]);
            k->saved = 0;
            k->broken = 1;
            k->on = 0;
            k->cur = k->home;
            k->applied = -1;
            return;
        }
    }
    k->applied = want;
    last_change = ph_ms();
}

/* the CPMAXCORES for `cur` cores. Rounded up it gives at least that many however
   Windows rounds; once Windows is seen rounding up, rounded down gives exactly that,
   except at the floor, which must hold even if that was misread. */
static int park_pct(const knob *k)
{
    return park_up && k->cur > k->limit ? k->cur * 100 / k->n : (k->cur * 100 + k->n - 1) / k->n;
}

static void kn_apply(int game)
{
    for (int i = KN_CORE0; i <= KN_CORE1; i++) {
        knob *k = &kn[i];
        if (!game || !k->on) k->cur = k->home;
        kn_write(i, k->n && k->cur < k->home ? park_pct(k) : -1);
    }
    knob *e = &kn[KN_EPP];
    if (!game || !e->on) e->cur = e->home;
    int m = g_auto.epp_mode;
    kn_write(KN_EPP, m == EPP_ZERO || (m == EPP_TUNE && game) ? e->cur : -1);
}

static void kn_restore(void)
{
    for (int i = 0; i < KN_N; i++) {
        saved_pw *sv = i == KN_EPP ? sv_epp : &sv_park[i * 2];
        pw_restore(&sv[0]);
        pw_restore(&sv[1]);
        kn[i].saved = 0;
        kn[i].applied = -1;
    }
}

/* ---------- trials ---------- */

enum { TK_NONE, TK_CPU, TK_GPU, TK_CORE0, TK_CORE1, TK_EPP };
enum { TR_KEEP, TR_GAIN, TR_HOLD };   /* keep unless it costs power / only if it saves power / if the frames held */
enum { PH_CHECK, PH_SETTLE, PH_MEASURE };

static struct {
    int      kind, mode, phase;
    int      from, to;
    uint64_t t;                     /* start of this phase */
    int      settle, meas;
    pstat    bs, bp;                /* before: the battery, the parts */
    int      bs_ok, bp_ok;
    float    low1;                  /* 1% low before */
    int      snap_c, snap_g;        /* clocks before, back if it fails */
    int      raises, act0;
} tr;

static int rr;                      /* the setting to try next */
static dom *lower_dom;              /* a quick clock step, watched for FAIL_MS */
static int lower_prev;
static uint64_t lower_t, last_lower;

static knob *tr_knob(void) { return tr.kind >= TK_CORE0 ? &kn[tr.kind - TK_CORE0] : NULL; }
static dom *tr_dom(void) { return tr.kind == TK_CPU ? &dc : tr.kind == TK_GPU ? &dg : NULL; }

static const char *tr_name(int kind)
{
    switch (kind) {
    case TK_CPU: return "CPU clock";
    case TK_GPU: return "GPU clock";
    case TK_CORE0: return g_plat.hybrid ? "E-cores" : "cores";
    case TK_CORE1: return "P-cores";
    case TK_EPP: return "EPP";
    }
    return "";
}

static int tr_begin(int kind, int mode, int from, int to, uint64_t now, float low1, int settle, int meas)
{
    uint64_t a = now > (uint64_t)meas ? now - (uint64_t)meas : 0;
    tr.bs_ok = pw_stat(1, a, now, &tr.bs) == 0;
    tr.bp_ok = pw_stat(0, a, now, &tr.bp) == 0;
    if (mode != TR_HOLD && !tr.bs_ok && !tr.bp_ok) return 0;
    tr.kind = kind;
    tr.mode = mode;
    tr.from = from;
    tr.to = to;
    tr.t = now;
    tr.settle = settle;
    tr.meas = mode == TR_HOLD ? IMAX(meas, 4000) : meas;
    tr.low1 = low1;
    tr.snap_c = dc.cur;
    tr.snap_g = dg.cur;
    tr.raises = 0;
    tr.act0 = kind == TK_CORE0 || kind == TK_CORE1 ? act_max(kind - TK_CORE0) : -1;
    tr.phase = tr.act0 >= 0 ? PH_CHECK : PH_SETTLE;
    return 1;
}

/* why: 0 = the power, 1 = the frames */
static void tr_end(int keep, int why, uint64_t now)
{
    knob *k = tr_knob();
    dom *x = tr_dom();
    if (!keep) {
        if (x) {
            dom_mark_bad(x, tr.to, now);
            x->cur = dom_q(x, tr.from);
        }
        if (k) {
            k->cur = tr.from;
            if (why) kn_mark_bad(k, tr.to, now);
            else kn_miss(k, now);
            /* the clocks were enough before */
            if (dc.d) dc.cur = dom_q(&dc, tr.snap_c);
            if (dg.d) dg.cur = dom_q(&dg, tr.snap_g);
        }
    } else if (k) {
        k->miss = 0;
        k->next_t = 0;
    }
    if (k || !keep)
        ph_log("autotdp: %s %d -> %d %s", tr_name(tr.kind), tr.from, tr.to,
               keep ? "kept" : why ? "cost frames" : k ? "saved nothing" : "drew more");
    tr.kind = TK_NONE;
}

/* a new game or target, or no game: put back what is being tried */
static void tr_cancel(void)
{
    knob *k = tr_knob();
    dom *x = tr_dom();
    if (x) x->cur = tr.from;
    if (k) k->cur = tr.from;
    tr.kind = TK_NONE;
    lower_dom = NULL;
}

static void tr_step(uint64_t now, float low1)
{
    knob *k = tr_knob();
    if (k && !k->on) { tr_cancel(); return; }
    if (tr.phase == PH_CHECK) {
        /* what Windows made of the new limit, read after it had time to act */
        int fresh = act_t >= tr.t + CHECK_MS / 2;
        if (now - tr.t < CHECK_MS || (!fresh && now - tr.t < 3 * CHECK_MS)) return;
        int a = fresh ? act[tr.kind - TK_CORE0] : -1;
        if (a >= 0 && !park_up && a == k->cur + 1 && k->cur * 100 % k->n) {
            park_up = 1;
            ph_log("autotdp: Windows rounds core parking up");
            tr.t = now;
            return;
        }
        if (a >= 0 && a >= tr.act0) {
            /* as many cores as before: this limit does not bind (yet), go one further */
            int v = k->cur + kn_dir(k) * k->step;
            if (v >= k->limit && !kn_bad(k, v, now) &&
                (tr.kind != TK_CORE0 || v + kn[KN_CORE1].cur >= 2) && (tr.kind != TK_CORE1 || v + kn[KN_CORE0].cur >= 2)) {
                k->cur = tr.to = v;
                tr.t = now;
                return;
            }
            tr_end(0, 0, now);
            return;
        }
        tr.phase = PH_SETTLE;
        tr.t = now;
        return;
    }
    if (tr.phase == PH_SETTLE) {
        if (now - tr.t >= (uint64_t)tr.settle) {
            tr.phase = PH_MEASURE;
            tr.t = now;
        }
        return;
    }
    if (now - tr.t < (uint64_t)tr.meas) return;
    int keep = 1;
    if (tr.mode != TR_HOLD) {
        pstat ts, tp;
        int cs_ = tr.bs_ok && pw_stat(1, tr.t, now, &ts) == 0 ? pw_cmp(&tr.bs, &ts) : 2;
        int cp = tr.bp_ok && pw_stat(0, tr.t, now, &tp) == 0 ? pw_cmp(&tr.bp, &tp) : 2;
        /* the battery is the whole machine but slow and coarse; the parts are exact
           but only part of it. Either one clearly down, and the other not clearly up. */
        int better = cs_ == -1 || (cp == -1 && cs_ != 1);
        int worse = cs_ == 1 || (cp == 1 && cs_ != -1);
        keep = tr.mode == TR_KEEP ? !worse : better;
    }
    /* fewer cores or a lazier CPU may keep the average but not the 1% lows */
    int held = tr.mode == TR_KEEP || !(tr.low1 > 0.0f && low1 > 0.0f && low1 < tr.low1 * 0.88f && tr.low1 - low1 > 3.0f);
    tr_end(keep && held, !held, now);
}

/* one step toward less power while the frame rate is steady: the clocks first,
   then the Windows settings in turn */
static void search(uint64_t now, const ph_fps *f, float eff, float cs_, float gs, int cmax, int bn)
{
    int sys, parts, settle, meas, la = g_auto.lower_aggr;
    pw_avail(now, &sys, &parts);
    int power = sys || parts;

    static int alt;
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
        /* far from the limit (frames to spare, or the part mostly idle): quick
           steps watched for dropped frames only */
        float busy = x == &dc ? cs_ : gs;
        int roomy = f->fps > eff * 1.08f || (busy >= 0.0f && busy < 0.6f);
        if (f->fps > eff * 1.08f) s *= 2;
        int n = dom_q(x, x->cur - s);
        if (bad_active(x, now) && n <= x->bad) n = dom_q(x, x->bad + x->step);
        if (n < x->cur) {
            last_lower = now;
            if (power && !roomy) {
                trial_times(sys, parts, 0, &settle, &meas);
                if (now - last_change < (uint64_t)(settle + meas)) return;   /* a clean reading before */
                if (tr_begin(x == &dc ? TK_CPU : TK_GPU, TR_KEEP, x->cur, n, now, f->low1, settle, meas)) {
                    x->cur = n;
                    return;
                }
            }
            lower_prev = x->cur;
            lower_dom = x;
            lower_t = now;
            x->cur = n;
            return;
        }
    }
    if (lower_dom) return;          /* a quick clock step is still being watched */

    for (int j = 0; j < KN_N; j++) {
        int i = (rr + j) % KN_N;
        knob *k = &kn[i];
        if (!k->on || now < k->next_t) continue;
        /* a CPU with no time to spare cannot run on fewer cores or clock up more
           lazily; anything short of that the trial itself finds out, as the clocks
           may make up for it */
        if (i == KN_EPP ? !power || cs_ >= 0.95f : cs_ >= 0.97f) continue;
        /* without a power reading, fewer cores only where the GPU is clearly the limit
           and the CPU mostly idles */
        int mode = power ? TR_GAIN : TR_HOLD;
        if (mode == TR_HOLD && !(bn == 2 && gs >= 0.9f && cmax >= 0 && cmax < 60)) continue;
        int to = kn_target(k, now);
        if (to == k->cur) continue;
        if (i != KN_EPP && kn[KN_CORE0].cur + kn[KN_CORE1].cur - (k->cur - to) < 2) continue;
        trial_times(sys, parts, 1, &settle, &meas);
        if (now - last_change < (uint64_t)(settle + meas)) return;
        if (!tr_begin(TK_CORE0 + i, mode, k->cur, to, now, f->low1, settle, meas)) return;
        k->cur = to;
        rr = i + 1;
        last_lower = now;
        return;
    }
}

/* frames fall short with fewer cores or a lazier EPP: back toward all cores and
   EPP 0, all the way if it is far off or the clocks are already at the top */
static void kn_back(int all, uint64_t now)
{
    for (int i = 0; i < KN_N; i++) {
        knob *k = &kn[i];
        if (k->cur == k->home) continue;
        kn_mark_bad(k, k->cur, now);
        k->cur = all ? k->home : k->cur - kn_dir(k) * k->step;
    }
}

/* ---------- worker ---------- */

static void publish(uint64_t now, int target, float fps, int cmax, float gs, int bn)
{
    int src, cores_max = g_auto.manage_cores ? kn[KN_CORE0].n + kn[KN_CORE1].n : 0;
    int mw = pw_now(now, &src);
    LK();
    st.cpu_mhz = dc.d ? dc.cur : 0;
    st.gpu_mhz = dg.d ? dg.cur : 0;
    st.target = target;
    st.fps = fps;
    st.cpu_util = cmax;
    st.gpu_util = gs >= 0.0f ? (int)(gs * 100.0f + 0.5f) : -1;
    st.bottleneck = bn;
    st.power_mw = mw;
    st.power_src = src;
    st.cores_max = kn[KN_CORE0].on || kn[KN_CORE1].on ? cores_max : 0;
    st.cores = kn[KN_CORE0].cur + kn[KN_CORE1].cur;
    st.cores_on = act[0] >= 0 ? act[0] + act[1] : -1;
    st.epp = kn[KN_EPP].applied;
    st.trial = tr.kind;
    UL();
}

static DWORD WINAPI worker(LPVOID p)
{
    (void)p;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    ph_fps f;
    uint64_t now = ph_ms(), last_tick = now, last_frame_t = 0, last_raise = 0;
    uint64_t stable_since = now, last_util = 0, last_refresh_q = 0, last_fps_start = now, last_reapply = now;
    uint64_t below_since = 0, cap_t = 0;
    double top_sum = 0.0;
    int top_n = 0;
    uint32_t last_frames = 0;
    float ff = 0.0f, gs = -1.0f, cs_ = -1.0f, cap_target = 0.0f;
    int refresh = 60, cmax = -1, cavg = -1, gutil = -1, below_n = 0, idle = 1, last_target = 0;
    int bn = 0, probe_ms = CAP_PROBE_MS;
    uint64_t idle_t = 0, game_t = 0;
    float last_cap = 0.0f;
    DWORD last_pid = 0;
    DWORD wait = 50;

    while (WaitForSingleObject(stop_ev, wait) == WAIT_TIMEOUT) {
        now = ph_ms();
        int force = now - last_tick > 1500 || now - last_reapply > 15000;
        if (force) last_reapply = now;
        last_tick = now;
        sync_cfg();
        kn_sync();

        ph_clk *g = ph_gpu_clk();
        if (!dom_usable(g)) g = NULL;
        if (g != dg.d) {
            tr_cancel();
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
        if (target != last_target) {
            cap_target = 0.0f;
            last_cap = 0.0f;
            probe_ms = CAP_PROBE_MS;
            if (last_target) { tr_cancel(); kn_reset(); }
            last_target = target;
        }

        int ok = fps_sample(&f);
        if (!ok && now - last_fps_start > 10000) { fps_start(); last_fps_start = now; }
        if (ok && f.pid != last_pid) {
            last_pid = f.pid;
            cap_target = last_cap = 0.0f;
            probe_ms = CAP_PROBE_MS;
            dc.bad = dg.bad = dc.bad_n = dg.bad_n = 0;
            tr_cancel();
            kn_reset();
        }
        if (ok && f.frames != last_frames) { last_frames = f.frames; last_frame_t = now; }
        int game = ok && f.fps > 1.0f && last_frame_t && now - last_frame_t < 1500;

        if (!game) {
            if (!idle) {
                idle = 1;
                idle_t = now;
                ff = 0.0f;
                cap_target = 0.0f;
                tr_cancel();
                for (int i = 0; i < KN_N; i++) kn[i].resume = kn[i].cur;
            }
            if (dc.d) dc.cur = dc.ceil;
            if (dg.d) dg.cur = dg.ceil;
            dom_apply(&dc, force);
            dom_apply(&dg, force);
            kn_apply(0);
            publish(now, target, 0.0f, -1, -1.0f, 0);
            wait = 250;
            continue;
        }
        if (idle) {
            idle = 0;
            game_t = now;
            stable_since = now;
            below_n = 0;
            /* the same game back within a minute (a pause, alt-tab): where the search was */
            if (idle_t && now - idle_t < 60000)
                for (int i = 0; i < KN_N; i++)
                    if (kn[i].on) kn[i].cur = kn[i].resume;
        }
        wait = 50;
        /* only with a game: every reading wakes the SMU, the GPU or the EC */
        pw_sample(now);

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
            if (g_auto.manage_cores) act_sample();
        }
        float ft = f.frametime_ms > 0.0f ? f.frametime_ms : 1000.0f / f.fps;
        gs = f.gpu_busy_ms > 0.0f && ft > 0.0f ? f.gpu_busy_ms / ft : gutil >= 0 ? (float)gutil / 100.0f : -1.0f;
        cs_ = f.cpu_busy_ms > 0.0f && ft > 0.0f ? f.cpu_busy_ms / ft : -1.0f;
        if (cmax >= 0 && (float)cmax / 100.0f > cs_) cs_ = (float)cmax / 100.0f;
        if (gs > 1.0f) gs = 1.0f;
        if (cs_ > 1.0f) cs_ = 1.0f;
        bn = gs >= 0.88f && gs >= cs_ ? 2 : cs_ >= 0.88f ? 1 : 0;

        float eff = cap_target > 0.0f ? cap_target : (float)target;
        if (cap_target > 0.0f && ff > cap_target * 1.10f) {
            cap_target = 0.0f;
            last_cap = 0.0f;
            probe_ms = CAP_PROBE_MS;
            eff = (float)target;
        }
        if (cap_target > 0.0f && now - cap_t > (uint64_t)probe_ms) { cap_target = 0.0f; eff = (float)target; }
        float thr_lo = eff * (1.0f - (float)g_auto.tolerance_pct / 100.0f);
        int ra = g_auto.raise_aggr;
        float big = 0.30f - 0.04f * (float)ra;

        /* the frame rate climbs back from nothing when frames start again: not a shortfall */
        int below = ff < thr_lo && now - game_t >= 2000;
        if (below) { below_n++; if (!below_since) below_since = now; }
        else { below_n = 0; below_since = 0; }
        float d = below ? (eff - ff) / eff : 0.0f;

        int clk_ceil = (!dc.d || dc.cur >= dc.ceil) && (!dg.d || dg.cur >= dg.ceil);
        /* the target is out of reach only with everything at full speed */
        int at_ceil = clk_ceil && kn_home();
        if (below && at_ceil) { top_sum += ff; top_n++; }
        else { top_sum = 0.0; top_n = 0; }
        if (below && at_ceil && cap_target <= 0.0f && below_since && now - below_since > 4000 && f.fps > 5.0f && top_n) {
            /* hold what it reaches at full speed, a little under so that the scene's
               own ups and downs do not count as falling short, with the least power */
            cap_target = (float)(top_sum / top_n) * 0.98f;
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

        int cooldown = 400 - 60 * ra;
        if (below && (below_n >= 2 || d >= big) && now - last_raise >= (uint64_t)cooldown) {
            int handled = 0;
            if (tr.kind == TK_CPU || tr.kind == TK_GPU) {
                /* the clock step being measured cost frames */
                dom *x = tr_dom();
                if (d < big) {
                    dom_mark_bad(x, tr.to, now);
                    x->cur = dom_q(x, tr.from + x->step);
                    handled = 1;
                }
                tr.kind = TK_NONE;
            } else if (tr.kind != TK_NONE) {
                /* fewer cores or a higher EPP: the clocks may make up for it, a
                   little and a few times, when the power can tell whether that
                   still pays; a big drop, or no room left, ends it */
                if (tr.mode == TR_HOLD || d >= big || tr.raises >= 3 || clk_ceil) {
                    tr_end(0, 1, now);
                    handled = 1;
                } else {
                    tr.raises++;
                    tr.phase = PH_SETTLE;
                    tr.t = now;
                }
            } else if (lower_dom && now - lower_t < FAIL_MS && d < big) {
                dom_mark_bad(lower_dom, lower_dom->cur, now);
                lower_dom->cur = dom_q(lower_dom, lower_prev + lower_dom->step);
                handled = 1;
            }
            lower_dom = NULL;
            if (!handled && tr.kind == TK_NONE && !kn_home() && (bn != 2 || clk_ceil)) kn_back(d >= big || clk_ceil, now);
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
            int lw = IMAX(1800 - 250 * g_auto.lower_aggr, 500);
            if (tr.kind != TK_NONE) tr_step(now, f.low1);
            else if (stable && now - stable_since >= (uint64_t)g_auto.settle_ms && now - last_lower >= (uint64_t)lw)
                search(now, &f, eff, cs_, gs, cmax, bn);
        }
        if (lower_dom && now - lower_t >= FAIL_MS) lower_dom = NULL;

        dom_apply(&dc, force);
        dom_apply(&dg, force);
        kn_apply(1);
        publish(now, (int)(eff + 0.5f), f.fps, cmax, gs, bn);
    }
    return 0;
}

/* ---------- start / stop ---------- */

static void notify(int on)
{
    ctls[K_ON].val = on;
    ctls[K_ON].dirty = 1;
    if (on) ctls[K_POWER].flags &= (uint16_t)~CF_HIDDEN;
    else ctls[K_POWER].flags |= CF_HIDDEN;
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

    kn_setup();
    memset(&tr, 0, sizeof tr);
    lower_dom = NULL;
    rr = 0;
    last_lower = 0;
    last_change = ph_ms();
    pw_n = pw_i = 0;
    pw_t = sys_t = 0;
    sys_prev = -1;
    sys_upd = 0;
    act[0] = act[1] = -1;
    act_n = act_i = 0;
    act_t = 0;
    park_up = 0;
    cpu_primed = 0;
    LK();
    memset(&st, 0, sizeof st);
    st.running = 1;
    st.cpu_util = st.gpu_util = st.power_mw = st.cores_on = st.epp = -1;
    UL();
    fps_start();
    InterlockedExchange(&running, 1);
    thr = CreateThread(NULL, 64 * 1024, worker, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!thr) {
        InterlockedExchange(&running, 0);
        LK();
        st.running = 0;
        UL();
        return -1;
    }
    ph_log("autotdp: started (cpu %ls, gpu %ls, cores %d+%d, epp mode %d)", dc.d ? dc.d->name : L"-",
           dg.d ? dg.d->name : L"-", kn[KN_CORE1].n, kn[KN_CORE0].n, g_auto.epp_mode);
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
    kn_restore();
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
    kn_restore();
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
    /* by name too, so another domain added later does not shift the choice; a name
       that cannot be stored must not leave an older one behind */
    char name[256];
    int ok = nw && nw->name && WideCharToMultiByte(CP_UTF8, 0, nw->name, -1, name, sizeof name, NULL, NULL);
    cfg_set_str("global", GPUDOM_NAME, ok ? name : NULL);
    return 0;
}

void autotdp_cfg_loaded(void)
{
    ph_ctl *c = &ctls[K_GPUDOM];
    char name[256];
    wchar_t w[128];
    if (!c->active || !cfg_get_str("global", GPUDOM_NAME, name, sizeof name) || !name[0] ||
        !MultiByteToWideChar(CP_UTF8, 0, name, -1, w, PH_ARRAY(w)))
        return;
    /* the saved index still names it (two domains can share a name) */
    ph_clk *at = ph_gpu_clk_at(c->val);
    if (at && at->name && !lstrcmpW(at->name, w)) return;
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

static void cat(wchar_t *b, int n, const wchar_t *s)
{
    int k = lstrlenW(b);
    if (k && k < n - 1) { lstrcpynW(b + k, L" \x00B7 ", n - k); k = lstrlenW(b); }
    if (k < n - 1) lstrcpynW(b + k, s, n - k);
}

/* what it draws, and what it has set: "9.8 W system · 6/8 cores · EPP 20 · trying cores" */
static void fmt_power(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c; (void)v;
    static const wchar_t *const what[] = { L"", L"CPU clock", L"GPU clock", L"cores", L"P-cores", L"EPP" };
    ph_auto_state s;
    wchar_t t[48];
    autotdp_state(&s);
    if (n < 1) return;
    b[0] = 0;
    if (!s.running || s.fps <= 0.0f) { lstrcpynW(b, L"--", n); return; }
    if (s.power_mw >= 0) {
        ph_swprintf(t, PH_ARRAY(t), L"%d.%d W%s", s.power_mw / 1000, s.power_mw % 1000 / 100,
                    s.power_src == PSRC_SYSTEM ? L" system" : L" CPU+GPU");
        cat(b, n, t);
    }
    if (s.cores_max > 0) {
        ph_swprintf(t, PH_ARRAY(t), L"%d/%d cores", s.cores_on >= 0 ? s.cores_on : s.cores, s.cores_max);
        cat(b, n, t);
    }
    if (s.epp >= 0) {
        ph_swprintf(t, PH_ARRAY(t), L"EPP %d", s.epp);
        cat(b, n, t);
    }
    if (s.fps > 0.0f && s.trial > 0 && s.trial < PH_ARRAY(what)) {
        ph_swprintf(t, PH_ARRAY(t), L"trying %s", s.trial == TK_CORE0 && g_plat.hybrid ? L"E-cores" : what[s.trial]);
        cat(b, n, t);
    }
    if (!b[0]) lstrcpynW(b, L"No power reading", n);
}

static const wchar_t *const target_ch[] = { L"Refresh rate", L"Half refresh", L"Custom", NULL };
static const wchar_t *const epp_ch[] = { L"Windows", L"0 (fastest)", L"Tune for power", NULL };
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
    [K_POWER] = { .key = NULL, .label = L"Power use", .type = CT_INFO, .page = PG_QUICK, .order = 5,
                  .flags = CF_HIDDEN, .fmt = fmt_power },
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
    [K_CORES] = { .key = "auto.cores", .label = L"Find the fewest cores", .type = CT_TOGGLE, .page = PG_CPU,
                  .order = 409, .flags = TUNE, .def = 1, .fmt = fmt_onoff, .set = set_store },
    [K_EPP] = { .key = "auto.epp0", .label = L"EPP while active", .type = CT_CHOICE, .page = PG_CPU,
                .order = 410, .flags = CF_ADVANCED, .def = EPP_TUNE, .choices = epp_ch, .set = set_store },
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
    /* with two cores or fewer there is nothing to park */
    if (g_plat.ncores < 3) ctls[K_CORES].flags |= CF_HIDDEN;
    if (!g_plat.epp) ctls[K_EPP].flags |= CF_HIDDEN;
    ctls[K_ON].flags &= (uint16_t)~CF_HIDDEN;
    ctls[K_TARGET].flags &= (uint16_t)~CF_HIDDEN;
    ctls[K_STATUS].flags &= (uint16_t)~CF_HIDDEN;
    ph_register_ctls(ctls, K_COUNT);
}
