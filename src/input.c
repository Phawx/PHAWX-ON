#include "phawx.h"
#include <xinput.h>
#include <winternl.h>
#include <hidusage.h>
#include <hidpi.h>
#include <hidsdi.h>
#include <setupapi.h>
#include "hidpad.h"

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
#define HID_MAX 4

typedef char hp_bits_match[(HP_A == XINPUT_GAMEPAD_A && HP_Y == XINPUT_GAMEPAD_Y && HP_BACK == XINPUT_GAMEPAD_BACK &&
                            HP_RB == XINPUT_GAMEPAD_RIGHT_SHOULDER && HP_GUIDE == BTN_GUIDE && HP_LT == BTN_LT &&
                            HP_RT == BTN_RT && HP_RIGHT == XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : -1];

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
typedef BOOLEAN (NTAPI *hd_attr_fn)(HANDLE, PHIDD_ATTRIBUTES);
typedef BOOLEAN (NTAPI *hd_pp_fn)(HANDLE, PHIDP_PREPARSED_DATA *);
typedef BOOLEAN (NTAPI *hd_freepp_fn)(PHIDP_PREPARSED_DATA);
typedef BOOLEAN (NTAPI *hd_nbuf_fn)(HANDLE, ULONG);
typedef void (NTAPI *hd_guid_fn)(LPGUID);

static HWND owner;
static HMODULE xdll, hdll;
static xget_fn xget;
static int have_guide;
static hp_caps_fn hp_caps;
static hp_vcaps_fn hp_vcaps;
static hp_val_fn hp_val;
static hp_usages_fn hp_usages;
static hd_attr_fn hd_attr;
static hd_pp_fn hd_pp;
static hd_freepp_fn hd_freepp;
static hd_nbuf_fn hd_nbuf;
static hd_guid_fn hd_guid;

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
static uint64_t toggle_quiet;

/* Pads read over HID (hidpad.h): opened only when one is present, found by a scan at
   start and after a device change, never polled for. Each keeps one overlapped
   read in flight; the input timer takes what arrived and the latest state. */
typedef struct hid_pad {
    HANDLE h;
    OVERLAPPED ov;
    uint8_t *buf;
    DWORD len;
    uint64_t id;            /* hash of the device path */
    uint8_t kind, bt, match, fresh, stick, pending, seen;
    hp_state st;
    uint32_t latch;         /* momentary presses since the last poll (ROG Ally keys) */
} hid_pad;

static hid_pad hpad[HID_MAX];
static int hid_n, hid_rescan;

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
    return hid_n > 0;
}

/* Guide counts when XInput reports it or a HID pad with one is there */
static int guide_ok(void) { return have_guide || hid_n > 0; }

static void rearm(void)
{
    if (!owner) return;
    UINT p = 0;
    if (xget || hid_n) {
        if (menu_open || cap_phase) p = POLL_OPEN_MS;
        else if (any_pad()) p = POLL_IDLE_MS;
        else p = SCAN_MS;
    }
    if (p == cur_period) return;
    cur_period = p;
    if (p) SetTimer(owner, TM_INPUT, p, NULL);
    else KillTimer(owner, TM_INPUT);
}

/* the left stick as a d-pad direction (1 up, 2 down, 3 left, 4 right), with hysteresis */
static uint8_t stick_step(uint8_t *dir, int ax, int ay)
{
    int mx = ax < 0 ? -ax : ax, my = ay < 0 ? -ay : ay;
    uint8_t d = *dir;
    if (d && (mx > 10000 || my > 10000)) {
        int keep = (d == 1 && ay > 10000) || (d == 2 && ay < -10000) ||
                   (d == 3 && ax < -10000) || (d == 4 && ax > 10000);
        if (!keep) d = 0;
    } else d = 0;
    if (!d && (mx > 16000 || my > 16000)) {
        if (my >= mx) d = ay > 0 ? 1 : 2;
        else d = ax < 0 ? 3 : 4;
    }
    *dir = d;
    return d;
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
    return b | ((uint32_t)stick_step(&stick_dir[i], g->sThumbLX, g->sThumbLY) << 24);
}

static int combo_hit(uint32_t b)
{
    b &= 0xFFFFFFu;
    if (combo) return (b & combo) == combo;
    if (b & BTN_GUIDE) return 1;    /* only set by sources that have one */
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

/* ---------- HID pads ---------- */

static void hid_close(hid_pad *p)
{
    if (p->h) {
        if (p->pending && CancelIoEx(p->h, &p->ov)) {
            DWORD n;
            GetOverlappedResult(p->h, &p->ov, &n, TRUE);
        }
        CloseHandle(p->h);
    }
    if (p->ov.hEvent) CloseHandle(p->ov.hEvent);
    ph_free(p->buf);
    ZeroMemory(p, sizeof *p);
}

static void hid_drop(int i)
{
    hid_close(&hpad[i]);
    if (i != hid_n - 1) { hpad[i] = hpad[hid_n - 1]; ZeroMemory(&hpad[hid_n - 1], sizeof hpad[0]); }
    hid_n--;
}

static uint64_t path_id(const wchar_t *s)
{
    uint64_t h = 14695981039346656037ull;
    for (; *s; s++) { h ^= (uint64_t)(*s | 0x20); h *= 1099511628211ull; }
    return h;
}

/* only paths that can be one of hidpad.h's vendors are opened at all */
static int path_maybe(const wchar_t *s)
{
    wchar_t v[5] = { 0 };
    for (; *s; s++) {
        for (int k = 0; k < 4; k++) v[k] = v[k + 1];
        v[3] = (wchar_t)(*s | 0x20);
        if (!lstrcmpW(v, L"054c") || !lstrcmpW(v, L"0b05")) return 1;
    }
    return 0;
}

static void hid_open(const wchar_t *path, uint64_t id)
{
    if (hid_n >= HID_MAX) return;
    HANDLE q = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (q == INVALID_HANDLE_VALUE) return;
    HIDD_ATTRIBUTES a;
    a.Size = sizeof a;
    const hp_dev *dv = hd_attr(q, &a) ? hp_match(a.VendorID, a.ProductID) : NULL;
    HIDP_CAPS caps;
    PHIDP_PREPARSED_DATA pp = NULL;
    int ok = 0;
    if (dv && hd_pp(q, &pp)) {
        ok = hp_caps(pp, &caps) == HIDP_OK && caps.UsagePage == dv->usage_page &&
             (!dv->usage || caps.Usage == dv->usage) && caps.InputReportByteLength >= 2 &&
             caps.InputReportByteLength <= 1024;
        hd_freepp(pp);
    }
    CloseHandle(q);
    if (!ok) return;

    hid_pad *p = &hpad[hid_n];
    ZeroMemory(p, sizeof *p);
    p->h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                       FILE_FLAG_OVERLAPPED, NULL);
    if (p->h == INVALID_HANDLE_VALUE) { p->h = NULL; return; }   /* e.g. another app holds it exclusively */
    p->len = caps.InputReportByteLength;
    p->buf = (uint8_t *)ph_alloc(p->len);
    p->ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!p->buf || !p->ov.hEvent) { hid_close(p); return; }
    p->id = id;
    p->kind = dv->kind;
    p->bt = dv->kind != HP_ALLY && p->len != 64;
    p->fresh = 1;
    p->seen = 1;
    /* a gamepad only needs its latest state: few buffered reports, little to drain */
    if (dv->kind != HP_ALLY) hd_nbuf(p->h, 2);
    hid_n++;
    ph_log("input: HID pad %04x:%04x (%s)", a.VendorID, a.ProductID,
           dv->kind == HP_ALLY ? "ROG Ally keys" : p->bt ? "Bluetooth" : "USB");
}

/* keep the pads still there, open new ones, close the ones that went */
static void hid_scan(void)
{
    hid_rescan = 0;
    if (!hd_guid || !hp_caps) return;
    for (int i = 0; i < hid_n; i++) hpad[i].seen = 0;
    GUID g;
    hd_guid(&g);
    HDEVINFO di = SetupDiGetClassDevsW(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di != INVALID_HANDLE_VALUE) {
        SP_DEVICE_INTERFACE_DATA d;
        d.cbSize = sizeof d;
        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, NULL, &g, i, &d); i++) {
            DWORD need = 0;
            SetupDiGetDeviceInterfaceDetailW(di, &d, NULL, 0, &need, NULL);
            if (need < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || need > 4096) continue;
            SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)ph_alloc(need);
            if (!det) break;
            det->cbSize = sizeof *det;
            if (SetupDiGetDeviceInterfaceDetailW(di, &d, det, need, NULL, NULL) && path_maybe(det->DevicePath)) {
                uint64_t id = path_id(det->DevicePath);
                int k = 0;
                while (k < hid_n && hpad[k].id != id) k++;
                if (k < hid_n) hpad[k].seen = 1;
                else hid_open(det->DevicePath, id);
            }
            ph_free(det);
        }
        SetupDiDestroyDeviceInfoList(di);
    }
    for (int i = hid_n - 1; i >= 0; i--) if (!hpad[i].seen) hid_drop(i);
}

static void hid_take(hid_pad *p, DWORD n)
{
    hp_state st;
    if (p->kind == HP_ALLY) p->latch |= hp_ally_parse(p->buf, (int)n);
    else if (hp_ps_parse(p->kind, p->bt, p->buf, (int)n, &st)) p->st = st;
}

/* drain what arrived since the last poll; 0 when the pad is gone */
static int hid_pump(hid_pad *p)
{
    for (int k = 0; k < 64; k++) {
        if (p->pending) {
            if (!HasOverlappedIoCompleted(&p->ov)) return 1;
            DWORD n = 0;
            p->pending = 0;
            if (!GetOverlappedResult(p->h, &p->ov, &n, FALSE)) return 0;
            hid_take(p, n);
        }
        ResetEvent(p->ov.hEvent);
        if (!ReadFile(p->h, p->buf, p->len, NULL, &p->ov) && GetLastError() != ERROR_IO_PENDING) return 0;
        p->pending = 1;
    }
    return 1;
}

static uint32_t hid_read(hid_pad *p, int *ok)
{
    *ok = hid_pump(p);
    if (!*ok) return 0;
    uint32_t b = p->st.btn | p->latch;
    p->latch = 0;
    return b | ((uint32_t)stick_step(&p->stick, p->st.lx, p->st.ly) << 24);
}

void input_on_timer(void)
{
    if (hid_rescan) hid_scan();
    if (!xget && !hid_n) { rearm(); return; }
    uint64_t now = ph_ms();
    int scan = now >= next_scan;
    if (scan) next_scan = now + SCAN_MS;

    uint32_t all = 0;
    int fire = 0;
    for (int i = 0; i < XUSER_MAX_COUNT && xget; i++) {
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
    for (int i = hid_n - 1; i >= 0; i--) {
        hid_pad *p = &hpad[i];
        int ok;
        uint32_t b = hid_read(p, &ok);
        if (!ok) { hid_drop(i); continue; }
        all |= b;
        int hit = combo_hit(b);
        if (hit && !p->match && !p->fresh && !cap_phase) fire = 1;
        p->match = (uint8_t)hit;
        p->fresh = 0;
    }

    suppress &= all;
    if (cap_phase) {
        suppress = all;
        capture_step(all, now);
    } else if (fire && now >= toggle_quiet) {
        /* one press seen by two sources (a pad and the virtual Xbox pad Steam makes
           of it) toggles once */
        toggle_quiet = now + 300;
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
    if (hd_guid) hid_rescan = 1;    /* the next timer tick: one scan for a burst of changes */
    if (owner && (xget || hid_rescan)) {
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
        hd_attr = (hd_attr_fn)(void *)GetProcAddress(hdll, "HidD_GetAttributes");
        hd_pp = (hd_pp_fn)(void *)GetProcAddress(hdll, "HidD_GetPreparsedData");
        hd_freepp = (hd_freepp_fn)(void *)GetProcAddress(hdll, "HidD_FreePreparsedData");
        hd_nbuf = (hd_nbuf_fn)(void *)GetProcAddress(hdll, "HidD_SetNumInputBuffers");
        hd_guid = (hd_guid_fn)(void *)GetProcAddress(hdll, "HidD_GetHidGuid");
        if (!hp_caps || !hd_attr || !hd_pp || !hd_freepp || !hd_nbuf) hd_guid = NULL;
    }
    hid_scan();

    combo = (uint32_t)cfg_get_int("global", "input.combo", 0) & 0x3FFFFu;
    if (combo & ~(uint32_t)0x3F7FFu) combo = 0;
    if (!guide_ok()) combo &= ~BTN_GUIDE;

    if (owner && hp_val) raw_register(1);
    next_scan = 0;
    if (owner && (xget || hid_n)) { cur_period = (UINT)-1; SetTimer(owner, TM_INPUT, 50, NULL); }
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
    while (hid_n) hid_drop(hid_n - 1);
    hd_guid = NULL;
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
    if (!xget && !hid_n) { ui_toast(L"No controller support"); return; }
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
    if (!guide_ok()) mask &= ~BTN_GUIDE;
    combo = mask;
    for (int i = 0; i < XUSER_MAX_COUNT; i++) pad_match[i] = (uint8_t)combo_hit(pad_btn[i]);
    for (int i = 0; i < hid_n; i++) hpad[i].match = 1;   /* a combo held while it was set does not fire */
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
        lstrcpynW(buf, guide_ok() ? L"Guide or Back+Start" : L"Back+Start", n);
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
