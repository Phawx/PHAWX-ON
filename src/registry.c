#include "phawx.h"

extern ph_backend bk_drv, bk_intel, bk_amd, bk_winpower, bk_nvidia, bk_amdgpu,
    bk_intelgpu, bk_display, bk_tweaks, bk_devices, bk_hwinfo;

ph_backend *const ph_backends[] = {
    &bk_drv,
    &bk_intel,
    &bk_amd,
    &bk_winpower,
    &bk_amdgpu,
    &bk_nvidia,
    &bk_intelgpu,
    &bk_display,
    &bk_tweaks,
    &bk_devices,
    &bk_plugins,    /* plugins may look at what the built-ins registered */
    &bk_hwinfo,     /* last: its table shows the sensors everyone else registered */
    NULL
};

#define MAX_CTLS 1024
#define MAX_CLK  8

static ph_ctl *ctls[MAX_CTLS];
static int nctls;
static ph_clk *cpu_clks[MAX_CLK], *gpu_clks[MAX_CLK];
static int ncpu, ngpu, gpu_sel = -1;
static CRITICAL_SECTION lock;
static INIT_ONCE lock_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK lock_init(PINIT_ONCE o, PVOID p, PVOID *ctx)
{
    (void)o; (void)p; (void)ctx;
    InitializeCriticalSection(&lock);
    return TRUE;
}

static void L(void) { InitOnceExecuteOnce(&lock_once, lock_init, NULL, NULL); EnterCriticalSection(&lock); }
static void U(void) { LeaveCriticalSection(&lock); }

void ph_register_ctl(ph_ctl *c)
{
    if (!c) return;
    if (nctls >= MAX_CTLS) { ph_log("registry: no room for %s", c->key ? c->key : "a row"); return; }
    if (!c->step) c->step = 1;
    if (!c->active) c->val = c->def;
    if (c->type == CT_CHOICE && c->choices && c->max == 0) {
        int n = 0;
        while (c->choices[n]) n++;
        c->min = 0;
        c->max = n - 1;
    }
    if (c->type == CT_TOGGLE) { c->min = 0; c->max = 1; }
    int i = nctls++;
    while (i > 0 && (ctls[i - 1]->page > c->page ||
                     (ctls[i - 1]->page == c->page && ctls[i - 1]->order > c->order))) {
        ctls[i] = ctls[i - 1];
        i--;
    }
    ctls[i] = c;
}

void ph_register_ctls(ph_ctl *c, int n)
{
    for (int i = 0; i < n; i++) ph_register_ctl(&c[i]);
}

int ph_ctl_count(void) { return nctls; }
int ph_ctl_room(void) { return MAX_CTLS - nctls; }
ph_ctl *ph_ctl_at(int i) { return (i >= 0 && i < nctls) ? ctls[i] : NULL; }

ph_ctl *ph_ctl_find(const char *key)
{
    if (!key) return NULL;
    for (int i = 0; i < nctls; i++)
        if (ctls[i]->key && !lstrcmpA(ctls[i]->key, key)) return ctls[i];
    return NULL;
}

int ph_ctl_apply(ph_ctl *c, int32_t v)
{
    if (!c) return -1;
    if (c->type == CT_SLIDER || c->type == CT_CHOICE || c->type == CT_TOGGLE)
        v = PH_CLAMP(v, c->min, c->max);
    int r = 0;
    L();
    if (c->set) r = c->set(c, v);
    U();
    if (r == 0) {
        c->val = v;
        c->active = 1;
        c->dirty = 1;
    }
    return r;
}

void ph_ctl_reset(ph_ctl *c)
{
    if (!c) return;
    c->active = 0;
    c->val = c->def;
    c->dirty = 1;
    if (!(c->flags & CF_OPTIONAL) && c->set) {
        L();
        c->set(c, c->def);
        U();
    } else if ((c->flags & CF_OPTIONAL) && c->release) {
        L();
        c->release(c);
        U();
    }
}

void ph_apply_all(int reapply_only)
{
    for (int i = 0; i < nctls; i++) {
        ph_ctl *c = ctls[i];
        if (!c->active || !c->set || (c->flags & CF_HIDDEN)) continue;
        if (c->type == CT_ACTION || c->type == CT_INFO || c->type == CT_HEADER || c->type == CT_CURVE) continue;
        if (reapply_only && !(c->flags & CF_REAPPLY)) continue;
        if (autotdp_owns(c)) continue;
        L();
        c->set(c, c->val);
        U();
    }
    if (!reapply_only) ph_backends_tick();
    wp_commit();
}

static void add_clk(ph_clk **arr, int *n, ph_clk *d)
{
    if (*n >= MAX_CLK || !d) return;
    int i = (*n)++;
    while (i > 0 && arr[i - 1]->prio < d->prio) { arr[i] = arr[i - 1]; i--; }
    arr[i] = d;
}

void ph_register_cpu_clk(ph_clk *d) { add_clk(cpu_clks, &ncpu, d); }
void ph_register_gpu_clk(ph_clk *d) { add_clk(gpu_clks, &ngpu, d); }
ph_clk *ph_cpu_clk(void) { return ncpu ? cpu_clks[0] : NULL; }
ph_clk *ph_gpu_clk(void) { return ngpu ? gpu_clks[(gpu_sel >= 0 && gpu_sel < ngpu) ? gpu_sel : 0] : NULL; }
ph_clk *ph_gpu_clk_at(int i) { return (i >= 0 && i < ngpu) ? gpu_clks[i] : NULL; }
int ph_gpu_clk_count(void) { return ngpu; }
void ph_select_gpu_clk(int i) { gpu_sel = i; }

void ph_backends_init(void)
{
    for (int i = 0; ph_backends[i]; i++) {
        ph_backend *b = ph_backends[i];
        b->alive = 0;
        if (b->probe && !b->probe()) continue;
        if (b->init && b->init() != 0) { ph_log("backend %s init failed", b->id); continue; }
        b->alive = 1;
        ph_log("backend %s ready", b->id);
    }
}

/* Built-in hardware first, so a plugin that faults cannot stop it, then plugins
   while PawnIO is still open, then the driver. The crash handler runs this again
   for whatever a fault interrupted: a built-in is never called twice, the plugin
   host skips plugins that are done or that crashed. */
void ph_backends_shutdown(void)
{
    for (int pass = 0; pass < 3; pass++)
        for (int i = PH_ARRAY(ph_backends) - 2; i >= 0; i--) {
            ph_backend *b = ph_backends[i];
            int k = b->kind == BK_PLUGIN ? 1 : b->kind == BK_DRIVER ? 2 : 0;
            if (k != pass) continue;
            if (k == 1 ? !b->alive : !InterlockedExchange(&b->alive, 0)) continue;
            if (b->shutdown) b->shutdown();
            b->alive = 0;
        }
}

void ph_backends_resume(void)
{
    for (int i = 0; ph_backends[i]; i++)
        if (ph_backends[i]->alive && ph_backends[i]->resume) ph_backends[i]->resume();
}

void ph_backends_tick(void)
{
    for (int i = 0; ph_backends[i]; i++)
        if (ph_backends[i]->alive && ph_backends[i]->tick) ph_backends[i]->tick();
}
