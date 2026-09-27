#include "phawx.h"
#define PHX_HOST             /* the SDK header's phx_plugin_init declaration is for plugins */
#include "phawx_plugin.h"
#include <stdarg.h>
#include <stdio.h>

/* Plugin host. DLLs in plugins\ (and plugins\<name>\) next to PhawxON.exe that
   export phx_plugin_init are listed on the Plugins page. A plugin runs only after
   the user turns it on, and turning one on or off takes effect at the next start:
   plugins register controls and clock domains that the rest of the app keeps
   pointers to, so they are loaded before AutoTDP builds its domain list and are
   never unloaded once running. */

#define MAX_PLUGINS 16
#define MAX_PCTL    48
#define MAX_PCAP    4
#define ORDER_BASE  1000     /* plugin rows follow the built-in rows on every page */
#define ORDER_SPAN  1000     /* one block per plugin so plugins do not interleave */
#define FAN_BASE    20000
#define RGB_BASE    25000
#define KEY_MAX     80
#define CTL_RESERVE 48       /* AutoTDP's and the Settings page's rows register after the plugins */

enum { PS_OFF, PS_RUNNING, PS_UNSUPPORTED, PS_FAILED, PS_CRASHED, PS_INVALID };

typedef struct pctl {
    ph_ctl c;
    phx_control *pub;
    struct phx_plugin *owner;
    char key[KEY_MAX];
} pctl;

typedef struct pclk {
    ph_clk c;
    phx_clock *pub;
    struct phx_plugin *owner;
    int touched;                 /* capped since the last reset */
} pclk;

enum { FC_HDR, FC_MODE, FC_SPEED, FC_RPM, FC_N };
enum { FM_AUTO, FM_MANUAL, FM_FULL };
typedef struct pfan {
    phx_fan *pub;
    struct phx_plugin *owner;
    ph_ctl c[FC_N];
    char kmode[KEY_MAX], kspeed[KEY_MAX];
    int touched;
} pfan;

enum { RC_HDR, RC_MODE, RC_COLOR, RC_BRIGHT, RC_N };
typedef struct prgb {
    phx_rgb *pub;
    struct phx_plugin *owner;
    ph_ctl c[RC_N];
    const wchar_t *modes[2 + 8 + 1];
    char kmode[KEY_MAX], kcolor[KEY_MAX], kbright[KEY_MAX];
    int touched;
} prgb;

struct phx_plugin {
    wchar_t path[MAX_PATH], dir[MAX_PATH];
    char    id[32];
    wchar_t name[64], ver[24];
    wchar_t status[128];         /* why it is not running, from the host */
    wchar_t note[128];           /* set_status from the plugin */
    wchar_t confirm[512];        /* shown before turning it on */
    int     index, state, enabled, enabled_at_start, is64, initing, busy;
    volatile LONG dead, down;    /* crashed; shut down */
    HMODULE h;
    phx_info info;
    CRITICAL_SECTION lk;         /* one call into the plugin at a time */
    phx_plugin *outer;           /* while busy: the plugin this thread was in before */
    pctl   *ctl[MAX_PCTL];
    pclk   *clk[MAX_PCAP];
    pfan   *fan[MAX_PCAP];
    prgb   *rgb[MAX_PCAP];
    ph_ctl  hdr[PG_COUNT];       /* section title per page the plugin uses */
    int     nctl, nclk, nfan, nrgb;
    ph_ctl  row;
};

static phx_plugin *plugs[MAX_PLUGINS];
static int nplugs, nfans_all, nrgbs_all, protected_dir;
static wchar_t plug_dir[MAX_PATH];
static char crashed_ids[4 * 33];   /* the ~loading marker the last run left */
static phx_platform platform;
static phx_host host;
static DWORD ui_tid, tls = TLS_OUT_OF_INDEXES;
static volatile LONG crash_mode;

#define ENTER(p) EnterCriticalSection(&(p)->lk)
#define LEAVE(p) LeaveCriticalSection(&(p)->lk)

static const uint8_t page_map[] = { PG_QUICK, PG_CPU, PG_POWER, PG_GPU, PG_DISPLAY, PG_SYSTEM, PG_PLUGINS };

static void a2w(const char *a, wchar_t *w, int n)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, a, -1, w, n)) w[0] = 0;
}

/* ---------- the start marker ----------
   [plugins] ~loading names the plugin Phawx ON is in while it starts: loading it,
   and every call into it until the first ph_apply_all has returned. If that
   crashes or hangs Phawx ON the marker stays, and the next start turns the
   plugin off instead of crashing again. After a crash it names the plugins that
   faulted. */
static phx_plugin *marked;
static int starting = 1;

static void mark(phx_plugin *p)
{
    if (crash_mode || p == marked) return;
    marked = p;
    cfg_set_str("plugins", "~loading", p ? p->id : NULL);
}

void plugins_started(void)
{
    mark(NULL);
    starting = 0;
}

/* ---------- calling a plugin ---------- */

/* the public value/active copies are refreshed before every call */
static void sync(phx_plugin *p)
{
    for (int i = 0; i < p->nctl; i++) {
        p->ctl[i]->pub->value = p->ctl[i]->c.val;
        p->ctl[i]->pub->active = p->ctl[i]->c.active;
    }
}

/* the plugin this thread is calling, for the crash filter */
static phx_plugin *cur_swap(phx_plugin *p)
{
    if (tls == TLS_OUT_OF_INDEXES) return NULL;
    phx_plugin *o = TlsGetValue(tls);
    TlsSetValue(tls, p);
    return o;
}

/* After a crash the thread that crashed may be stuck holding a plugin's lock, so
   wait for it briefly and then go ahead: putting the hardware back matters more. */
static int grab(phx_plugin *p)
{
    if (!crash_mode) { ENTER(p); return 1; }
    for (int i = 0; i < 30; i++) {
        if (TryEnterCriticalSection(&p->lk)) return 1;
        Sleep(10);
    }
    return 0;
}

/* Every call into a plugin: one at a time, never after it crashed, and never
   nested on one thread (a plugin that shows a message box or pumps STA COM in a
   callback would otherwise be called again from the UI's timers and paint). 0 =
   do not call it. */
static int enter(phx_plugin *p)
{
    if (p->dead || !grab(p)) return 0;
    if (p->busy || p->dead) { LEAVE(p); return 0; }
    p->busy = 1;
    p->outer = cur_swap(p);
    if (starting) mark(p);
    sync(p);
    return 1;
}

static void leave(phx_plugin *p)
{
    if (starting) mark(p->outer);
    cur_swap(p->outer);
    p->busy = 0;
    LEAVE(p);
}

/* ---------- controls: plugin structs wrapped in ph_ctl ---------- */

static int t_get(ph_ctl *c, int32_t *out)
{
    pctl *w = c->ctx;
    if (!enter(w->owner)) return -1;
    int r = w->pub->get(w->pub, out);
    leave(w->owner);
    return r == 0 ? 0 : -1;
}

static int t_set(ph_ctl *c, int32_t v)
{
    pctl *w = c->ctx;
    if (!enter(w->owner)) return -1;
    int r = w->pub->set(w->pub, v);
    leave(w->owner);
    return r == 0 ? 0 : -1;
}

/* a nested or refused call shows no value */
static void t_fmt(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    pctl *w = c->ctx;
    if (n <= 0) return;
    b[0] = 0;
    if (!enter(w->owner)) return;
    w->pub->fmt(w->pub, v, b, n);
    leave(w->owner);
    b[n - 1] = 0;
}

static int valid_key(const char *k)
{
    int n = 0;
    for (; k[n]; n++) {
        char ch = k[n];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '_' || ch == '-' || ch == '.'))
            return 0;
    }
    return n > 0 && n <= 40;
}

static int count_choices(const wchar_t *const *ch)
{
    int n = 0;
    while (ch && ch[n] && n < 64) n++;
    return n;
}

static int reject(phx_plugin *p, const char *what, const char *why)
{
    ph_log("plugin %s: %s rejected (%s)", p ? p->id : "?", what, why);
    return PHX_ERROR;
}

/* against the plugin's own rows, fans and lights, which register only at commit,
   and everything registered already */
static int key_taken(phx_plugin *p, const char *full)
{
    for (int i = 0; i < p->nctl; i++)
        if (!lstrcmpA(p->ctl[i]->key, full)) return 1;
    for (int i = 0; i < p->nfan; i++)
        if (!lstrcmpA(p->fan[i]->kmode, full) || !lstrcmpA(p->fan[i]->kspeed, full)) return 1;
    for (int i = 0; i < p->nrgb; i++)
        if (!lstrcmpA(p->rgb[i]->kmode, full) || !lstrcmpA(p->rgb[i]->kcolor, full) || !lstrcmpA(p->rgb[i]->kbright, full))
            return 1;
    return ph_ctl_find(full) != NULL;
}

static int PHX_CALL h_add_control(phx_plugin *p, phx_control *c)
{
    if (!p || !p->initing) return reject(p, "control", "added outside phx_plugin_init");
    if (!c || c->size < PHX_CONTROL_SIZE_V1) return reject(p, "control", "bad size");
    if (p->nctl >= MAX_PCTL) return reject(p, "control", "too many controls");
    if (c->type > PHX_HEADER || c->page > PHX_PAGE_PLUGINS || !c->label) return reject(p, "control", "bad type, page or label");
    int setting = c->type <= PHX_ACTION;
    if (setting && (!c->key || !c->set)) return reject(p, "control", "settings need a key and set()");
    if (c->key && !valid_key(c->key)) return reject(p, "control", "bad key");
    if (c->type == PHX_INFO && !c->get && !c->fmt) return reject(p, "control", "info rows need get() or fmt()");
    int nch = c->type == PHX_CHOICE ? count_choices(c->choices) : 0;
    if (c->type == PHX_CHOICE && !nch) return reject(p, "control", "choice without choices");
    if (c->type == PHX_SLIDER && c->max < c->min) return reject(p, "control", "max < min");

    pctl *w = ph_alloc(sizeof *w);
    if (!w) return PHX_ERROR;
    if (c->key) {
        ph_snprintf(w->key, sizeof w->key, "%s.%s", p->id, c->key);
        if (key_taken(p, w->key)) { ph_free(w); return reject(p, c->key, "duplicate key"); }
    }
    ph_ctl *k = &w->c;
    k->key = c->key ? w->key : NULL;
    k->label = c->label;
    k->unit = c->unit;
    k->type = (uint8_t)c->type;
    k->page = page_map[c->page];
    k->flags = (uint16_t)(c->flags & 0x3FFu);
    k->order = (int16_t)(ORDER_BASE + p->index * ORDER_SPAN + PH_CLAMP(c->order, 0, ORDER_SPAN - 1));
    k->min = c->min;
    k->max = c->max;
    k->step = c->step > 0 ? c->step : 1;
    k->def = c->def;
    if (c->type == PHX_CHOICE) {
        k->choices = c->choices;
        k->min = 0;
        k->max = nch - 1;
    }
    if (c->type == PHX_TOGGLE) { k->min = 0; k->max = 1; }
    if (c->type == PHX_SLIDER || c->type == PHX_CHOICE || c->type == PHX_TOGGLE) k->def = PH_CLAMP(k->def, k->min, k->max);
    k->get = c->get ? t_get : NULL;
    k->set = c->set ? t_set : NULL;
    k->fmt = c->fmt ? t_fmt : NULL;
    k->desc = c->desc;
    k->ctx = w;
    w->pub = c;
    w->owner = p;
    p->ctl[p->nctl++] = w;
    return PHX_OK;
}

/* on the UI thread, which reads these rows while it paints */
void plugins_update(void *plugin, void *control)
{
    phx_plugin *p = NULL;
    phx_control *c = control;
    for (int i = 0; i < nplugs && !p; i++)
        if (plugs[i] == plugin) p = plugs[i];
    if (!p || !c) return;
    for (int i = 0; i < p->nctl; i++) {
        pctl *w = p->ctl[i];
        if (w->pub != c) continue;
        ph_ctl *k = &w->c;
        if (c->label) k->label = c->label;
        k->unit = c->unit;
        k->desc = c->desc;
        k->flags = (uint16_t)(c->flags & 0x3FFu);
        if (k->type == CT_CHOICE && count_choices(c->choices)) {
            k->choices = c->choices;
            k->max = count_choices(c->choices) - 1;
        } else if (k->type == CT_SLIDER && c->max >= c->min) {
            k->min = c->min;
            k->max = c->max;
            k->step = c->step > 0 ? c->step : 1;
        }
        if (k->type == CT_SLIDER || k->type == CT_CHOICE) {
            k->def = PH_CLAMP(c->def, k->min, k->max);
            k->val = PH_CLAMP(k->val, k->min, k->max);
        }
        if (g_main) PostMessageW(g_main, WM_PH_REFRESH, 0, 0);
        return;
    }
}

/* from any thread: off the UI thread it is applied there a moment later */
static void PHX_CALL h_update(phx_plugin *p, phx_control *c)
{
    if (!p || !c) return;
    if (GetCurrentThreadId() == ui_tid) plugins_update(p, c);
    else if (g_main) PostMessageW(g_main, WM_PH_UPDATE, (WPARAM)p, (LPARAM)c);
}

/* ---------- clock domains ---------- */

/* called from AutoTDP's worker thread (and the UI thread while AutoTDP is off);
   touched: 1 = a cap may be set, 0 = removed, -1 = unchanged */
#define KCALL(expr, cap) do {                                   \
        pclk *w = d->ctx;                                       \
        if (!enter(w->owner)) return -1;                        \
        int r_ = (expr);                                        \
        if (!r_ && (cap) >= 0) w->touched = (cap);              \
        leave(w->owner);                                        \
        return r_ == 0 ? 0 : -1;                                \
    } while (0)
static int k_set_max(ph_clk *d, int mhz) { KCALL(w->pub->set_max(w->pub, mhz), 1); }
static int k_set_min(ph_clk *d, int mhz) { KCALL(w->pub->set_min(w->pub, mhz), 1); }
static int k_reset(ph_clk *d)            { KCALL(w->pub->reset(w->pub), 0); }
static int k_uncap(ph_clk *d)            { KCALL(w->pub->set_max(w->pub, w->pub->max_mhz), 0); }   /* no reset of its own */
static int k_cur(ph_clk *d, int *mhz)    { KCALL(w->pub->cur(w->pub, mhz), -1); }
static int k_util(ph_clk *d, int *pct)   { KCALL(w->pub->util(w->pub, pct), -1); }
#undef KCALL

static int PHX_CALL h_add_clock(phx_plugin *p, phx_clock *d)
{
    if (!p || !p->initing) return reject(p, "clock", "added outside phx_plugin_init");
    if (!d || d->size < PHX_CLOCK_SIZE_V1 || d->kind > PHX_CLOCK_GPU || !d->set_max) return reject(p, "clock", "bad struct");
    if (d->min_mhz < 0 || d->max_mhz <= d->min_mhz || d->max_mhz > 20000) return reject(p, "clock", "bad range");
    if (p->nclk >= MAX_PCAP) return reject(p, "clock", "too many clocks");
    pclk *w = ph_alloc(sizeof *w);
    if (!w) return PHX_ERROR;
    w->pub = d;
    w->owner = p;
    w->c.name = d->name ? d->name : p->name;
    w->c.min_mhz = d->min_mhz;
    w->c.max_mhz = d->max_mhz;
    w->c.step_mhz = d->step_mhz > 0 ? d->step_mhz : 50;
    w->c.set_max = k_set_max;
    w->c.set_min = d->set_min ? k_set_min : NULL;
    w->c.reset = d->reset ? k_reset : k_uncap;
    w->c.cur = d->cur ? k_cur : NULL;
    w->c.util = d->util ? k_util : NULL;
    w->c.ctx = w;
    w->c.prio = PH_CLAMP(d->prio, 0, 100);
    w->c.plugin = 1;
    p->clk[p->nclk++] = w;
    return PHX_OK;
}

/* ---------- fans ---------- */

static const wchar_t *const fan_modes[] = { L"Auto", L"Manual", L"Full speed", NULL };

static int fan_apply(pfan *f, int mode, int pct)
{
    int r;
    if (!enter(f->owner)) return -1;
    if (mode == FM_AUTO) {
        r = f->pub->set_auto(f->pub);
        if (!r) f->touched = 0;
    } else {
        r = f->pub->set_duty(f->pub, mode == FM_FULL ? 100 : PH_CLAMP(pct, f->pub->min_pct, 100));
        if (!r) f->touched = 1;
    }
    leave(f->owner);
    return r ? -1 : 0;
}

static int fan_set_mode(ph_ctl *c, int32_t v) { pfan *f = c->ctx; return fan_apply(f, v, f->c[FC_SPEED].val); }

/* Default: back to the firmware at once, not only at the next tick */
static void fan_release(ph_ctl *c)
{
    pfan *f = c->ctx;
    if (f->touched) fan_apply(f, FM_AUTO, 0);
}

static int fan_set_speed(ph_ctl *c, int32_t v)
{
    pfan *f = c->ctx;
    ph_ctl *m = &f->c[FC_MODE];
    return m->active && m->val == FM_MANUAL ? fan_apply(f, FM_MANUAL, v) : 0;
}

static int fan_rpm(ph_ctl *c, int32_t *o)
{
    pfan *f = c->ctx;
    int r = 0;
    if (!enter(f->owner)) return -1;
    int e = f->pub->get_rpm(f->pub, &r);
    leave(f->owner);
    if (e || r < 0) return -1;
    *o = r;
    return 0;
}

static int PHX_CALL h_add_fan(phx_plugin *p, phx_fan *f)
{
    char km[KEY_MAX], ks[KEY_MAX];
    if (!p || !p->initing) return reject(p, "fan", "added outside phx_plugin_init");
    if (!f || f->size < PHX_FAN_SIZE_V1 || !f->id || !valid_key(f->id) || !f->set_auto || !f->set_duty)
        return reject(p, "fan", "bad struct");
    if (p->nfan >= MAX_PCAP) return reject(p, "fan", "too many fans");
    ph_snprintf(km, sizeof km, "%s.%s.mode", p->id, f->id);
    ph_snprintf(ks, sizeof ks, "%s.%s.speed", p->id, f->id);
    if (key_taken(p, km) || key_taken(p, ks)) return reject(p, f->id, "duplicate key");
    pfan *w = ph_alloc(sizeof *w);
    if (!w) return PHX_ERROR;
    w->pub = f;
    w->owner = p;
    lstrcpyA(w->kmode, km);
    lstrcpyA(w->kspeed, ks);
    p->fan[p->nfan++] = w;
    return PHX_OK;
}

static void fan_commit(pfan *f)
{
    int base = FAN_BASE + 10 * nfans_all++, lo = PH_CLAMP(f->pub->min_pct, 0, 100);
    f->c[FC_HDR] = (ph_ctl){ .label = f->pub->name ? f->pub->name : L"Fan", .type = CT_HEADER, .page = PG_SYSTEM,
                             .order = (int16_t)base };
    f->c[FC_MODE] = (ph_ctl){ .key = f->kmode, .label = L"Fan mode", .type = CT_CHOICE, .page = PG_SYSTEM,
                              .order = (int16_t)(base + 1), .flags = CF_OPTIONAL | CF_REAPPLY | CF_PROFILE,
                              .choices = fan_modes, .set = fan_set_mode, .release = fan_release, .ctx = f };
    f->c[FC_SPEED] = (ph_ctl){ .key = f->kspeed, .label = L"Manual fan speed", .type = CT_SLIDER, .page = PG_SYSTEM,
                               .order = (int16_t)(base + 2), .flags = CF_PROFILE, .min = lo, .max = 100, .step = 5,
                               .def = lo > 50 ? lo : 50, .unit = L"%", .fmt = fmt_pct, .set = fan_set_speed, .ctx = f };
    f->c[FC_RPM] = (ph_ctl){ .label = L"Fan speed", .type = CT_INFO, .page = PG_SYSTEM, .order = (int16_t)(base + 3),
                             .unit = L"RPM", .get = f->pub->get_rpm ? fan_rpm : NULL,
                             .flags = f->pub->get_rpm ? 0 : CF_HIDDEN, .ctx = f };
    ph_register_ctls(f->c, FC_N);
}

/* ---------- RGB ---------- */

static const wchar_t *const color_names[] = {
    L"Red", L"Orange", L"Yellow", L"Green", L"Cyan", L"Blue", L"Purple", L"Pink", L"White", NULL
};
static const uint8_t color_rgb[][3] = {
    { 255, 0, 0 }, { 255, 96, 0 }, { 255, 208, 0 }, { 0, 255, 0 }, { 0, 255, 255 },
    { 0, 0, 255 }, { 128, 0, 255 }, { 255, 64, 160 }, { 255, 255, 255 }
};

static int rgb_push(prgb *l, int mode, int color, int bright)
{
    phx_rgb_state s;
    color = PH_CLAMP(color, 0, PH_ARRAY(color_rgb) - 1);
    s.mode = mode;
    s.r = color_rgb[color][0];
    s.g = color_rgb[color][1];
    s.b = color_rgb[color][2];
    s.reserved = 0;
    s.brightness = PH_CLAMP(bright, 0, 100);
    if (!enter(l->owner)) return -1;
    int r = l->pub->apply(l->pub, &s);
    if (!r) l->touched = 1;
    leave(l->owner);
    return r ? -1 : 0;
}

static void rgb_restore(prgb *l)
{
    if (!enter(l->owner)) return;
    if (l->pub->restore(l->pub) == 0) l->touched = 0;
    leave(l->owner);
}

/* Default: back to the firmware at once */
static void rgb_release(ph_ctl *c)
{
    prgb *l = c->ctx;
    if (l->touched) rgb_restore(l);
}

static int rgb_set_mode(ph_ctl *c, int32_t v)
{
    prgb *l = c->ctx;
    return rgb_push(l, v, l->c[RC_COLOR].val, l->c[RC_BRIGHT].val);
}

static int rgb_set_color(ph_ctl *c, int32_t v)
{
    prgb *l = c->ctx;
    ph_ctl *m = &l->c[RC_MODE];
    return m->active ? rgb_push(l, m->val, v, l->c[RC_BRIGHT].val) : 0;
}

static int rgb_set_bright(ph_ctl *c, int32_t v)
{
    prgb *l = c->ctx;
    ph_ctl *m = &l->c[RC_MODE];
    return m->active ? rgb_push(l, m->val, l->c[RC_COLOR].val, v) : 0;
}

static int PHX_CALL h_add_rgb(phx_plugin *p, phx_rgb *l)
{
    char km[KEY_MAX], kc[KEY_MAX], kb[KEY_MAX];
    if (!p || !p->initing) return reject(p, "rgb", "added outside phx_plugin_init");
    if (!l || l->size < PHX_RGB_SIZE_V1 || !l->id || !valid_key(l->id) || !l->apply || !l->restore)
        return reject(p, "rgb", "bad struct");
    if (p->nrgb >= MAX_PCAP) return reject(p, "rgb", "too many lights");
    ph_snprintf(km, sizeof km, "%s.%s.mode", p->id, l->id);
    ph_snprintf(kc, sizeof kc, "%s.%s.color", p->id, l->id);
    ph_snprintf(kb, sizeof kb, "%s.%s.bright", p->id, l->id);
    if (key_taken(p, km) || key_taken(p, kc) || key_taken(p, kb)) return reject(p, l->id, "duplicate key");
    prgb *w = ph_alloc(sizeof *w);
    if (!w) return PHX_ERROR;
    w->pub = l;
    w->owner = p;
    lstrcpyA(w->kmode, km);
    lstrcpyA(w->kcolor, kc);
    lstrcpyA(w->kbright, kb);
    p->rgb[p->nrgb++] = w;
    return PHX_OK;
}

static void rgb_commit(prgb *l)
{
    int base = RGB_BASE + 10 * nrgbs_all++, n = 0;
    l->modes[n++] = L"Off";
    l->modes[n++] = L"Solid";
    for (int i = 0; l->pub->effects && l->pub->effects[i] && i < 8; i++) l->modes[n++] = l->pub->effects[i];
    l->modes[n] = NULL;
    l->c[RC_HDR] = (ph_ctl){ .label = l->pub->name ? l->pub->name : L"Lighting", .type = CT_HEADER, .page = PG_SYSTEM,
                             .order = (int16_t)base };
    l->c[RC_MODE] = (ph_ctl){ .key = l->kmode, .label = L"Lighting", .type = CT_CHOICE, .page = PG_SYSTEM,
                              .order = (int16_t)(base + 1), .flags = CF_OPTIONAL | CF_REAPPLY | CF_PROFILE,
                              .choices = l->modes, .def = PHX_RGB_SOLID, .set = rgb_set_mode, .release = rgb_release,
                              .ctx = l };
    l->c[RC_COLOR] = (ph_ctl){ .key = l->kcolor, .label = L"Color", .type = CT_CHOICE, .page = PG_SYSTEM,
                               .order = (int16_t)(base + 2), .choices = color_names, .set = rgb_set_color, .ctx = l,
                               .flags = (uint16_t)(CF_PROFILE | ((l->pub->caps & PHX_RGB_COLOR) ? 0 : CF_HIDDEN)) };
    l->c[RC_BRIGHT] = (ph_ctl){ .key = l->kbright, .label = L"Brightness", .type = CT_SLIDER, .page = PG_SYSTEM,
                                .order = (int16_t)(base + 3), .min = 0, .max = 100, .step = 10, .def = 100,
                                .unit = L"%", .fmt = fmt_pct, .set = rgb_set_bright, .ctx = l,
                                .flags = (uint16_t)(CF_PROFILE | ((l->pub->caps & PHX_RGB_BRIGHTNESS) ? 0 : CF_HIDDEN)) };
    ph_register_ctls(l->c, RC_N);
}

/* ---------- the rest of the host API ---------- */

static void cfg_sec(phx_plugin *p, char *s, int n) { ph_snprintf(s, n, "plugin:%s", p->id); }

static int PHX_CALL h_cfg_get_int(phx_plugin *p, const char *key, int def)
{
    char s[48];
    if (!p || !key || !valid_key(key)) return def;
    cfg_sec(p, s, sizeof s);
    return cfg_get_int(s, key, def);
}

static int PHX_CALL h_cfg_set_int(phx_plugin *p, const char *key, int v)
{
    char s[48], b[16];
    if (!p || !key || !valid_key(key)) return reject(p, "config key", "bad key");
    cfg_sec(p, s, sizeof s);
    ph_snprintf(b, sizeof b, "%d", v);
    return cfg_set_str(s, key, b) ? reject(p, key, "not written") : PHX_OK;
}

static int PHX_CALL h_cfg_get_str(phx_plugin *p, const char *key, char *out, int n)
{
    char s[48];
    if (!out || n <= 0) return 0;
    out[0] = 0;
    if (!p || !key || !valid_key(key)) return 0;
    cfg_sec(p, s, sizeof s);
    return cfg_get_str(s, key, out, n);
}

/* values over 511 characters are refused, not written empty */
static int PHX_CALL h_cfg_set_str(phx_plugin *p, const char *key, const char *v)
{
    char s[48];
    if (!p || !key || !valid_key(key)) return reject(p, "config key", "bad key");
    /* a line break would let a value write its own ini lines */
    for (const char *c = v; c && *c; c++)
        if (*c == '\r' || *c == '\n') return reject(p, key, "line break in the value");
    cfg_sec(p, s, sizeof s);
    return cfg_set_str(s, key, v) ? reject(p, key, "value too long") : PHX_OK;
}

static void PHX_CALL h_log(phx_plugin *p, const char *msg)
{
    if (msg) ph_log("plugin %s: %.400s", p ? p->id : "?", msg);
}

static void PHX_CALL h_toast(phx_plugin *p, const wchar_t *msg) { (void)p; if (msg) ui_toast(msg); }

static void PHX_CALL h_set_status(phx_plugin *p, const wchar_t *msg)
{
    if (!p) return;
    if (msg) lstrcpynW(p->note, msg, PH_ARRAY(p->note));
    else p->note[0] = 0;
    if (g_main) PostMessageW(g_main, WM_PH_REFRESH, 0, 0);
}

static const wchar_t *PHX_CALL h_plugin_dir(phx_plugin *p) { return p ? p->dir : L""; }

static int PHX_CALL h_ctl_get(phx_plugin *p, const char *key, int32_t *value, int *active)
{
    (void)p;
    ph_ctl *c = ph_ctl_find(key);
    if (!c || (c->flags & CF_HIDDEN)) return -1;
    if (value) *value = c->active ? c->val : c->def;
    if (active) *active = c->active;
    return 0;
}

static int PHX_CALL h_hw_ready(phx_plugin *p) { (void)p; return drv_ok(); }
static int PHX_CALL h_ec_read(phx_plugin *p, uint8_t reg, uint8_t *v) { (void)p; return ph_ec_read(reg, v) ? -1 : 0; }
static int PHX_CALL h_ec_write(phx_plugin *p, uint8_t reg, uint8_t v) { (void)p; return ph_ec_write(reg, v) ? -1 : 0; }

static int PHX_CALL h_msr_read(phx_plugin *p, uint32_t msr, int cpu, uint64_t *v)
{
    (void)p;
    return v && drv_rdmsr_cpu(msr, cpu, v) == 0 ? 0 : -1;
}

static int PHX_CALL h_msr_write(phx_plugin *p, uint32_t msr, int cpu, uint64_t v)
{
    (void)p;
    return drv_wrmsr_cpu(msr, cpu, v) == 0 ? 0 : -1;
}

/* ---------- discovery ---------- */

/* Does this file export phx_plugin_init? Read from disk and parse the PE headers
   by hand, so nothing in a plugin that is turned off ever runs. */
static int pe_is_plugin(const wchar_t *path, int *is64)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz;
    BYTE *b = NULL;
    DWORD n = 0;
    int ok = 0;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0x200 && sz.QuadPart <= (64 << 20) &&
        (b = ph_alloc((size_t)sz.QuadPart)) != NULL && ReadFile(f, b, (DWORD)sz.QuadPart, &n, NULL) &&
        n == (DWORD)sz.QuadPart) {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)b;
        LONG nt = dos->e_lfanew;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt < 0 || (DWORD)nt > n - 4 - sizeof(IMAGE_FILE_HEADER)) goto out;
        if (*(const DWORD *)(b + nt) != IMAGE_NT_SIGNATURE) goto out;
        const IMAGE_FILE_HEADER *fh = (const IMAGE_FILE_HEADER *)(b + nt + 4);
        const BYTE *oh = (const BYTE *)(fh + 1);
        DWORD ohsz = fh->SizeOfOptionalHeader, nsec = fh->NumberOfSections;
        if (!(fh->Characteristics & IMAGE_FILE_DLL) || ohsz < 2 || (DWORD)(oh - b) + ohsz > n) goto out;
        const IMAGE_DATA_DIRECTORY *dd;
        DWORD ndd, ddoff;
        if (*(const WORD *)oh == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            ddoff = (DWORD)offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory);
            if (ohsz < ddoff + sizeof(IMAGE_DATA_DIRECTORY)) goto out;
            ndd = ((const IMAGE_OPTIONAL_HEADER64 *)oh)->NumberOfRvaAndSizes;
            *is64 = fh->Machine == IMAGE_FILE_MACHINE_AMD64 ? 1 : 2;   /* 2: ARM64 or another CPU */
        } else if (*(const WORD *)oh == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            ddoff = (DWORD)offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory);
            if (ohsz < ddoff + sizeof(IMAGE_DATA_DIRECTORY)) goto out;
            ndd = ((const IMAGE_OPTIONAL_HEADER32 *)oh)->NumberOfRvaAndSizes;
            *is64 = 0;
        } else goto out;
        dd = (const IMAGE_DATA_DIRECTORY *)(oh + ddoff);
        if (ndd <= IMAGE_DIRECTORY_ENTRY_EXPORT || !dd[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress) goto out;
        const IMAGE_SECTION_HEADER *sec = (const IMAGE_SECTION_HEADER *)(oh + ohsz);
        if (nsec > 96 || (DWORD)((const BYTE *)sec - b) + nsec * sizeof *sec > n) goto out;

#define RVA2OFF(rva, out_off) do {                                                        \
            DWORD r_ = (rva); out_off = 0;                                                \
            for (DWORD s_ = 0; s_ < nsec; s_++) {                                         \
                DWORD va_ = sec[s_].VirtualAddress, raw_ = sec[s_].SizeOfRawData;         \
                if (r_ >= va_ && r_ - va_ < raw_) {                                       \
                    DWORD o_ = sec[s_].PointerToRawData + (r_ - va_);                     \
                    if (o_ >= sec[s_].PointerToRawData && o_ < n) out_off = o_;           \
                    break;                                                                \
                }                                                                         \
            }                                                                             \
        } while (0)

        DWORD eo;
        RVA2OFF(dd[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress, eo);
        if (!eo || eo + sizeof(IMAGE_EXPORT_DIRECTORY) > n) goto out;
        const IMAGE_EXPORT_DIRECTORY *ed = (const IMAGE_EXPORT_DIRECTORY *)(b + eo);
        DWORD nn = ed->NumberOfNames, no;
        if (nn > 65536) goto out;
        RVA2OFF(ed->AddressOfNames, no);
        if (!no || no + (uint64_t)nn * 4 > n) goto out;
        static const char want[] = PHX_INIT_EXPORT;
        for (DWORD i = 0; i < nn && !ok; i++) {
            DWORD so;
            RVA2OFF(((const DWORD *)(b + no))[i], so);
            if (so && so + sizeof want <= n && !memcmp(b + so, want, sizeof want)) ok = 1;
        }
#undef RVA2OFF
    }
out:
    ph_free(b);
    CloseHandle(f);
    return ok;
}

static void make_id(const wchar_t *file, char *id, int n)
{
    int k = 0;
    for (const wchar_t *s = file; *s && *s != L'.' && k < n - 1; s++) {
        wchar_t ch = *s;
        if (ch >= L'A' && ch <= L'Z') ch = (wchar_t)(ch - L'A' + L'a');
        id[k++] = (ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9') || ch == L'-' ? (char)ch : '_';
    }
    id[k] = 0;
}

static int ends_dll(const wchar_t *s)
{
    int n = lstrlenW(s);
    return n > 4 && !lstrcmpiW(s + n - 4, L".dll");
}

static void consider(const wchar_t *dir, const wchar_t *file)
{
    wchar_t path[MAX_PATH];
    int is64 = 0;
    if (nplugs >= MAX_PLUGINS) return;
    if (ph_swprintf(path, MAX_PATH, L"%s\\%s", dir, file) < 0) return;
    if (!pe_is_plugin(path, &is64)) return;
    char id[32];
    make_id(file, id, sizeof id);
    if (!id[0]) return;
    for (int i = 0; i < nplugs; i++)
        if (!lstrcmpA(plugs[i]->id, id)) { ph_log("plugins: %ls ignored, a plugin named %s already exists", path, id); return; }
    phx_plugin *p = ph_alloc(sizeof *p);
    if (!p) return;
    InitializeCriticalSection(&p->lk);
    lstrcpynW(p->path, path, MAX_PATH);
    lstrcpynW(p->dir, dir, MAX_PATH);
    lstrcpyA(p->id, id);
    p->is64 = is64;
    int fv[3];
    wchar_t desc[64], warn[320];
    if (ph_file_version(path, fv, desc, PH_ARRAY(desc)) == 0) ph_fmt_version(p->ver, PH_ARRAY(p->ver), fv);
    if (desc[0]) lstrcpynW(p->name, desc, PH_ARRAY(p->name));
    else a2w(id, p->name, PH_ARRAY(p->name));
    /* the description is the plugin's own claim, so the confirmation names the file
       too; a plugin can add its own warning (PhawxWarning) after the host's */
    ph_file_string(path, L"PhawxWarning", warn, PH_ARRAY(warn));
    ph_swprintf(p->confirm, PH_ARRAY(p->confirm),
                L"Turn on %s (%s)? Plugins run with administrator rights and full hardware access.%s%s",
                p->name, file, warn[0] ? L" " : L"", warn);
    plugs[nplugs++] = p;
}

static void scan(const wchar_t *dir, int depth)
{
    wchar_t pat[MAX_PATH], sub[MAX_PATH];
    WIN32_FIND_DATAW fd;
    if (ph_swprintf(pat, MAX_PATH, L"%s\\*", dir) < 0) return;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth || !lstrcmpW(fd.cFileName, L".") || !lstrcmpW(fd.cFileName, L"..")) continue;
            if (ph_swprintf(sub, MAX_PATH, L"%s\\%s", dir, fd.cFileName) >= 0) scan(sub, 1);
        } else if (ends_dll(fd.cFileName)) {
            consider(dir, fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* ---------- loading ---------- */

static void discard(phx_plugin *p)
{
    for (int i = 0; i < p->nctl; i++) ph_free(p->ctl[i]);
    for (int i = 0; i < p->nclk; i++) ph_free(p->clk[i]);
    for (int i = 0; i < p->nfan; i++) ph_free(p->fan[i]);
    for (int i = 0; i < p->nrgb; i++) ph_free(p->rgb[i]);
    p->nctl = p->nclk = p->nfan = p->nrgb = 0;
}

/* -1: no room left for its rows */
static int commit(phx_plugin *p)
{
    /* rows under the plugin's own title, not under whatever built-in section is last */
    int has_hdr[PG_COUNT] = { 0 }, used[PG_COUNT] = { 0 }, n = p->nctl + FC_N * p->nfan + RC_N * p->nrgb;
    for (int i = 0; i < p->nctl; i++) {
        used[p->ctl[i]->c.page] = 1;
        if (p->ctl[i]->c.type == CT_HEADER) has_hdr[p->ctl[i]->c.page] = 1;
    }
    for (int pg = 0; pg < PG_COUNT; pg++) n += used[pg] && !has_hdr[pg];
    if (n > ph_ctl_room() - CTL_RESERVE) return -1;
    for (int pg = 0; pg < PG_COUNT; pg++) {
        if (!used[pg] || has_hdr[pg]) continue;
        p->hdr[pg] = (ph_ctl){ .label = p->name, .type = CT_HEADER, .page = (uint8_t)pg,
                               .order = (int16_t)(ORDER_BASE + p->index * ORDER_SPAN) };
        ph_register_ctl(&p->hdr[pg]);
    }
    for (int i = 0; i < p->nctl; i++) ph_register_ctl(&p->ctl[i]->c);
    for (int i = 0; i < p->nclk; i++) {
        if (p->clk[i]->pub->kind == PHX_CLOCK_CPU) ph_register_cpu_clk(&p->clk[i]->c);
        else ph_register_gpu_clk(&p->clk[i]->c);
    }
    for (int i = 0; i < p->nfan; i++) fan_commit(p->fan[i]);
    for (int i = 0; i < p->nrgb; i++) rgb_commit(p->rgb[i]);
    sync(p);
    return 0;
}

/* keys "<id>.<key>" must never land on a built-in's: these prefixes are theirs */
static int reserved_id(const char *id)
{
    static const char *const ids[] = {
        "amdgpu", "app", "arc", "auto", "autotdp", "cpu", "dev", "display", "gpu", "input", "intelgpu",
        "nvidia", "power", "radeon", "sys",
    };
    for (int i = 0; i < PH_ARRAY(ids); i++)
        if (!lstrcmpA(ids[i], id)) return 1;
    return 0;
}

/* 0 when the plugin can never run as it is */
static int loadable(phx_plugin *p)
{
    const wchar_t *why = !p->is64 ? L"built for 32-bit Windows, needs a 64-bit build" :
                         p->is64 != 1 ? L"not built for x64 Windows, needs an x64 build" :
                         reserved_id(p->id) ? L"its file name is reserved, rename it" : NULL;
    if (why) {
        p->state = PS_INVALID;
        lstrcpynW(p->status, why, PH_ARRAY(p->status));
        return 0;
    }
    /* Phawx ON runs as administrator. Outside Program Files, any program the user
       runs could drop a DLL into plugins\ and have it run with those rights. */
    if (!protected_dir) {
        lstrcpynW(p->status, L"install Phawx ON in Program Files", PH_ARRAY(p->status));
        return 0;
    }
    return 1;
}

static void load(phx_plugin *p)
{
    /* the start marker names it until its DLL is loaded, or unloaded again */
    if (!enter(p)) return;
    LPTOP_LEVEL_EXCEPTION_FILTER filt = SetUnhandledExceptionFilter(NULL);
    SetUnhandledExceptionFilter(filt);
    int rc = PHX_ERROR;
    /* dependencies from the plugin's folder and System32 only, never PATH or the cwd */
    HMODULE h = LoadLibraryExW(p->path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    DWORD err = h ? 0 : GetLastError();
    phx_plugin_init_fn init = h ? (phx_plugin_init_fn)(void *)GetProcAddress(h, PHX_INIT_EXPORT) : NULL;
    const phx_info *info = NULL;
    if (!h) {
        ph_swprintf(p->status, PH_ARRAY(p->status), err == ERROR_MOD_NOT_FOUND ? L"a DLL it needs is missing" :
                    L"could not be loaded (error %lu)", err);
    } else if (!init) {
        lstrcpynW(p->status, L"not a Phawx ON plugin", PH_ARRAY(p->status));
    } else {
        p->initing = 1;
        rc = init(&host, p, &info);
        p->initing = 0;
    }
    SetUnhandledExceptionFilter(filt);    /* a plugin or its runtime may have replaced ours */
    ZeroMemory(&p->info, sizeof p->info);
    if (rc == PHX_OK && info && info->size >= 8) {
        /* read only what both sides know, so a plugin built for a newer API still
           works; whole members only */
        size_t n = info->size < sizeof p->info ? info->size : sizeof p->info;
        memcpy(&p->info, info, n & ~(size_t)7);
    }

    if (rc == PHX_OK) {
        p->h = h;
        if (p->info.name && p->info.name[0]) lstrcpynW(p->name, p->info.name, PH_ARRAY(p->name));
        if (p->info.version && p->info.version[0]) lstrcpynW(p->ver, p->info.version, PH_ARRAY(p->ver));
        if (commit(p) == 0) {
            p->state = PS_RUNNING;
            ph_log("plugins: %s running (%d controls, %d clocks, %d fans, %d lights)", p->id, p->nctl, p->nclk, p->nfan, p->nrgb);
            leave(p);
            return;
        }
        /* it may have started threads, so it stays loaded, but it is never called again */
        if (p->info.shutdown) p->info.shutdown();
        discard(p);
        p->state = PS_FAILED;
        lstrcpynW(p->status, L"too many settings", PH_ARRAY(p->status));
        ph_log("plugins: %s not running: no room for its rows", p->id);
        leave(p);
        return;
    }
    discard(p);
    if (h) FreeLibrary(h);
    p->state = rc == PHX_UNSUPPORTED ? PS_UNSUPPORTED : PS_FAILED;
    if (rc == PHX_UNSUPPORTED && !p->note[0]) lstrcpynW(p->status, L"not for this device", PH_ARRAY(p->status));
    else if (rc == PHX_E_VERSION) lstrcpynW(p->status, L"needs a newer Phawx ON", PH_ARRAY(p->status));
    else if (init && !p->status[0]) ph_swprintf(p->status, PH_ARRAY(p->status), L"failed to start (%d)", rc);
    ph_log("plugins: %s not running: %ls", p->id, p->note[0] ? p->note : p->status);
    leave(p);
}

/* ---------- Plugins page ---------- */

enum { R_HDR, R_PAWNIO, R_PAWNIO_DL, R_PM, R_PM_DL, R_LHDR, R_NONE, R_FOLDER, R_RESTART, R_N };
static ph_ctl rows[R_N];

#define URL_PAWNIO     L"https://pawnio.eu"
#define URL_MODULES    L"https://github.com/namazso/PawnIO.Modules/releases"
#define URL_PRESENTMON L"https://game.intel.com/story/intel-presentmon/"

static int pawnio_get(ph_ctl *c, int32_t *out) { (void)c; *out = drv_ok() ? TONE_GOOD : TONE_BAD; return 0; }

static void pawnio_fmt(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c; (void)v;
    wchar_t ver[16];
    switch (drv_state()) {
    case DRV_OK:
        a2w(drv_version(), ver, PH_ARRAY(ver));
        if (ver[0]) ph_swprintf(b, n, L"PawnIO %s is loaded and running", ver);
        else lstrcpynW(b, L"PawnIO is loaded and running", n);
        break;
    case DRV_STOPPED: lstrcpynW(b, L"PawnIO driver is not running", n); break;
    case DRV_NOMODULES: lstrcpynW(b, L"PawnIO modules are missing", n); break;
    default: lstrcpynW(b, L"PawnIO is not installed", n); break;
    }
}

static int open_link(const wchar_t *url, const wchar_t *what)
{
    if (ph_open_url(url)) return -1;
    wchar_t m[96];
    ph_swprintf(m, PH_ARRAY(m), L"Opening %s in your browser", what);
    ui_toast(m);
    ui_show(0);
    return 0;
}

/* the driver is there but some modules are not: point at the module releases */
static int need_modules(void) { return drv_state() == DRV_NOMODULES || (drv_ok() && drv_missing()[0]); }

static int pawnio_dl(ph_ctl *c, int32_t v)
{
    (void)c; (void)v;
    return need_modules() ? open_link(URL_MODULES, L"PawnIO.Modules") : open_link(URL_PAWNIO, L"pawnio.eu");
}

static void pawnio_dl_fmt(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c; (void)v;
    if (need_modules() && drv_missing()[0]) a2w(drv_missing(), b, n);
    else lstrcpynW(b, need_modules() ? L"github.com" : L"pawnio.eu", n);
}

/* PresentMon can come and go while the app runs; the Download row follows it */
static int pm_get(ph_ctl *c, int32_t *out)
{
    (void)c;
    int s = fps_status(), ok = s == 2 || s < 0;
    *out = s == 2 ? TONE_GOOD : s < 0 ? TONE_DIM : TONE_BAD;
    ph_ctl *dl = &rows[R_PM_DL];
    uint16_t f = ok ? (uint16_t)(dl->flags | CF_HIDDEN) : (uint16_t)(dl->flags & ~CF_HIDDEN);
    if (f != dl->flags) {
        dl->flags = f;
        if (g_main) PostMessageW(g_main, WM_PH_REFRESH, 0, 0);
    }
    return 0;
}

static void pm_fmt(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c; (void)v;
    int s = fps_status();
    const wchar_t *ver = fps_version();
    if (s == 2 && ver[0]) ph_swprintf(b, n, L"PresentMon %s is loaded and running", ver);
    else if (s == 2) lstrcpynW(b, L"PresentMon is loaded and running", n);
    else if (s == 1) lstrcpynW(b, L"PresentMon service is not running", n);
    else if (s < 0) lstrcpynW(b, L"Checking PresentMon\x2026", n);
    else lstrcpynW(b, L"PresentMon is not installed", n);
}

static int pm_dl(ph_ctl *c, int32_t v) { (void)c; (void)v; return open_link(URL_PRESENTMON, L"Intel PresentMon"); }

static void pm_dl_fmt(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c; (void)v;
    lstrcpynW(b, L"intel.com", n);
}

static int none_get(ph_ctl *c, int32_t *out) { (void)c; *out = TONE_DIM; return 0; }

static void none_fmt(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c; (void)v;
    lstrcpynW(b, L"No plugins in the plugins folder yet", n);
}

static int folder_act(ph_ctl *c, int32_t v)
{
    (void)c; (void)v;
    CreateDirectoryW(plug_dir, NULL);
    if (ph_open_url(plug_dir)) return -1;
    ui_show(0);
    return 0;
}

static void folder_fmt(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c; (void)v;
    lstrcpynW(b, L"plugins\\", n);
}

static int restart_act(ph_ctl *c, int32_t v) { (void)c; (void)v; return app_restart(); }

/* turning a plugin on waits for a restart only if it can load, off only if it runs */
static int needs_restart(const phx_plugin *p)
{
    if (p->enabled == p->enabled_at_start) return 0;
    return p->enabled ? protected_dir && p->state != PS_INVALID : p->state == PS_RUNNING;
}

int plugins_restart_pending(void)
{
    for (int i = 0; i < nplugs; i++)
        if (needs_restart(plugs[i])) return 1;
    return 0;
}

static void restart_row_sync(void)
{
    if (plugins_restart_pending()) rows[R_RESTART].flags &= (uint16_t)~CF_HIDDEN;
    else rows[R_RESTART].flags |= CF_HIDDEN;
}

static int row_get(ph_ctl *c, int32_t *out) { *out = ((phx_plugin *)c->ctx)->enabled; return 0; }

static int row_set(ph_ctl *c, int32_t v)
{
    phx_plugin *p = c->ctx;
    p->enabled = v != 0;
    cfg_set_int("plugins", p->id, p->enabled);
    if (p->enabled && p->state == PS_CRASHED) p->state = PS_OFF;
    restart_row_sync();
    if (needs_restart(p)) ui_toast(L"Takes effect when Phawx ON restarts");
    return 0;
}

static int row_sub(const ph_ctl *c, wchar_t *b, int n)
{
    const phx_plugin *p = c->ctx;
    const wchar_t *why = p->note[0] ? p->note : p->status;
    if (p->state == PS_RUNNING) {
        if (!p->enabled) { lstrcpynW(b, L"Running \x00B7 stops after a restart", n); return TONE_DIM; }
        if (p->note[0]) ph_swprintf(b, n, L"Running \x00B7 %s", p->note);
        else lstrcpynW(b, L"Running", n);
        return TONE_GOOD;
    }
    /* a plugin that cannot run here says so, on or off */
    if (p->state == PS_INVALID || !protected_dir) {
        ph_swprintf(b, n, L"Not running \x00B7 %s", p->status);
        return p->enabled || p->state == PS_INVALID ? TONE_BAD : TONE_DIM;
    }
    if (p->enabled && !p->enabled_at_start) { lstrcpynW(b, L"Not running \x00B7 starts after a restart", n); return TONE_DIM; }
    if (p->state == PS_CRASHED) {
        lstrcpynW(b, L"Not running \x00B7 turned off after it crashed Phawx ON", n);
        return TONE_BAD;
    }
    if (!p->enabled) {
        lstrcpynW(b, L"Not running", n);
        return TONE_DIM;
    }
    if (why[0]) ph_swprintf(b, n, L"Not running \x00B7 %s", why);
    else lstrcpynW(b, L"Not running", n);
    return p->state == PS_UNSUPPORTED ? TONE_DIM : TONE_BAD;
}

#define NS (CF_NOSAVE | CF_NOPIN)

static void page_init(void)
{
    int mods = need_modules();
    rows[R_HDR] = (ph_ctl){ .label = L"Required components", .type = CT_HEADER, .page = PG_PLUGINS, .order = 0 };
    rows[R_PAWNIO] = (ph_ctl){ .label = L"PawnIO", .type = CT_STATUS, .page = PG_PLUGINS, .order = 1,
                               .flags = NS, .get = pawnio_get, .fmt = pawnio_fmt };
    rows[R_PAWNIO_DL] = (ph_ctl){ .label = mods ? L"Download PawnIO modules" : L"Download PawnIO", .type = CT_ACTION,
                                  .page = PG_PLUGINS, .order = 2, .set = pawnio_dl, .fmt = pawnio_dl_fmt,
                                  .flags = (uint16_t)(NS | (drv_ok() && !mods ? CF_HIDDEN : 0)) };
    rows[R_PM] = (ph_ctl){ .label = L"PresentMon", .type = CT_STATUS, .page = PG_PLUGINS, .order = 3,
                           .flags = NS, .get = pm_get, .fmt = pm_fmt };
    rows[R_PM_DL] = (ph_ctl){ .label = L"Download PresentMon", .type = CT_ACTION, .page = PG_PLUGINS, .order = 4,
                              .flags = NS | CF_HIDDEN, .set = pm_dl, .fmt = pm_dl_fmt };
    rows[R_LHDR] = (ph_ctl){ .label = L"Plugins", .type = CT_HEADER, .page = PG_PLUGINS, .order = 10 };
    rows[R_NONE] = (ph_ctl){ .label = L"None", .type = CT_STATUS, .page = PG_PLUGINS, .order = 11,
                             .flags = (uint16_t)(NS | (nplugs ? CF_HIDDEN : 0)), .get = none_get, .fmt = none_fmt };
    rows[R_FOLDER] = (ph_ctl){ .label = L"Open plugins folder", .type = CT_ACTION, .page = PG_PLUGINS, .order = 90,
                               .flags = NS, .set = folder_act, .fmt = folder_fmt };
    rows[R_RESTART] = (ph_ctl){ .label = L"Restart Phawx ON to apply", .type = CT_ACTION, .page = PG_PLUGINS,
                                .order = 91, .flags = NS | CF_HIDDEN, .set = restart_act };
    ph_register_ctls(rows, R_N);
    for (int i = 0; i < nplugs; i++) {
        phx_plugin *p = plugs[i];
        p->row = (ph_ctl){ .label = p->name, .type = CT_TOGGLE, .page = PG_PLUGINS, .order = (int16_t)(20 + i),
                           .flags = NS | CF_CONFIRM, .desc = p->confirm, .get = row_get, .set = row_set,
                           .sub = row_sub, .ctx = p };
        ph_register_ctl(&p->row);
    }
}

/* ---------- backend ---------- */

static void host_init(void)
{
    platform = (phx_platform){
        .size = sizeof platform, .vendor = (uint32_t)g_plat.vendor, .family = g_plat.family,
        .model = g_plat.model, .stepping = g_plat.stepping, .logical_cpus = g_plat.nlogical,
        .cores = g_plat.ncores, .hybrid = g_plat.hybrid, .cpu_name = g_plat.cpu_name,
        .maker = g_plat.maker, .product = g_plat.product, .board = g_plat.board,
    };
    host = (phx_host){
        .size = sizeof host, .version = PHX_API_VERSION, .platform = &platform,
        .add_control = h_add_control, .add_clock = h_add_clock, .add_fan = h_add_fan, .add_rgb = h_add_rgb,
        .update = h_update, .cfg_get_int = h_cfg_get_int, .cfg_set_int = h_cfg_set_int,
        .cfg_get_str = h_cfg_get_str, .cfg_set_str = h_cfg_set_str, .log = h_log, .toast = h_toast,
        .set_status = h_set_status, .plugin_dir = h_plugin_dir, .ctl_get = h_ctl_get, .hw_ready = h_hw_ready,
        .ec_read = h_ec_read, .ec_write = h_ec_write, .msr_read = h_msr_read, .msr_write = h_msr_write,
    };
}

static int pl_probe(void) { return 1; }

/* is id one of the comma-separated ids in list? */
static int in_list(const char *list, const char *id)
{
    int n = lstrlenA(id);
    for (const char *s = list; *s; s++) {
        const char *e = s;
        while (*e && *e != ',') e++;
        if (e - s == n && !memcmp(s, id, (size_t)n)) return 1;
        if (!*e) break;
        s = e;
    }
    return 0;
}

static int pl_init(void)
{
    wchar_t exe[MAX_PATH];
    host_init();
    ui_tid = GetCurrentThreadId();
    if (tls == TLS_OUT_OF_INDEXES) tls = TlsAlloc();
    if (ph_exe_dir(exe, MAX_PATH) == 0) {
        ph_swprintf(plug_dir, MAX_PATH, L"%s\\plugins", exe);
        protected_dir = ph_path_protected(exe);
    }
    if (!cfg_get_str("plugins", "~loading", crashed_ids, sizeof crashed_ids)) crashed_ids[0] = 0;
    if (crashed_ids[0]) {
        char ids[sizeof crashed_ids], *s = ids, *e;
        ph_log("plugins: %s crashed or hung Phawx ON last time, turning it off", crashed_ids);
        lstrcpyA(ids, crashed_ids);
        for (; *s; s = e) {
            for (e = s; *e && *e != ','; e++) {}
            if (*e) *e++ = 0;
            if (valid_key(s)) cfg_set_int("plugins", s, 0);
        }
        cfg_set_str("plugins", "~loading", NULL);
    }
    if (plug_dir[0]) scan(plug_dir, 0);
    for (int i = 1; i < nplugs; i++)
        for (int j = i; j > 0 && lstrcmpA(plugs[j - 1]->id, plugs[j]->id) > 0; j--) {
            phx_plugin *t = plugs[j]; plugs[j] = plugs[j - 1]; plugs[j - 1] = t;
        }
    for (int i = 0; i < nplugs; i++) {
        phx_plugin *p = plugs[i];
        p->index = i;
        if (in_list(crashed_ids, p->id)) p->state = PS_CRASHED;
        p->enabled = p->enabled_at_start = cfg_get_int("plugins", p->id, 0) != 0;
    }
    /* the Plugins page first, so plugins cannot crowd it out of the registry */
    page_init();
    for (int i = 0; i < nplugs; i++)
        if (loadable(plugs[i]) && plugs[i]->enabled) load(plugs[i]);
    return 0;
}

static void call(phx_plugin *p, void (PHX_CALL *fn)(void))
{
    if (p->state != PS_RUNNING || !fn || !enter(p)) return;
    fn();
    leave(p);
}

static void pl_tick(void)
{
    for (int i = 0; i < nplugs; i++) {
        phx_plugin *p = plugs[i];
        if (p->state != PS_RUNNING || p->dead) continue;
        for (int k = 0; k < p->nfan; k++) {
            pfan *f = p->fan[k];
            if (f->touched && !f->c[FC_MODE].active) fan_apply(f, FM_AUTO, 0);
        }
        for (int k = 0; k < p->nrgb; k++) {
            prgb *l = p->rgb[k];
            if (l->touched && !l->c[RC_MODE].active) rgb_restore(l);
        }
        call(p, p->info.tick);
    }
}

static void pl_resume(void)
{
    for (int i = 0; i < nplugs; i++) call(plugs[i], plugs[i]->info.resume);
}

static char faulted[4 * 33];        /* for the marker: the plugins that crashed */
static volatile LONG unsaved;

/* Also runs from the crash handler's thread, after the built-in backends, for
   the plugins a fault kept it from reaching. Each plugin is shut down once. */
static void pl_shutdown(void)
{
    if (crash_mode && InterlockedExchange(&unsaved, 0)) cfg_set_str("plugins", "~loading", faulted[0] ? faulted : NULL);
    for (int i = nplugs - 1; i >= 0; i--) {
        phx_plugin *p = plugs[i];
        if (p->state != PS_RUNNING || p->dead || InterlockedExchange(&p->down, 1)) continue;
        int held = grab(p), was = p->busy;
        p->busy = 1;
        phx_plugin *outer = cur_swap(p);
        sync(p);
        /* AutoTDP leaves plugin clock domains to this after a crash */
        for (int k = 0; k < p->nclk; k++) {
            pclk *w = p->clk[k];
            if (!w->touched) continue;
            if (!w->pub->reset || w->pub->reset(w->pub)) w->pub->set_max(w->pub, w->pub->max_mhz);
            w->touched = 0;
        }
        for (int k = 0; k < p->nfan; k++) {
            pfan *f = p->fan[k];
            if (f->touched && f->pub->set_auto(f->pub) == 0) f->touched = 0;
        }
        for (int k = 0; k < p->nrgb; k++) {
            prgb *l = p->rgb[k];
            if (l->touched && l->pub->restore(l->pub) == 0) l->touched = 0;
        }
        if (p->info.shutdown) p->info.shutdown();
        cur_swap(outer);
        p->busy = was;
        if (held) LEAVE(p);
    }
}

/* ---------- crash attribution ---------- */

/* the plugin a code address belongs to: its DLL or, when deep, any DLL loaded
   from its own folder (libraries it ships with) */
static phx_plugin *owner_of(const void *addr, int deep)
{
    HMODULE m;
    wchar_t f[MAX_PATH];
    if (!addr || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     (LPCWSTR)addr, &m))
        return NULL;
    for (int i = 0; i < nplugs; i++)
        if (plugs[i]->h == m) return plugs[i];
    DWORD n = deep ? GetModuleFileNameW(m, f, MAX_PATH) : 0;
    if (!n || n >= MAX_PATH) return NULL;
    for (int i = 0; i < nplugs; i++) {
        phx_plugin *p = plugs[i];
        int k = lstrlenW(p->dir);
        if (!p->enabled_at_start) continue;
        if (!lstrcmpiW(f, p->path)) return p;    /* while its DllMain runs */
        if (lstrcmpiW(p->dir, plug_dir) && (int)n > k && f[k] == L'\\' &&
            CompareStringOrdinal(f, k, p->dir, k, TRUE) == CSTR_EQUAL)
            return p;
    }
    return NULL;
}

/* A fault in the C runtime or in Windows, on a thread the plugin started: look
   for the plugin's code up the stack. Every frame must stay inside the thread's
   stack, so a smashed stack ends the walk instead of faulting again. */
static phx_plugin *walk(const CONTEXT *ctx)
{
    static CONTEXT c;                  /* too big for a thread that just faulted */
    static volatile LONG walking;      /* ... so one thread at a time uses it */
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    DWORD64 lo = (DWORD64)tib->StackLimit, hi = (DWORD64)tib->StackBase;
    phx_plugin *p = NULL;
    if (!ctx || InterlockedExchange(&walking, 1)) return NULL;
    c = *ctx;
    for (int i = 0; i < 64 && !p; i++) {
        DWORD64 sp = c.Rsp, base = 0, fv = 0, est;
        PVOID hd;
        if (sp < lo || sp > hi - 8 || (sp & 7)) break;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &base, NULL);
        if (f) {
            int fr = ((const BYTE *)(base + f->UnwindData))[3] & 15;   /* UNWIND_INFO.FrameRegister */
            memcpy(&fv, (const BYTE *)&c + offsetof(CONTEXT, Rax) + 8 * fr, sizeof fv);
            if (fr && (fv < lo || fv >= hi)) break;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &hd, &est, NULL);
        } else if (i == 0) {
            c.Rip = *(const DWORD64 *)sp;      /* a leaf, or a call through a bad pointer */
            c.Rsp = sp + 8;
        } else {
            break;
        }
        if (!c.Rip || c.Rsp <= sp) break;
        p = owner_of((const void *)c.Rip, 1);
    }
    InterlockedExchange(&walking, 0);
    return p;
}

/* From the crash filter, on the faulting thread: the plugin whose DLL faulted,
   that the thread was calling, or whose code is up the stack is not called
   again, and the marker turns it off at the next start. */
void plugins_crash_note(EXCEPTION_POINTERS *ep)
{
    int first = !InterlockedExchange(&crash_mode, 1);
    const EXCEPTION_RECORD *er = ep ? ep->ExceptionRecord : NULL;
    /* after a stack overflow there is little stack left: no file names, stack walk or file writes */
    int tight = !er || er->ExceptionCode == EXCEPTION_STACK_OVERFLOW;
    phx_plugin *p = er ? owner_of(er->ExceptionAddress, !tight) : NULL;
    if (!p && tls != TLS_OUT_OF_INDEXES) p = TlsGetValue(tls);
    if (!p && !tight) p = walk(ep->ContextRecord);
    if (p) {
        p->dead = 1;
        if (!in_list(faulted, p->id) && lstrlenA(faulted) + lstrlenA(p->id) + 2 <= (int)sizeof faulted) {
            if (faulted[0]) lstrcatA(faulted, ",");
            lstrcatA(faulted, p->id);
        }
    }
    /* the marker now names what crashed, not what happened to be starting */
    int save = p || (first && marked);
    if (tight) { if (save) InterlockedExchange(&unsaved, 1); return; }
    if (save) cfg_set_str("plugins", "~loading", faulted[0] ? faulted : NULL);
    ph_log("crash at %p, %s%s", er->ExceptionAddress, p ? "in plugin " : "not in a plugin", p ? p->id : "");
}

ph_backend bk_plugins = {
    "plugins",
    L"Plugins",
    BK_PLUGIN,
    pl_probe,
    pl_init,
    pl_shutdown,
    pl_resume,
    pl_tick,
    0
};
