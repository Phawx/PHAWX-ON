#include "phawx.h"
#include <shellapi.h>

HWND g_main;
HINSTANCE g_inst;

static NOTIFYICONDATAW nid;
static UINT wm_taskbar;
static HWINEVENTHOOK fg_hook;
static wchar_t cur_profile[64];
static int profiles_on;
static int advanced;
static int autostart_state = -1;
static HICON tray_icon;
static volatile LONG cleaned, hw_restored;

#define ID_OPEN   1
#define ID_AUTO   2
#define ID_START  3
#define ID_EXIT   4
#define ID_SAVEP  5
#define HK_MENU   1

enum { TM_SAVE = 10, TM_PROFILE = 11, TM_TRAY = 12 };
#define WM_PH_DIRTY (WM_APP + 20)

static void tray_add(void)
{
    if (!g_main) return;
    if (!tray_icon)
        tray_icon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    nid.cbSize = sizeof nid;
    nid.hWnd = g_main;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_PH_TRAY;
    nid.hIcon = tray_icon;
    lstrcpynW(nid.szTip, autotdp_running() ? L"Phawx ON - AutoTDP active" : L"Phawx OFF", PH_ARRAY(nid.szTip));
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        SetTimer(g_main, TM_TRAY, 3000, NULL);
        return;
    }
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void app_tray_update(void)
{
    if (!g_main) return;
    lstrcpynW(nid.szTip, autotdp_running() ? L"Phawx ON - AutoTDP active" : L"Phawx OFF", PH_ARRAY(nid.szTip));
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void tray_menu(int x, int y)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_OPEN, L"Open menu");
    AppendMenuW(m, MF_STRING | (autotdp_running() ? MF_CHECKED : 0), ID_AUTO,
                autotdp_running() ? L"Phawx ON (AutoTDP)" : L"Phawx OFF (AutoTDP)");
    AppendMenuW(m, MF_STRING | (app_autostart_get() ? MF_CHECKED : 0), ID_START, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    SetMenuDefaultItem(m, ID_OPEN, FALSE);
    SetForegroundWindow(g_main);
    int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, x, y, 0, g_main, NULL);
    DestroyMenu(m);
    PostMessageW(g_main, WM_NULL, 0, 0);
    switch (id) {
    case ID_OPEN: ui_show(1); break;
    case ID_AUTO: if (autotdp_running()) autotdp_stop(); else autotdp_start(); app_tray_update(); ui_refresh(); break;
    case ID_START: app_autostart_set(!app_autostart_get()); break;
    case ID_EXIT: app_quit(); break;
    }
}

int app_autostart_get(void)
{
    if (autostart_state < 0) autostart_state = ph_run(L"schtasks.exe /query /tn \"PhawxON\"", 5000) == 0;
    return autostart_state;
}

int app_autostart_set(int on)
{
    wchar_t exe[MAX_PATH], cmd[1024];
    on = on != 0;
    if (on == app_autostart_get()) return 0;
    DWORD len = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!len || len >= MAX_PATH) return -1;
    if (on && !ph_path_protected(exe)) {
        ui_toast(L"Move Phawx ON to Program Files to enable autostart");
        return -1;
    }
    if (on)
        ph_swprintf(cmd, PH_ARRAY(cmd),
                    L"schtasks.exe /create /f /tn \"PhawxON\" /sc onlogon /rl highest /delay 0000:05 /tr \"\\\"%s\\\" /tray\"", exe);
    else
        lstrcpyW(cmd, L"schtasks.exe /delete /f /tn \"PhawxON\"");
    int r = ph_run(cmd, 10000);
    autostart_state = -1;
    return r == 0 ? 0 : -1;
}

/* Settings are saved into the active game profile for CF_PROFILE controls, and into
   the global section for everything else, so a game profile never leaks into global. */
static void app_save(void)
{
    if (!cur_profile[0]) { cfg_save(); return; }
    cfg_save_profile(cur_profile);
    for (int i = 0, n = ph_ctl_count(); i < n; i++) {
        ph_ctl *c = ph_ctl_at(i);
        if (!c || !c->key || (c->flags & (CF_NOSAVE | CF_PROFILE))) continue;
        if (c->type == CT_ACTION || c->type == CT_INFO || c->type == CT_HEADER) continue;
        if (c->active) cfg_set_int("global", c->key, c->val);
        else cfg_set_str("global", c->key, "d");
        c->dirty = 0;
    }
}

/* Load global then the given profile; controls that were active and are no longer
   are put back to default instead of keeping the previous profile's value. */
static void profile_switch(const wchar_t *exe)
{
    int n = ph_ctl_count();
    uint8_t *was = (uint8_t *)ph_alloc((size_t)(n > 0 ? n : 1));
    for (int i = 0; i < n; i++) {
        ph_ctl *c = ph_ctl_at(i);
        if (!c || !c->key || !(c->flags & CF_PROFILE) || (c->flags & CF_NOSAVE)) continue;
        if (c->type == CT_ACTION || c->type == CT_INFO || c->type == CT_HEADER) continue;
        if (was) was[i] = c->active;
        c->active = 0;
        c->val = c->def;
    }
    cfg_load_profile(NULL);
    if (exe && exe[0]) cfg_load_profile(exe);
    for (int i = 0; was && i < n; i++) {
        ph_ctl *c = ph_ctl_at(i);
        if (was[i] && !c->active && !((c->flags & CF_AUTOTDP) && autotdp_running())) ph_ctl_reset(c);
    }
    ph_free(was);
    if (exe && exe[0]) lstrcpynW(cur_profile, exe, PH_ARRAY(cur_profile));
    else cur_profile[0] = 0;
    ph_apply_all(0);
    ui_refresh();
}

static void exe_of_window(HWND w, wchar_t *out, int n)
{
    out[0] = 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (!pid || pid == GetCurrentProcessId()) return;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return;
    wchar_t p[MAX_PATH];
    DWORD cch = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, p, &cch)) {
        wchar_t *s = p + lstrlenW(p);
        while (s > p && s[-1] != L'\\') s--;
        lstrcpynW(out, s, n);
    }
    CloseHandle(h);
}

static void profile_check(void)
{
    if (!profiles_on) return;
    wchar_t exe[64];
    exe_of_window(GetForegroundWindow(), exe, 64);
    if (!exe[0] || !lstrcmpiW(exe, L"explorer.exe")) return;
    int has = cfg_has_profile(exe);
    if (has && lstrcmpiW(exe, cur_profile)) {
        app_save();
        profile_switch(exe);
    } else if (!has && cur_profile[0]) {
        app_save();
        profile_switch(NULL);
    }
}

static void CALLBACK fg_proc(HWINEVENTHOOK h, DWORD ev, HWND w, LONG o, LONG c, DWORD t, DWORD tm)
{
    (void)h; (void)ev; (void)w; (void)o; (void)c; (void)t; (void)tm;
    SetTimer(g_main, TM_PROFILE, 1500, NULL);
}

static int act_save_profile(ph_ctl *c, int32_t v)
{
    (void)c; (void)v;
    wchar_t exe[64];
    ph_fps f;
    exe[0] = 0;
    if (fps_sample(&f) && f.exe[0]) lstrcpynW(exe, f.exe, 64);
    if (!exe[0] && cur_profile[0]) lstrcpynW(exe, cur_profile, 64);
    if (!exe[0]) { ui_toast(L"No game detected"); return -1; }
    if (cur_profile[0] && lstrcmpiW(exe, cur_profile)) app_save();
    cfg_save_profile(exe);
    lstrcpynW(cur_profile, exe, PH_ARRAY(cur_profile));
    /* saving a profile only makes sense with switching on, so turn it on rather than
       leave the user stuck in a profile that never switches back */
    int turned_on = 0;
    ph_ctl *p = ph_ctl_find("app.profiles");
    if (!profiles_on && p && ph_ctl_apply(p, 1) == 0) { turned_on = 1; cfg_mark_dirty(); }
    wchar_t m[128];
    ph_swprintf(m, 128, turned_on ? L"Saved profile for %s, per-game profiles on" : L"Saved profile for %s", exe);
    ui_toast(m);
    return 0;
}

static int act_del_profile(ph_ctl *c, int32_t v)
{
    (void)c; (void)v;
    if (!cur_profile[0]) { ui_toast(L"No active game profile"); return -1; }
    app_save();
    wchar_t s[160];
    ph_swprintf(s, 160, L"game:%s", cur_profile);
    wchar_t ini[MAX_PATH];
    ph_swprintf(ini, MAX_PATH, L"%s\\phawx.ini", cfg_dir());
    WritePrivateProfileStringW(s, NULL, NULL, ini);
    profile_switch(NULL);
    ui_toast(L"Profile deleted");
    return 0;
}

static int set_profiles(ph_ctl *c, int32_t v)
{
    (void)c;
    profiles_on = v;
    if (!v && cur_profile[0]) { app_save(); profile_switch(NULL); }
    return 0;
}
static int set_autostart(ph_ctl *c, int32_t v) { (void)c; return app_autostart_set(v) == 0 ? 0 : -1; }
static int get_autostart(ph_ctl *c, int32_t *o) { (void)c; *o = app_autostart_get(); return 0; }
static int act_combo(ph_ctl *c, int32_t v) { (void)c; (void)v; input_capture_combo(); ui_toast(L"Hold the new button combo..."); return 0; }
static int set_adv(ph_ctl *c, int32_t v) { (void)c; advanced = v; ui_refresh(); return 0; }
static int act_quit(ph_ctl *c, int32_t v) { (void)c; (void)v; app_quit(); return 0; }
static int act_reset_all(ph_ctl *c, int32_t v)
{
    (void)c; (void)v;
    if (autotdp_running()) autotdp_stop();
    for (int i = 0, n = ph_ctl_count(); i < n; i++) {
        ph_ctl *k = ph_ctl_at(i);
        if (k->key && k->active && k->type != CT_ACTION) ph_ctl_reset(k);
    }
    wp_commit();
    app_save();
    ui_toast(L"All settings reverted to defaults");
    return 0;
}

static void fmt_combo(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c; (void)v;
    if (input_capturing()) lstrcpynW(b, L"Press combo...", n);
    else combo_to_text(input_combo(), b, n);
}

static void fmt_drv(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c; (void)v;
    if (drv_ok()) MultiByteToWideChar(CP_UTF8, 0, drv_name(), -1, b, n);
    else lstrcpynW(b, L"Not loaded", n);
}

static void fmt_cpu(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c; (void)v;
    lstrcpynW(b, g_plat.cpu_name, n);
}

static void fmt_ver(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c; (void)v;
    lstrcpynW(b, L"Phawx ON " PH_VERSION_W, n);
}

static ph_ctl app_ctls[] = {
    { .key = NULL, .label = L"Menu", .type = CT_HEADER, .page = PG_SETTINGS, .order = 0 },
    { .key = "app.combo", .label = L"Open menu combo", .type = CT_ACTION, .page = PG_SETTINGS, .set = act_combo, .fmt = fmt_combo, .flags = CF_NOSAVE, .order = 1 },
    { .key = "app.autostart", .label = L"Start with Windows", .type = CT_TOGGLE, .page = PG_SETTINGS, .set = set_autostart, .get = get_autostart, .flags = CF_NOSAVE, .order = 2 },
    { .key = "app.advanced", .label = L"Show advanced options", .type = CT_TOGGLE, .page = PG_SETTINGS, .set = set_adv, .order = 3 },
    { .key = NULL, .label = L"Profiles", .type = CT_HEADER, .page = PG_SETTINGS, .order = 10 },
    { .key = "app.profiles", .label = L"Per-game profiles", .type = CT_TOGGLE, .page = PG_SETTINGS, .def = 0, .set = set_profiles, .order = 11 },
    { .key = "app.saveprof", .label = L"Save profile for current game", .type = CT_ACTION, .page = PG_SETTINGS, .set = act_save_profile, .flags = CF_NOSAVE, .order = 12 },
    { .key = "app.delprof", .label = L"Delete current game profile", .type = CT_ACTION, .page = PG_SETTINGS, .set = act_del_profile, .flags = CF_NOSAVE, .order = 13 },
    { .key = "app.resetall", .label = L"Revert everything to defaults", .type = CT_ACTION, .page = PG_SETTINGS, .set = act_reset_all, .flags = CF_NOSAVE | CF_DANGER, .order = 14 },
    { .key = NULL, .label = L"About", .type = CT_HEADER, .page = PG_SETTINGS, .order = 20 },
    { .key = NULL, .label = L"Version", .type = CT_INFO, .page = PG_SETTINGS, .fmt = fmt_ver, .order = 21 },
    { .key = NULL, .label = L"CPU", .type = CT_INFO, .page = PG_SETTINGS, .fmt = fmt_cpu, .order = 22 },
    { .key = NULL, .label = L"Hardware driver", .type = CT_INFO, .page = PG_SETTINGS, .fmt = fmt_drv, .order = 23 },
    { .key = "app.quit", .label = L"Exit Phawx", .type = CT_ACTION, .page = PG_SETTINGS, .set = act_quit, .flags = CF_NOSAVE, .order = 30 },
};

int ph_advanced(void) { return advanced; }

void app_quit(void)
{
    if (g_main) DestroyWindow(g_main);
}

/* The new instance waits on the single-instance mutex until this one has exited
   and restored the hardware. The quit is posted so it happens after the control
   that asked for it has finished. */
int app_restart(void)
{
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 16];
    DWORD len = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!len || len >= MAX_PATH || !g_main) return -1;
    ph_swprintf(cmd, PH_ARRAY(cmd), L"\"%s\" /restart", exe);
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        ui_toast(L"Could not restart Phawx ON");
        return -1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    PostMessageW(g_main, WM_CLOSE, 0, 0);
    return 0;
}

/* Restore everything that must not outlive the process. Runs once, from
   WM_ENDSESSION or the end of wWinMain. g_main is cleared first so AutoTDP does not
   record itself as switched off. */
static void app_cleanup(void)
{
    if (InterlockedExchange(&cleaned, 1)) return;
    HWND m = g_main;
    if (m) {
        KillTimer(m, TM_SAVE);
        KillTimer(m, TM_PROFILE);
        KillTimer(m, TM_TRAY);
        UnregisterHotKey(m, HK_MENU);
    }
    if (fg_hook) { UnhookWinEvent(fg_hook); fg_hook = NULL; }
    ui_show(0);
    g_main = NULL;
    autotdp_stop();
    fps_stop();
    input_shutdown();
    app_save();
    if (!InterlockedExchange(&hw_restored, 1)) {
        ph_backends_shutdown();
        drv_close();
    }
    if (m) Shell_NotifyIconW(NIM_DELETE, &nid);
}

static DWORD crash_tid;
static volatile LONG crashed;

/* also when a fault stopped app_cleanup halfway: ph_backends_shutdown then only
   runs the backends it had not finished */
static DWORD WINAPI crash_restore(LPVOID p)
{
    (void)p;
    autotdp_crash_release(crash_tid);
    ph_backends_shutdown();
    drv_close();
    ph_log("crash: hardware restored");
    return 0;
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    plugins_crash_note(ep);
    if (!InterlockedExchange(&crashed, 1)) {
        InterlockedExchange(&hw_restored, 1);
        g_main = NULL;
        crash_tid = GetCurrentThreadId();
        HANDLE t = CreateThread(NULL, 0, crash_restore, NULL, 0, NULL);
        if (t) { WaitForSingleObject(t, 4000); CloseHandle(t); }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PH_TRAY:
        switch (LOWORD(l)) {
        case NIN_SELECT:
        case NIN_KEYSELECT: ui_toggle(); break;
        case WM_CONTEXTMENU: tray_menu((short)LOWORD(w), (short)HIWORD(w)); break;
        }
        return 0;
    case WM_PH_TOGGLE: ui_toggle(); return 0;
    case WM_PH_NAV: ui_nav((int)w); return 0;
    case WM_PH_HOLD: ui_hold((int)w, (int)l); return 0;
    case WM_PH_REFRESH: ui_refresh(); app_tray_update(); return 0;
    case WM_PH_UPDATE: plugins_update((void *)w, (void *)l); return 0;
    case WM_HOTKEY: if (w == HK_MENU) ui_toggle(); return 0;
    case WM_INPUT: input_on_rawinput(h, l); return DefWindowProcW(h, m, w, l);
    case WM_INPUT_DEVICE_CHANGE: input_on_devchange(); return 0;
    case WM_PH_DIRTY: SetTimer(h, TM_SAVE, 2000, NULL); return 0;
    case WM_TIMER:
        if (w == TM_SAVE) { KillTimer(h, TM_SAVE); app_save(); }
        else if (w == TM_TRAY) { KillTimer(h, TM_TRAY); tray_add(); }
        else if (w == TM_PROFILE) { KillTimer(h, TM_PROFILE); profile_check(); }
        else input_on_timer();
        return 0;
    case WM_POWERBROADCAST:
        if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) {
            static uint64_t last;
            if (ph_ms() - last > 3000) {
                last = ph_ms();
                ph_backends_resume();
                ph_apply_all(0);
            }
        } else if (w == PBT_APMPOWERSTATUSCHANGE) {
            int ac = platform_on_ac();
            if (ac != g_plat.on_ac) { g_plat.on_ac = ac; ph_apply_all(0); }
        }
        return TRUE;
    case WM_DISPLAYCHANGE: ui_refresh(); return 0;
    case WM_ENDSESSION:
        if (w) app_cleanup();
        return 0;
    case WM_DESTROY:
        /* cleanup runs after the message loop: here Windows is calling back into
           the app, and a fault in a plugin's shutdown can be swallowed on the way
           out instead of reaching the crash filter */
        PostQuitMessage(0);
        return 0;
    default:
        if (m == wm_taskbar && m) { tray_add(); return 0; }
    }
    return DefWindowProcW(h, m, w, l);
}

void cfg_mark_dirty(void)
{
    if (g_main) PostMessageW(g_main, WM_PH_DIRTY, 0, 0);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev; (void)show;
    g_inst = hi;
    int restart = cmd && wcsstr(cmd, L"/restart") != NULL;
    HANDLE mtx = CreateMutexW(NULL, TRUE, L"Local\\PhawxON.single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        /* after "Restart Phawx ON": wait for the old instance to finish restoring */
        DWORD wr = restart && mtx ? WaitForSingleObject(mtx, 15000) : WAIT_TIMEOUT;
        if (wr != WAIT_OBJECT_0 && wr != WAIT_ABANDONED) {
            if (!restart) {
                HWND o = FindWindowW(L"PhawxON.main", NULL);
                if (o) PostMessageW(o, WM_PH_TOGGLE, 0, 0);
            }
            if (mtx) CloseHandle(mtx);
            return 0;
        }
    }
    /* elevated: DLLs loaded by name come from the exe folder and System32 only,
       never the current directory or PATH (users can write to parts of PATH) */
    SetDllDirectoryW(L"");
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    SetUnhandledExceptionFilter(crash_filter);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    PROCESS_POWER_THROTTLING_STATE pt = { PROCESS_POWER_THROTTLING_CURRENT_VERSION, PROCESS_POWER_THROTTLING_EXECUTION_SPEED, 0 };
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.lpszClassName = L"PhawxON.main";
    RegisterClassW(&wc);
    /* hidden top-level window: message-only windows get no broadcasts (TaskbarCreated,
       WM_POWERBROADCAST, WM_DISPLAYCHANGE, WM_ENDSESSION) and FindWindow cannot see them */
    g_main = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, PH_APPNAME_W, WS_POPUP, 0, 0, 0, 0, NULL, NULL, hi, NULL);
    if (!g_main) { if (mtx) CloseHandle(mtx); return 1; }
    wm_taskbar = RegisterWindowMessageW(L"TaskbarCreated");
    if (wm_taskbar) ChangeWindowMessageFilterEx(g_main, wm_taskbar, MSGFLT_ALLOW, NULL);
    ChangeWindowMessageFilterEx(g_main, WM_PH_TOGGLE, MSGFLT_ALLOW, NULL);

    cfg_dir();
    platform_detect();
    g_plat.on_ac = platform_on_ac();
    ph_backends_init();
    autotdp_register_ctls();
    ph_register_ctls(app_ctls, PH_ARRAY(app_ctls));
    cfg_load();
    autotdp_cfg_loaded();
    pins_load();
    /* 1.1 turned per-game profiles off by default; keep them switching for anyone
       who already saved one under 1.0 (asked once, so a later Default sticks) */
    ph_ctl *prof = ph_ctl_find("app.profiles");
    if (!cfg_get_int("global", "app.migrated", 0)) {
        if (prof && !prof->active && cfg_any_profile()) { prof->active = 1; prof->val = 1; prof->dirty = 1; }
        cfg_set_int("global", "app.migrated", 1);
    }
    ph_apply_all(0);
    plugins_started();

    ui_init(hi);
    input_init(g_main);
    tray_add();
    RegisterHotKey(g_main, HK_MENU, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'P');
    fg_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, fg_proc, 0, 0,
                              WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    if (cfg_get_int("global", "autotdp.on", 0)) autotdp_start();
    if (restart) ui_show_page(PG_PLUGINS);
    else if (cmd && !wcsstr(cmd, L"/tray") && !cfg_get_int("global", "app.silent", 0)) ui_show(1);
    app_tray_update();
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    app_cleanup();
    if (ui_hwnd()) DestroyWindow(ui_hwnd());
    if (tray_icon) DestroyIcon(tray_icon);
    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    return 0;
}
