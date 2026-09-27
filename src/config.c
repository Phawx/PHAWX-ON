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
}

static void w2a(const wchar_t *w, char *a, int n)
{
    if (n <= 0) return;
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, a, n, NULL, NULL)) a[0] = 0;
    a[n - 1] = 0;
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

void cfg_set_str(const char *sec, const char *key, const char *v)
{
    wchar_t ws[128], wk[128], wv[512];
    cfg_open();
    if (!MultiByteToWideChar(CP_UTF8, 0, sec, -1, ws, 128) || !MultiByteToWideChar(CP_UTF8, 0, key, -1, wk, 128))
        return;
    if (v && !MultiByteToWideChar(CP_UTF8, 0, v, -1, wv, 512)) wv[0] = 0;
    WritePrivateProfileStringW(ws, wk, v ? wv : NULL, ini);
}

void cfg_clear(const char *sec)
{
    wchar_t ws[128];
    cfg_open();
    if (MultiByteToWideChar(CP_UTF8, 0, sec, -1, ws, 128)) WritePrivateProfileStringW(ws, NULL, NULL, ini);
}

int cfg_get_int(const char *sec, const char *key, int def)
{
    char b[32];
    if (!cfg_get_str(sec, key, b, sizeof b) || !b[0]) return def;
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

void cfg_load_profile(const wchar_t *exe)
{
    wchar_t ws[160];
    char s[200];
    sec_for(exe, ws, 160);
    w2a(ws, s, sizeof s);
    for (int i = 0, n = ph_ctl_count(); i < n; i++) {
        ph_ctl *c = ph_ctl_at(i);
        if (!c->key || (c->flags & CF_NOSAVE)) continue;
        if (exe && !(c->flags & CF_PROFILE)) continue;
        char v[32];
        if (!cfg_get_str(s, c->key, v, sizeof v)) continue;
        if (!v[0] || v[0] == 'd') { c->active = 0; c->val = c->def; continue; }
        c->val = PH_CLAMP(cfg_get_int(s, c->key, c->def), c->min, c->max);
        c->active = 1;
        c->dirty = 1;
    }
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
        if (c->active) cfg_set_int(s, c->key, c->val);
        else cfg_set_str(s, c->key, "d");
        c->dirty = 0;
    }
}

void cfg_save(void)
{
    cfg_save_profile(NULL);
}
