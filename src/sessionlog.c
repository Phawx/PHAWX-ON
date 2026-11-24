#include "phawx.h"
#include <string.h>

/* Session log for testers (Settings -> "Save a session log", off by default).
   While AutoTDP runs a game, its worker hands over one row a second; the rows wait
   in memory and go to logs\<date> <time> <game>.csv every few seconds, and when
   the game changes, the setting goes off or AutoTDP stops. The folder is next to
   the exe, or %LOCALAPPDATA%\PhawxON\logs when that cannot be written. Off, nothing
   is allocated, opened or formatted. The file and the buffer belong to the worker
   while it runs and to the thread that stopped it afterwards. */

volatile LONG g_slog;

#define SLOG_BUF   8192
#define SLOG_FLUSH 5000

static const char head[] =
    "time,seconds,game,fps,low_1pct,target_fps,cpu_mhz,gpu_mhz,cores_unparked,cores,epp,"
    "power_w,power_from,tdp_w,trying,bottleneck\r\n";

static HANDLE fh = INVALID_HANDLE_VALUE;
static char *buf;
static int blen, failed;
static uint64_t t_open, t_flush;
static wchar_t cur_exe[64];

/* ---------- text (portable, tests/logic_test.c) ---------- */

static int put(char *b, int n, int k, const char *s)
{
    while (*s && k < n - 1) b[k++] = *s++;
    b[k] = 0;
    return k;
}

/* an integer, or nothing when unknown (< 0); tenths when div is 10 */
static int put_num(char *b, int n, int k, int v, int tenths)
{
    char t[24];
    if (v < 0) t[0] = 0;
    else if (tenths) ph_snprintf(t, sizeof t, "%d.%d", v / 10, v % 10);
    else ph_snprintf(t, sizeof t, "%d", v);
    k = put(b, n, k, t);
    return put(b, n, k, ",");
}

int slog_head(char *b, int n) { b[0] = 0; return put(b, n, 0, head); }

int slog_text(char *b, int n, const ph_slog_row *r, const char *clock, unsigned secs)
{
    char t[192];
    int k = 0;
    if (n < 2) return 0;
    b[0] = 0;
    k = put(b, n, k, clock);
    ph_snprintf(t, sizeof t, ",%u,", secs);
    k = put(b, n, k, t);
    /* the exe as UTF-8, with nothing in it that would break a CSV field */
    int w = r->exe ? WideCharToMultiByte(CP_UTF8, 0, r->exe, -1, t, sizeof t, NULL, NULL) : 0;
    if (w <= 0) t[0] = 0;
    for (char *p = t; *p; p++) if (*p == ',' || *p == '"' || (unsigned char)*p < 32) *p = '_';
    k = put(b, n, k, t);
    k = put(b, n, k, ",");
    k = put_num(b, n, k, r->fps > 0.0f ? (int)(r->fps * 10.0f + 0.5f) : -1, 1);
    k = put_num(b, n, k, r->low1 > 0.0f ? (int)(r->low1 * 10.0f + 0.5f) : -1, 1);
    k = put_num(b, n, k, r->target > 0 ? r->target : -1, 0);
    k = put_num(b, n, k, r->cpu_mhz > 0 ? r->cpu_mhz : -1, 0);
    k = put_num(b, n, k, r->gpu_mhz > 0 ? r->gpu_mhz : -1, 0);
    k = put_num(b, n, k, r->cores_on, 0);
    k = put_num(b, n, k, r->cores_max > 0 ? r->cores_max : -1, 0);
    k = put_num(b, n, k, r->epp, 0);
    k = put_num(b, n, k, r->power_mw >= 0 ? (r->power_mw + 50) / 100 : -1, 1);
    k = put(b, n, k, r->power_mw < 0 ? "," : r->power_src == PSRC_SYSTEM ? "system," : "cpu+gpu,");
    k = put_num(b, n, k, r->tdp_mw > 0 ? (r->tdp_mw + 50) / 100 : -1, 1);
    k = put(b, n, k, r->trying ? r->trying : "");
    k = put(b, n, k, r->bottleneck == 2 ? ",gpu\r\n" : r->bottleneck == 1 ? ",cpu\r\n" : ",\r\n");
    /* a line cut short would shift every column after it */
    return k < n - 1 ? k : 0;
}

/* "2026-11-24 14.03.05 eldenring.csv": the exe without ".exe", made safe for a file name */
void slog_name(wchar_t *b, int n, int y, int mo, int d, int h, int mi, int s, const wchar_t *exe)
{
    wchar_t g[64];
    lstrcpynW(g, exe && exe[0] ? exe : L"game", PH_ARRAY(g));
    int l = lstrlenW(g);
    if (l > 4 && !lstrcmpiW(g + l - 4, L".exe")) g[l - 4] = 0;
    for (wchar_t *p = g; *p; p++)
        if (*p < 32 || *p == L'\\' || *p == L'/' || *p == L':' || *p == L'*' || *p == L'?' || *p == L'"' ||
            *p == L'<' || *p == L'>' || *p == L'|')
            *p = L'_';
    ph_swprintf(b, n, L"%04d-%02d-%02d %02d.%02d.%02d %s.csv", y, mo, d, h, mi, s, g);
}

/* ---------- files ---------- */

static int dir_ok(const wchar_t *d)
{
    wchar_t p[MAX_PATH];
    if (!CreateDirectoryW(d, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    ph_swprintf(p, MAX_PATH, L"%s\\~write.tmp", d);
    HANDLE f = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(f);
    return 1;
}

int slog_dir(wchar_t *out, int n)
{
    wchar_t d[MAX_PATH];
    if (ph_exe_dir(d, MAX_PATH) == 0) {
        ph_swprintf(out, n, L"%s\\logs", d);
        if (dir_ok(out)) return 0;
    }
    DWORD k = GetEnvironmentVariableW(L"LOCALAPPDATA", d, MAX_PATH);
    if (k && k < MAX_PATH - 20) {
        ph_swprintf(out, n, L"%s\\PhawxON", d);
        CreateDirectoryW(out, NULL);
        ph_swprintf(out, n, L"%s\\PhawxON\\logs", d);
        if (dir_ok(out)) return 0;
    }
    out[0] = 0;
    return -1;
}

static void flush(void)
{
    DWORD w;
    if (fh != INVALID_HANDLE_VALUE && blen > 0) WriteFile(fh, buf, (DWORD)blen, &w, NULL);
    blen = 0;
}

static int open_file(const wchar_t *exe)
{
    wchar_t dir[MAX_PATH], name[96], path[MAX_PATH + 96];
    SYSTEMTIME t;
    if (slog_dir(dir, MAX_PATH)) return -1;
    GetLocalTime(&t);
    slog_name(name, PH_ARRAY(name), t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, exe);
    ph_swprintf(path, PH_ARRAY(path), L"%s\\%s", dir, name);
    if (!buf) buf = ph_alloc(SLOG_BUF);
    if (!buf) return -1;
    fh = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) return -1;
    blen = slog_head(buf, SLOG_BUF);
    ph_log("session log: %ls", path);
    return 0;
}

void slog_row(uint64_t now, const ph_slog_row *r)
{
    if (!r->exe || !r->exe[0]) return;
    if (fh != INVALID_HANDLE_VALUE && lstrcmpiW(r->exe, cur_exe)) slog_close();
    if (fh == INVALID_HANDLE_VALUE) {
        /* one try per game, not one a second */
        if (failed && !lstrcmpiW(r->exe, cur_exe)) return;
        lstrcpynW(cur_exe, r->exe, PH_ARRAY(cur_exe));
        failed = open_file(r->exe) != 0;
        if (failed) {
            ph_log("session log: cannot create a file");
            ph_free(buf);
            buf = NULL;
            return;
        }
        t_open = t_flush = now;
    }
    SYSTEMTIME lt;
    char clock[16], line[320];
    GetLocalTime(&lt);
    ph_snprintf(clock, sizeof clock, "%02d:%02d:%02d", lt.wHour, lt.wMinute, lt.wSecond);
    int k = slog_text(line, sizeof line, r, clock, (unsigned)((now - t_open) / 1000));
    if (k <= 0) return;
    if (blen + k > SLOG_BUF) flush();
    memcpy(buf + blen, line, (size_t)k);
    blen += k;
    if (now - t_flush >= SLOG_FLUSH) { flush(); t_flush = now; }
}

void slog_close(void)
{
    failed = 0;
    cur_exe[0] = 0;
    if (fh != INVALID_HANDLE_VALUE) {
        flush();
        CloseHandle(fh);
        fh = INVALID_HANDLE_VALUE;
    }
    blen = 0;
    if (buf) { ph_free(buf); buf = NULL; }
}
