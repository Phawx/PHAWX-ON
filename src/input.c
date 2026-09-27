#include "phawx.h"
#include <xinput.h>
#include <winternl.h>
#include <hidusage.h>
#include <hidpi.h>

#define TM_INPUT      20
#define POLL_OPEN_MS  16
#define POLL_IDLE_MS  66
#define SCAN_MS       2000
#define CAPTURE_MS    10000
#define REPEAT_DELAY  350
#define REPEAT_RATE   90

#define BTN_GUIDE 0x0400u
#define BTN_LT    0x10000u
#define BTN_RT    0x20000u
#define BTN_DEFAULT_ALT (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START)

#define HIDP_OK 0x00110000L
#define TOUCH_SCREEN_USAGE 0x04

/* XInputGetStateEx (ordinal 100) writes an XINPUT_GAMEPAD_EX with a trailing reserved DWORD */
typedef struct xstate_ex {
    DWORD dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
    DWORD dwPaddingReserved;
} xstate_ex;
typedef DWORD (WINAPI *xget_fn)(DWORD, void *);
typedef NTSTATUS (NTAPI *hp_caps_fn)(PHIDP_PREPARSED_DATA, PHIDP_CAPS);
typedef NTSTATUS (NTAPI *hp_vcaps_fn)(HIDP_REPORT_TYPE, PHIDP_VALUE_CAPS, PUSHORT, PHIDP_PREPARSED_DATA);
typedef NTSTATUS (NTAPI *hp_val_fn)(HIDP_REPORT_TYPE, USAGE, USHORT, USAGE, PULONG, PHIDP_PREPARSED_DATA, PCHAR, ULONG);
typedef NTSTATUS (NTAPI *hp_usages_fn)(HIDP_REPORT_TYPE, USAGE, USHORT, PUSAGE, PULONG, PHIDP_PREPARSED_DATA, PCHAR, ULONG);

static HWND owner;
static HMODULE xdll, hdll;
static xget_fn xget;
static int have_guide;
static hp_caps_fn hp_caps;
static hp_vcaps_fn hp_vcaps;
static hp_val_fn hp_val;
static hp_usages_fn hp_usages;

static uint32_t combo;
static int menu_open;
static UINT cur_period = (UINT)-1;

static int pad_on[XUSER_MAX_COUNT];
static uint32_t pad_btn[XUSER_MAX_COUNT];
static uint8_t pad_match[XUSER_MAX_COUNT];
static uint8_t stick_dir[XUSER_MAX_COUNT];
static uint64_t next_scan;

static uint32_t prev_all, suppress;
static uint32_t nav_prev;
static uint64_t nav_next[IN_NAV_COUNT];
static int pad_hold, key_hold, kbd_on;

static int cap_phase;
static uint32_t cap_peak;
static uint64_t cap_deadline;

typedef struct touch_dev {
    HANDLE h;
    PHIDP_PREPARSED_DATA pp;
    int ok;
    USHORT lc;
    LONG xmin, xmax, ymin, ymax;
} touch_dev;

static touch_dev tdev[4];
static int tdev_n;
static BYTE *rbuf;
static UINT rbuf_n;

static struct {
    int down, armed, fired;
    ULONG cid;
    float x0, y0;
    uint64_t t0;
    DWORD orient;
} tg;

static int popcount(uint32_t v)
{
    int n = 0;
    for (; v; v &= v - 1) n++;
    return n;
}

static int any_pad(void)
{
    for (int i = 0; i < XUSER_MAX_COUNT; i++) if (pad_on[i]) return 1;
    return 0;
}

static void rearm(void)
{
    if (!owner) return;
    UINT p = 0;
    if (xget) {
        if (menu_open || cap_phase) p = POLL_OPEN_MS;
        else if (any_pad()) p = POLL_IDLE_MS;
        else p = SCAN_MS;
    }
    if (p == cur_period) return;
    cur_period = p;
    if (p) SetTimer(owner, TM_INPUT, p, NULL);
    else KillTimer(owner, TM_INPUT);
}

static uint32_t read_pad(int i, int *ok)
{
    xstate_ex s;
    ZeroMemory(&s, sizeof s);
    if (xget((DWORD)i, &s) != ERROR_SUCCESS) { *ok = 0; return 0; }
    *ok = 1;
    const XINPUT_GAMEPAD *g = &s.Gamepad;
    uint32_t b = g->wButtons & 0xF7FFu;
    if (!have_guide) b &= ~BTN_GUIDE;
    if (g->bLeftTrigger > 100) b |= BTN_LT;
    if (g->bRightTrigger > 100) b |= BTN_RT;

    int ax = g->sThumbLX, ay = g->sThumbLY;
    int mx = ax < 0 ? -ax : ax, my = ay < 0 ? -ay : ay;
    uint8_t d = stick_dir[i];
    if (d && (mx > 10000 || my > 10000)) {
        int keep = (d == 1 && ay > 10000) || (d == 2 && ay < -10000) ||
                   (d == 3 && ax < -10000) || (d == 4 && ax > 10000);
        if (!keep) d = 0;
    } else d = 0;
    if (!d && (mx > 16000 || my > 16000)) {
        if (my >= mx) d = ay > 0 ? 1 : 2;
        else d = ax < 0 ? 3 : 4;
    }
    stick_dir[i] = d;
    return b | ((uint32_t)d << 24);
}

static int combo_hit(uint32_t b)
{
    b &= 0xFFFFFFu;
    if (combo) return (b & combo) == combo;
    if (have_guide && (b & BTN_GUIDE)) return 1;
    return (b & BTN_DEFAULT_ALT) == BTN_DEFAULT_ALT;
}

static uint32_t nav_bits(uint32_t b)
{
    uint32_t n = 0;
    uint32_t d = b >> 24;
    if ((b & XINPUT_GAMEPAD_DPAD_UP) || d == 1) n |= 1u << IN_UP;
    if ((b & XINPUT_GAMEPAD_DPAD_DOWN) || d == 2) n |= 1u << IN_DOWN;
    if ((b & XINPUT_GAMEPAD_DPAD_LEFT) || d == 3) n |= 1u << IN_LEFT;
    if ((b & XINPUT_GAMEPAD_DPAD_RIGHT) || d == 4) n |= 1u << IN_RIGHT;
    if (b & XINPUT_GAMEPAD_A) n |= 1u << IN_OK;
    if (b & XINPUT_GAMEPAD_B) n |= 1u << IN_BACK;
    if (b & XINPUT_GAMEPAD_LEFT_SHOULDER) n |= 1u << IN_TAB_PREV;
    if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) n |= 1u << IN_TAB_NEXT;
    if (b & XINPUT_GAMEPAD_Y) n |= 1u << IN_PIN;
    return n;
}

/* X (the left face button on any XInput pad) resets only when held, so the UI
   gets its press and its release rather than a single nav event */
static void pad_hold_edge(uint32_t all)
{
    int h = (all & ~suppress & XINPUT_GAMEPAD_X) != 0;
    if (h == pad_hold) return;
    pad_hold = h;
    PostMessageW(owner, WM_PH_HOLD, HOLD_PAD, h);
}

static void do_nav(uint32_t all, uint64_t now)
{
    uint32_t n = nav_bits(all & ~suppress);
    pad_hold_edge(all);
    for (int k = 0; k < IN_NAV_COUNT; k++) {
        uint32_t m = 1u << k;
        if (!(n & m)) continue;
        int dir = k <= IN_RIGHT;
        if (!(nav_prev & m)) {
            PostMessageW(owner, WM_PH_NAV, (WPARAM)k, 0);
            nav_next[k] = now + REPEAT_DELAY;
        } else if (dir && now >= nav_next[k]) {
            PostMessageW(owner, WM_PH_NAV, (WPARAM)k, 0);
            nav_next[k] = now + REPEAT_RATE;
        }
    }
    nav_prev = n;
}

static void capture_step(uint32_t all, uint64_t now)
{
    uint32_t b = all & 0xFFFFFFu;
    if (now > cap_deadline) {
        cap_phase = 0;
        ui_toast(L"Combo unchanged");
        PostMessageW(owner, WM_PH_REFRESH, 0, 0);
        return;
    }
    if (cap_phase == 1) {
        if (!b) { cap_phase = 2; cap_peak = 0; }
        return;
    }
    if (b) {
        if (popcount(b) >= popcount(cap_peak)) cap_peak = b;
        return;
    }
    if (!cap_peak) return;
    if (popcount(cap_peak) < 2 && !(cap_peak & BTN_GUIDE)) {
        ui_toast(L"Use two or more buttons");
        cap_peak = 0;
        cap_deadline = now + CAPTURE_MS;
        return;
    }
    cap_phase = 0;
    input_set_combo(cap_peak);
    wchar_t t[96], m[128];
    combo_to_text(cap_peak, t, PH_ARRAY(t));
    ph_swprintf(m, PH_ARRAY(m), L"Menu combo: %s", t);
    ui_toast(m);
    PostMessageW(owner, WM_PH_REFRESH, 0, 0);
}

void input_on_timer(void)
{
    if (!xget) { rearm(); return; }
    uint64_t now = ph_ms();
    int scan = now >= next_scan;
    if (scan) next_scan = now + SCAN_MS;

    uint32_t all = 0;
    int fire = 0;
    for (int i = 0; i < XUSER_MAX_COUNT; i++) {
        if (!pad_on[i] && !scan) continue;
        int ok;
        uint32_t b = read_pad(i, &ok);
        if (!ok) {
            pad_on[i] = 0; pad_btn[i] = 0; pad_match[i] = 0; stick_dir[i] = 0;
            continue;
        }
        if (!pad_on[i]) {
            pad_on[i] = 1;
            pad_match[i] = (uint8_t)combo_hit(b);
        }
        pad_btn[i] = b;
        all |= b;
        int hit = combo_hit(b);
        if (hit && !pad_match[i] && !cap_phase) fire = 1;
        pad_match[i] = (uint8_t)hit;
    }

    suppress &= all;
    if (cap_phase) {
        suppress = all;
        capture_step(all, now);
    } else if (fire) {
        suppress = all;
        nav_prev = nav_bits(all);
        PostMessageW(owner, WM_PH_TOGGLE, 0, 0);
    } else if (menu_open) {
        do_nav(all, now);
    }
    prev_all = all;
    rearm();
}

static void touch_flush(void)
{
    for (int i = 0; i < tdev_n; i++) ph_free(tdev[i].pp);
    ZeroMemory(tdev, sizeof tdev);
    tdev_n = 0;
    tg.down = tg.armed = 0;
}

static touch_dev *touch_get(HANDLE h)
{
    for (int i = 0; i < tdev_n; i++) if (tdev[i].h == h) return &tdev[i];
    touch_dev *d;
    if (tdev_n < PH_ARRAY(tdev)) d = &tdev[tdev_n++];
    else {
        int v = 0;
        for (int i = 0; i < tdev_n; i++) if (!tdev[i].ok) { v = i; break; }
        d = &tdev[v];
        ph_free(d->pp);
    }
    ZeroMemory(d, sizeof *d);
    d->h = h;
    if (!hp_vcaps) return d;

    RID_DEVICE_INFO di;
    UINT sz = sizeof di;
    di.cbSize = sizeof di;
    if (GetRawInputDeviceInfoW(h, RIDI_DEVICEINFO, &di, &sz) == (UINT)-1 || di.dwType != RIM_TYPEHID ||
        di.hid.usUsagePage != HID_USAGE_PAGE_DIGITIZER || di.hid.usUsage != TOUCH_SCREEN_USAGE)
        return d;
    sz = 0;
    if (GetRawInputDeviceInfoW(h, RIDI_PREPARSEDDATA, NULL, &sz) != 0 || !sz || sz > 65536) return d;
    d->pp = (PHIDP_PREPARSED_DATA)ph_alloc(sz);
    if (!d->pp || GetRawInputDeviceInfoW(h, RIDI_PREPARSEDDATA, d->pp, &sz) == (UINT)-1) return d;

    HIDP_CAPS caps;
    if (hp_caps(d->pp, &caps) != HIDP_OK || !caps.NumberInputValueCaps) return d;
    USHORT n = caps.NumberInputValueCaps;
    HIDP_VALUE_CAPS *vc = (HIDP_VALUE_CAPS *)ph_alloc(sizeof *vc * n);
    if (!vc) return d;
    if (hp_vcaps(HidP_Input, vc, &n, d->pp) == HIDP_OK) {
        int fx = -1;
        for (int i = 0; i < n; i++) {
            USAGE u = vc[i].IsRange ? vc[i].Range.UsageMin : vc[i].NotRange.Usage;
            if (vc[i].UsagePage == HID_USAGE_PAGE_GENERIC && u == HID_USAGE_GENERIC_X &&
                (fx < 0 || vc[i].LinkCollection < vc[fx].LinkCollection))
                fx = i;
        }
        if (fx >= 0) {
            d->lc = vc[fx].LinkCollection;
            d->xmin = vc[fx].LogicalMin; d->xmax = vc[fx].LogicalMax;
            for (int i = 0; i < n; i++) {
                USAGE u = vc[i].IsRange ? vc[i].Range.UsageMin : vc[i].NotRange.Usage;
                if (vc[i].UsagePage == HID_USAGE_PAGE_GENERIC && u == HID_USAGE_GENERIC_Y && vc[i].LinkCollection == d->lc) {
                    d->ymin = vc[i].LogicalMin; d->ymax = vc[i].LogicalMax;
                    d->ok = d->xmax > d->xmin && d->ymax > d->ymin;
                    break;
                }
            }
        }
    }
    ph_free(vc);
    return d;
}

static void to_screen(float nx, float ny, DWORD o, float *sx, float *sy)
{
    switch (o) {
    case DMDO_90:  *sx = ny;       *sy = 1.f - nx; break;
    case DMDO_180: *sx = 1.f - nx; *sy = 1.f - ny; break;
    case DMDO_270: *sx = 1.f - ny; *sy = nx;       break;
    default:       *sx = nx;       *sy = ny;       break;
    }
}

static DWORD primary_orientation(void)
{
    DEVMODEW dm;
    ZeroMemory(&dm, sizeof dm);
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) && (dm.dmFields & DM_DISPLAYORIENTATION))
        return dm.dmDisplayOrientation;
    return DMDO_DEFAULT;
}

static void touch_report(touch_dev *d, PCHAR rep, ULONG len)
{
    ULONG x, y, cid = 0;
    if (hp_val(HidP_Input, HID_USAGE_PAGE_GENERIC, d->lc, HID_USAGE_GENERIC_X, &x, d->pp, rep, len) != HIDP_OK ||
        hp_val(HidP_Input, HID_USAGE_PAGE_GENERIC, d->lc, HID_USAGE_GENERIC_Y, &y, d->pp, rep, len) != HIDP_OK)
        return;
    hp_val(HidP_Input, HID_USAGE_PAGE_DIGITIZER, d->lc, 0x51, &cid, d->pp, rep, len);

    USAGE us[16];
    ULONG nu = PH_ARRAY(us);
    int tip = 0;
    if (hp_usages(HidP_Input, HID_USAGE_PAGE_DIGITIZER, d->lc, us, &nu, d->pp, rep, len) == HIDP_OK)
        for (ULONG i = 0; i < nu; i++) if (us[i] == HID_USAGE_DIGITIZER_TIP_SWITCH) tip = 1;

    if (tg.down && cid != tg.cid) return;
    if (!tip) { tg.down = tg.armed = tg.fired = 0; return; }

    float nx = (float)((LONG)x - d->xmin) / (float)(d->xmax - d->xmin);
    float ny = (float)((LONG)y - d->ymin) / (float)(d->ymax - d->ymin);
    float sx, sy;
    if (!tg.down) {
        tg.down = 1;
        tg.cid = cid;
        tg.fired = 0;
        tg.orient = primary_orientation();
        to_screen(nx, ny, tg.orient, &sx, &sy);
        tg.x0 = sx; tg.y0 = sy;
        tg.t0 = ph_ms();
        tg.armed = sx >= 0.95f && sy <= 0.25f && !menu_open;
        return;
    }
    if (!tg.armed || tg.fired) return;
    to_screen(nx, ny, tg.orient, &sx, &sy);
    float dx = tg.x0 - sx, dy = sy - tg.y0;
    if (dy < 0) dy = -dy;
    if (ph_ms() - tg.t0 > 1000 || dy > 0.2f) { tg.armed = 0; return; }
    if (dx >= 0.10f && dy < dx) {
        tg.fired = 1;
        if (!menu_open) PostMessageW(owner, WM_PH_TOGGLE, 0, 0);
    }
}

/* R on the keyboard. The panel never takes focus, so while it is open the main
   window listens passively (RIDEV_INPUTSINK): nothing is hooked or swallowed. */
static void kbd_register(int on)
{
    if (!owner || on == kbd_on) return;
    RAWINPUTDEVICE r;
    r.usUsagePage = HID_USAGE_PAGE_GENERIC;
    r.usUsage = HID_USAGE_GENERIC_KEYBOARD;
    r.dwFlags = on ? RIDEV_INPUTSINK : RIDEV_REMOVE;
    r.hwndTarget = on ? owner : NULL;
    if (RegisterRawInputDevices(&r, 1, sizeof r) || !on) kbd_on = on;
    if (key_hold) { key_hold = 0; PostMessageW(owner, WM_PH_HOLD, HOLD_KEY, 0); }
}

static void on_key(const RAWINPUT *ri)
{
    const RAWKEYBOARD *k = &ri->data.keyboard;
    /* hDevice is NULL for input injected with SendInput */
    if (!menu_open || !ri->header.hDevice || k->VKey != 'R') return;
    int down = !(k->Flags & RI_KEY_BREAK);
    if (down == key_hold) return;   /* autorepeat */
    key_hold = down;
    PostMessageW(owner, WM_PH_HOLD, HOLD_KEY, down);
}

void input_on_rawinput(HWND h, LPARAM lp)
{
    (void)h;
    if (!hp_val && !kbd_on) return;
    UINT sz = 0;
    if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, NULL, &sz, sizeof(RAWINPUTHEADER)) != 0 || !sz) return;
    if (sz > rbuf_n) {
        if (sz > 1 << 20) return;
        ph_free(rbuf);
        rbuf = (BYTE *)ph_alloc(sz);
        rbuf_n = rbuf ? sz : 0;
        if (!rbuf) return;
    }
    if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, rbuf, &sz, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
    RAWINPUT *ri = (RAWINPUT *)rbuf;
    if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        if (sz >= sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD)) on_key(ri);
        return;
    }
    if (!hp_val || ri->header.dwType != RIM_TYPEHID || !ri->header.hDevice) return;
    touch_dev *d = touch_get(ri->header.hDevice);
    if (!d->ok) return;
    DWORD each = ri->data.hid.dwSizeHid, cnt = ri->data.hid.dwCount;
    BYTE *p = ri->data.hid.bRawData;
    if (!each || (uint64_t)each * cnt > sz) return;
    for (DWORD i = 0; i < cnt; i++, p += each) touch_report(d, (PCHAR)p, each);
}

void input_on_devchange(void)
{
    touch_flush();
    next_scan = 0;
    if (owner && xget) {
        cur_period = (UINT)-1;
        SetTimer(owner, TM_INPUT, 50, NULL);
    }
}

static void raw_register(DWORD flags)
{
    RAWINPUTDEVICE r[2];
    r[0].usUsagePage = HID_USAGE_PAGE_DIGITIZER;
    r[0].usUsage = TOUCH_SCREEN_USAGE;
    r[0].dwFlags = flags ? RIDEV_INPUTSINK | RIDEV_DEVNOTIFY : RIDEV_REMOVE;
    r[0].hwndTarget = flags ? owner : NULL;
    r[1].usUsagePage = HID_USAGE_PAGE_GENERIC;
    r[1].usUsage = HID_USAGE_GENERIC_GAMEPAD;
    r[1].dwFlags = flags ? RIDEV_DEVNOTIFY : RIDEV_REMOVE;
    r[1].hwndTarget = flags ? owner : NULL;
    if (!RegisterRawInputDevices(r, 2, sizeof r[0]) && flags)
        RegisterRawInputDevices(r, 1, sizeof r[0]);
}

void input_init(HWND o)
{
    owner = o;
    xdll = LoadLibraryExW(L"xinput1_4.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (xdll) {
        xget = (xget_fn)(void *)GetProcAddress(xdll, (LPCSTR)(ULONG_PTR)100);
        have_guide = xget != NULL;
        if (!xget) xget = (xget_fn)(void *)GetProcAddress(xdll, "XInputGetState");
    } else {
        xdll = LoadLibraryExW(L"xinput9_1_0.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (xdll) xget = (xget_fn)(void *)GetProcAddress(xdll, "XInputGetState");
    }

    hdll = LoadLibraryExW(L"hid.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (hdll) {
        hp_caps = (hp_caps_fn)(void *)GetProcAddress(hdll, "HidP_GetCaps");
        hp_vcaps = (hp_vcaps_fn)(void *)GetProcAddress(hdll, "HidP_GetValueCaps");
        hp_val = (hp_val_fn)(void *)GetProcAddress(hdll, "HidP_GetUsageValue");
        hp_usages = (hp_usages_fn)(void *)GetProcAddress(hdll, "HidP_GetUsages");
        if (!hp_caps || !hp_vcaps || !hp_val || !hp_usages) hp_caps = NULL, hp_vcaps = NULL, hp_val = NULL, hp_usages = NULL;
    }

    combo = (uint32_t)cfg_get_int("global", "input.combo", 0) & 0x3FFFFu;
    if (combo & ~(uint32_t)0x3F7FFu) combo = 0;
    if (!have_guide) combo &= ~BTN_GUIDE;

    if (owner && hp_val) raw_register(1);
    next_scan = 0;
    if (owner && xget) { cur_period = (UINT)-1; SetTimer(owner, TM_INPUT, 50, NULL); }
}

void input_menu_open(int open)
{
    menu_open = open != 0;
    nav_prev = nav_bits(prev_all);
    suppress = prev_all;
    pad_hold = 0;
    if (menu_open) tg.armed = 0;
    kbd_register(menu_open);
    rearm();
}

void input_shutdown(void)
{
    if (owner) {
        KillTimer(owner, TM_INPUT);
        kbd_register(0);
        if (hp_val) raw_register(0);
    }
    cur_period = (UINT)-1;
    touch_flush();
    ph_free(rbuf);
    rbuf = NULL;
    rbuf_n = 0;
    xget = NULL;
    hp_caps = NULL; hp_vcaps = NULL; hp_val = NULL; hp_usages = NULL;
    if (xdll) FreeLibrary(xdll);
    if (hdll) FreeLibrary(hdll);
    xdll = hdll = NULL;
    owner = NULL;
}

void input_capture_combo(void)
{
    if (!xget) { ui_toast(L"No controller support"); return; }
    cap_phase = 1;
    cap_peak = 0;
    cap_deadline = ph_ms() + CAPTURE_MS;
    next_scan = 0;
    rearm();
}

int input_capturing(void) { return cap_phase != 0; }

uint32_t input_combo(void) { return combo; }

void input_set_combo(uint32_t mask)
{
    mask &= 0x3F7FFu;
    if (!have_guide) mask &= ~BTN_GUIDE;
    combo = mask;
    for (int i = 0; i < XUSER_MAX_COUNT; i++) pad_match[i] = (uint8_t)combo_hit(pad_btn[i]);
    cfg_set_int("global", "input.combo", (int)mask);
}

void combo_to_text(uint32_t mask, wchar_t *buf, int n)
{
    static const struct { uint32_t bit; const wchar_t *name; } names[] = {
        { BTN_GUIDE, L"Guide" }, { XINPUT_GAMEPAD_BACK, L"Back" }, { XINPUT_GAMEPAD_START, L"Start" },
        { XINPUT_GAMEPAD_LEFT_SHOULDER, L"LB" }, { XINPUT_GAMEPAD_RIGHT_SHOULDER, L"RB" },
        { BTN_LT, L"LT" }, { BTN_RT, L"RT" },
        { XINPUT_GAMEPAD_A, L"A" }, { XINPUT_GAMEPAD_B, L"B" }, { XINPUT_GAMEPAD_X, L"X" }, { XINPUT_GAMEPAD_Y, L"Y" },
        { XINPUT_GAMEPAD_LEFT_THUMB, L"LS" }, { XINPUT_GAMEPAD_RIGHT_THUMB, L"RS" },
        { XINPUT_GAMEPAD_DPAD_UP, L"Up" }, { XINPUT_GAMEPAD_DPAD_DOWN, L"Down" },
        { XINPUT_GAMEPAD_DPAD_LEFT, L"Left" }, { XINPUT_GAMEPAD_DPAD_RIGHT, L"Right" },
    };
    if (n <= 0) return;
    buf[0] = 0;
    if (!mask) {
        lstrcpynW(buf, have_guide ? L"Guide or Back+Start" : L"Back+Start", n);
        return;
    }
    int len = 0;
    for (int i = 0; i < PH_ARRAY(names); i++) {
        if (!(mask & names[i].bit)) continue;
        int w = ph_swprintf(buf + len, n - len, len ? L"+%s" : L"%s", names[i].name);
        if (w <= 0) break;
        len += w;
        if (len >= n - 1) break;
    }
}
