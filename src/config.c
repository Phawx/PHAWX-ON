#include "phawx.h"

static wchar_t dir[MAX_PATH];
static wchar_t ini[MAX_PATH];

const wchar_t *cfg_dir(void)
{
    if (!dir[0]) {
        GetModuleFileNameW(NULL, dir, MAX_PATH);
        wchar_t *s = dir + lstrlenW(dir);
        while (s > dir && s[-1] != L'\\') s--;
        *s = 0;
        if (s > dir) s[-1] = 0;
    }
    return dir;
}

/* Resolved on first use, so backends and plugins can read settings before cfg_load. */
static void cfg_open(void)
{
    if (ini[0]) return;
    ph_swprintf(ini, MAX_PATH, L"%s\\phawx.ini", cfg_dir());
    HANDLE f = CreateFileW(ini, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    else if (GetLastError() != ERROR_FILE_EXISTS) {
        wchar_t app[MAX_PATH];
        if (GetEnvironmentVariableW(L"APPDATA", app, MAX_PATH)) {
            ph_swprintf(dir, MAX_PATH, L"%s\\PhawxON", app);
            CreateDirectoryW(dir, NULL);
            ph_swprintf(ini, MAX_PATH, L"%s\\phawx.ini", dir);
        }
    }
}

const wchar_t *cfg_file(void)
{
    cfg_open();
    return ini;
}

void cfg_load(void)
{
    cfg_open();
    cfg_load_profile(NULL);
    cfg_dc_use(!g_plat.on_ac);
}

/* a value that does not fit is cut after its last whole UTF-8 character */
static void w2a(const wchar_t *w, char *a, int n)
{
    char t[512 * 3 + 1];
    if (n <= 0) return;
    int k = WideCharToMultiByte(CP_UTF8, 0, w, -1, t, sizeof t, NULL, NULL) - 1;
    if (k < 0) k = 0;
    if (k > n - 1) {
        k = n - 1;
        while (k > 0 && ((unsigned char)t[k] & 0xC0) == 0x80) k--;
    }
    for (int i = 0; i < k; i++) a[i] = t[i];
    a[k] = 0;
}

static void sec_for(const wchar_t *exe, wchar_t *out, int n)
{
    if (!exe || !exe[0]) lstrcpynW(out, L"global", n);
    else ph_swprintf(out, n, L"game:%s", exe);
}

int cfg_get_str(const char *sec, const char *key, char *out, int n)
{
    wchar_t ws[128], wk[128], wv[512];
    cfg_open();
    if (!MultiByteToWideChar(CP_UTF8, 0, sec, -1, ws, 128) || !MultiByteToWideChar(CP_UTF8, 0, key, -1, wk, 128)) {
        if (n) out[0] = 0;
        return 0;
    }
    DWORD r = GetPrivateProfileStringW(ws, wk, L"\x01", wv, 512, ini);
    if (r == 1 && wv[0] == 1) { if (n) out[0] = 0; return 0; }
    w2a(wv, out, n);
    return 1;
}

/* a value longer than 511 characters is refused rather than written empty */
int cfg_set_str(const char *sec, const char *key, const char *v)
{
    wchar_t ws[128], wk[128], wv[512];
    cfg_open();
    if (!MultiByteToWideChar(CP_UTF8, 0, sec, -1, ws, 128) || !MultiByteToWideChar(CP_UTF8, 0, key, -1, wk, 128))
        return -1;
    if (v && !MultiByteToWideChar(CP_UTF8, 0, v, -1, wv, 512)) return -1;
    return WritePrivateProfileStringW(ws, wk, v ? wv : NULL, ini) ? 0 : -1;
}

void cfg_clear(const char *sec)
{
    wchar_t ws[128];
    cfg_open();
    if (MultiByteToWideChar(CP_UTF8, 0, sec, -1, ws, 128)) WritePrivateProfileStringW(ws, NULL, NULL, ini);
}

static int parse_int(const char *b, int def)
{
    int neg = b[0] == '-', v = 0;
    const char *p = b + neg;
    if (*p < '0' || *p > '9') return def;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        for (p += 2; *p; p++) {
            int d = (*p >= '0' && *p <= '9') ? *p - '0' : (*p | 32) >= 'a' && (*p | 32) <= 'f' ? (*p | 32) - 'a' + 10 : -1;
            if (d < 0) break;
            v = v * 16 + d;
        }
    } else {
        for (; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
    }
    return neg ? -v : v;
}

int cfg_get_int(const char *sec, const char *key, int def)
{
    char b[32];
    if (!cfg_get_str(sec, key, b, sizeof b) || !b[0]) return def;
    return parse_int(b, def);
}

void cfg_set_int(const char *sec, const char *key, int v)
{
    char b[32];
    ph_snprintf(b, sizeof b, "%d", v);
    cfg_set_str(sec, key, b);
}

int cfg_any_profile(void)
{
    DWORD n = 32768, r;
    int hit = 0;
    cfg_open();
    wchar_t *b = ph_alloc(n * sizeof(wchar_t));
    if (!b) return 0;
    r = GetPrivateProfileSectionNamesW(b, n, ini);
    for (wchar_t *s = b; r && s < b + r && *s; s += lstrlenW(s) + 1)
        if (!_wcsnicmp(s, L"game:", 5)) { hit = 1; break; }
    ph_free(b);
    return hit;
}

int cfg_has_profile(const wchar_t *exe)
{
    wchar_t s[160], b[8];
    cfg_open();
    sec_for(exe, s, 160);
    return GetPrivateProfileSectionW(s, b, 8, ini) > 0;
}

/* ---------- on-battery values ----------
   val/active always hold the value in force, so the backends, AutoTDP and the
   reapply read them as before. While plugged in a battery value waits in dc_val;
   on battery the plugged-in one waits in ac_val/ac_active. Only rows saved per game
   profile take one, so a profile carries its own; nothing is read or done unless
   one exists or the power source changes. */

static int dc_in;

int cfg_dc_able(const ph_ctl *c)
{
    return c && c->key && (c->flags & CF_PROFILE) && !(c->flags & CF_NOSAVE) &&
           (c->type == CT_SLIDER || c->type == CT_TOGGLE || c->type == CT_CHOICE);
}

int cfg_dc_in(void) { return dc_in; }

void cfg_dc_use(int on)
{
    on = on != 0;
    if (on == dc_in) return;
    for (int i = 0, n = ph_ctl_count(); i < n; i++) {
        ph_ctl *c = ph_ctl_at(i);
        if (!c->dc) continue;
        if (on) {
            c->ac_val = c->val;
            c->ac_active = c->active;
            c->val = c->dc_val;
            c->active = 1;
        } else {
            /* a battery value something reset to Default is gone */
            if (c->active) c->dc_val = c->val;
            else c->dc = 0;
            c->val = c->ac_val;
            c->active = c->ac_active;
        }
    }
    dc_in = on;
}

void cfg_dc_power(int dc)
{
    if ((dc != 0) == dc_in) return;
    cfg_dc_use(dc);
    if (dc) return;     /* ph_apply_all puts the battery values in */
    /* back on the plugged-in set: what it has at Default goes back to Default */
    for (int i = 0, n = ph_ctl_count(); i < n; i++) {
        ph_ctl *c = ph_ctl_at(i);
        if (c->dc && !c->active && !autotdp_owns(c)) ph_ctl_reset(c);
    }
}

int cfg_dc_get(const ph_ctl *c, int set, int32_t *v)
{
    set = set != 0;
    if (cfg_dc_able(c) && c->dc && set != dc_in) {
        if (set) { *v = c->dc_val; return 1; }
        *v = c->ac_val;
        return c->ac_active;
    }
    *v = c->val;
    return c->active;
}

int cfg_dc_edit(ph_ctl *c, int set, int32_t v)
{
    set = set != 0;
    if (!cfg_dc_able(c)) return ph_ctl_apply(c, v);
    v = PH_CLAMP(v, c->min, c->max);
    if (set == dc_in) {
        /* the set in force: the hardware takes it */
        int fresh = set && !c->dc;
        if (fresh) { c->ac_val = c->val; c->ac_active = c->active; }
        int r = ph_ctl_apply(c, v);
        if (!r && fresh) c->dc = 1;
        return r;
    }
    if (set) { c->dc_val = v; c->dc = 1; }                  /* plugged in: kept for battery */
    else if (c->dc) { c->ac_val = v; c->ac_active = 1; }    /* on battery, which has its own */
    else return ph_ctl_apply(c, v);                         /* on battery, the same value */
    c->dirty = 1;
    return 0;
}

void cfg_dc_clear(ph_ctl *c, int set)
{
    if (!cfg_dc_able(c)) { ph_ctl_reset(c); return; }
    if (set) {
        if (!c->dc) return;
        c->dc = 0;
        c->dirty = 1;
        if (!dc_in) return;
        /* on battery: back to the plugged-in value */
        if (!c->ac_active) ph_ctl_reset(c);
        else if (ph_ctl_apply(c, c->ac_val)) c->val = c->ac_val;
        return;
    }
    if (dc_in && c->dc) { c->ac_active = 0; c->ac_val = c->def; c->dirty = 1; return; }
    ph_ctl_reset(c);
}

static void dc_sec_for(const wchar_t *exe, wchar_t *out, int n)
{
    if (!exe || !exe[0]) lstrcpynW(out, L"dc", n);
    else ph_swprintf(out, n, L"dc:game:%s", exe);
}

static wchar_t *trim(wchar_t *s)
{
    while (*s == L' ' || *s == L'\t') s++;
    wchar_t *e = s + lstrlenW(s);
    while (e > s && (e[-1] == L' ' || e[-1] == L'\t')) *--e = 0;
    return s;
}

#define DC_BUF 16384

/* the whole section in one read; called with the plugged-in set in force */
static void dc_load(const wchar_t *exe)
{
    wchar_t ws[168];
    for (int i = 0, n = ph_ctl_count(); i < n; i++) ph_ctl_at(i)->dc = 0;
    dc_sec_for(exe, ws, PH_ARRAY(ws));
    wchar_t *b = ph_alloc(DC_BUF * sizeof(wchar_t));
    if (!b) return;
    DWORD r = GetPrivateProfileSectionW(ws, b, DC_BUF, ini);
    for (wchar_t *l = b; r && l < b + r && *l; l += lstrlenW(l) + 1) {
        wchar_t *eq = l;
        while (*eq && *eq != L'=') eq++;
        if (!*eq || *l == L';') continue;
        *eq = 0;
        char k[128], v[32];
        w2a(trim(l), k, sizeof k);
        w2a(trim(eq + 1), v, sizeof v);
        *eq = L'=';
        ph_ctl *c = ph_ctl_find(k);
        if (!cfg_dc_able(c) || !v[0] || v[0] == 'd') continue;
        c->dc_val = PH_CLAMP(parse_int(v, c->def), c->min, c->max);
        c->dc = 1;
    }
    ph_free(b);
}

/* one write for the whole section, none when there is nothing to keep or remove */
static void dc_save(const wchar_t *exe)
{
    wchar_t ws[168];
    int cnt = 0, n = ph_ctl_count();
    for (int i = 0; i < n; i++) if (cfg_dc_able(ph_ctl_at(i)) && ph_ctl_at(i)->dc) cnt++;
    dc_sec_for(exe, ws, PH_ARRAY(ws));
    if (!cnt) {
        wchar_t t[4];
        if (GetPrivateProfileSectionW(ws, t, PH_ARRAY(t), ini) > 0) WritePrivateProfileStringW(ws, NULL, NULL, ini);
        return;
    }
    int cap = cnt * 112 + 2, k = 0;
    wchar_t *b = ph_alloc((size_t)cap * sizeof(wchar_t));
    if (!b) return;
    for (int i = 0; i < n; i++) {
        ph_ctl *c = ph_ctl_at(i);
        int32_t v;
        if (!cfg_dc_able(c) || !c->dc) continue;
        cfg_dc_get(c, 1, &v);
        int w = MultiByteToWideChar(CP_UTF8, 0, c->key, -1, b + k, cap - k - 16);
        if (w <= 0) continue;
        k += w - 1;
        ph_swprintf(b + k, cap - k, L"=%d", (int)v);
        k += lstrlenW(b + k) + 1;
    }
    b[k] = 0;
    WritePrivateProfileSectionW(ws, b, ini);
    ph_free(b);
}

/* loads into the plugged-in set and its battery values, then puts back whichever
   set was in force */
void cfg_load_profile(const wchar_t *exe)
{
    wchar_t ws[160];
    char s[200];
    int was = dc_in;
    cfg_dc_use(0);
    sec_for(exe, ws, 160);
    w2a(ws, s, sizeof s);
    for (int i = 0, n = ph_ctl_count(); i < n; i++) {
        ph_ctl *c = ph_ctl_at(i);
        if (!c->key || (c->flags & CF_NOSAVE)) continue;
        if (exe && !(c->flags & CF_PROFILE)) continue;
        char v[32];
        if (!cfg_get_str(s, c->key, v, sizeof v)) continue;
        if (!v[0] || v[0] == 'd') { c->active = 0; c->val = c->def; continue; }
        c->val = PH_CLAMP(parse_int(v, c->def), c->min, c->max);
        c->active = 1;
        c->dirty = 1;
    }
    dc_load(exe);
    cfg_dc_use(was);
}

void cfg_save_profile(const wchar_t *exe)
{
    wchar_t ws[160];
    char s[200];
    sec_for(exe, ws, 160);
    w2a(ws, s, sizeof s);
    for (int i = 0, n = ph_ctl_count(); i < n; i++) {
        ph_ctl *c = ph_ctl_at(i);
        if (!c->key || (c->flags & CF_NOSAVE)) continue;
        if (c->type == CT_ACTION || c->type == CT_INFO || c->type == CT_HEADER) continue;
        if (exe && !(c->flags & CF_PROFILE)) continue;
        int32_t v;
        if (cfg_dc_get(c, 0, &v)) cfg_set_int(s, c->key, v);
        else cfg_set_str(s, c->key, "d");
        c->dirty = 0;
    }
    dc_save(exe);
}

/* a game profile and its battery values */
void cfg_del_profile(const wchar_t *exe)
{
    wchar_t s[168];
    if (!exe || !exe[0]) return;
    cfg_open();
    sec_for(exe, s, PH_ARRAY(s));
    WritePrivateProfileStringW(s, NULL, NULL, ini);
    dc_sec_for(exe, s, PH_ARRAY(s));
    WritePrivateProfileStringW(s, NULL, NULL, ini);
}

void cfg_save(void)
{
    cfg_save_profile(NULL);
}
