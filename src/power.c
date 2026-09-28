#include "phawx.h"
#include <winioctl.h>
#include <setupapi.h>
#include <poclass.h>

/* What the machine draws, for AutoTDP's choices and the Hardware info table.

   The battery's discharge rate is the whole system (screen, memory, SSD, fans and
   all) and is the best measure while it runs on battery. The parts come from the
   backends that can read them: the CPU package or APU (Intel RAPL, the AMD PM
   table) and a discrete GPU (NVML). On AC only the parts are known. A charger too
   weak for the load leaves the battery discharging while on line: then the battery
   covers only the shortfall, so it does not count as the system either. */

#define MAX_BAT  4
#define BAT_MS   2000       /* the battery at most this often: it updates slowly, and each query may reach the EC */
#define SCAN_MS  60000      /* look for batteries again, for one added or removed */
#define RETRY_MS 10000      /* sooner after a battery stopped answering */

static const GUID bat_guid = { 0x72631e54, 0x78a4, 0x11d0, { 0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a } };

static CRITICAL_SECTION lk;         /* the batteries and their cached reading */
static CRITICAL_SECTION rlk;        /* the readers' own state (their counters) */
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static wchar_t *bat_path[MAX_BAT];
static int nbat, bat_cache = -1;
static uint64_t next_scan, bat_t;
static int (*volatile readers[PWR_KINDS])(int *mw);

static BOOL CALLBACK init_once(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&lk);
    InitializeCriticalSection(&rlk);
    return TRUE;
}

static void L(void) { InitOnceExecuteOnce(&once, init_once, NULL, NULL); EnterCriticalSection(&lk); }
static void U(void) { LeaveCriticalSection(&lk); }

/* NULL takes it away again (a backend that unloads the library it reads with) */
void ph_set_power_reader(int kind, int (*fn)(int *mw))
{
    if (kind < 0 || kind >= PWR_KINDS) return;
    if (!fn) InterlockedExchangePointer((void *volatile *)&readers[kind], NULL);
    else InterlockedCompareExchangePointer((void *volatile *)&readers[kind], (void *)fn, NULL);
}

static HANDLE open_bat(const wchar_t *path)
{
    return CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
}

static ULONG bat_tag(HANDLE h)
{
    ULONG wait = 0, t = 0;
    DWORD got;
    return DeviceIoControl(h, IOCTL_BATTERY_QUERY_TAG, &wait, sizeof wait, &t, sizeof t, &got, NULL) ? t : 0;
}

/* system batteries that report in mW (some report "relative" units only), not a
   UPS. Only their paths are kept: an open handle would stand in the way of the
   device being removed or updated. */
static void scan(void)
{
    for (int i = 0; i < nbat; i++) ph_free(bat_path[i]);
    nbat = 0;
    next_scan = ph_ms() + SCAN_MS;
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
        if (SetupDiGetDeviceInterfaceDetailW(di, &d, det, need, NULL, NULL)) h = open_bat(det->DevicePath);
        if (h != INVALID_HANDLE_VALUE) {
            BATTERY_QUERY_INFORMATION q = { 0 };
            BATTERY_INFORMATION bi = { 0 };
            DWORD got;
            q.BatteryTag = bat_tag(h);
            q.InformationLevel = BatteryInformation;
            if (q.BatteryTag &&
                DeviceIoControl(h, IOCTL_BATTERY_QUERY_INFORMATION, &q, sizeof q, &bi, sizeof bi, &got, NULL) &&
                (bi.Capabilities & BATTERY_SYSTEM_BATTERY) &&
                !(bi.Capabilities & (BATTERY_CAPACITY_RELATIVE | BATTERY_IS_SHORT_TERM))) {
                int n = lstrlenW(det->DevicePath) + 1;
                wchar_t *p = ph_alloc((size_t)n * sizeof *p);
                if (p) {
                    lstrcpynW(p, det->DevicePath, n);
                    bat_path[nbat++] = p;
                }
            }
            CloseHandle(h);
        }
        ph_free(det);
    }
    SetupDiDestroyDeviceInfoList(di);
}

static void retry_soon(uint64_t now)
{
    if (next_scan > now + RETRY_MS) next_scan = now + RETRY_MS;
}

/* the whole system while it runs on battery; -1 on AC (charging or not), or unknown */
static int battery_mw(void)
{
    uint64_t now = ph_ms();
    if (now >= next_scan) scan();
    int sum = 0, any = 0, line = 0;
    for (int i = 0; i < nbat; i++) {
        HANDLE h = open_bat(bat_path[i]);
        if (h == INVALID_HANDLE_VALUE) { retry_soon(now); continue; }
        BATTERY_WAIT_STATUS w = { 0 };
        BATTERY_STATUS s = { 0 };
        DWORD got;
        w.BatteryTag = bat_tag(h);
        BOOL ok = w.BatteryTag && DeviceIoControl(h, IOCTL_BATTERY_QUERY_STATUS, &w, sizeof w, &s, sizeof s, &got, NULL);
        CloseHandle(h);
        if (!ok) { retry_soon(now); continue; }         /* removed, or no battery in the slot now */
        if (s.PowerState & BATTERY_POWER_ON_LINE) line = 1;
        if (!(s.PowerState & BATTERY_DISCHARGING) || (ULONG)s.Rate == BATTERY_UNKNOWN_RATE || s.Rate >= 0) continue;
        if (s.Rate < -500000) continue;                 /* not a real reading */
        sum += (int)-s.Rate;
        any = 1;
    }
    return any && !line && sum < 500000 ? sum : -1;
}

static int battery_cached(void)
{
    uint64_t now = ph_ms();
    if (!bat_t || now - bat_t >= BAT_MS) {
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
    U();
    /* the readers may wait on the SMU for a while: not under the battery's lock,
       which the UI takes for Hardware info */
    EnterCriticalSection(&rlk);
    int (*fp)(int *) = readers[PWR_PKG], (*fg)(int *) = readers[PWR_GPU];
    p->pkg_mw = fp && fp(&v) == 0 && v >= 0 ? v : -1;
    p->gpu_mw = fg && fg(&v) == 0 && v >= 0 ? v : -1;
    LeaveCriticalSection(&rlk);
    /* all of them or none: one missing would look like a saving */
    int all = (fp || fg) && (!fp || p->pkg_mw >= 0) && (!fg || p->gpu_mw >= 0);
    p->parts_mw = all ? (p->pkg_mw > 0 ? p->pkg_mw : 0) + (p->gpu_mw > 0 ? p->gpu_mw : 0) : -1;
    return p->sys_mw >= 0 || p->parts_mw >= 0 ? 0 : -1;
}

int ph_power_total(const ph_power *p, int *src)
{
    if (p->sys_mw >= 0) { if (src) *src = PSRC_SYSTEM; return p->sys_mw; }
    if (p->parts_mw >= 0) { if (src) *src = PSRC_PARTS; return p->parts_mw; }
    if (src) *src = PSRC_NONE;
    return -1;
}
