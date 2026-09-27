#include "phawx.h"
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>
#include <winver.h>
#include <objbase.h>
#include <shlobj.h>

static HANDLE heap;

void *ph_alloc(size_t n)
{
    if (!heap) heap = GetProcessHeap();
    return HeapAlloc(heap, HEAP_ZERO_MEMORY, n);
}

void ph_free(void *p)
{
    if (p) HeapFree(heap ? heap : GetProcessHeap(), 0, p);
}

int ph_snprintf(char *b, int n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = _vsnprintf(b, n, fmt, ap);
    va_end(ap);
    if (n > 0) b[n - 1] = 0;
    return r;
}

int ph_swprintf(wchar_t *b, int n, const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = _vsnwprintf(b, n, fmt, ap);
    va_end(ap);
    if (n > 0) b[n - 1] = 0;
    return r;
}

static int log_on = -1;

void ph_log(const char *fmt, ...)
{
    if (log_on < 0) log_on = GetEnvironmentVariableA("PHAWX_LOG", NULL, 0) > 0;
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int k = _vsnprintf(b, sizeof b - 3, fmt, ap);
    va_end(ap);
    if (k < 0 || k > (int)sizeof b - 3) k = sizeof b - 3;
    b[k++] = '\r';
    b[k++] = '\n';
    b[k] = 0;
    OutputDebugStringA(b);
    if (log_on) {
        wchar_t p[MAX_PATH];
        ph_swprintf(p, MAX_PATH, L"%s\\phawx.log", cfg_dir());
        HANDLE f = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w;
            WriteFile(f, b, k, &w, NULL);
            CloseHandle(f);
        }
    }
}

uint64_t ph_ms(void) { return GetTickCount64(); }

double ph_qpc_ms(void)
{
    static double inv;
    LARGE_INTEGER t;
    if (!inv) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        inv = 1000.0 / (double)f.QuadPart;
    }
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * inv;
}

int ph_run(const wchar_t *cmdline, int wait_ms)
{
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    wchar_t buf[2048], app[MAX_PATH], *ap = NULL;
    lstrcpynW(buf, cmdline, PH_ARRAY(buf));
    /* bare program names resolve to System32 only, never the exe or current directory */
    int n = 0;
    while (buf[n] && buf[n] != L' ' && buf[n] != L'"') n++;
    if (n && buf[n] != L'"' && !wmemchr(buf, L'\\', n) && !wmemchr(buf, L'/', n) && !wmemchr(buf, L':', n)) {
        UINT sl = GetSystemDirectoryW(app, MAX_PATH);
        if (!sl || sl + n + 2 > MAX_PATH) return -1;
        app[sl] = L'\\';
        wmemcpy(app + sl + 1, buf, n);
        app[sl + 1 + n] = 0;
        ap = app;
    }
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(ap, buf, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return -1;
    DWORD code = 0;
    if (wait_ms) {
        if (WaitForSingleObject(pi.hProcess, wait_ms < 0 ? INFINITE : (DWORD)wait_ms) != WAIT_OBJECT_0 ||
            !GetExitCodeProcess(pi.hProcess, &code))
            code = (DWORD)-1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

int reg_get_dword(HKEY root, const wchar_t *path, const wchar_t *name, DWORD *v)
{
    DWORD sz = sizeof *v, t;
    return RegGetValueW(root, path, name, RRF_RT_REG_DWORD, &t, v, &sz) == ERROR_SUCCESS ? 0 : -1;
}

int reg_set_dword(HKEY root, const wchar_t *path, const wchar_t *name, DWORD v)
{
    HKEY k;
    if (RegCreateKeyExW(root, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return -1;
    LONG r = RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof v);
    RegCloseKey(k);
    return r ? -1 : 0;
}

int reg_del_value(HKEY root, const wchar_t *path, const wchar_t *name)
{
    HKEY k;
    if (RegOpenKeyExW(root, path, 0, KEY_SET_VALUE, &k)) return -1;
    LONG r = RegDeleteValueW(k, name);
    RegCloseKey(k);
    return r ? -1 : 0;
}

int reg_get_str(HKEY root, const wchar_t *path, const wchar_t *name, wchar_t *out, DWORD cch)
{
    DWORD sz = cch * sizeof(wchar_t);
    out[0] = 0;
    if (RegGetValueW(root, path, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, NULL, out, &sz) == ERROR_SUCCESS)
        return 0;
    out[0] = 0;
    return -1;
}

int reg_set_str(HKEY root, const wchar_t *path, const wchar_t *name, const wchar_t *v)
{
    HKEY k;
    if (RegCreateKeyExW(root, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return -1;
    LONG r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)v, (lstrlenW(v) + 1) * sizeof(wchar_t));
    RegCloseKey(k);
    return r ? -1 : 0;
}

int reg_set_bin(HKEY root, const wchar_t *path, const wchar_t *name, const void *d, DWORD n)
{
    HKEY k;
    if (RegCreateKeyExW(root, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return -1;
    LONG r = RegSetValueExW(k, name, 0, REG_BINARY, (const BYTE *)d, n);
    RegCloseKey(k);
    return r ? -1 : 0;
}

#define CLASS_DISPLAY L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}"

int gpu_class_keys(const wchar_t *ven, wchar_t keys[][128], int max)
{
    HKEY cls;
    int n = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, CLASS_DISPLAY, 0, KEY_READ, &cls)) return 0;
    for (DWORD i = 0; n < max; i++) {
        wchar_t sub[16], path[128], id[256];
        DWORD cch = PH_ARRAY(sub);
        LONG e = RegEnumKeyExW(cls, i, sub, &cch, NULL, NULL, NULL, NULL);
        if (e == ERROR_MORE_DATA) continue;
        if (e) break;
        if (sub[0] < L'0' || sub[0] > L'9') continue;
        ph_swprintf(path, PH_ARRAY(path), CLASS_DISPLAY L"\\%s", sub);
        if (reg_get_str(HKEY_LOCAL_MACHINE, path, L"MatchingDeviceId", id, PH_ARRAY(id))) continue;
        CharUpperW(id);
        if (wcsstr(id, ven)) lstrcpynW(keys[n++], path, 128);
    }
    RegCloseKey(cls);
    return n;
}

int ph_exe_dir(wchar_t *out, int n)
{
    wchar_t p[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, p, MAX_PATH);
    if (!len || len >= MAX_PATH) return -1;
    while (len && p[len - 1] != L'\\' && p[len - 1] != L'/') len--;
    if (len) len--;
    p[len] = 0;
    if ((int)len >= n) return -1;
    lstrcpynW(out, p, n);
    return 0;
}

/* known folders come from HKLM; %ProgramFiles% can be overridden per user */
static const GUID folder_ids[] = {
    { 0x905e63b6, 0xc1bf, 0x494e, { 0xb2, 0x9c, 0x65, 0xb7, 0x32, 0xd3, 0xd2, 0x1a } },  /* ProgramFiles */
    { 0x7c5a40ef, 0xa0fb, 0x4bfc, { 0x87, 0x4a, 0xc0, 0xf2, 0xe0, 0xb9, 0xfa, 0x8e } },  /* ProgramFilesX86 */
};

int ph_program_files(wchar_t *out, int n)
{
    PWSTR pf = NULL;
    out[0] = 0;
    if (SUCCEEDED(SHGetKnownFolderPath(&folder_ids[0], 0, NULL, &pf)) && pf && lstrlenW(pf) < n) lstrcpynW(out, pf, n);
    CoTaskMemFree(pf);
    return out[0] ? 0 : -1;
}

/* Under Program Files, where only administrators can write. Anything that runs
   elevated from a folder must live there (autostart task, plugins). */
int ph_path_protected(const wchar_t *path)
{
    for (int i = 0; i < PH_ARRAY(folder_ids); i++) {
        PWSTR pf = NULL;
        int hit = 0;
        if (SUCCEEDED(SHGetKnownFolderPath(&folder_ids[i], 0, NULL, &pf)) && pf) {
            int n = lstrlenW(pf);
            hit = n > 2 && !_wcsnicmp(path, pf, (size_t)n) && path[n] == L'\\';
        }
        CoTaskMemFree(pf);
        if (hit) return 1;
    }
    return 0;
}

/* Reads the version resource without running any code from the file. */
static BYTE *ver_load(const wchar_t *path)
{
    DWORD h = 0, sz = GetFileVersionInfoSizeW(path, &h);
    if (!sz || sz > (1u << 20)) return NULL;
    BYTE *b = ph_alloc(sz);
    if (b && !GetFileVersionInfoW(path, 0, sz, b)) { ph_free(b); b = NULL; }
    return b;
}

/* a StringFileInfo value, in the file's first translation */
static void ver_string(BYTE *b, const wchar_t *name, wchar_t *out, int n)
{
    WORD *tr = NULL;
    wchar_t q[96], *s = NULL;
    UINT tn = 0, sl = 0;
    out[0] = 0;
    if (!VerQueryValueW(b, L"\\VarFileInfo\\Translation", (void **)&tr, &tn) || !tr || tn < 4) return;
    ph_swprintf(q, PH_ARRAY(q), L"\\StringFileInfo\\%04x%04x\\%s", tr[0], tr[1], name);
    if (VerQueryValueW(b, q, (void **)&s, &sl) && s && sl > 1) lstrcpynW(out, s, n);
}

int ph_file_version(const wchar_t *path, int ver[3], wchar_t *desc, int ndesc)
{
    int rc = -1;
    if (desc && ndesc > 0) desc[0] = 0;
    BYTE *b = ver_load(path);
    if (!b) return -1;
    VS_FIXEDFILEINFO *fi = NULL;
    UINT n = 0;
    if (ver && VerQueryValueW(b, L"\\", (void **)&fi, &n) && fi && n >= sizeof *fi && fi->dwSignature == 0xFEEF04BD) {
        ver[0] = HIWORD(fi->dwProductVersionMS);
        ver[1] = LOWORD(fi->dwProductVersionMS);
        ver[2] = HIWORD(fi->dwProductVersionLS);
        rc = 0;
    }
    if (desc && ndesc > 0) ver_string(b, L"FileDescription", desc, ndesc);
    ph_free(b);
    return rc;
}

int ph_file_string(const wchar_t *path, const wchar_t *name, wchar_t *out, int n)
{
    if (n <= 0) return -1;
    out[0] = 0;
    BYTE *b = ver_load(path);
    if (!b) return -1;
    ver_string(b, name, out, n);
    ph_free(b);
    return out[0] ? 0 : -1;
}

void ph_fmt_version(wchar_t *out, int n, const int ver[3])
{
    if (ver[2]) ph_swprintf(out, n, L"%d.%d.%d", ver[0], ver[1], ver[2]);
    else ph_swprintf(out, n, L"%d.%d", ver[0], ver[1]);
}

void fmt_watts_mw(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c;
    if (v % 1000) ph_swprintf(b, n, L"%d.%d W", v / 1000, (v % 1000) / 100);
    else ph_swprintf(b, n, L"%d W", v / 1000);
}

void fmt_mhz(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c;
    if (v <= 0) lstrcpynW(b, L"Auto", n);
    else ph_swprintf(b, n, L"%d MHz", v);
}

void fmt_mv(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c;
    ph_swprintf(b, n, v > 0 ? L"+%d mV" : L"%d mV", v);
}

void fmt_pct(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c;
    ph_swprintf(b, n, L"%d%%", v);
}

void fmt_onoff(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c;
    lstrcpynW(b, v ? L"On" : L"Off", n);
}
