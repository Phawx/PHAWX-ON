/* Legion Go / Go S plugin for Phawx ON: the firmware's performance mode and fan
 * full speed, through Lenovo's WMI interface (the one Legion Space uses).
 *
 * Runs only when the SMBIOS maker is LENOVO and the BIOS family or version names
 * a Legion Go (or the product is a Legion Go machine type: 83E1 for the Legion Go,
 * 83L3, 83N6, 83Q2, 83Q3 for the Go S). No driver of its own.
 *
 * The exact calls, for testers (as Lenovo Legion Toolkit and Handheld Companion
 * make them), all in namespace ROOT\WMI on the first instance of the class:
 *   LENOVO_GAMEZONE_DATA.GetSmartFanMode()            -> out uint32 Data
 *   LENOVO_GAMEZONE_DATA.SetSmartFanMode(uint32 Data)
 *     Data: 1 Quiet, 2 Balanced, 3 Performance (255 Custom is Legion Space's own
 *     and is not offered, but is put back if it was set at start)
 *   LENOVO_FAN_METHOD.Fan_Get_FullSpeed()             -> out Status
 *   LENOVO_FAN_METHOD.Fan_Set_FullSpeed(Status)        0 off, 1 on
 * Each row is offered only when its Get call answers at start, and every Set is
 * read back. What was found at start is written back when a row goes to Default
 * and at exit. Fan curves (Fan_Set_Table) are left out: their layout on the
 * Legion Go is not established well enough.
 *
 * WMI runs on a thread of its own in the multithreaded apartment, so no callback
 * pumps messages on the UI thread. Rows show the last values the thread read;
 * tick asks it for new ones at most once a second without waiting.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <wbemcli.h>
#include "phawx_plugin.h"

static const GUID clsid_locator = { 0x4590f811, 0x1d3a, 0x11d0, { 0x89, 0x1f, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24 } };
static const GUID iid_locator = { 0xdc12a687, 0x737f, 0x11cf, { 0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24 } };

static const phx_host *H;
static phx_plugin *SELF;

/* ---------- WMI, on the worker ---------- */

enum { K_ZONE, K_FAN, K_N };
static const wchar_t *const cls_name[K_N] = { L"LENOVO_GAMEZONE_DATA", L"LENOVO_FAN_METHOD" };

static IWbemServices *svc;
static BSTR cls_path[K_N];

static int wmi_open(void)
{
    IWbemLocator *loc = NULL;
    if (FAILED(CoCreateInstance(&clsid_locator, NULL, CLSCTX_INPROC_SERVER, &iid_locator, (void **)&loc)) || !loc)
        return -1;
    BSTR ns = SysAllocString(L"ROOT\\WMI");
    HRESULT hr = ns ? IWbemLocator_ConnectServer(loc, ns, NULL, NULL, NULL, 0, NULL, NULL, &svc) : E_OUTOFMEMORY;
    SysFreeString(ns);
    IWbemLocator_Release(loc);
    if (FAILED(hr) || !svc) { svc = NULL; return -1; }
    CoSetProxyBlanket((IUnknown *)svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    return 0;
}

static void wmi_close(void)
{
    for (int k = 0; k < K_N; k++) { SysFreeString(cls_path[k]); cls_path[k] = NULL; }
    if (svc) IWbemServices_Release(svc);
    svc = NULL;
}

/* the object path of the class's first instance, found once */
static BSTR inst_path(int k)
{
    if (cls_path[k] || !svc) return cls_path[k];
    wchar_t q[96];
    _snwprintf(q, 96, L"SELECT * FROM %s", cls_name[k]);
    q[95] = 0;
    BSTR lang = SysAllocString(L"WQL"), qs = SysAllocString(q);
    IEnumWbemClassObject *en = NULL;
    if (lang && qs &&
        SUCCEEDED(IWbemServices_ExecQuery(svc, lang, qs, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &en)) && en) {
        IWbemClassObject *o = NULL;
        ULONG n = 0;
        if (SUCCEEDED(IEnumWbemClassObject_Next(en, 2000, 1, &o, &n)) && n == 1 && o) {
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(IWbemClassObject_Get(o, L"__PATH", 0, &v, NULL, NULL)) && v.vt == VT_BSTR && v.bstrVal)
                cls_path[k] = SysAllocString(v.bstrVal);
            VariantClear(&v);
            IWbemClassObject_Release(o);
        }
        IEnumWbemClassObject_Release(en);
    }
    SysFreeString(lang);
    SysFreeString(qs);
    return cls_path[k];
}

/* cls.method(in_name = in_val) when in_name is set; the out_name result into *out */
static int wmi_call(int k, const wchar_t *method, const wchar_t *in_name, int in_val, const wchar_t *out_name, int *out)
{
    BSTR path = inst_path(k), cn = SysAllocString(cls_name[k]), mn = SysAllocString(method);
    IWbemClassObject *cls = NULL, *sig = NULL, *in = NULL, *res = NULL;
    int rc = -1;
    if (!path || !cn || !mn || FAILED(IWbemServices_GetObject(svc, cn, 0, NULL, &cls, NULL)) || !cls) goto out;
    if (in_name) {
        VARIANT v;
        CIMTYPE t = CIM_UINT32;
        if (FAILED(IWbemClassObject_GetMethod(cls, method, 0, &sig, NULL)) || !sig ||
            FAILED(IWbemClassObject_SpawnInstance(sig, 0, &in)) || !in)
            goto out;
        IWbemClassObject_Get(in, in_name, 0, NULL, &t, NULL);
        VariantInit(&v);
        if (t == CIM_BOOLEAN) { v.vt = VT_BOOL; v.boolVal = in_val ? VARIANT_TRUE : VARIANT_FALSE; }
        else { v.vt = VT_I4; v.lVal = in_val; }
        if (FAILED(IWbemClassObject_Put(in, in_name, 0, &v, 0))) goto out;
    }
    if (FAILED(IWbemServices_ExecMethod(svc, path, mn, 0, NULL, in, out_name ? &res : NULL, NULL))) goto out;
    if (out_name) {
        VARIANT v;
        VariantInit(&v);
        if (!res || FAILED(IWbemClassObject_Get(res, out_name, 0, &v, NULL, NULL)) ||
            FAILED(VariantChangeType(&v, &v, 0, VT_I4))) {
            VariantClear(&v);
            goto out;
        }
        *out = (int)v.lVal;
        VariantClear(&v);
    }
    rc = 0;
out:
    if (res) IWbemClassObject_Release(res);
    if (in) IWbemClassObject_Release(in);
    if (sig) IWbemClassObject_Release(sig);
    if (cls) IWbemClassObject_Release(cls);
    SysFreeString(cn);
    SysFreeString(mn);
    return rc;
}

static int get_mode(int *v) { return wmi_call(K_ZONE, L"GetSmartFanMode", NULL, 0, L"Data", v); }
static int set_mode(int v)  { return wmi_call(K_ZONE, L"SetSmartFanMode", L"Data", v, NULL, NULL); }
static int get_full(int *v)
{
    int r = wmi_call(K_FAN, L"Fan_Get_FullSpeed", NULL, 0, L"Status", v);
    if (!r) *v = *v != 0;
    return r;
}
static int set_full(int v)  { return wmi_call(K_FAN, L"Fan_Set_FullSpeed", L"Status", v != 0, NULL, NULL); }

/* ---------- the worker ----------
   One job at a time. A job the caller waits for gives up after a bounded time;
   one that never comes back leaves busy set, and from then on nothing is asked
   of WMI again. */

enum { J_OPEN, J_READ, J_MODE, J_FULL, J_QUIT };

#define CALL_MS 2000
#define OPEN_MS 5000

static HANDLE wk, wk_go, wk_done;
static volatile LONG busy;
static int stuck, job, job_arg, job_async, job_rc;
static DWORD job_since;

/* the last values read: -1 = unknown */
static volatile LONG cur_mode = -1, cur_full = -1;
static int have_mode, have_full;
static DWORD last_read;

static int read_all(void)
{
    int v;
    if (have_mode) InterlockedExchange(&cur_mode, get_mode(&v) ? -1 : v);
    if (have_full) InterlockedExchange(&cur_full, get_full(&v) ? -1 : v);
    return 0;
}

static int do_job(void)
{
    int v;
    switch (job) {
    case J_OPEN:
        if (wmi_open()) return -1;
        have_mode = get_mode(&v) == 0;
        if (have_mode) cur_mode = v;
        have_full = get_full(&v) == 0;
        if (have_full) cur_full = v;
        return 0;
    case J_READ: return read_all();
    case J_MODE:
        if (set_mode(job_arg) || get_mode(&v)) return -1;
        InterlockedExchange(&cur_mode, v);
        return v == job_arg ? 0 : -1;
    case J_FULL:
        if (set_full(job_arg) || get_full(&v)) return -1;
        InterlockedExchange(&cur_full, v);
        return v == (job_arg != 0) ? 0 : -1;
    }
    return -1;
}

static DWORD WINAPI worker(void *arg)
{
    (void)arg;
    int com = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED));
    for (;;) {
        WaitForSingleObject(wk_go, INFINITE);
        if (job == J_QUIT) break;
        int rc = com ? do_job() : -1;
        if (job_async) { InterlockedExchange(&busy, 0); continue; }
        job_rc = rc;
        SetEvent(wk_done);
    }
    wmi_close();
    if (com) CoUninitialize();
    return 0;
}

static int claim(DWORD ms)
{
    DWORD t0 = GetTickCount();
    while (InterlockedCompareExchange(&busy, 1, 0)) {
        /* a refresh that does not finish means WMI hangs */
        if (job_async && GetTickCount() - job_since > CALL_MS) {
            stuck = 1;
            phx_logf(H, SELF, "WMI did not answer within %d ms, no longer calling it", CALL_MS);
        }
        if (stuck || GetTickCount() - t0 > ms) return -1;
        Sleep(5);
    }
    return 0;
}

static int run(int j, int arg, DWORD ms)
{
    if (!wk || stuck || claim(300)) return -1;
    job = j;
    job_arg = arg;
    job_async = 0;
    job_since = GetTickCount();
    SetEvent(wk_go);
    if (WaitForSingleObject(wk_done, ms) != WAIT_OBJECT_0) {
        stuck = 1;
        H->set_status(SELF, L"Lenovo's WMI interface stopped responding");
        phx_logf(H, SELF, "WMI did not answer within %lu ms, no longer calling it", ms);
        return -1;
    }
    int rc = job_rc;
    InterlockedExchange(&busy, 0);
    return rc;
}

static void refresh(void)
{
    if (!wk || stuck || GetTickCount() - last_read < 1000) return;
    if (InterlockedCompareExchange(&busy, 1, 0)) {
        if (job_async && GetTickCount() - job_since > CALL_MS) {
            stuck = 1;
            H->set_status(SELF, L"Lenovo's WMI interface stopped responding");
        }
        return;
    }
    last_read = GetTickCount();
    job = J_READ;
    job_async = 1;
    job_since = last_read;
    SetEvent(wk_go);
}

/* ---------- controls ---------- */

static const wchar_t *const mode_ch[] = { L"Quiet", L"Balanced", L"Performance", NULL };
static int mode_orig = -1, full_orig = -1, mode_wrote, full_wrote;

static int PHX_CALL mode_get(phx_control *c, int32_t *out)
{
    (void)c;
    LONG m = cur_mode;
    if (m < 1 || m > 3) return -1;
    *out = m - 1;
    return 0;
}

static int PHX_CALL mode_set(phx_control *c, int32_t v)
{
    (void)c;
    if (v < 0 || v > 2 || run(J_MODE, v + 1, CALL_MS)) return -1;
    mode_wrote = 1;
    return 0;
}

static int PHX_CALL full_get(phx_control *c, int32_t *out)
{
    (void)c;
    LONG f = cur_full;
    if (f < 0) return -1;
    *out = f;
    return 0;
}

static int PHX_CALL full_set(phx_control *c, int32_t v)
{
    (void)c;
    if (run(J_FULL, v != 0, CALL_MS)) return -1;
    full_wrote = 1;
    return 0;
}

static void PHX_CALL onoff(const phx_control *c, int32_t v, wchar_t *b, int n)
{
    (void)c;
    lstrcpynW(b, v ? L"On" : L"Off", n);
}

enum { C_MODE, C_FULL, C_N };
static phx_control ctl[C_N] = {
    [C_MODE] = { .size = sizeof(phx_control), .type = PHX_CHOICE, .key = "mode", .label = L"Performance mode",
                 .choices = mode_ch, .page = PHX_PAGE_POWER, .flags = PHX_F_OPTIONAL | PHX_F_PROFILE,
                 .order = 0, .max = 2, .def = 1, .get = mode_get, .set = mode_set },
    [C_FULL] = { .size = sizeof(phx_control), .type = PHX_TOGGLE, .key = "fanfull", .label = L"Fan full speed",
                 .page = PHX_PAGE_POWER, .flags = PHX_F_OPTIONAL | PHX_F_ADVANCED, .order = 1, .max = 1,
                 .get = full_get, .set = full_set, .fmt = onoff },
};

static void restore(int force)
{
    if (mode_wrote && mode_orig >= 0 && (force || !ctl[C_MODE].active)) {
        if (run(J_MODE, mode_orig, CALL_MS) == 0) phx_logf(H, SELF, "performance mode back to %d", mode_orig);
        mode_wrote = 0;
    }
    if (full_wrote && full_orig >= 0 && (force || !ctl[C_FULL].active)) {
        if (run(J_FULL, full_orig, CALL_MS) == 0) phx_logf(H, SELF, "fan full speed back to %d", full_orig);
        full_wrote = 0;
    }
}

/* ---------- the plugin ---------- */

static void stop_worker(void)
{
    if (wk && !stuck && claim(CALL_MS) == 0) {
        job = J_QUIT;
        SetEvent(wk_go);
        if (WaitForSingleObject(wk, 1000) == WAIT_OBJECT_0) {
            CloseHandle(wk);
            CloseHandle(wk_go);
            CloseHandle(wk_done);
            wk_go = wk_done = NULL;
        }
    }
    /* a worker that hangs inside WMI is left alone, with its events */
    wk = NULL;
}

static void PHX_CALL on_tick(void)
{
    /* Default leaves a row alone without calling set(): put the start value back */
    restore(0);
    refresh();
}

static void PHX_CALL on_shutdown(void)
{
    restore(1);
    stop_worker();
}

static const phx_info info = {
    .size = sizeof(phx_info), .name = L"Legion Go", .version = L"1.0", .shutdown = on_shutdown, .tick = on_tick,
};

static int legion_go(const phx_platform *pf)
{
    static const wchar_t *const types[] = { L"83E1", L"83L3", L"83N6", L"83Q2", L"83Q3" };
    wchar_t s[128];
    if (!pf->maker || (!wcsstr(pf->maker, L"LENOVO") && !wcsstr(pf->maker, L"Lenovo"))) return 0;
    for (int i = 0; i < (int)(sizeof types / sizeof types[0]); i++)
        if (pf->product && !wcsncmp(pf->product, types[i], 4)) return 1;
    static const wchar_t *const vals[] = { L"SystemFamily", L"SystemVersion" };
    for (int i = 0; i < 2; i++) {
        DWORD sz = sizeof s;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", vals[i], RRF_RT_REG_SZ, NULL, s, &sz) == ERROR_SUCCESS &&
            wcsstr(s, L"Legion Go"))
            return 1;
    }
    return 0;
}

static int fail(int rc, const wchar_t *why)
{
    if (why) H->set_status(SELF, why);
    stop_worker();
    return rc;
}

PHX_EXPORT int PHX_CALL phx_plugin_init(const phx_host *host, phx_plugin *self, const phx_info **out)
{
    H = host;
    SELF = self;
    if (host->version < 1) return PHX_E_VERSION;
    if (!legion_go(host->platform)) {
        host->set_status(self, L"only for the Legion Go and Legion Go S");
        return PHX_UNSUPPORTED;
    }
    wk_go = CreateEventW(NULL, FALSE, FALSE, NULL);
    wk_done = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (wk_go && wk_done) wk = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (!wk) return fail(PHX_ERROR, L"could not start its worker thread");
    if (run(J_OPEN, 0, OPEN_MS)) return fail(PHX_ERROR, stuck ? NULL : L"could not open Lenovo's WMI interface");
    if (have_mode && cur_mode >= 1 && (cur_mode <= 3 || cur_mode == 255)) mode_orig = (int)cur_mode;
    else ctl[C_MODE].flags |= PHX_F_HIDDEN;
    if (have_full && cur_full >= 0) full_orig = (int)cur_full;
    else ctl[C_FULL].flags |= PHX_F_HIDDEN;
    if (mode_orig < 0 && full_orig < 0) return fail(PHX_UNSUPPORTED, L"the firmware does not report its modes");
    for (int i = 0; i < C_N; i++)
        if (host->add_control(self, &ctl[i])) return fail(PHX_ERROR, L"could not register its controls");
    last_read = GetTickCount();
    phx_logf(host, self, "performance mode %d, fan full speed %d", mode_orig, full_orig);
    *out = &info;
    return PHX_OK;
}
