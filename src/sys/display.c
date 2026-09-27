#include "phawx.h"

#define MAX_RATES 16
#define MAX_RES   24

typedef struct { int w, h; } res_t;

static wchar_t dev[CCHDEVICENAME];
static int rates[MAX_RATES], nrates;
static res_t modes[MAX_RES];
static int nmodes;
static const wchar_t *rate_ch[MAX_RATES + 1];
static const wchar_t *res_ch[MAX_RES + 1];
static int applied_rate, applied_res;

static int cur_mode(DEVMODEW *dm)
{
    ZeroMemory(dm, sizeof *dm);
    dm->dmSize = sizeof *dm;
    return EnumDisplaySettingsExW(dev, ENUM_CURRENT_SETTINGS, dm, 0) ? 0 : -1;
}

static int has_mode(int w, int h, int hz)
{
    DEVMODEW dm = { .dmSize = sizeof dm };
    for (DWORD i = 0; EnumDisplaySettingsExW(dev, i, &dm, 0); i++)
        if ((int)dm.dmPelsWidth == w && (int)dm.dmPelsHeight == h && (int)dm.dmDisplayFrequency == hz &&
            dm.dmBitsPerPel >= 24)
            return 1;
    return 0;
}

static int best_rate(int w, int h, int want)
{
    DEVMODEW dm = { .dmSize = sizeof dm };
    int best = 0, bd = 1 << 30, top = 0;
    for (DWORD i = 0; EnumDisplaySettingsExW(dev, i, &dm, 0); i++) {
        if ((int)dm.dmPelsWidth != w || (int)dm.dmPelsHeight != h || dm.dmBitsPerPel < 24) continue;
        int f = (int)dm.dmDisplayFrequency;
        if (f <= 1) continue;
        if (f > top) top = f;
        int d = f > want ? f - want : want - f;
        if (d < bd) { bd = d; best = f; }
    }
    return want > 0 ? best : top;
}

static int change(int w, int h, int hz)
{
    DEVMODEW cur;
    if (cur_mode(&cur)) return -1;
    if ((int)cur.dmPelsWidth == w && (int)cur.dmPelsHeight == h && (int)cur.dmDisplayFrequency == hz) return 0;
    DEVMODEW dm = { .dmSize = sizeof dm };
    dm.dmPelsWidth = w;
    dm.dmPelsHeight = h;
    dm.dmDisplayFrequency = hz;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    LONG r = ChangeDisplaySettingsExW(dev, &dm, NULL, 0, NULL);
    if (r != DISP_CHANGE_SUCCESSFUL) { ph_log("display: change %dx%d@%d failed %ld", w, h, hz, r); return -1; }
    return 0;
}

static void restore(void)
{
    ChangeDisplaySettingsExW(dev, NULL, NULL, 0, NULL);
    applied_rate = applied_res = 0;
}

static int set_rate(ph_ctl *c, int32_t v)
{
    (void)c;
    if (v < 0 || v >= nrates) return -1;
    DEVMODEW cur;
    if (cur_mode(&cur)) return -1;
    int w = (int)cur.dmPelsWidth, h = (int)cur.dmPelsHeight;
    int hz = has_mode(w, h, rates[v]) ? rates[v] : best_rate(w, h, rates[v]);
    if (!hz || change(w, h, hz)) return -1;
    applied_rate = 1;
    return 0;
}

static int get_rate(ph_ctl *c, int32_t *out)
{
    (void)c;
    DEVMODEW cur;
    if (cur_mode(&cur) || !nrates) return -1;
    int f = (int)cur.dmDisplayFrequency, bi = 0, bd = 1 << 30;
    for (int i = 0; i < nrates; i++) {
        int d = rates[i] > f ? rates[i] - f : f - rates[i];
        if (d < bd) { bd = d; bi = i; }
    }
    *out = bi;
    return 0;
}

static int set_res(ph_ctl *c, int32_t v)
{
    (void)c;
    if (v < 0 || v >= nmodes) return -1;
    DEVMODEW cur;
    if (cur_mode(&cur)) return -1;
    int w = modes[v].w, h = modes[v].h;
    int f = (int)cur.dmDisplayFrequency;
    int hz = has_mode(w, h, f) ? f : best_rate(w, h, 0);
    if (!hz || change(w, h, hz)) return -1;
    applied_res = 1;
    return 0;
}

static int get_res(ph_ctl *c, int32_t *out)
{
    (void)c;
    DEVMODEW cur;
    if (cur_mode(&cur)) return -1;
    for (int i = 0; i < nmodes; i++)
        if (modes[i].w == (int)cur.dmPelsWidth && modes[i].h == (int)cur.dmPelsHeight) { *out = i; return 0; }
    return -1;
}

static void fmt_cur(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c; (void)v;
    DEVMODEW cur;
    if (cur_mode(&cur)) { lstrcpynW(b, L"Unknown", n); return; }
    ph_swprintf(b, n, L"%lux%lu @ %lu Hz", cur.dmPelsWidth, cur.dmPelsHeight, cur.dmDisplayFrequency);
}

enum { K_RATE, K_HDR, K_RES, K_CUR, K_N };

static ph_ctl ctls[K_N] = {
    [K_RATE] = { .key = "display.refresh", .label = L"Refresh rate", .type = CT_CHOICE, .page = PG_QUICK, .order = 60,
                 .flags = CF_OPTIONAL | CF_PROFILE | CF_REAPPLY, .choices = rate_ch, .get = get_rate, .set = set_rate },
    [K_HDR] = { .key = NULL, .label = L"Display", .type = CT_HEADER, .page = PG_DISPLAY, .order = 0 },
    [K_RES] = { .key = "display.res", .label = L"Resolution", .type = CT_CHOICE, .page = PG_DISPLAY, .order = 10,
                .flags = CF_OPTIONAL | CF_PROFILE | CF_REAPPLY, .choices = res_ch, .get = get_res, .set = set_res },
    [K_CUR] = { .key = NULL, .label = L"Current mode", .type = CT_INFO, .page = PG_DISPLAY, .order = 20, .fmt = fmt_cur },
};

static int find_primary(void)
{
    DISPLAY_DEVICEW dd = { .cb = sizeof dd };
    for (DWORD i = 0; EnumDisplayDevicesW(NULL, i, &dd, 0); i++) {
        if ((dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) && (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) {
            lstrcpynW(dev, dd.DeviceName, PH_ARRAY(dev));
            return 0;
        }
        dd.cb = sizeof dd;
    }
    return -1;
}

static wchar_t *dup_label(const wchar_t *fmt, int a, int b)
{
    wchar_t *s = ph_alloc(24 * sizeof(wchar_t));
    if (s) ph_swprintf(s, 24, fmt, a, b);
    return s;
}

static void collect(void)
{
    DEVMODEW base = { .dmSize = sizeof base };
    if (!EnumDisplaySettingsExW(dev, ENUM_REGISTRY_SETTINGS, &base, 0) || base.dmPelsWidth == 0) cur_mode(&base);
    int bw = (int)base.dmPelsWidth, bh = (int)base.dmPelsHeight;
    int land = bw >= bh;
    DEVMODEW dm = { .dmSize = sizeof dm };
    nrates = nmodes = 0;
    for (DWORD i = 0; EnumDisplaySettingsExW(dev, i, &dm, 0); i++) {
        if (dm.dmBitsPerPel < 24) continue;
        int w = (int)dm.dmPelsWidth, h = (int)dm.dmPelsHeight, f = (int)dm.dmDisplayFrequency;
        if (w == bw && h == bh && f > 1) {
            int k = 0;
            while (k < nrates && rates[k] != f) k++;
            if (k == nrates && nrates < MAX_RATES) rates[nrates++] = f;
        }
        if (w < 640 || h < 480 || (w >= h) != land) continue;
        int k = 0;
        while (k < nmodes && (modes[k].w != w || modes[k].h != h)) k++;
        if (k == nmodes && nmodes < MAX_RES) { modes[nmodes].w = w; modes[nmodes].h = h; nmodes++; }
    }
    for (int i = 1; i < nrates; i++)
        for (int j = i; j > 0 && rates[j - 1] > rates[j]; j--) { int t = rates[j]; rates[j] = rates[j - 1]; rates[j - 1] = t; }
    for (int i = 1; i < nmodes; i++)
        for (int j = i; j > 0 && (int64_t)modes[j - 1].w * modes[j - 1].h < (int64_t)modes[j].w * modes[j].h; j--) {
            res_t t = modes[j]; modes[j] = modes[j - 1]; modes[j - 1] = t;
        }
    for (int i = 0; i < nrates; i++) rate_ch[i] = dup_label(L"%d Hz", rates[i], 0);
    rate_ch[nrates] = NULL;
    for (int i = 0; i < nmodes; i++) res_ch[i] = dup_label(L"%dx%d", modes[i].w, modes[i].h);
    res_ch[nmodes] = NULL;
}

int display_refresh(void)
{
    HMONITOR m = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi = { 0 };
    mi.cbSize = sizeof mi;
    if (!m || !GetMonitorInfoW(m, (MONITORINFO *)&mi)) return 0;
    DEVMODEW dm = { .dmSize = sizeof dm };
    if (!EnumDisplaySettingsExW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm, 0)) return 0;
    return dm.dmDisplayFrequency > 1 ? (int)dm.dmDisplayFrequency : 0;
}

static int d_probe(void)
{
    return find_primary() == 0;
}

static int d_init(void)
{
    collect();
    if (nrates < 2) ctls[K_RATE].flags |= CF_HIDDEN;
    if (nmodes < 2) ctls[K_RES].flags |= CF_HIDDEN;
    ph_register_ctls(ctls, K_N);
    return 0;
}

static void d_tick(void)
{
    int lost_rate = applied_rate && !ctls[K_RATE].active;
    int lost_res = applied_res && !ctls[K_RES].active;
    if (!lost_rate && !lost_res) return;
    restore();
    if (ctls[K_RES].active) set_res(&ctls[K_RES], ctls[K_RES].val);
    if (ctls[K_RATE].active) set_rate(&ctls[K_RATE], ctls[K_RATE].val);
}

static void d_shutdown(void)
{
    if (applied_rate || applied_res) restore();
}

ph_backend bk_display = {
    "display",
    L"Display",
    BK_SYS,
    d_probe,
    d_init,
    d_shutdown,
    NULL,
    d_tick,
    0
};
