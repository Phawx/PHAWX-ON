/* Tests of the parts of Phawx ON that need no hardware, built with the native
   compiler against tests/native/windows.h (profile files live in memory).

   make test */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <wctype.h>
#include "phawx.h"

/* ---------- the code under test ---------- */
#include "../src/config.c"
#include "../src/registry.c"
#include "../src/sessionlog.c"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---------- what they need from the rest of Phawx ON ---------- */

ph_platform g_plat;
HWND g_main;
ph_backend bk_drv, bk_intel, bk_amd, bk_winpower, bk_nvidia, bk_amdgpu, bk_intelgpu, bk_display, bk_tweaks,
    bk_devices, bk_hwinfo, bk_plugins;
static int owned;
int autotdp_owns(const ph_ctl *c) { (void)c; return owned; }
void wp_commit(void) {}
void ph_log(const char *fmt, ...) { (void)fmt; }
void *ph_alloc(size_t n) { return calloc(1, n); }
void ph_free(void *p) { free(p); }

/* the Windows %s (a wide string in a wide format) is %ls here */
int ph_swprintf(wchar_t *b, int n, const wchar_t *fmt, ...)
{
    wchar_t f[256];
    int k = 0;
    for (const wchar_t *p = fmt; *p && k < 250; p++) {
        f[k++] = *p;
        if (p[0] == L'%' && p[1] == L's') f[k++] = L'l';
    }
    f[k] = 0;
    va_list ap;
    va_start(ap, fmt);
    int r = vswprintf(b, (size_t)n, f, ap);
    va_end(ap);
    if (n > 0) b[n - 1] = 0;
    return r;
}

int ph_snprintf(char *b, int n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(b, (size_t)n, fmt, ap);
    va_end(ap);
    return r;
}

DWORD GetModuleFileNameW(HANDLE m, wchar_t *b, DWORD n) { (void)m; lstrcpynW(b, L"C:\\Phawx\\PhawxON.exe", (int)n); return (DWORD)lstrlenW(b); }
/* files: phawx.ini is always "there" (the ini is in memory); the session log's
   files are recorded, with what was written to the last one */
static wchar_t f_path[8][300];
static int f_n, f_open, f_closed, f_writes, f_dir_ok = 1;
static char f_data[65536];
static int f_len;
static HANDLE const F_LOG = (HANDLE)(intptr_t)0x10;
int ph_exe_dir(wchar_t *out, int n) { lstrcpynW(out, L"C:\\Phawx", n); return 0; }
HANDLE CreateFileW(const wchar_t *n, DWORD a, DWORD s, void *sa, DWORD c, DWORD f, HANDLE t)
{
    int l = lstrlenW(n);
    if (l > 10 && !wcscmp(n + l - 10, L"~write.tmp")) return f_dir_ok ? (HANDLE)(intptr_t)0x11 : INVALID_HANDLE_VALUE;
    if (l > 4 && !wcscmp(n + l - 4, L".csv")) {
        if (f_n < 8) lstrcpynW(f_path[f_n++], n, 300);
        f_open++;
        f_len = 0;
        return F_LOG;
    }
    return INVALID_HANDLE_VALUE;
}
BOOL CloseHandle(HANDLE h) { if (h == F_LOG) f_closed++; return TRUE; }
BOOL WriteFile(HANDLE h, const void *b, DWORD n, DWORD *w, void *ov)
{
    if (h != F_LOG || f_len + (int)n > (int)sizeof f_data) return FALSE;
    memcpy(f_data + f_len, b, n);
    f_len += (int)n;
    f_data[f_len] = 0;
    f_writes++;
    *w = n;
    return TRUE;
}
void GetLocalTime(SYSTEMTIME *t) { memset(t, 0, sizeof *t); t->wYear = 2026; t->wMonth = 11; t->wDay = 24; t->wHour = 14; t->wMinute = 3; t->wSecond = 5; }
DWORD GetLastError(void) { return ERROR_FILE_EXISTS; }
DWORD GetEnvironmentVariableW(const wchar_t *n, wchar_t *b, DWORD sz) { lstrcpynW(b, L"C:\\Users\\u\\AppData\\Local", (int)sz); return (DWORD)lstrlenW(b); }
BOOL CreateDirectoryW(const wchar_t *p, void *sa) { return TRUE; }

/* ASCII is all the tests use */
int WideCharToMultiByte(UINT cp, DWORD fl, const wchar_t *w, int nw, char *a, int na, const char *d, BOOL *u)
{
    int n = nw < 0 ? lstrlenW(w) + 1 : nw;
    if (n > na) return 0;
    for (int i = 0; i < n; i++) a[i] = (char)w[i];
    return n;
}

int MultiByteToWideChar(UINT cp, DWORD fl, const char *a, int na, wchar_t *w, int nw)
{
    int n = na < 0 ? (int)strlen(a) + 1 : na;
    if (n > nw) return 0;
    for (int i = 0; i < n; i++) w[i] = (unsigned char)a[i];
    return n;
}

/* ---------- an ini file in memory ---------- */

#define NSEC 32
#define NKEY 64
typedef struct { wchar_t name[160]; int n; wchar_t k[NKEY][128], v[NKEY][128]; } isec;
static isec ini_s[NSEC];
static int ini_n, ini_writes;

static isec *sec_find(const wchar_t *s, int add)
{
    for (int i = 0; i < ini_n; i++) if (!wcscasecmp(ini_s[i].name, s)) return &ini_s[i];
    if (!add || ini_n >= NSEC) return NULL;
    memset(&ini_s[ini_n], 0, sizeof ini_s[0]);
    lstrcpynW(ini_s[ini_n].name, s, 160);
    return &ini_s[ini_n++];
}

static void sec_del(isec *x)
{
    int i = (int)(x - ini_s);
    memmove(&ini_s[i], &ini_s[i + 1], (size_t)(ini_n - i - 1) * sizeof ini_s[0]);
    ini_n--;
}

static void ini_reset(void) { ini_n = 0; ini_writes = 0; }

DWORD GetPrivateProfileStringW(const wchar_t *sec, const wchar_t *key, const wchar_t *def, wchar_t *out, DWORD n, const wchar_t *file)
{
    isec *x = sec_find(sec, 0);
    for (int i = 0; x && i < x->n; i++)
        if (!wcscasecmp(x->k[i], key)) { lstrcpynW(out, x->v[i], (int)n); return (DWORD)lstrlenW(out); }
    lstrcpynW(out, def, (int)n);
    return (DWORD)lstrlenW(out);
}

BOOL WritePrivateProfileStringW(const wchar_t *sec, const wchar_t *key, const wchar_t *v, const wchar_t *file)
{
    ini_writes++;
    isec *x = sec_find(sec, key && v);
    if (!x) return TRUE;
    if (!key) { sec_del(x); return TRUE; }
    for (int i = 0; i < x->n; i++)
        if (!wcscasecmp(x->k[i], key)) {
            if (v) lstrcpynW(x->v[i], v, 128);
            else { memmove(&x->k[i], &x->k[i + 1], (size_t)(x->n - i - 1) * sizeof x->k[0]);
                   memmove(&x->v[i], &x->v[i + 1], (size_t)(x->n - i - 1) * sizeof x->v[0]); x->n--; }
            return TRUE;
        }
    if (v && x->n < NKEY) { lstrcpynW(x->k[x->n], key, 128); lstrcpynW(x->v[x->n++], v, 128); }
    return TRUE;
}

DWORD GetPrivateProfileSectionW(const wchar_t *sec, wchar_t *out, DWORD n, const wchar_t *file)
{
    isec *x = sec_find(sec, 0);
    DWORD k = 0;
    for (int i = 0; x && i < x->n; i++) {
        wchar_t line[260];
        swprintf(line, 260, L"%ls=%ls", x->k[i], x->v[i]);
        DWORD l = (DWORD)lstrlenW(line) + 1;
        if (k + l + 1 > n) break;
        memcpy(out + k, line, l * sizeof(wchar_t));
        k += l;
    }
    if (n) out[k] = 0;
    return k;
}

BOOL WritePrivateProfileSectionW(const wchar_t *sec, const wchar_t *data, const wchar_t *file)
{
    ini_writes++;
    isec *x = sec_find(sec, 1);
    x->n = 0;
    for (const wchar_t *l = data; *l; l += lstrlenW(l) + 1) {
        const wchar_t *eq = wcschr(l, L'=');
        if (!eq || x->n >= NKEY) continue;
        lstrcpynW(x->k[x->n], l, (int)(eq - l + 1));
        lstrcpynW(x->v[x->n++], eq + 1, 128);
    }
    return TRUE;
}

DWORD GetPrivateProfileSectionNamesW(wchar_t *out, DWORD n, const wchar_t *file)
{
    DWORD k = 0;
    for (int i = 0; i < ini_n; i++) {
        DWORD l = (DWORD)lstrlenW(ini_s[i].name) + 1;
        if (k + l + 1 > n) break;
        memcpy(out + k, ini_s[i].name, l * sizeof(wchar_t));
        k += l;
    }
    out[k] = 0;
    return k;
}

static const wchar_t *ini_get(const wchar_t *sec, const wchar_t *key)
{
    static wchar_t b[128];
    GetPrivateProfileStringW(sec, key, L"<none>", b, 128, NULL);
    return b;
}

static void ini_put(const wchar_t *sec, const wchar_t *key, const wchar_t *v) { WritePrivateProfileStringW(sec, key, v, NULL); }

/* ---------- controls with a fake piece of hardware behind them ---------- */

static int32_t hw_tdp = -1, hw_target = -1, hw_ui = -1;
static int released;

static int set_tdp(ph_ctl *c, int32_t v) { (void)c; hw_tdp = v; return 0; }
static void rel_tdp(ph_ctl *c) { (void)c; hw_tdp = -1; released++; }
static int set_target(ph_ctl *c, int32_t v) { (void)c; hw_target = v; return 0; }
static int set_ui(ph_ctl *c, int32_t v) { (void)c; hw_ui = v; return 0; }
static const wchar_t *const tch[] = { L"Refresh", L"Half", L"Custom", NULL };

static ph_ctl t_ctls[] = {
    { .key = "cpu.tdp", .label = L"TDP", .type = CT_SLIDER, .page = PG_QUICK, .flags = CF_OPTIONAL | CF_PROFILE,
      .min = 3000, .max = 30000, .step = 1000, .def = 15000, .set = set_tdp, .release = rel_tdp, .order = 20 },
    { .key = "auto.target", .label = L"Target", .type = CT_CHOICE, .page = PG_QUICK, .flags = CF_PROFILE,
      .choices = tch, .set = set_target, .order = 2 },
    { .key = "app.uiscale", .label = L"UI scale", .type = CT_SLIDER, .page = PG_SETTINGS, .min = 75, .max = 200,
      .step = 5, .def = 100, .set = set_ui, .order = 4 },
};
static ph_ctl *tdp = &t_ctls[0], *tgt = &t_ctls[1], *uis = &t_ctls[2];

static void ctl_fresh(void)
{
    for (int i = 0; i < PH_ARRAY(t_ctls); i++) {
        ph_ctl *c = &t_ctls[i];
        c->active = 0; c->val = c->def; c->dc = 0; c->ac_active = 0; c->ac_val = c->dc_val = 0;
    }
    hw_tdp = hw_target = hw_ui = -1;
    released = 0;
    dc_in = 0;
}

static void power(int ac)
{
    g_plat.on_ac = ac;
    cfg_dc_power(!ac);
    ph_apply_all(0);
}

/* ---------- on-battery values ---------- */

static void test_dc(void)
{
    int32_t v;
    printf("on-battery values\n");
    ini_reset();
    ph_register_ctls(t_ctls, PH_ARRAY(t_ctls));
    ctl_fresh();
    g_plat.on_ac = 1;

    /* a config from before: one value, whatever the power source */
    ini_put(L"global", L"cpu.tdp", L"12000");
    cfg_load();
    ph_apply_all(0);
    CHECK(hw_tdp == 12000 && !tdp->dc);
    power(0);
    CHECK(hw_tdp == 12000 && tdp->val == 12000 && cfg_dc_in());
    power(1);
    CHECK(hw_tdp == 12000 && !cfg_dc_in());
    cfg_save();
    CHECK(!wcscmp(ini_get(L"dc", L"cpu.tdp"), L"<none>"));
    CHECK(!sec_find(L"dc", 0));

    /* plugged in, editing the battery set: nothing changes now */
    CHECK(cfg_dc_edit(tdp, 1, 8000) == 0);
    CHECK(hw_tdp == 12000 && tdp->val == 12000 && tdp->dc);
    CHECK(cfg_dc_get(tdp, 1, &v) == 1 && v == 8000);
    CHECK(cfg_dc_get(tdp, 0, &v) == 1 && v == 12000);
    cfg_save();
    CHECK(!wcscmp(ini_get(L"global", L"cpu.tdp"), L"12000"));
    CHECK(!wcscmp(ini_get(L"dc", L"cpu.tdp"), L"8000"));

    /* unplugged: the battery value goes in, the plugged-in one waits */
    power(0);
    CHECK(hw_tdp == 8000 && tdp->val == 8000 && tdp->active);
    CHECK(cfg_dc_get(tdp, 0, &v) == 1 && v == 12000);
    cfg_save();
    CHECK(!wcscmp(ini_get(L"global", L"cpu.tdp"), L"12000"));
    CHECK(!wcscmp(ini_get(L"dc", L"cpu.tdp"), L"8000"));

    /* on battery: the battery set is in force, the plugged-in one is kept */
    CHECK(cfg_dc_edit(tdp, 1, 9000) == 0 && hw_tdp == 9000);
    CHECK(cfg_dc_edit(tdp, 0, 16000) == 0 && hw_tdp == 9000);
    CHECK(cfg_dc_get(tdp, 0, &v) == 1 && v == 16000);
    CHECK(cfg_dc_edit(tdp, 1, 99999) == 0 && hw_tdp == 30000);   /* clamped like any value */
    CHECK(cfg_dc_edit(tdp, 1, 9000) == 0);

    /* a control that is not saved per game has one value */
    CHECK(cfg_dc_edit(uis, 1, 150) == 0 && hw_ui == 150 && !uis->dc);
    CHECK(cfg_dc_get(uis, 0, &v) && v == 150);

    /* hold-to-reset on the battery set: the same as plugged in again */
    cfg_dc_clear(tdp, 1);
    CHECK(!tdp->dc && hw_tdp == 16000 && tdp->val == 16000);
    power(1);
    CHECK(hw_tdp == 16000);

    /* plugged-in Default, a battery value: the hardware goes back to Default on AC */
    cfg_dc_clear(tdp, 0);
    CHECK(!tdp->active && released == 1);
    CHECK(cfg_dc_edit(tdp, 1, 7000) == 0 && hw_tdp == -1);
    power(0);
    CHECK(hw_tdp == 7000);
    power(1);
    CHECK(hw_tdp == -1 && released == 2 && !tdp->active && tdp->dc);
    CHECK(cfg_dc_get(tdp, 1, &v) && v == 7000);

    /* AutoTDP holding the row: the swap leaves the hardware to it */
    owned = 1;
    power(0);
    power(1);
    CHECK(released == 2);
    owned = 0;

    /* saved, then read back on battery */
    CHECK(cfg_dc_edit(tgt, 0, 2) == 0 && cfg_dc_edit(tgt, 1, 1) == 0 && hw_target == 2);
    cfg_save();
    CHECK(!wcscmp(ini_get(L"global", L"cpu.tdp"), L"d"));
    CHECK(!wcscmp(ini_get(L"dc", L"auto.target"), L"1"));
    ctl_fresh();
    g_plat.on_ac = 0;
    cfg_load();
    ph_apply_all(0);
    CHECK(cfg_dc_in() && hw_tdp == 7000 && hw_target == 1);
    CHECK(cfg_dc_get(tdp, 0, &v) == 0 && cfg_dc_get(tgt, 0, &v) == 1 && v == 2);
    power(1);
    CHECK(hw_target == 2 && hw_tdp == -1);

    /* a hand-edited [dc] line with spaces, a Default, a stray key */
    ini_put(L"dc", L"cpu.tdp ", L" 5000");
    ini_put(L"dc", L"auto.target", L"d");
    ini_put(L"dc", L"nothing.here", L"3");
    ctl_fresh();
    g_plat.on_ac = 1;
    cfg_load();
    CHECK(tdp->dc && tdp->dc_val == 5000 && !tgt->dc);

    /* a game profile carries its own battery values, and loses them with it */
    ctl_fresh();
    g_plat.on_ac = 1;
    ini_reset();
    cfg_load();
    CHECK(cfg_dc_edit(tdp, 0, 20000) == 0 && cfg_dc_edit(tdp, 1, 10000) == 0);
    cfg_save_profile(L"game.exe");
    CHECK(!wcscmp(ini_get(L"game:game.exe", L"cpu.tdp"), L"20000"));
    CHECK(!wcscmp(ini_get(L"dc:game:game.exe", L"cpu.tdp"), L"10000"));
    CHECK(!sec_find(L"dc", 0));
    ctl_fresh();
    cfg_load_profile(NULL);
    CHECK(!tdp->dc);
    g_plat.on_ac = 0;
    cfg_dc_use(1);
    cfg_load_profile(L"game.exe");
    CHECK(cfg_dc_in() && tdp->val == 10000 && cfg_dc_get(tdp, 0, &v) && v == 20000);
    cfg_del_profile(L"game.exe");
    CHECK(!sec_find(L"game:game.exe", 0) && !sec_find(L"dc:game:game.exe", 0));

    /* nothing to keep: a save writes no [dc] at all */
    ctl_fresh();
    g_plat.on_ac = 1;
    ini_reset();
    cfg_load();
    cfg_save();
    CHECK(!sec_find(L"dc", 0));
}

/* ---------- hotkey steps ---------- */

static void test_step(void)
{
    printf("hotkey steps\n");
    ph_ctl c = { .min = 3000, .max = 30000, .step = 1000 };
    CHECK(ph_ctl_step(&c, 15000, 1, 3000, 30000) == 16000);
    CHECK(ph_ctl_step(&c, 15000, -1, 3000, 30000) == 14000);
    CHECK(ph_ctl_step(&c, 15400, 1, 3000, 30000) == 16000);    /* a reading between steps */
    CHECK(ph_ctl_step(&c, 15400, -1, 3000, 30000) == 15000);
    CHECK(ph_ctl_step(&c, 30000, 1, 3000, 30000) == 30000);
    CHECK(ph_ctl_step(&c, 3000, -1, 3000, 30000) == 3000);
    CHECK(ph_ctl_step(&c, 2500, -1, 3000, 30000) == 3000);
    ph_ctl g = { .min = 0, .max = 2000, .step = 50 };
    CHECK(ph_ctl_step(&g, 2000, -1, 300, 2000) == 1950);       /* no cap: from full speed */
    CHECK(ph_ctl_step(&g, 300, -1, 300, 2000) == 300);
    CHECK(ph_ctl_step(&g, 1975, 1, 300, 2000) == 2000);
}

/* ---------- session log ---------- */

static int count(const char *s, char ch) { int n = 0; for (; *s; s++) n += *s == ch; return n; }

static void test_slog(void)
{
    char b[320], h[256];
    wchar_t w[128];
    printf("session log\n");
    ph_slog_row r = { .exe = L"Elden,Ring.exe", .fps = 59.94f, .low1 = 48.2f, .target = 60, .cpu_mhz = 2400,
                      .gpu_mhz = 1600, .cores_on = 6, .cores_max = 8, .epp = 20, .power_mw = 9840,
                      .power_src = PSRC_SYSTEM, .tdp_mw = 15000, .trying = "cores", .bottleneck = 2 };
    int k = slog_text(b, sizeof b, &r, "14:03:05", 12);
    CHECK(k > 0 && !strcmp(b, "14:03:05,12,Elden_Ring.exe,59.9,48.2,60,2400,1600,6,8,20,9.8,system,15.0,cores,gpu\r\n"));
    slog_head(h, sizeof h);
    CHECK(count(h, ',') == count(b, ','));   /* every row has the header's columns */
    ph_slog_row u = { .exe = L"g.exe", .cores_on = -1, .epp = -1, .power_mw = -1, .tdp_mw = -1, .trying = "" };
    k = slog_text(b, sizeof b, &u, "00:00:00", 0);
    CHECK(k > 0 && !strcmp(b, "00:00:00,0,g.exe,,,,,,,,,,,,,\r\n"));
    CHECK(count(b, ',') == count(h, ','));
    CHECK(slog_text(b, 20, &r, "14:03:05", 12) == 0);   /* cut short: no line at all */

    slog_name(w, PH_ARRAY(w), 2026, 11, 24, 14, 3, 5, L"eldenring.exe");
    CHECK(!wcscmp(w, L"2026-11-24 14.03.05 eldenring.csv"));
    slog_name(w, PH_ARRAY(w), 2026, 1, 2, 3, 4, 5, L"a:b?.EXE");
    CHECK(!wcscmp(w, L"2026-01-02 03.04.05 a_b_.csv"));

    /* rows wait in memory, go out every few seconds and when the game changes */
    f_n = f_open = f_closed = f_writes = f_len = 0;
    ph_slog_row g = r;
    g.exe = L"one.exe";
    uint64_t t = 1000;
    for (int i = 0; i < 5; i++, t += 1000) slog_row(t, &g);
    CHECK(f_open == 1 && f_writes == 0 && buf);
    CHECK(!wcscmp(f_path[0], L"C:\\Phawx\\logs\\2026-11-24 14.03.05 one.csv"));
    slog_row(t, &g);                                      /* 5 s after the first */
    t += 1000;
    CHECK(f_writes == 1 && count(f_data, '\n') == 7);    /* the header and six rows */
    CHECK(!strncmp(f_data, "time,", 5));
    g.exe = L"two.exe";
    slog_row(t, &g);
    CHECK(f_closed == 1 && f_open == 2 && f_len == 0);   /* the new file has its header waiting */
    slog_close();
    CHECK(f_closed == 2 && !buf && count(f_data, '\n') == 2);
    slog_close();                                         /* twice is harmless */
    CHECK(f_closed == 2);

    /* the exe folder cannot be written: %LOCALAPPDATA% */
    f_dir_ok = 0;
    CHECK(slog_dir(w, PH_ARRAY(w)) == -1);
    slog_row(t, &g);
    slog_row(t + 1000, &g);
    CHECK(f_open == 2 && !buf);                           /* tried once for this game, nothing held */
    slog_close();
    f_dir_ok = 1;
}

int main(void)
{
    test_dc();
    test_step();
    test_slog();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
