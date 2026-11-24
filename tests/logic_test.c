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
HANDLE CreateFileW(const wchar_t *n, DWORD a, DWORD s, void *sa, DWORD c, DWORD f, HANDLE t) { return INVALID_HANDLE_VALUE; }
BOOL CloseHandle(HANDLE h) { return TRUE; }
DWORD GetLastError(void) { return ERROR_FILE_EXISTS; }
DWORD GetEnvironmentVariableW(const wchar_t *n, wchar_t *b, DWORD sz) { return 0; }
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

int main(void)
{
    test_dc();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
