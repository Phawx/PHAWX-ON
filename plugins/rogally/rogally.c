/* ROG Ally / Ally X plugin for Phawx ON: the firmware's operating mode and the
 * fan speeds, through ASUS's ATKACPI device.
 *
 * Runs only on ASUS machines whose SMBIOS product or board names RC71L (ROG Ally)
 * or RC72L (ROG Ally X), and needs the ASUS System Control Interface driver that
 * Armoury Crate installs (it creates \\.\ATKACPI). No driver of its own.
 *
 * The exact calls, for testers (the same as G-Helper's AsusACPI.cs and Linux
 * asus-wmi use):
 *   CreateFileW(L"\\\\.\\ATKACPI", GENERIC_READ | GENERIC_WRITE,
 *               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL)
 *   DeviceIoControl(h, 0x0022240C, in, sizeof in, out, 16, ...) with
 *     in = [method u32][length of args u32][args], everything little-endian:
 *     DSTS (0x53545344, read)  args = [device id u32][0 u32]   -> out[0..3] status
 *     DEVS (0x53564544, write) args = [device id u32][value u32] -> out[0..3] result
 *   A DSTS status has bit 16 (0x10000) set when the device is there; the value is
 *   in the low 16 bits.
 *   0x00120075 throttle thermal policy ("Operating mode"):
 *     0 Performance, 1 Turbo, 2 Silent
 *   0x00110013 / 0x00110014 CPU / GPU fan: speed in units of 100 RPM (read only)
 *
 * Operating mode is offered only when DSTS reports the device and a mode Armoury
 * Crate uses (0..2); every write is read back, and one that does not stick fails.
 * The mode found at start is written back when the row goes to Default and at
 * exit. Fan curves (0x00110024/25) are left out: on the Ally they only go back to
 * the firmware's own curve by setting the mode again, which is not established
 * well enough to rely on.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "phawx_plugin.h"

#define ATK_IOCTL   0x0022240Cu
#define ATK_DSTS    0x53545344u
#define ATK_DEVS    0x53564544u
#define ATK_PRESENT 0x00010000u

#define DEV_POLICY  0x00120075u
#define DEV_FAN1    0x00110013u
#define DEV_FAN2    0x00110014u

static const phx_host *H;
static phx_plugin *SELF;
static HANDLE atk = INVALID_HANDLE_VALUE;

static int atk_call(uint32_t method, uint32_t dev, uint32_t arg, uint32_t *out)
{
    uint32_t in[4] = { method, 8, dev, arg }, res[4] = { 0 };
    DWORD got = 0;
    if (atk == INVALID_HANDLE_VALUE) return -1;
    if (!DeviceIoControl(atk, ATK_IOCTL, in, sizeof in, res, sizeof res, &got, NULL) || got < 4) return -1;
    *out = res[0];
    return 0;
}

/* the device's value, or -1 when it is not there */
static int dsts(uint32_t dev)
{
    uint32_t s;
    if (atk_call(ATK_DSTS, dev, 0, &s) || !(s & ATK_PRESENT)) return -1;
    return (int)(s & 0xFFFFu);
}

static int devs(uint32_t dev, uint32_t v)
{
    uint32_t r;
    return atk_call(ATK_DEVS, dev, v, &r);
}

/* ---------- operating mode ---------- */

/* choice index -> firmware value, in Armoury Crate's order */
static const uint32_t mode_val[] = { 2, 0, 1 };
static const wchar_t *const mode_ch[] = { L"Silent", L"Performance", L"Turbo", NULL };
static int mode_orig = -1, mode_wrote;

static int mode_index(int v)
{
    for (int i = 0; i < 3; i++) if ((int)mode_val[i] == v) return i;
    return -1;
}

static int write_mode(uint32_t v)
{
    if (devs(DEV_POLICY, v) || dsts(DEV_POLICY) != (int)v) {
        phx_logf(H, SELF, "operating mode %u did not stick", v);
        return -1;
    }
    return 0;
}

static int PHX_CALL mode_get(phx_control *c, int32_t *out)
{
    (void)c;
    int i = mode_index(dsts(DEV_POLICY));
    if (i < 0) return -1;
    *out = i;
    return 0;
}

static int PHX_CALL mode_set(phx_control *c, int32_t v)
{
    (void)c;
    if (v < 0 || v > 2 || write_mode(mode_val[v])) return -1;
    mode_wrote = 1;
    return 0;
}

static void mode_restore(void)
{
    if (!mode_wrote || mode_orig < 0) return;
    if (write_mode((uint32_t)mode_orig) == 0) phx_logf(H, SELF, "operating mode back to %d", mode_orig);
    mode_wrote = 0;
}

/* ---------- fans ---------- */

static int PHX_CALL fan_get(phx_control *c, int32_t *out)
{
    int v = dsts(c->user ? DEV_FAN2 : DEV_FAN1);
    if (v < 0) return -1;
    *out = v * 100;
    return 0;
}

static void PHX_CALL fan_fmt(const phx_control *c, int32_t v, wchar_t *b, int n)
{
    (void)c;
    _snwprintf(b, (size_t)n, L"%d RPM", (int)v);
    b[n - 1] = 0;
}

enum { C_MODE, C_FAN1, C_FAN2, C_N };
static phx_control ctl[C_N] = {
    [C_MODE] = { .size = sizeof(phx_control), .type = PHX_CHOICE, .key = "mode", .label = L"Operating mode",
                 .choices = mode_ch, .page = PHX_PAGE_POWER, .flags = PHX_F_OPTIONAL | PHX_F_PROFILE,
                 .order = 0, .max = 2, .def = 1, .get = mode_get, .set = mode_set },
    [C_FAN1] = { .size = sizeof(phx_control), .type = PHX_INFO, .label = L"Fan 1 speed", .page = PHX_PAGE_POWER,
                 .order = 1, .get = fan_get, .fmt = fan_fmt, .user = NULL },
    [C_FAN2] = { .size = sizeof(phx_control), .type = PHX_INFO, .label = L"Fan 2 speed", .page = PHX_PAGE_POWER,
                 .order = 2, .get = fan_get, .fmt = fan_fmt, .user = (void *)1 },
};

/* ---------- the plugin ---------- */

static void PHX_CALL on_tick(void)
{
    /* Default leaves the row alone without calling set(): put the start mode back */
    if (mode_wrote && !ctl[C_MODE].active) mode_restore();
}

static void PHX_CALL on_shutdown(void)
{
    mode_restore();
    if (atk != INVALID_HANDLE_VALUE) CloseHandle(atk);
    atk = INVALID_HANDLE_VALUE;
}

static const phx_info info = {
    .size = sizeof(phx_info), .name = L"ROG Ally", .version = L"1.0", .shutdown = on_shutdown, .tick = on_tick,
};

static int has(const wchar_t *s, const wchar_t *sub) { return s && wcsstr(s, sub) != NULL; }

PHX_EXPORT int PHX_CALL phx_plugin_init(const phx_host *host, phx_plugin *self, const phx_info **out)
{
    H = host;
    SELF = self;
    if (host->version < 1) return PHX_E_VERSION;
    const phx_platform *pf = host->platform;
    int ally = has(pf->product, L"RC71L") || has(pf->board, L"RC71L");
    int allyx = has(pf->product, L"RC72L") || has(pf->board, L"RC72L");
    if (!(has(pf->maker, L"ASUS") || has(pf->maker, L"Asus")) || !(ally || allyx)) {
        host->set_status(self, L"only for the ROG Ally and Ally X");
        return PHX_UNSUPPORTED;
    }
    atk = CreateFileW(L"\\\\.\\ATKACPI", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      NULL, OPEN_EXISTING, 0, NULL);
    if (atk == INVALID_HANDLE_VALUE) {
        host->set_status(self, L"needs the ASUS System Control Interface driver (Armoury Crate)");
        return PHX_ERROR;
    }
    mode_orig = dsts(DEV_POLICY);
    if (mode_index(mode_orig) < 0) {
        ctl[C_MODE].flags |= PHX_F_HIDDEN;
        mode_orig = -1;
    }
    int32_t rpm;
    if (fan_get(&ctl[C_FAN1], &rpm)) ctl[C_FAN1].flags |= PHX_F_HIDDEN;
    if (fan_get(&ctl[C_FAN2], &rpm)) ctl[C_FAN2].flags |= PHX_F_HIDDEN;
    if ((ctl[C_MODE].flags & ctl[C_FAN1].flags & ctl[C_FAN2].flags) & PHX_F_HIDDEN) {
        on_shutdown();
        host->set_status(self, L"the firmware does not report the operating mode or fans");
        return PHX_UNSUPPORTED;
    }
    for (int i = 0; i < C_N; i++)
        if (host->add_control(self, &ctl[i])) { on_shutdown(); host->set_status(self, L"could not register its controls"); return PHX_ERROR; }
    phx_logf(host, self, "%s, operating mode %d", allyx ? "ROG Ally X" : "ROG Ally", mode_orig);
    if (mode_orig < 0) host->set_status(self, L"operating mode not reported by this firmware");
    *out = &info;
    return PHX_OK;
}
