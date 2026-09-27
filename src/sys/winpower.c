#include "phawx.h"
#include <powrprof.h>

#define DG(n, a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) \
    static const GUID n = { a, b, c, { d0, d1, d2, d3, d4, d5, d6, d7 } }

const GUID GUID_SUB_PROCESSOR_ = { 0x54533251, 0x82be, 0x4824, { 0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00 } };
const GUID GUID_SUB_NONE_      = { 0xfea3413e, 0x7e05, 0x4911, { 0x9a, 0x71, 0x70, 0x03, 0x31, 0xf1, 0xc2, 0x94 } };

DG(G_PERFEPP,        0x36687f9e, 0xe3a5, 0x4dbf, 0xb1, 0xdc, 0x15, 0xeb, 0x38, 0x1c, 0x68, 0x63);
DG(G_PERFEPP1,       0x36687f9e, 0xe3a5, 0x4dbf, 0xb1, 0xdc, 0x15, 0xeb, 0x38, 0x1c, 0x68, 0x64);
DG(G_BOOSTMODE,      0xbe337238, 0x0d82, 0x4146, 0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7);
DG(G_BOOSTPOL,       0x45bcc044, 0xd885, 0x43e2, 0x86, 0x05, 0xee, 0x0e, 0xc6, 0xe9, 0x6b, 0x59);
DG(G_FREQMAX,        0x75b0ae3f, 0xbce0, 0x45a7, 0x8c, 0x89, 0xc9, 0x61, 0x1c, 0x25, 0xe1, 0x00);
DG(G_FREQMAX1,       0x75b0ae3f, 0xbce0, 0x45a7, 0x8c, 0x89, 0xc9, 0x61, 0x1c, 0x25, 0xe1, 0x01);
DG(G_THRMAX,         0xbc5038f7, 0x23e0, 0x4960, 0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xec);
DG(G_THRMAX1,        0xbc5038f7, 0x23e0, 0x4960, 0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xed);
DG(G_THRMIN,         0x893dee8e, 0x2bef, 0x41e0, 0x89, 0xc6, 0xb5, 0x5d, 0x09, 0x29, 0x96, 0x4c);
DG(G_THRMIN1,        0x893dee8e, 0x2bef, 0x41e0, 0x89, 0xc6, 0xb5, 0x5d, 0x09, 0x29, 0x96, 0x4d);
DG(G_CPMIN,          0x0cc5b647, 0xc1df, 0x4637, 0x89, 0x1a, 0xde, 0xc3, 0x5c, 0x31, 0x85, 0x83);
DG(G_CPMIN1,         0x0cc5b647, 0xc1df, 0x4637, 0x89, 0x1a, 0xde, 0xc3, 0x5c, 0x31, 0x85, 0x84);
DG(G_CPMAX,          0xea062031, 0x0e34, 0x4ff1, 0x9b, 0x6d, 0xeb, 0x10, 0x59, 0x33, 0x40, 0x28);
DG(G_CPMAX1,         0xea062031, 0x0e34, 0x4ff1, 0x9b, 0x6d, 0xeb, 0x10, 0x59, 0x33, 0x40, 0x29);
DG(G_SCHED,          0x93b8b6dc, 0x0698, 0x4d1c, 0x9e, 0xe4, 0x06, 0x44, 0xe9, 0x00, 0xc8, 0x5d);
DG(G_SHORTSCHED,     0xbae08b81, 0x2d5e, 0x4688, 0xad, 0x6a, 0x13, 0x24, 0x33, 0x56, 0x65, 0x4b);
DG(G_HETEROPOL,      0x7f2f5cfa, 0xf10c, 0x4823, 0xb5, 0xe1, 0xe9, 0x3a, 0xe8, 0x5f, 0x46, 0xb5);
DG(G_HETINCTIME,     0x4009efa7, 0xe72d, 0x4cba, 0x9e, 0xdf, 0x91, 0x08, 0x4e, 0xa8, 0xcb, 0xc3);
DG(G_HETDECTIME,     0x7f2492b6, 0x60b1, 0x45e5, 0xae, 0x55, 0x77, 0x3f, 0x8c, 0xd5, 0xca, 0xec);
DG(G_HETC1INIT,      0x1facfc65, 0xa930, 0x4bc5, 0x9f, 0x38, 0x50, 0x4e, 0xc0, 0x97, 0xbb, 0xc0);
DG(G_HETC0FLOOR,     0xfddc842b, 0x8364, 0x4edc, 0x94, 0xcf, 0xc1, 0x7f, 0x60, 0xde, 0x1c, 0x80);
DG(G_AUTONOMOUS,     0x8baa4a8a, 0x14c6, 0x4451, 0x8e, 0x8b, 0x14, 0xbd, 0xbd, 0x19, 0x75, 0x37);
DG(G_AUTOWINDOW,     0xcfeda3d0, 0x7697, 0x4566, 0xa9, 0x22, 0xa9, 0x08, 0x6c, 0xd4, 0x9d, 0xfa);
DG(G_DUTYCYCLING,    0x4e4450b3, 0x6179, 0x4e91, 0xb8, 0xf1, 0x5b, 0xb9, 0x93, 0x8f, 0x81, 0xa1);
DG(G_LATPERF,        0x619b7505, 0x003b, 0x4e82, 0xb7, 0xa6, 0x4d, 0xd2, 0x9c, 0x30, 0x09, 0x71);
DG(G_LATPERF1,       0x619b7505, 0x003b, 0x4e82, 0xb7, 0xa6, 0x4d, 0xd2, 0x9c, 0x30, 0x09, 0x72);
DG(G_LATUNPARK,      0x616cdaa5, 0x695e, 0x4545, 0x97, 0xad, 0x97, 0xdc, 0x2d, 0x1b, 0xdd, 0x88);
DG(G_LATUNPARK1,     0x616cdaa5, 0x695e, 0x4545, 0x97, 0xad, 0x97, 0xdc, 0x2d, 0x1b, 0xdd, 0x89);
DG(G_PERFCHECK,      0x4d2b0152, 0x7d5c, 0x498b, 0x88, 0xe2, 0x34, 0x34, 0x53, 0x92, 0xa2, 0xc5);

DG(OV_BEST_EFF,      0x961cc777, 0x2547, 0x4f9d, 0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a);
DG(OV_BALANCED,      0x00000000, 0x0000, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
DG(OV_BEST_PERF,     0xded574b5, 0x45a0, 0x4f42, 0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38);

#define SUBP (&GUID_SUB_PROCESSOR_)
#define COMMIT_GAP_MS 250
#define MAX_TOUCH 64

typedef DWORD (WINAPI *pfn_set_ov)(GUID);
typedef DWORD (WINAPI *pfn_get_ov)(GUID *);

typedef struct {
    GUID scheme, sub, set;
    DWORD ac, dc;
    ph_ctl *owner;
    uint8_t mod;
} wtouch;

static wtouch tt[MAX_TOUCH];
static int ntt;
static CRITICAL_SECTION wl;
static INIT_ONCE wl_once = INIT_ONCE_STATIC_INIT;
static PTP_TIMER ctimer;
static int armed, pending;
static uint64_t last_commit;

#define JKEY L"Software\\PhawxON"
#define JVAL L"PowerRestore"
#define JOV  L"OverlayRestore"

typedef struct { GUID scheme, sub, set; DWORD ac, dc; } jrec;
static int jdirty;

static pfn_set_ov p_set_ov;
static pfn_get_ov p_get_ov;
static int ov_resolved, ov_saved, ov_mod;
static GUID ov_orig;
static ph_ctl *ov_owner;

static BOOL CALLBACK lock_init(PINIT_ONCE o, PVOID p, PVOID *ctx)
{
    (void)o; (void)p; (void)ctx;
    InitializeCriticalSection(&wl);
    return TRUE;
}

static void LK(void) { InitOnceExecuteOnce(&wl_once, lock_init, NULL, NULL); EnterCriticalSection(&wl); }
static void UL(void) { LeaveCriticalSection(&wl); }

static int active_scheme(GUID *g)
{
    GUID *p = NULL;
    if (PowerGetActiveScheme(NULL, &p) != ERROR_SUCCESS || !p) return -1;
    *g = *p;
    LocalFree(p);
    return 0;
}

static int read_pair(const GUID *sch, const GUID *sub, const GUID *set, DWORD *ac, DWORD *dc)
{
    if (PowerReadACValueIndex(NULL, sch, sub, set, ac) != ERROR_SUCCESS) return -1;
    if (PowerReadDCValueIndex(NULL, sch, sub, set, dc) != ERROR_SUCCESS) *dc = *ac;
    return 0;
}

static int write_pair(const GUID *sch, const GUID *sub, const GUID *set, DWORD ac, DWORD dc)
{
    if (PowerWriteACValueIndex(NULL, sch, sub, set, ac) != ERROR_SUCCESS) return -1;
    if (PowerWriteDCValueIndex(NULL, sch, sub, set, dc) != ERROR_SUCCESS) return -1;
    return 0;
}

static void journal_sync(void)
{
    if (!jdirty) return;
    jdirty = 0;
    jrec r[MAX_TOUCH];
    int n = 0;
    for (int i = 0; i < ntt; i++) {
        if (!tt[i].mod) continue;
        r[n].scheme = tt[i].scheme;
        r[n].sub = tt[i].sub;
        r[n].set = tt[i].set;
        r[n].ac = tt[i].ac;
        r[n].dc = tt[i].dc;
        n++;
    }
    if (n) reg_set_bin(HKEY_CURRENT_USER, JKEY, JVAL, r, (DWORD)(n * sizeof r[0]));
    else reg_del_value(HKEY_CURRENT_USER, JKEY, JVAL);
    if (ov_mod && ov_saved) reg_set_bin(HKEY_CURRENT_USER, JKEY, JOV, &ov_orig, sizeof ov_orig);
    else reg_del_value(HKEY_CURRENT_USER, JKEY, JOV);
}

static wtouch *find_touch(const GUID *sch, const GUID *sub, const GUID *set)
{
    for (int i = 0; i < ntt; i++)
        if (IsEqualGUID(&tt[i].scheme, sch) && IsEqualGUID(&tt[i].sub, sub) && IsEqualGUID(&tt[i].set, set))
            return &tt[i];
    return NULL;
}

static void commit_locked(void);

static VOID CALLBACK timer_cb(PTP_CALLBACK_INSTANCE inst, PVOID ctx, PTP_TIMER t)
{
    (void)inst; (void)ctx; (void)t;
    LK();
    armed = 0;
    commit_locked();
    UL();
}

static void arm_commit(void)
{
    if (armed) return;
    if (!ctimer) ctimer = CreateThreadpoolTimer(timer_cb, NULL, NULL);
    if (!ctimer) { commit_locked(); return; }
    uint64_t since = ph_ms() - last_commit;
    LONGLONG d = since >= COMMIT_GAP_MS ? 30 : (LONGLONG)(COMMIT_GAP_MS - since);
    if (d < 30) d = 30;
    ULARGE_INTEGER u;
    u.QuadPart = (ULONGLONG)(-(d * 10000));
    FILETIME ft = { u.LowPart, u.HighPart };
    armed = 1;
    SetThreadpoolTimer(ctimer, &ft, 0, 0);
}

static int write_ex(const GUID *sub, const GUID *set, DWORD ac, DWORD dc, ph_ctl *owner)
{
    GUID sch;
    DWORD cac, cdc;
    int r = -1;
    LK();
    if (active_scheme(&sch) || read_pair(&sch, sub, set, &cac, &cdc)) goto out;
    wtouch *t = find_touch(&sch, sub, set);
    if (!t) {
        if (ntt >= MAX_TOUCH) goto out;
        t = &tt[ntt++];
        t->scheme = sch;
        t->sub = *sub;
        t->set = *set;
        t->mod = 0;
    }
    if (!t->mod) {
        t->ac = cac;
        t->dc = cdc;
    }
    if (cac != ac || cdc != dc) {
        if (!t->mod) {
            t->mod = 1;
            jdirty = 1;
            journal_sync();
        }
        if (write_pair(&sch, sub, set, ac, dc)) goto out;
        pending = 1;
        arm_commit();
    }
    t->owner = owner;
    r = 0;
out:
    UL();
    return r;
}

static void restore_touch(wtouch *t, const GUID *act)
{
    if (!t->mod) return;
    write_pair(&t->scheme, &t->sub, &t->set, t->ac, t->dc);
    t->mod = 0;
    t->owner = NULL;
    jdirty = 1;
    if (act && IsEqualGUID(&t->scheme, act)) pending = 1;
}

static void resolve_ov(void)
{
    if (ov_resolved) return;
    ov_resolved = 1;
    HMODULE m = GetModuleHandleW(L"powrprof.dll");
    if (!m) m = LoadLibraryExW(L"powrprof.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return;
    p_set_ov = (pfn_set_ov)(void *)GetProcAddress(m, "PowerSetActiveOverlayScheme");
    p_get_ov = (pfn_get_ov)(void *)GetProcAddress(m, "PowerGetEffectiveOverlayScheme");
    if (!p_get_ov) p_get_ov = (pfn_get_ov)(void *)GetProcAddress(m, "PowerGetActualOverlayScheme");
}

static int deactivated(ph_ctl *o) { return o && !o->active; }

static void restore_deactivated(void)
{
    GUID act;
    int has = active_scheme(&act) == 0;
    for (int i = 0; i < ntt; i++)
        if (tt[i].mod && deactivated(tt[i].owner)) restore_touch(&tt[i], has ? &act : NULL);
    if (ov_mod && deactivated(ov_owner)) {
        if (ov_saved && p_set_ov) p_set_ov(ov_orig);
        ov_mod = 0;
        ov_owner = NULL;
        jdirty = 1;
    }
    journal_sync();
}

static void commit_locked(void)
{
    restore_deactivated();
    if (!pending) return;
    GUID sch;
    if (active_scheme(&sch) == 0) PowerSetActiveScheme(NULL, &sch);
    pending = 0;
    last_commit = ph_ms();
}

int wp_read(const GUID *sub, const GUID *set, int dc, DWORD *v)
{
    GUID sch;
    if (active_scheme(&sch)) return -1;
    DWORD r = dc ? PowerReadDCValueIndex(NULL, &sch, sub, set, v) : PowerReadACValueIndex(NULL, &sch, sub, set, v);
    return r == ERROR_SUCCESS ? 0 : -1;
}

int wp_write(const GUID *sub, const GUID *set, DWORD ac, DWORD dc) { return write_ex(sub, set, ac, dc, NULL); }
int wp_write_both(const GUID *sub, const GUID *set, DWORD v) { return write_ex(sub, set, v, v, NULL); }

void wp_commit(void)
{
    LK();
    commit_locked();
    UL();
}

static int overlay_ex(const GUID *ov, ph_ctl *owner)
{
    resolve_ov();
    if (!p_set_ov || !ov) return -1;
    LK();
    if (!ov_saved && p_get_ov && p_get_ov(&ov_orig) == ERROR_SUCCESS) ov_saved = 1;
    if (!ov_mod && ov_saved) {
        ov_mod = 1;
        jdirty = 1;
        journal_sync();
    }
    DWORD r = p_set_ov(*ov);
    if (r == ERROR_SUCCESS) { ov_mod = 1; ov_owner = owner; }
    UL();
    return r == ERROR_SUCCESS ? 0 : -1;
}

int wp_set_overlay(const GUID *ov) { return overlay_ex(ov, NULL); }

int wp_set_epp(int cls, int pct)
{
    DWORD v = (DWORD)PH_CLAMP(pct, 0, 100);
    int r = 0;
    if (cls != 1) r |= wp_write_both(SUBP, &G_PERFEPP, v);
    if (cls != 0) r |= wp_write_both(SUBP, &G_PERFEPP1, v);
    return r ? -1 : 0;
}

int wp_get_epp(int cls)
{
    DWORD v;
    if (wp_read(SUBP, cls == 1 ? &G_PERFEPP1 : &G_PERFEPP, !g_plat.on_ac, &v)) return -1;
    return (int)v;
}

int wp_set_cores(int cls, int min_pct, int max_pct)
{
    const GUID *gmin = cls == 1 ? &G_CPMIN1 : &G_CPMIN, *gmax = cls == 1 ? &G_CPMAX1 : &G_CPMAX;
    int r = 0;
    if (max_pct >= 0) r |= wp_write_both(SUBP, gmax, (DWORD)PH_CLAMP(max_pct, 0, 100));
    if (min_pct >= 0) {
        if (max_pct >= 0 && min_pct > max_pct) min_pct = max_pct;
        r |= wp_write_both(SUBP, gmin, (DWORD)PH_CLAMP(min_pct, 0, 100));
    }
    return r ? -1 : 0;
}

int wp_set_freq_cap(int cls, int mhz)
{
    DWORD v = mhz > 0 ? (DWORD)mhz : 0;
    int r = 0;
    if (cls != 1) r |= wp_write_both(SUBP, &G_FREQMAX, v);
    if (cls != 0) r |= wp_write_both(SUBP, &G_FREQMAX1, v);
    return r ? -1 : 0;
}

/* ---------- controls ---------- */

typedef struct { const GUID *g0, *g1; } wpm;

static wpm M_EPP = { &G_PERFEPP, &G_PERFEPP1 }, M_EPP0 = { &G_PERFEPP, NULL }, M_EPP1 = { &G_PERFEPP1, NULL };
static wpm M_BOOST = { &G_BOOSTMODE, NULL }, M_BOOSTPOL = { &G_BOOSTPOL, NULL };
static wpm M_FREQ0 = { &G_FREQMAX, NULL }, M_FREQ1 = { &G_FREQMAX1, NULL };
static wpm M_THRMIN = { &G_THRMIN, &G_THRMIN1 }, M_THRMAX = { &G_THRMAX, &G_THRMAX1 };
static wpm M_CPMAX0 = { &G_CPMAX, NULL }, M_CPMIN0 = { &G_CPMIN, NULL };
static wpm M_CPMAX1 = { &G_CPMAX1, NULL }, M_CPMIN1 = { &G_CPMIN1, NULL };
static wpm M_SCHED = { &G_SCHED, NULL }, M_SHORT = { &G_SHORTSCHED, NULL }, M_HPOL = { &G_HETEROPOL, NULL };
static wpm M_HINC = { &G_HETINCTIME, NULL }, M_HDEC = { &G_HETDECTIME, NULL };
static wpm M_HC1 = { &G_HETC1INIT, NULL }, M_HC0 = { &G_HETC0FLOOR, NULL };
static wpm M_AUTO = { &G_AUTONOMOUS, NULL }, M_AWIN = { &G_AUTOWINDOW, NULL }, M_DUTY = { &G_DUTYCYCLING, NULL };
static wpm M_LATP = { &G_LATPERF, &G_LATPERF1 }, M_LATU = { &G_LATUNPARK, &G_LATUNPARK1 };
static wpm M_PCHK = { &G_PERFCHECK, NULL };

enum { A_HYB = 1, A_EPP = 2, A_OV = 4 };

static int gen_set(ph_ctl *c, int32_t v)
{
    const wpm *m = c->ctx;
    DWORD d = (DWORD)(v < 0 ? 0 : v);
    if (write_ex(SUBP, m->g0, d, d, c)) return -1;
    if (m->g1) write_ex(SUBP, m->g1, d, d, c);
    return 0;
}

static int gen_get(ph_ctl *c, int32_t *out)
{
    const wpm *m = c->ctx;
    DWORD v;
    if (wp_read(SUBP, m->g0, !g_plat.on_ac, &v)) return -1;
    *out = (int32_t)v;
    return 0;
}

static const GUID *const ov_list[] = { &OV_BEST_EFF, &OV_BALANCED, &OV_BEST_PERF };

static int mode_set(ph_ctl *c, int32_t v)
{
    if (v < 0 || v >= PH_ARRAY(ov_list)) return -1;
    return overlay_ex(ov_list[v], c);
}

static int mode_get(ph_ctl *c, int32_t *out)
{
    (void)c;
    GUID g;
    resolve_ov();
    if (!p_get_ov || p_get_ov(&g) != ERROR_SUCCESS) return -1;
    for (int i = 0; i < PH_ARRAY(ov_list); i++)
        if (IsEqualGUID(&g, ov_list[i])) { *out = i; return 0; }
    *out = 1;
    return 0;
}

static void fmt_us(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c;
    if (v <= 0) lstrcpynW(b, L"Auto", n);
    else if (v % 1000) ph_swprintf(b, n, L"%d us", v);
    else ph_swprintf(b, n, L"%d ms", v / 1000);
}

static void fmt_ms(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c;
    ph_swprintf(b, n, L"%d ms", v);
}

static void fmt_checks(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c;
    ph_swprintf(b, n, L"%d checks", v);
}

static const wchar_t *const boost_ch[] = {
    L"Disabled", L"Enabled", L"Aggressive", L"Efficient enabled", L"Efficient aggressive",
    L"Aggressive at guaranteed", L"Efficient aggr. at guar.", NULL
};
static const wchar_t *const sched_ch[] = {
    L"All cores", L"Performant only", L"Prefer performant", L"Efficient only", L"Prefer efficient",
    L"Automatic", NULL
};
static const wchar_t *const mode_ch[] = { L"Best efficiency", L"Balanced", L"Best performance", NULL };

#define OPT   (CF_OPTIONAL | CF_PROFILE)
#define OPTA  (CF_OPTIONAL | CF_PROFILE | CF_AUTOTDP)
#define ADV   (CF_OPTIONAL | CF_ADVANCED)
#define SL(k, l, pg, o, fl, lo, hi, st, dflt, u, f, m, a) \
    { .key = k, .label = l, .type = CT_SLIDER, .page = pg, .order = o, .flags = fl, .min = lo, .max = hi, \
      .step = st, .def = dflt, .unit = u, .fmt = f, .get = gen_get, .set = gen_set, .ctx = &m, .arg = a }
#define CH(k, l, pg, o, fl, ch, dflt, m, a) \
    { .key = k, .label = l, .type = CT_CHOICE, .page = pg, .order = o, .flags = fl, .choices = ch, .def = dflt, \
      .get = gen_get, .set = gen_set, .ctx = &m, .arg = a }
#define TG(k, l, pg, o, fl, dflt, m, a) \
    { .key = k, .label = l, .type = CT_TOGGLE, .page = pg, .order = o, .flags = fl, .def = dflt, \
      .fmt = fmt_onoff, .get = gen_get, .set = gen_set, .ctx = &m, .arg = a }
#define HD(l, pg, o, fl, a) { .key = NULL, .label = l, .type = CT_HEADER, .page = pg, .order = o, .flags = fl, .arg = a }

enum { I_FREQ0 = 3, I_FREQ1 = 4 };

static ph_ctl ctls[] = {
    SL("power.epp", L"Energy preference (EPP)", PG_QUICK, 50, OPTA, 0, 100, 5, 33, L"%", fmt_pct, M_EPP, A_EPP),
    CH("power.boost", L"Boost mode", PG_QUICK, 70, OPT, boost_ch, 2, M_BOOST, 0),
    { .key = "power.mode", .label = L"Power mode", .type = CT_CHOICE, .page = PG_QUICK, .order = 80, .flags = OPT,
      .choices = mode_ch, .def = 1, .get = mode_get, .set = mode_set, .arg = A_OV },

    SL("cpu.freqcap", L"Max clock (Windows)", PG_CPU, 302, OPTA, 0, 6000, 100, 0, L"MHz", fmt_mhz, M_FREQ0, 0),
    SL("cpu.freqcap_p", L"P-core max clock", PG_CPU, 301, OPTA, 0, 6000, 100, 0, L"MHz", fmt_mhz, M_FREQ1, A_HYB),

    HD(L"Windows core control", PG_CPU, 300, 0, 0),
    SL("cpu.epp_p", L"P-core EPP", PG_CPU, 303, OPTA, 0, 100, 5, 33, L"%", fmt_pct, M_EPP1, A_HYB | A_EPP),
    SL("cpu.epp_e", L"E-core EPP", PG_CPU, 304, OPTA, 0, 100, 5, 33, L"%", fmt_pct, M_EPP0, A_HYB | A_EPP),

    HD(L"Core parking", PG_CPU, 310, 0, 0),
    SL("cpu.parkmax_p", L"P-core max unparked", PG_CPU, 311, OPTA, 0, 100, 5, 100, L"%", fmt_pct, M_CPMAX1, A_HYB),
    SL("cpu.parkmin_p", L"P-core min unparked", PG_CPU, 312, OPTA, 0, 100, 5, 0, L"%", fmt_pct, M_CPMIN1, A_HYB),
    SL("cpu.parkmax", L"Max unparked cores", PG_CPU, 313, OPTA, 0, 100, 5, 100, L"%", fmt_pct, M_CPMAX0, 0),
    SL("cpu.parkmin", L"Min unparked cores", PG_CPU, 314, OPTA, 0, 100, 5, 0, L"%", fmt_pct, M_CPMIN0, 0),

    HD(L"Hybrid scheduling", PG_CPU, 320, 0, A_HYB),
    CH("cpu.sched", L"Long thread policy", PG_CPU, 321, OPT, sched_ch, 5, M_SCHED, A_HYB),
    CH("cpu.shortsched", L"Short thread policy", PG_CPU, 322, OPT, sched_ch, 5, M_SHORT, A_HYB),
    SL("cpu.heteropol", L"Hetero policy slot", PG_CPU, 323, ADV, 0, 4, 1, 0, NULL, NULL, M_HPOL, A_HYB),
    SL("cpu.het_up", L"P-core unpark delay", PG_CPU, 324, ADV, 1, 100, 1, 3, NULL, fmt_checks, M_HINC, A_HYB),
    SL("cpu.het_down", L"P-core park delay", PG_CPU, 325, ADV, 1, 100, 1, 3, NULL, fmt_checks, M_HDEC, A_HYB),
    SL("cpu.het_pinit", L"P-core initial perf", PG_CPU, 326, ADV, 0, 100, 5, 100, L"%", fmt_pct, M_HC1, A_HYB),
    SL("cpu.het_efloor", L"E-core floor perf", PG_CPU, 327, ADV, 0, 100, 5, 0, L"%", fmt_pct, M_HC0, A_HYB),

    HD(L"Processor power", PG_POWER, 100, 0, 0),
    SL("power.minstate", L"Min processor state", PG_POWER, 101, OPT, 0, 100, 5, 5, L"%", fmt_pct, M_THRMIN, 0),
    SL("power.maxstate", L"Max processor state", PG_POWER, 102, OPT, 0, 100, 5, 100, L"%", fmt_pct, M_THRMAX, 0),

    HD(L"Processor tuning", PG_POWER, 110, CF_ADVANCED, 0),
    TG("power.autonomous", L"Autonomous mode", PG_POWER, 111, ADV, 1, M_AUTO, 0),
    SL("power.autowindow", L"Autonomous window", PG_POWER, 112, ADV, 0, 100000, 1000, 0, NULL, fmt_us, M_AWIN, 0),
    TG("power.dutycycle", L"Duty cycling", PG_POWER, 113, ADV, 1, M_DUTY, 0),
    SL("power.boostpol", L"Boost policy", PG_POWER, 114, ADV, 0, 100, 5, 60, L"%", fmt_pct, M_BOOSTPOL, 0),
    SL("power.lathint_perf", L"Latency hint perf", PG_POWER, 115, ADV, 0, 100, 5, 99, L"%", fmt_pct, M_LATP, 0),
    SL("power.lathint_unpark", L"Latency hint unpark", PG_POWER, 116, ADV, 0, 100, 5, 100, L"%", fmt_pct, M_LATU, 0),
    SL("power.perfcheck", L"Perf check interval", PG_POWER, 117, ADV, 5, 100, 5, 30, NULL, fmt_ms, M_PCHK, 0),
};

/* ---------- Windows frequency cap clock domain ---------- */

static int cpu_max_mhz(void)
{
    uint32_t r[4];
    DWORD base = 0;
    cpuidex(0, 0, r);
    if (g_plat.vendor == VENDOR_INTEL && r[0] >= 0x16) {
        cpuidex(0x16, 0, r);
        if ((r[1] & 0xFFFF) >= 800) return (int)(r[1] & 0xFFFF);
        base = r[0] & 0xFFFF;
    }
    if (!base) reg_get_dword(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz", &base);
    int est = (int)base * 16 / 10;
    if (est < 5100) est = 5100;
    if (est > 6000) est = 6000;
    return (est + 99) / 100 * 100;
}

static int clk_set_max(ph_clk *d, int mhz)
{
    (void)d;
    return wp_set_freq_cap(-1, mhz);
}

static void restore_setting(const GUID *set)
{
    GUID sch;
    LK();
    if (active_scheme(&sch) == 0) {
        wtouch *t = find_touch(&sch, SUBP, set);
        if (t && t->mod) {
            restore_touch(t, &sch);
            journal_sync();
            arm_commit();
        }
    }
    UL();
}

static int clk_reset(ph_clk *d)
{
    (void)d;
    for (int i = I_FREQ0; i <= I_FREQ1; i++) {
        ph_ctl *c = &ctls[i];
        const wpm *m = c->ctx;
        if (c->active && !(c->flags & CF_HIDDEN)) gen_set(c, c->val);
        else restore_setting(m->g0);
    }
    return 0;
}

static int clk_util(ph_clk *d, int *pct)
{
    (void)d;
    static ULONGLONG li, lk, lu;
    FILETIME fi, fk, fu;
    if (!GetSystemTimes(&fi, &fk, &fu)) return -1;
    ULONGLONG i = ((ULONGLONG)fi.dwHighDateTime << 32) | fi.dwLowDateTime;
    ULONGLONG k = ((ULONGLONG)fk.dwHighDateTime << 32) | fk.dwLowDateTime;
    ULONGLONG u = ((ULONGLONG)fu.dwHighDateTime << 32) | fu.dwLowDateTime;
    ULONGLONG tot = (k - lk) + (u - lu), idle = i - li;
    int first = !lk;
    li = i; lk = k; lu = u;
    if (first || !tot) return -1;
    *pct = idle >= tot ? 0 : (int)((tot - idle) * 100 / tot);
    return 0;
}

static ph_clk winclk = {
    .name = L"Windows frequency cap",
    .min_mhz = 400, .max_mhz = 5100, .step_mhz = 100,
    .set_max = clk_set_max,
    .reset = clk_reset,
    .util = clk_util,
    .prio = 10,
};

/* ---------- backend ---------- */

static int count_choices(const wchar_t *const *ch)
{
    int n = 0;
    while (ch && ch[n]) n++;
    return n;
}

static void journal_recover(void)
{
    jrec r[MAX_TOUCH];
    DWORD sz = sizeof r;
    GUID act, ov;
    int has = active_scheme(&act) == 0, hit = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, JKEY, JVAL, RRF_RT_REG_BINARY, NULL, r, &sz) == ERROR_SUCCESS) {
        for (DWORD i = 0; i < sz / sizeof r[0]; i++) {
            write_pair(&r[i].scheme, &r[i].sub, &r[i].set, r[i].ac, r[i].dc);
            if (has && IsEqualGUID(&r[i].scheme, &act)) hit = 1;
        }
        ph_log("winpower: restored %u settings left by an unclean exit", (unsigned)(sz / sizeof r[0]));
    }
    sz = sizeof ov;
    if (RegGetValueW(HKEY_CURRENT_USER, JKEY, JOV, RRF_RT_REG_BINARY, NULL, &ov, &sz) == ERROR_SUCCESS &&
        sz == sizeof ov && p_set_ov)
        p_set_ov(ov);
    if (hit) PowerSetActiveScheme(NULL, &act);
    reg_del_value(HKEY_CURRENT_USER, JKEY, JVAL);
    reg_del_value(HKEY_CURRENT_USER, JKEY, JOV);
}

static int wp_init(void)
{
    GUID sch;
    resolve_ov();
    journal_recover();
    if (active_scheme(&sch)) return -1;
    int maxmhz = cpu_max_mhz();
    int hyb = g_plat.hybrid;

    if (hyb) {
        ctls[I_FREQ0].label = L"E-core max clock";
        for (int i = 0; i < PH_ARRAY(ctls); i++) {
            if (ctls[i].ctx == &M_CPMAX0) ctls[i].label = L"E-core max unparked";
            else if (ctls[i].ctx == &M_CPMIN0) ctls[i].label = L"E-core min unparked";
        }
    }
    for (int i = 0; i < PH_ARRAY(ctls); i++) {
        ph_ctl *c = &ctls[i];
        int hide = 0;
        if ((c->arg & A_HYB) && !hyb) hide = 1;
        if ((c->arg & A_EPP) && !g_plat.epp) hide = 1;
        if ((c->arg & A_OV) && (!p_set_ov || !p_get_ov)) hide = 1;
        if (c->ctx == &M_FREQ0 || c->ctx == &M_FREQ1) c->max = maxmhz;
        if (!hide && c->type != CT_HEADER) {
            int32_t v;
            if (c->get(c, &v)) hide = 1;
            else {
                int hi = c->type == CT_CHOICE ? count_choices(c->choices) - 1 : c->type == CT_TOGGLE ? 1 : c->max;
                int lo = c->type == CT_SLIDER ? c->min : 0;
                c->def = PH_CLAMP(v, lo, hi);
            }
        }
        if (hide) c->flags |= CF_HIDDEN;
    }
    ph_register_ctls(ctls, PH_ARRAY(ctls));

    winclk.max_mhz = maxmhz;
    ph_register_cpu_clk(&winclk);
    return 0;
}

static void wp_tick(void)
{
    int need = 0;
    LK();
    for (int i = 0; i < ntt && !need; i++)
        if (tt[i].mod && deactivated(tt[i].owner)) need = 1;
    if (ov_mod && deactivated(ov_owner)) need = 1;
    if (need) commit_locked();
    UL();
}

static void wp_shutdown(void)
{
    if (ctimer) {
        SetThreadpoolTimer(ctimer, NULL, 0, 0);
        WaitForThreadpoolTimerCallbacks(ctimer, TRUE);
        CloseThreadpoolTimer(ctimer);
        ctimer = NULL;
    }
    LK();
    armed = 0;
    GUID act;
    int has = active_scheme(&act) == 0;
    for (int i = 0; i < ntt; i++) restore_touch(&tt[i], has ? &act : NULL);
    if (ov_mod && ov_saved && p_set_ov) p_set_ov(ov_orig);
    ov_mod = 0;
    ov_owner = NULL;
    if (pending && has) PowerSetActiveScheme(NULL, &act);
    pending = 0;
    jdirty = 1;
    journal_sync();
    UL();
}

ph_backend bk_winpower = {
    .id = "winpower",
    .name = L"Windows power",
    .kind = BK_POWER,
    .init = wp_init,
    .shutdown = wp_shutdown,
    .tick = wp_tick,
};
