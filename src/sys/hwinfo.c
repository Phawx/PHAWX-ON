#include "phawx.h"
#include <string.h>

/* The System page's layout and its Hardware info table.

   System is made of sections: Power (winpower.c, 10), Fan control (100), Lighting
   (2000), Windows (tweaks.c, 3000), one per plugin (5000+), Hardware info (30000).
   A section without rows is not shown, so Fan control and Lighting appear only when
   a fan or a light was found.

   The table has its own rows (system, processor, memory, battery, software) and a
   copy of every sensor row the other backends and plugins registered on the CPU,
   GPU, Display and System pages, so it shows everything Phawx ON can read. */

enum { O_SEC = 30000, O_SYS = 30010, O_CPU = 30100, O_MEM = 30300, O_GPU = 30400, O_DISP = 30600,
       O_DEV = 30700, O_BAT = 30800, O_SW = 30900 };

#define MAX_PROXY 64

static ph_ctl sec_fans = { .label = L"Fan control", .type = CT_HEADER, .page = PG_SYSTEM, .order = 100, .flags = CF_SECTION };
static ph_ctl sec_lights = { .label = L"Lighting", .type = CT_HEADER, .page = PG_SYSTEM, .order = 2000, .flags = CF_SECTION };

/* ---------- live values, refreshed by the tick ---------- */

static wchar_t s_bios[64], s_win[64], s_cores[48], s_types[48];
static int cpu_load = -1, mem_pct = -1, bat_pct = -1, bat_secs = -1, bat_flag, on_ac = -1, bat_mw = -1;
static uint64_t mem_used, mem_total;
static ULONGLONG last_idle, last_total;

static uint64_t ft64(FILETIME f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }

static void sample(void)
{
    FILETIME idle, kern, user;
    if (GetSystemTimes(&idle, &kern, &user)) {
        uint64_t i = ft64(idle), t = ft64(kern) + ft64(user);   /* kernel time includes idle */
        if (last_total && t > last_total) cpu_load = (int)(100 - (i - last_idle) * 100 / (t - last_total));
        last_idle = i;
        last_total = t;
    }
    MEMORYSTATUSEX m = { sizeof m };
    if (GlobalMemoryStatusEx(&m)) {
        mem_total = m.ullTotalPhys;
        mem_used = m.ullTotalPhys - m.ullAvailPhys;
        mem_pct = (int)m.dwMemoryLoad;
    }
    SYSTEM_POWER_STATUS ps;
    if (GetSystemPowerStatus(&ps)) {
        on_ac = ps.ACLineStatus == 1 ? 1 : ps.ACLineStatus == 0 ? 0 : -1;
        bat_flag = ps.BatteryFlag;
        bat_pct = ps.BatteryLifePercent <= 100 ? ps.BatteryLifePercent : -1;
        bat_secs = ps.BatteryLifeTime != (DWORD)-1 ? (int)ps.BatteryLifeTime : -1;
    }
    bat_mw = ph_battery_mw();
}

/* ---------- formatters ---------- */

static int ok(ph_ctl *c, int32_t *out) { (void)c; *out = 0; return 0; }
static void f_str(const ph_ctl *c, int32_t v, wchar_t *b, int n) { (void)v; lstrcpynW(b, c->ctx, n); }

static void f_load(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c; (void)v;
    if (cpu_load < 0) lstrcpynW(b, L"--", n);
    else ph_swprintf(b, n, L"%d%%", PH_CLAMP(cpu_load, 0, 100));
}

static void f_clock(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c; (void)v;
    ph_clk *d = ph_cpu_clk();
    int mhz = 0;
    if (!d || !d->cur || d->cur(d, &mhz) || mhz <= 0) lstrcpynW(b, L"--", n);
    else ph_swprintf(b, n, L"%d MHz", mhz);
}

static void gb(wchar_t *b, int n, uint64_t bytes)
{
    uint64_t t = bytes * 10 / (1024ull * 1024 * 1024);
    ph_swprintf(b, n, L"%u.%u", (unsigned)(t / 10), (unsigned)(t % 10));
}

static void f_mem(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c; (void)v;
    wchar_t u[16], t[16];
    if (!mem_total) { lstrcpynW(b, L"--", n); return; }
    gb(u, PH_ARRAY(u), mem_used);
    gb(t, PH_ARRAY(t), mem_total);
    ph_swprintf(b, n, L"%s of %s GB (%d%%)", u, t, mem_pct);
}

static void f_source(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c; (void)v;
    lstrcpynW(b, on_ac > 0 ? L"AC power" : on_ac == 0 ? L"Battery" : L"--", n);
}

static int has_battery(void) { return !(bat_flag & 128) && bat_flag != 255; }

static void f_battery(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c; (void)v;
    if (bat_pct < 0) { lstrcpynW(b, L"--", n); return; }
    if (bat_flag & 8) ph_swprintf(b, n, L"%d%%, charging", bat_pct);
    else if (on_ac == 0 && bat_secs > 0) ph_swprintf(b, n, L"%d%%, %d h %02d min left", bat_pct, bat_secs / 3600, bat_secs / 60 % 60);
    else ph_swprintf(b, n, L"%d%%", bat_pct);
}

/* the whole machine, while it runs on battery */
static void f_draw(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c; (void)v;
    if (bat_mw < 0) lstrcpynW(b, L"--", n);
    else ph_swprintf(b, n, L"%d.%d W", bat_mw / 1000, bat_mw % 1000 / 100);
}

static void f_pawnio(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c; (void)v;
    switch (drv_state()) {
    case DRV_OK:
        if (drv_version()[0]) ph_swprintf(b, n, L"%hs, running", drv_version());
        else lstrcpynW(b, L"Running", n);
        break;
    case DRV_NOMODULES: lstrcpynW(b, L"Modules missing", n); break;
    case DRV_STOPPED: lstrcpynW(b, L"Driver not running", n); break;
    default: lstrcpynW(b, L"Not installed", n); break;
    }
}

static void f_pm(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c; (void)v;
    const wchar_t *ver = fps_version();
    switch (fps_status()) {
    case 2: ph_swprintf(b, n, L"%s, running", ver[0] ? ver : L"PresentMon"); break;
    case 1: lstrcpynW(b, L"Service not running", n); break;
    case 0: lstrcpynW(b, L"Not installed", n); break;
    default: lstrcpynW(b, L"Checking\x2026", n); break;
    }
}

static void f_fps(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c; (void)v;
    ph_fps f;
    if (fps_sample(&f) && f.fps > 0.5f) ph_swprintf(b, n, L"%d FPS \x00B7 %s", (int)(f.fps + 0.5f), f.exe);
    else lstrcpynW(b, L"No game", n);
}

/* ---------- copies of other rows ---------- */

typedef struct proxy {
    ph_ctl c;
    ph_ctl *src;
    wchar_t label[64];
} proxy;

static proxy px[MAX_PROXY];
static int npx;

static int px_get(ph_ctl *c, int32_t *out) { ph_ctl *s = ((proxy *)c->ctx)->src; return s->get(s, out); }

static void px_fmt(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    ph_ctl *s = ((proxy *)c->ctx)->src;
    if (s->fmt) { s->fmt(s, v, b, n); return; }
    const wchar_t *u = s->unit ? s->unit : L"";
    ph_swprintf(b, n, u[0] && u[0] != L'%' ? L"%d %s" : L"%d%s", v, u);
}

/* the title of the part of the page a row is in, e.g. the fan's name */
static const wchar_t *header_of(const ph_ctl *c)
{
    const wchar_t *h = NULL;
    for (int i = 0, k = ph_ctl_count(); i < k; i++) {
        ph_ctl *o = ph_ctl_at(i);
        if (o == c) break;
        if (o->page == c->page && o->type == CT_HEADER) h = o->label;
    }
    return h;
}

static void sync_proxies(void)
{
    for (int i = 0; i < npx; i++) {
        uint16_t hide = px[i].src->flags & (CF_HIDDEN | CF_ADVANCED);
        px[i].c.flags = (uint16_t)((px[i].c.flags & ~(CF_HIDDEN | CF_ADVANCED)) | hide);
    }
}

static void add_proxies(int page, int16_t order)
{
    int16_t o = order;
    for (int i = 0, k = ph_ctl_count(); i < k && npx < MAX_PROXY; i++) {
        ph_ctl *s = ph_ctl_at(i);
        if (!s || s->page != page || s->type != CT_INFO || (s->flags & CF_TABLE) || !s->label) continue;
        proxy *p = &px[npx++];
        p->src = s;
        const wchar_t *h = header_of(s);
        /* "Fan speed" says little in a table: name the fan instead */
        if (page == PG_SYSTEM && h && !lstrcmpW(s->label, L"Fan speed")) lstrcpynW(p->label, h, PH_ARRAY(p->label));
        else lstrcpynW(p->label, s->label, PH_ARRAY(p->label));
        p->c = (ph_ctl){ .label = p->label, .type = CT_INFO, .page = PG_SYSTEM, .order = ++o, .flags = CF_TABLE,
                         .unit = s->unit, .get = s->get ? px_get : NULL, .fmt = px_fmt, .ctx = p, .val = s->val };
    }
}

/* ---------- registration ---------- */

#define ROW(l, o, g, f, x) { .label = l, .type = CT_INFO, .page = PG_SYSTEM, .order = o, .flags = CF_TABLE, .get = g, \
                             .fmt = f, .ctx = x }
#define HDR(l, o) { .label = l, .type = CT_HEADER, .page = PG_SYSTEM, .order = o }

static ph_ctl sec_hw = { .label = L"Hardware info", .type = CT_HEADER, .page = PG_SYSTEM, .order = O_SEC, .flags = CF_SECTION };

enum { R_SYS, R_MAKER, R_MODEL, R_BOARD, R_BIOS, R_WIN,
       R_CPU, R_NAME, R_CORES, R_TYPES, R_LOAD, R_CLOCK,
       R_MEMH, R_MEM,
       R_GPU, R_DISP, R_DEV,
       R_BATH, R_SRC, R_BAT, R_DRAW,
       R_SWH, R_PAWN, R_PM, R_FPS, R_N };

static ph_ctl rows[R_N] = {
    [R_SYS] = HDR(L"System", O_SYS),
    [R_MAKER] = ROW(L"Manufacturer", O_SYS + 1, ok, f_str, g_plat.maker),
    [R_MODEL] = ROW(L"Model", O_SYS + 2, ok, f_str, g_plat.product),
    [R_BOARD] = ROW(L"Board", O_SYS + 3, ok, f_str, g_plat.board),
    [R_BIOS] = ROW(L"BIOS", O_SYS + 4, ok, f_str, s_bios),
    [R_WIN] = ROW(L"Windows", O_SYS + 5, ok, f_str, s_win),
    [R_CPU] = HDR(L"Processor", O_CPU),
    [R_NAME] = ROW(L"Name", O_CPU + 1, ok, f_str, g_plat.cpu_name),
    [R_CORES] = ROW(L"Cores", O_CPU + 2, ok, f_str, s_cores),
    [R_TYPES] = ROW(L"Core types", O_CPU + 3, ok, f_str, s_types),
    [R_LOAD] = ROW(L"Load", O_CPU + 4, ok, f_load, NULL),
    [R_CLOCK] = ROW(L"Clock", O_CPU + 5, ok, f_clock, NULL),
    [R_MEMH] = HDR(L"Memory", O_MEM),
    [R_MEM] = ROW(L"In use", O_MEM + 1, ok, f_mem, NULL),
    [R_GPU] = HDR(L"Graphics", O_GPU),
    [R_DISP] = HDR(L"Display", O_DISP),
    [R_DEV] = HDR(L"Fans and devices", O_DEV),
    [R_BATH] = HDR(L"Power supply", O_BAT),
    [R_SRC] = ROW(L"Source", O_BAT + 1, ok, f_source, NULL),
    [R_BAT] = ROW(L"Battery", O_BAT + 2, ok, f_battery, NULL),
    [R_DRAW] = ROW(L"Battery draw", O_BAT + 3, ok, f_draw, NULL),
    [R_SWH] = HDR(L"Software", O_SW),
    [R_PAWN] = ROW(L"PawnIO", O_SW + 1, ok, f_pawnio, NULL),
    [R_PM] = ROW(L"PresentMon", O_SW + 2, ok, f_pm, NULL),
    [R_FPS] = ROW(L"Frame rate", O_SW + 3, ok, f_fps, NULL),
};

static void read_strings(void)
{
    wchar_t v[64], d[32], name[64];
    DWORD build = 0, ubr = 0;
    if (!reg_get_str(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"BIOSVersion", v, PH_ARRAY(v)) && v[0]) {
        if (!reg_get_str(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"BIOSReleaseDate", d, PH_ARRAY(d)) && d[0])
            ph_swprintf(s_bios, PH_ARRAY(s_bios), L"%s (%s)", v, d);
        else
            lstrcpynW(s_bios, v, PH_ARRAY(s_bios));
    }
    const wchar_t *nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    wchar_t cb[16];
    if (!reg_get_str(HKEY_LOCAL_MACHINE, nt, L"CurrentBuild", cb, PH_ARRAY(cb))) build = (DWORD)_wtoi(cb);
    reg_get_dword(HKEY_LOCAL_MACHINE, nt, L"UBR", &ubr);
    /* ProductName still says Windows 10 on Windows 11 */
    lstrcpynW(name, build >= 22000 ? L"Windows 11" : L"Windows 10", PH_ARRAY(name));
    if (!reg_get_str(HKEY_LOCAL_MACHINE, nt, L"DisplayVersion", v, PH_ARRAY(v)) && v[0])
        ph_swprintf(s_win, PH_ARRAY(s_win), L"%s %s (%lu.%lu)", name, v, build, ubr);
    else if (build)
        ph_swprintf(s_win, PH_ARRAY(s_win), L"%s (%lu.%lu)", name, build, ubr);

    if (g_plat.ncores > 0) ph_swprintf(s_cores, PH_ARRAY(s_cores), L"%d cores, %d threads", g_plat.ncores, g_plat.nlogical);
    if (g_plat.hybrid) {
        /* physical cores per efficiency class */
        uint8_t seen[256] = { 0 };
        int p = 0, e = 0;
        for (int i = 0; i < g_plat.nlogical && i < 256; i++) {
            int core = g_plat.core_of[i];
            if (seen[core]) continue;
            seen[core] = 1;
            if (g_plat.cls[i] == g_plat.max_class) p++;
            else e++;
        }
        ph_swprintf(s_types, PH_ARRAY(s_types), L"%d performance, %d efficiency", p, e);
    }
}

static int hw_probe(void) { return 1; }

static int hw_init(void)
{
    ph_register_ctl(&sec_fans);
    ph_register_ctl(&sec_lights);
    read_strings();
    sample();
    if (!g_plat.maker[0]) rows[R_MAKER].flags |= CF_HIDDEN;
    if (!g_plat.product[0]) rows[R_MODEL].flags |= CF_HIDDEN;
    if (!g_plat.board[0]) rows[R_BOARD].flags |= CF_HIDDEN;
    if (!s_bios[0]) rows[R_BIOS].flags |= CF_HIDDEN;
    if (!s_win[0]) rows[R_WIN].flags |= CF_HIDDEN;
    if (!g_plat.cpu_name[0]) rows[R_NAME].flags |= CF_HIDDEN;
    if (!s_cores[0]) rows[R_CORES].flags |= CF_HIDDEN;
    if (!s_types[0]) rows[R_TYPES].flags |= CF_HIDDEN;
    if (!ph_cpu_clk() || !ph_cpu_clk()->cur) rows[R_CLOCK].flags |= CF_HIDDEN;
    if (!has_battery()) rows[R_BAT].flags |= CF_HIDDEN;
    if (bat_mw < 0) rows[R_DRAW].flags |= CF_HIDDEN;
    /* the copies first: they look at what is registered, not at this table */
    add_proxies(PG_CPU, O_CPU + 10);
    add_proxies(PG_GPU, O_GPU);
    add_proxies(PG_DISPLAY, O_DISP);
    add_proxies(PG_SYSTEM, O_DEV);
    sync_proxies();
    ph_register_ctl(&sec_hw);
    ph_register_ctls(rows, R_N);
    for (int i = 0; i < npx; i++) ph_register_ctl(&px[i].c);
    return 0;
}

static void hw_tick(void)
{
    sample();
    sync_proxies();
    if (has_battery()) rows[R_BAT].flags &= (uint16_t)~CF_HIDDEN;
    else rows[R_BAT].flags |= CF_HIDDEN;
    if (bat_mw >= 0) rows[R_DRAW].flags &= (uint16_t)~CF_HIDDEN;
    else rows[R_DRAW].flags |= CF_HIDDEN;
}

ph_backend bk_hwinfo = {
    "hwinfo",
    L"Hardware info",
    BK_SYS,
    hw_probe,
    hw_init,
    NULL,
    NULL,
    hw_tick,
    0
};
