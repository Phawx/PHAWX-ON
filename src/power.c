#include "phawx.h"
#include <winioctl.h>
#include <setupapi.h>
#include <poclass.h>

/* What the machine draws, for AutoTDP's choices and the Hardware info table.

   The battery's discharge rate is the whole system (screen, memory, SSD, fans and
   all) and is the best measure while it runs on battery. The parts come from the
   backends that can read them: the CPU package or APU (Intel RAPL, the AMD PM
   table) and a discrete GPU (NVML). On AC only the parts are known. */

#define MAX_BAT 4

static const GUID bat_guid = { 0x72631e54, 0x78a4, 0x11d0, { 0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a } };

static CRITICAL_SECTION lk;
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static HANDLE bat[MAX_BAT];
static ULONG tag[MAX_BAT];
static int nbat, scanned;
static uint64_t last_scan, bat_t;
static int bat_cache = -1;
static int (*readers[PWR_KINDS])(int *mw);

static BOOL CALLBACK init_once(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&lk);
    return TRUE;
}

static void L(void) { InitOnceExecuteOnce(&once, init_once, NULL, NULL); EnterCriticalSection(&lk); }
static void U(void) { LeaveCriticalSection(&lk); }

void ph_set_power_reader(int kind, int (*fn)(int *mw))
{
    if (kind < 0 || kind >= PWR_KINDS) return;
    L();
    if (!readers[kind]) readers[kind] = fn;
    U();
}

static void close_all(void)
{
    for (int i = 0; i < nbat; i++) CloseHandle(bat[i]);
    nbat = 0;
}

/* system batteries that report in mW (some report "relative" units only) */
static void scan(void)
{
    close_all();
    scanned = 1;
    last_scan = ph_ms();
    HDEVINFO di = SetupDiGetClassDevsW(&bat_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return;
    for (DWORD i = 0; i < 16 && nbat < MAX_BAT; i++) {
        SP_DEVICE_INTERFACE_DATA d = { sizeof d };
        if (!SetupDiEnumDeviceInterfaces(di, NULL, &bat_guid, i, &d)) break;
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(di, &d, NULL, 0, &need, NULL);
        if (!need || need > 4096) continue;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = ph_alloc(need);
        if (!det) continue;
        det->cbSize = sizeof *det;
        HANDLE h = INVALID_HANDLE_VALUE;
        if (SetupDiGetDeviceInterfaceDetailW(di, &d, det, need, NULL, NULL))
            h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        ph_free(det);
        if (h == INVALID_HANDLE_VALUE) continue;
        ULONG wait = 0, t = 0;
        DWORD got;
        BATTERY_QUERY_INFORMATION q = { 0 };
        BATTERY_INFORMATION bi = { 0 };
        if (!DeviceIoControl(h, IOCTL_BATTERY_QUERY_TAG, &wait, sizeof wait, &t, sizeof t, &got, NULL) || !t) goto skip;
        q.BatteryTag = t;
        q.InformationLevel = BatteryInformation;
        if (!DeviceIoControl(h, IOCTL_BATTERY_QUERY_INFORMATION, &q, sizeof q, &bi, sizeof bi, &got, NULL)) goto skip;
        if (!(bi.Capabilities & BATTERY_SYSTEM_BATTERY) || (bi.Capabilities & BATTERY_CAPACITY_RELATIVE)) goto skip;
        bat[nbat] = h;
        tag[nbat++] = t;
        continue;
    skip:
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(di);
}

/* the whole system while it runs on battery; -1 on AC, while charging, or unknown */
static int battery_mw(void)
{
    if (!scanned || (!nbat && ph_ms() - last_scan > 60000)) scan();
    int sum = 0, any = 0;
    for (int i = 0; i < nbat; i++) {
        BATTERY_WAIT_STATUS w = { 0 };
        BATTERY_STATUS s = { 0 };
        DWORD got;
        w.BatteryTag = tag[i];
        if (!DeviceIoControl(bat[i], IOCTL_BATTERY_QUERY_STATUS, &w, sizeof w, &s, sizeof s, &got, NULL)) {
            scan();         /* removed, or its tag changed */
            return -1;
        }
        if (!(s.PowerState & BATTERY_DISCHARGING) || (ULONG)s.Rate == BATTERY_UNKNOWN_RATE) continue;
        if (s.Rate >= 0) continue;
        sum += -s.Rate;
        any = 1;
    }
    return any && sum < 500000 ? sum : -1;
}

/* at most once a second: each query can make the battery driver talk to the EC,
   and most batteries do not update their rate any faster */
static int battery_cached(void)
{
    uint64_t now = ph_ms();
    if (!bat_t || now - bat_t >= 1000) {
        bat_cache = battery_mw();
        bat_t = now;
    }
    return bat_cache;
}

int ph_battery_mw(void)
{
    L();
    int v = battery_cached();
    U();
    return v;
}

int ph_power_read(ph_power *p)
{
    int v;
    L();
    p->sys_mw = battery_cached();
    p->pkg_mw = readers[PWR_PKG] && readers[PWR_PKG](&v) == 0 && v >= 0 ? v : -1;
    p->gpu_mw = readers[PWR_GPU] && readers[PWR_GPU](&v) == 0 && v >= 0 ? v : -1;
    U();
    return p->sys_mw >= 0 || p->pkg_mw >= 0 || p->gpu_mw >= 0 ? 0 : -1;
}

int ph_power_total(const ph_power *p, int *src)
{
    if (p->sys_mw >= 0) { if (src) *src = PSRC_SYSTEM; return p->sys_mw; }
    if (p->pkg_mw >= 0 || p->gpu_mw >= 0) {
        if (src) *src = PSRC_PARTS;
        return (p->pkg_mw > 0 ? p->pkg_mw : 0) + (p->gpu_mw > 0 ? p->gpu_mw : 0);
    }
    if (src) *src = PSRC_NONE;
    return -1;
}
