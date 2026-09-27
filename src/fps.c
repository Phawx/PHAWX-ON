#include "phawx.h"
#include <objbase.h>
#include <string.h>

typedef int pm_status;
typedef void *pm_session;
typedef void *pm_fquery;

enum {
    PMS_SUCCESS = 0, PMS_SERVICE_ERROR = 4, PMS_ALREADY_TRACKING = 7,
    PMS_PIPE_ERROR = 12, PMS_SESSION_NOT_OPEN = 13
};
enum { PMT_FRAME_EVENT = 2, PMT_DYNAMIC_FRAME = 3 };
enum { PMD_DOUBLE = 0, PMD_UINT64 = 5 };

typedef struct { int metric, stat; uint32_t device, index; uint64_t offset, size; } pm_elem;
typedef struct { const char *data; } pm_str;
typedef struct { const void **data; size_t size; } pm_arr;
typedef struct { int enum_id, value; pm_str *symbol, *name, *short_name, *desc; } pm_key;
typedef struct { int id; pm_str *symbol, *desc; pm_arr *keys; } pm_enum;
typedef struct { int polled, frame, enum_id; } pm_tinfo;
typedef struct { int id, type, unit, pref_unit; pm_tinfo *tinfo; pm_arr *stats, *dev_info; } pm_metric;
typedef struct { pm_arr *metrics, *enums, *devices, *units; } pm_root;
typedef struct { uint16_t major, minor, patch; char tag[22], hash[8], config[4]; } pm_version;

static struct {
    HMODULE dll;
    pm_status (*open)(pm_session *);
    pm_status (*close)(pm_session);
    pm_status (*track)(pm_session, uint32_t);
    pm_status (*untrack)(pm_session, uint32_t);
    pm_status (*intro)(pm_session, const pm_root **);
    pm_status (*intro_free)(const pm_root *);
    pm_status (*reg_frames)(pm_session, pm_fquery *, pm_elem *, uint64_t, uint32_t *);
    pm_status (*consume)(pm_fquery, uint32_t, uint8_t *, uint32_t *);
    pm_status (*free_frames)(pm_fquery);
    pm_status (*flush_period)(pm_session, uint32_t);
    pm_status (*version)(pm_version *);
} pm;

#define RING      1024
#define RMASK     (RING - 1)
#define POLL_MS   50
#define BATCH     128
#define FLUSH_MS  50

typedef struct { int64_t qpc; float ft, cpu, gpu; } frame;

enum { E_QPC, E_CPU, E_GPU, E_N };

static SRWLOCK   lock = SRWLOCK_INIT;
static SRWLOCK   ctl = SRWLOCK_INIT;
static HANDLE    thr, quit_ev, wake_ev, frame_ev;
static volatile LONG status;
static ph_fps    snap;
static int       snap_ok;
static uint64_t  last_probe;

static pm_session sess;
static pm_fquery  fq;
static pm_elem    el[E_N];
static int        nel, eidx[E_N];
static uint32_t   blob_sz;
static uint8_t   *blob;
static double     qpf;
static int        stall_ms;

static frame     ring[RING];
static int       rhead, rcount;
static uint32_t  frames_total;
static DWORD     cur_pid, own_pid;
static int       tracking;
static wchar_t   cur_exe[64];
static uint64_t  fg_check, track_retry, sess_retry;
static int       errs;
static float     scratch[RING];

static const GUID FOLDERID_PF = { 0x905e63b6, 0xc1bf, 0x494e, { 0xb2, 0x9c, 0x65, 0xb7, 0x32, 0xd3, 0xd2, 0x1a } };
typedef HRESULT (WINAPI *kf_fn)(const GUID *, DWORD, HANDLE, PWSTR *);

static int program_files(wchar_t *out, int n)
{
    HMODULE sh = GetModuleHandleW(L"shell32.dll");
    if (!sh) sh = LoadLibraryW(L"shell32.dll");
    kf_fn f = sh ? (kf_fn)GetProcAddress(sh, "SHGetKnownFolderPath") : NULL;
    PWSTR p = NULL;
    out[0] = 0;
    if (f && SUCCEEDED(f(&FOLDERID_PF, 0, NULL, &p)) && p) {
        lstrcpynW(out, p, n);
        CoTaskMemFree(p);
    }
    return out[0] ? 0 : -1;
}

static int under_dir(const wchar_t *path, const wchar_t *dir)
{
    int n = lstrlenW(dir);
    if (!n || lstrlenW(path) <= n) return 0;
    if (CompareStringOrdinal(path, n, dir, n, TRUE) != CSTR_EQUAL) return 0;
    return path[n] == L'\\';
}

static void parent_dir(wchar_t *p)
{
    wchar_t *s = wcsrchr(p, L'\\');
    if (s) *s = 0;
}

static int service_dir(wchar_t *dir, int n, int start)
{
    static const wchar_t *names[] = { L"PresentMonSharedService", L"PresentMonService", L"Intel PresentMon Service" };
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    int ok = -1;
    if (!scm) return -1;
    for (int i = 0; i < PH_ARRAY(names) && ok; i++) {
        SC_HANDLE s = OpenServiceW(scm, names[i], SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_START);
        if (!s) s = OpenServiceW(scm, names[i], SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS);
        if (!s) continue;
        DWORD need = 0;
        QueryServiceConfigW(s, NULL, 0, &need);
        QUERY_SERVICE_CONFIGW *cfg = need ? ph_alloc(need) : NULL;
        if (cfg && QueryServiceConfigW(s, cfg, need, &need) && cfg->lpBinaryPathName) {
            const wchar_t *b = cfg->lpBinaryPathName;
            wchar_t tmp[MAX_PATH];
            int k = 0;
            if (*b == L'"') {
                b++;
                while (*b && *b != L'"' && k < MAX_PATH - 1) tmp[k++] = *b++;
            } else {
                while (*b && k < MAX_PATH - 1) tmp[k++] = *b++;
                tmp[k] = 0;
                wchar_t *e = wcsstr(tmp, L".exe");
                if (!e) e = wcsstr(tmp, L".EXE");
                if (e) k = (int)(e - tmp) + 4;
            }
            tmp[k] = 0;
            parent_dir(tmp);
            lstrcpynW(dir, tmp, n);
            ok = 0;
            SERVICE_STATUS ss;
            if (start && QueryServiceStatus(s, &ss) && ss.dwCurrentState == SERVICE_STOPPED && StartServiceW(s, 0, NULL)) {
                for (int w = 0; w < 20 && QueryServiceStatus(s, &ss) && ss.dwCurrentState != SERVICE_RUNNING; w++) Sleep(100);
            }
        }
        ph_free(cfg);
        CloseServiceHandle(s);
    }
    CloseServiceHandle(scm);
    return ok;
}

static HMODULE load_file(const wchar_t *p, const wchar_t *pf)
{
    wchar_t full[MAX_PATH];
    if (!GetFullPathNameW(p, MAX_PATH, full, NULL)) return NULL;
    if (!under_dir(full, pf)) return NULL;
    if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) return NULL;
    return LoadLibraryExW(full, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}

static HMODULE try_load(const wchar_t *dir, const wchar_t *sub, const wchar_t *pf)
{
    wchar_t p[MAX_PATH];
    ph_swprintf(p, MAX_PATH, sub[0] ? L"%s\\%s\\PresentMonAPI2.dll" : L"%s%s\\PresentMonAPI2.dll", dir, sub);
    return load_file(p, pf);
}

static int pm_load(int start_svc)
{
    if (pm.dll) return 0;
    wchar_t pf[MAX_PATH], svc[MAX_PATH], par[MAX_PATH], def[MAX_PATH];
    if (program_files(pf, MAX_PATH)) return -1;
    static const wchar_t *subs[] = { L"", L"SDK", L"PresentMonApplication", L"PresentMonService" };
    HMODULE h = NULL;
    int have_svc = !service_dir(svc, MAX_PATH, start_svc);
    if (!reg_get_str(HKEY_LOCAL_MACHINE, L"SOFTWARE\\INTEL\\PresentMon\\Service", L"sharedMiddlewarePath", def, MAX_PATH) && def[0])
        h = load_file(def, pf);
    if (!h && have_svc) {
        lstrcpynW(par, svc, MAX_PATH);
        parent_dir(par);
        h = try_load(svc, L"", pf);
        for (int i = 0; !h && i < PH_ARRAY(subs); i++) h = try_load(par, subs[i], pf);
    }
    ph_swprintf(def, MAX_PATH, L"%s\\Intel\\PresentMon", pf);
    for (int i = 0; !h && i < PH_ARRAY(subs); i++) h = try_load(def, subs[i], pf);
    if (!h) return -1;
#define PMFN(f, s) pm.f = (__typeof__(pm.f))GetProcAddress(h, s)
    PMFN(open, "pmOpenSession");
    PMFN(close, "pmCloseSession");
    PMFN(track, "pmStartTrackingProcess");
    PMFN(untrack, "pmStopTrackingProcess");
    PMFN(intro, "pmGetIntrospectionRoot");
    PMFN(intro_free, "pmFreeIntrospectionRoot");
    PMFN(reg_frames, "pmRegisterFrameQuery");
    PMFN(consume, "pmConsumeFrames");
    PMFN(free_frames, "pmFreeFrameQuery");
    PMFN(flush_period, "pmSetEtwFlushPeriod");
    PMFN(version, "pmGetApiVersion");
#undef PMFN
    if (!pm.open || !pm.close || !pm.track || !pm.untrack || !pm.intro || !pm.intro_free ||
        !pm.reg_frames || !pm.consume || !pm.free_frames) {
        FreeLibrary(h);
        memset(&pm, 0, sizeof pm);
        return -1;
    }
    pm.dll = h;
    pm_version v;
    memset(&v, 0, sizeof v);
    if (pm.version && !pm.version(&v)) ph_log("fps: PresentMon API %u.%u.%u", v.major, v.minor, v.patch);
    return 0;
}

static int key_value(const pm_root *r, const char *sym, int *out)
{
    if (!r || !r->enums) return -1;
    for (size_t i = 0; i < r->enums->size; i++) {
        const pm_enum *e = r->enums->data[i];
        if (!e || !e->keys) continue;
        for (size_t j = 0; j < e->keys->size; j++) {
            const pm_key *k = e->keys->data[j];
            if (k && k->symbol && k->symbol->data && !strcmp(k->symbol->data, sym)) {
                *out = k->value;
                return 0;
            }
        }
    }
    return -1;
}

static int metric_ok(const pm_root *r, int id, int dtype)
{
    if (!r || !r->metrics) return 0;
    for (size_t i = 0; i < r->metrics->size; i++) {
        const pm_metric *m = r->metrics->data[i];
        if (!m || m->id != id) continue;
        if (m->type != PMT_FRAME_EVENT && m->type != PMT_DYNAMIC_FRAME) return 0;
        return !m->tinfo || m->tinfo->frame == dtype;
    }
    return 0;
}

static void sess_close(void)
{
    if (sess && tracking && cur_pid) pm.untrack(sess, cur_pid);
    tracking = 0;
    if (fq) pm.free_frames(fq);
    fq = NULL;
    if (sess) pm.close(sess);
    sess = NULL;
    ph_free(blob);
    blob = NULL;
}

static int sess_open(void)
{
    static const char *syms[E_N] = { "PM_METRIC_CPU_START_QPC", "PM_METRIC_CPU_BUSY", "PM_METRIC_GPU_BUSY" };
    static const int types[E_N] = { PMD_UINT64, PMD_DOUBLE, PMD_DOUBLE };
    const pm_root *root = NULL;
    int stat_none = 0;
    if (pm.open(&sess) || !sess) { sess = NULL; return -1; }
    if (pm.intro(sess, &root) || !root) goto fail;
    key_value(root, "PM_STAT_NONE", &stat_none);
    nel = 0;
    for (int i = 0; i < E_N; i++) {
        int m;
        eidx[i] = -1;
        if (key_value(root, syms[i], &m) || !metric_ok(root, m, types[i])) continue;
        memset(&el[nel], 0, sizeof el[nel]);
        el[nel].metric = m;
        el[nel].stat = stat_none;
        eidx[i] = nel++;
    }
    pm.intro_free(root);
    if (eidx[E_QPC] != 0) goto fail;
    blob_sz = 0;
    fq = NULL;
    if (pm.reg_frames(sess, &fq, el, (uint64_t)nel, &blob_sz) || !fq || !blob_sz) {
        fq = NULL;
        nel = 1;
        eidx[E_CPU] = eidx[E_GPU] = -1;
        blob_sz = 0;
        if (pm.reg_frames(sess, &fq, el, 1, &blob_sz) || !fq || !blob_sz) { fq = NULL; goto fail; }
    }
    for (int i = 0; i < E_N; i++)
        if (eidx[i] >= 0 && el[eidx[i]].size != 8) {
            if (i == E_QPC) goto fail;
            eidx[i] = -1;
        }
    blob = ph_alloc((size_t)blob_sz * BATCH);
    if (!blob) goto fail;
    stall_ms = pm.flush_period && !pm.flush_period(sess, FLUSH_MS) ? 400 : 1500;
    return 0;
fail:
    sess_close();
    return -1;
}

static void exe_of(DWORD pid, wchar_t *out, int n)
{
    wchar_t p[MAX_PATH];
    DWORD cch = MAX_PATH;
    out[0] = 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return;
    if (QueryFullProcessImageNameW(h, 0, p, &cch)) {
        wchar_t *s = wcsrchr(p, L'\\');
        lstrcpynW(out, s ? s + 1 : p, n);
    }
    CloseHandle(h);
}

static BOOL CALLBACK afh_child(HWND w, LPARAM l)
{
    DWORD *p = (DWORD *)l, pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid && pid != p[1]) { p[0] = pid; return FALSE; }
    return TRUE;
}

static int ignored(const wchar_t *exe)
{
    static const wchar_t *names[] = {
        L"explorer.exe", L"dwm.exe", L"csrss.exe", L"ShellExperienceHost.exe", L"StartMenuExperienceHost.exe",
        L"SearchHost.exe", L"SearchApp.exe", L"TextInputHost.exe", L"LockApp.exe", L"ShellHost.exe"
    };
    if (!exe[0]) return 1;
    for (int i = 0; i < PH_ARRAY(names); i++)
        if (!lstrcmpiW(exe, names[i])) return 1;
    return 0;
}

static void reset_ring(void)
{
    rhead = rcount = 0;
}

static int sess_lost(pm_status r)
{
    return r == PMS_SERVICE_ERROR || r == PMS_PIPE_ERROR || r == PMS_SESSION_NOT_OPEN;
}

static int dead;

static int track(DWORD pid)
{
    pm_status r = pm.track(sess, pid);
    if (sess_lost(r)) dead = 1;
    return r == PMS_SUCCESS || r == PMS_ALREADY_TRACKING;
}

static void check_fg(uint64_t now)
{
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    wchar_t exe[64];
    if (w) GetWindowThreadProcessId(w, &pid);
    if (!pid || pid == own_pid) return;
    exe_of(pid, exe, PH_ARRAY(exe));
    if (!lstrcmpiW(exe, L"ApplicationFrameHost.exe")) {
        DWORD p[2] = { 0, pid };
        EnumChildWindows(w, afh_child, (LPARAM)p);
        if (!p[0]) return;
        pid = p[0];
        exe_of(pid, exe, PH_ARRAY(exe));
    }
    if (ignored(exe)) return;
    if (pid == cur_pid) {
        if (!tracking && now >= track_retry) {
            tracking = track(pid);
            if (!tracking) track_retry = now + 1000;
        }
        return;
    }
    if (tracking && cur_pid) pm.untrack(sess, cur_pid);
    cur_pid = pid;
    lstrcpynW(cur_exe, exe, PH_ARRAY(cur_exe));
    reset_ring();
    errs = 0;
    tracking = track(pid);
    track_retry = tracking ? 0 : now + 1000;
}

static int consume(void)
{
    int got = 0;
    for (int round = 0; round < 16; round++) {
        uint32_t n = BATCH;
        pm_status r = pm.consume(fq, cur_pid, blob, &n);
        if (r) {
            if (sess_lost(r)) { dead = 1; return got; }
            if (++errs > 20) {
                pm.untrack(sess, cur_pid);
                tracking = 0;
                track_retry = ph_ms() + 1000;
                errs = 0;
            }
            return got;
        }
        errs = 0;
        for (uint32_t i = 0; i < n && i < BATCH; i++) {
            const uint8_t *b = blob + (size_t)i * blob_sz;
            int64_t q;
            memcpy(&q, b + el[eidx[E_QPC]].offset, 8);
            frame *prev = rcount ? &ring[(rhead - 1) & RMASK] : NULL;
            if (q <= 0 || (prev && q <= prev->qpc)) continue;
            frame *f = &ring[rhead];
            f->qpc = q;
            f->ft = prev ? (float)((double)(q - prev->qpc) * 1000.0 / qpf) : 0.0f;
            f->cpu = f->gpu = -1.0f;
            double d;
            if (eidx[E_CPU] >= 0) { memcpy(&d, b + el[eidx[E_CPU]].offset, 8); if (d >= 0 && d < 10000) f->cpu = (float)d; }
            if (eidx[E_GPU] >= 0) { memcpy(&d, b + el[eidx[E_GPU]].offset, 8); if (d >= 0 && d < 10000) f->gpu = (float)d; }
            rhead = (rhead + 1) & RMASK;
            if (rcount < RING) rcount++;
            frames_total++;
            got++;
        }
        if (n < BATCH) break;
    }
    return got;
}

static inline frame *back(int i) { return &ring[(rhead - 1 - i) & RMASK]; }

static float win_fps(int64_t now, int stalled, double wms, int minf)
{
    int64_t w = (int64_t)(wms * qpf / 1000.0);
    int k = 0;
    if (stalled) {
        for (int i = 0; i < rcount && back(i)->qpc > now - w; i++) k++;
        return (float)(k * 1000.0 / wms);
    }
    int64_t end = back(0)->qpc, first = end, hard = end - (int64_t)qpf;
    for (int i = 0; i < rcount; i++) {
        int64_t t = back(i)->qpc;
        if (t > end - w || (k < minf && t > hard)) { first = t; k++; }
        else break;
    }
    if (k < 2 || first >= end) return 0.0f;
    return (float)((k - 1) * qpf / (double)(end - first));
}

static float select_k(float *a, int n, int k)
{
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        float p = a[(lo + hi) / 2];
        int i = lo, j = hi;
        while (i <= j) {
            while (a[i] < p) i++;
            while (a[j] > p) j--;
            if (i <= j) { float t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
        }
        if (k <= j) hi = j;
        else if (k >= i) lo = i;
        else break;
    }
    return a[k];
}

static void publish(int running)
{
    ph_fps s;
    int ok = 0;
    memset(&s, 0, sizeof s);
    s.source = 1;
    if (running && cur_pid) {
        LARGE_INTEGER li;
        QueryPerformanceCounter(&li);
        int64_t now = li.QuadPart;
        s.pid = cur_pid;
        lstrcpynW(s.exe, cur_exe, PH_ARRAY(s.exe));
        s.frames = frames_total;
        if (rcount) {
            double age = (double)(now - back(0)->qpc) * 1000.0 / qpf;
            int stalled = age > stall_ms;
            s.fps = win_fps(now, stalled, 500.0, 3);
            s.fps_fast = win_fps(now, stalled, 150.0, 6);
            s.frametime_ms = back(0)->ft;
            int64_t w05 = back(0)->qpc - (int64_t)(qpf / 2), w3 = back(0)->qpc - (int64_t)(qpf * 3);
            double cs = 0, gs = 0;
            int cn = 0, gn = 0, n = 0;
            for (int i = 0; i < rcount - 1; i++) {
                frame *f = back(i);
                if (f->qpc <= w3) break;
                if (f->qpc > w05) {
                    if (f->cpu >= 0) { cs += f->cpu; cn++; }
                    if (f->gpu >= 0) { gs += f->gpu; gn++; }
                }
                if (f->ft > 0) scratch[n++] = f->ft;
            }
            s.cpu_busy_ms = cn ? (float)(cs / cn) : 0.0f;
            s.gpu_busy_ms = gn ? (float)(gs / gn) : 0.0f;
            if (n >= 10) {
                s.ft_p99_ms = select_k(scratch, n, (int)(0.99 * (n - 1) + 0.5));
                s.low1 = s.ft_p99_ms > 0 ? 1000.0f / s.ft_p99_ms : 0.0f;
                if (stalled && s.low1 > s.fps) s.low1 = s.fps;
            } else {
                s.low1 = s.fps;
                s.ft_p99_ms = s.frametime_ms;
            }
            ok = age < 2000.0;
        }
    }
    AcquireSRWLockExclusive(&lock);
    snap = s;
    snap_ok = ok;
    ReleaseSRWLockExclusive(&lock);
}

static DWORD WINAPI worker(LPVOID u)
{
    HANDLE waits[2] = { quit_ev, wake_ev };
    int idle = 0;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpf = (double)f.QuadPart;
    own_pid = GetCurrentProcessId();
    for (;;) {
        if (!autotdp_running() && !ui_visible()) {
            if (!idle) {
                if (sess && tracking && cur_pid) pm.untrack(sess, cur_pid);
                tracking = 0;
                cur_pid = 0;
                cur_exe[0] = 0;
                reset_ring();
                publish(0);
                idle = 1;
            }
            if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) break;
            continue;
        }
        idle = 0;
        uint64_t now = ph_ms();
        if (!sess && now >= sess_retry) {
            if (sess_open()) {
                InterlockedExchange(&status, 1);
                sess_retry = now + 5000;
            } else {
                InterlockedExchange(&status, 2);
            }
        }
        if (sess) {
            if (now >= fg_check) {
                check_fg(now);
                fg_check = now + 250;
            }
            if (tracking && consume() && frame_ev) SetEvent(frame_ev);
            if (dead) {
                dead = 0;
                sess_close();
                cur_pid = 0;
                cur_exe[0] = 0;
                reset_ring();
                InterlockedExchange(&status, 1);
                sess_retry = now + 2000;
            }
        }
        publish(sess != NULL);
        if (WaitForSingleObject(quit_ev, POLL_MS) == WAIT_OBJECT_0) break;
    }
    sess_close();
    cur_pid = 0;
    return 0;
}

static int start_locked(void)
{
    if (thr && WaitForSingleObject(thr, 0) == WAIT_OBJECT_0) {
        CloseHandle(thr);
        thr = NULL;
    }
    if (thr) {
        if (WaitForSingleObject(quit_ev, 0) == WAIT_OBJECT_0) return -1;
        SetEvent(wake_ev);
        return 0;
    }
    if (!pm.dll) {
        uint64_t now = ph_ms();
        if (last_probe && now - last_probe < 10000) return -1;
        last_probe = now;
        if (pm_load(1)) {
            InterlockedExchange(&status, 0);
            ph_log("fps: PresentMon not found");
            return -1;
        }
        InterlockedExchange(&status, 1);
    }
    fps_event();
    if (!quit_ev) quit_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!wake_ev) wake_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!quit_ev || !wake_ev) return -1;
    ResetEvent(quit_ev);
    sess_retry = fg_check = 0;
    dead = 0;
    thr = CreateThread(NULL, 64 * 1024, worker, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    return thr ? 0 : -1;
}

int fps_start(void)
{
    AcquireSRWLockExclusive(&ctl);
    int r = start_locked();
    ReleaseSRWLockExclusive(&ctl);
    return r;
}

void fps_stop(void)
{
    AcquireSRWLockExclusive(&ctl);
    if (thr) {
        SetEvent(quit_ev);
        if (WaitForSingleObject(thr, 5000) == WAIT_OBJECT_0) {
            CloseHandle(thr);
            thr = NULL;
        } else {
            ph_log("fps: worker did not stop");
        }
    }
    ReleaseSRWLockExclusive(&ctl);
    AcquireSRWLockExclusive(&lock);
    memset(&snap, 0, sizeof snap);
    snap_ok = 0;
    ReleaseSRWLockExclusive(&lock);
}

int fps_sample(ph_fps *out)
{
    int ok;
    if (thr && wake_ev) SetEvent(wake_ev);
    AcquireSRWLockShared(&lock);
    *out = snap;
    ok = thr && snap_ok;
    ReleaseSRWLockShared(&lock);
    if (!thr || status < 2) memset(out, 0, sizeof *out);
    return ok;
}

HANDLE fps_event(void)
{
    if (!frame_ev) {
        HANDLE e = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (e && InterlockedCompareExchangePointer(&frame_ev, e, NULL)) CloseHandle(e);
    }
    return frame_ev;
}

int fps_status(void)
{
    if (!pm.dll && !thr && TryAcquireSRWLockExclusive(&ctl)) {
        uint64_t now = ph_ms();
        if (!pm.dll && !thr && (!last_probe || now - last_probe >= 10000)) {
            last_probe = now;
            InterlockedExchange(&status, pm_load(0) ? 0 : 1);
        }
        ReleaseSRWLockExclusive(&ctl);
    }
    return (int)status;
}
