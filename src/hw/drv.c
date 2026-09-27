#include "phawx.h"
#include "drv_priv.h"

enum { LK_NONE, LK_PCI, LK_EC };

static const ph_drv *const providers[] = { &drv_pawnio };

static const ph_drv *g_drv;
static CRITICAL_SECTION cs;
static int cs_ready;
static HANDLE mx_pci, mx_ec;

static HANDLE named_mutex(const wchar_t *name)
{
    HANDLE h = CreateMutexW(NULL, FALSE, name);
    if (!h) h = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name);
    return h;
}

static HANDLE lock_mutex(int kind)
{
    return kind == LK_PCI ? mx_pci : kind == LK_EC ? mx_ec : NULL;
}

static int lock(int kind)
{
    if (!cs_ready || !g_drv) return -1;
    HANDLE m = lock_mutex(kind);
    if (m) {
        DWORD w = WaitForSingleObject(m, 500);
        if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) return -1;
    }
    EnterCriticalSection(&cs);
    if (!g_drv) {
        LeaveCriticalSection(&cs);
        if (m) ReleaseMutex(m);
        return -1;
    }
    return 0;
}

static void unlock(int kind)
{
    HANDLE m = lock_mutex(kind);
    LeaveCriticalSection(&cs);
    if (m) ReleaseMutex(m);
}

int drv_open(void)
{
    if (!cs_ready) { InitializeCriticalSection(&cs); cs_ready = 1; }
    EnterCriticalSection(&cs);
    if (!g_drv) {
        if (!mx_pci) mx_pci = named_mutex(L"Global\\Access_PCI");
        if (!mx_ec) mx_ec = named_mutex(L"Global\\Access_EC");
        for (int i = 0; i < PH_ARRAY(providers); i++)
            if (providers[i]->open() == 0) { g_drv = providers[i]; break; }
    }
    int rc = g_drv ? 0 : -1;
    LeaveCriticalSection(&cs);
    ph_log("driver: %s", g_drv ? drv_name() : "none");
    return rc;
}

int drv_ok(void)
{
    return g_drv != NULL;
}

int drv_state(void)
{
    return g_drv ? DRV_OK : pawnio_state == DRV_OK ? DRV_STOPPED : pawnio_state;
}

const char *drv_version(void)
{
    return pawnio_ver;
}

const char *drv_missing(void)
{
    return pawnio_missing;
}

const char *drv_name(void)
{
    const ph_drv *d = g_drv;
    if (!d) return "none";
    if (d == &drv_pawnio) return pawnio_name;
    return d->id;
}

void drv_close(void)
{
    if (!cs_ready) return;
    EnterCriticalSection(&cs);
    if (g_drv) { g_drv->close(); g_drv = NULL; }
    LeaveCriticalSection(&cs);
}

int drv_rdmsr_cpu(uint32_t msr, int cpu, uint64_t *v)
{
    if (!v || lock(LK_NONE)) return -1;
    int rc = g_drv->rdmsr ? g_drv->rdmsr(msr, cpu, v) : -1;
    unlock(LK_NONE);
    return rc;
}

int drv_wrmsr_cpu(uint32_t msr, int cpu, uint64_t v)
{
    if (lock(LK_NONE)) return -1;
    int rc = g_drv->wrmsr ? g_drv->wrmsr(msr, cpu, v) : -1;
    unlock(LK_NONE);
    return rc;
}

int drv_rdmsr(uint32_t msr, uint64_t *v)  { return drv_rdmsr_cpu(msr, -1, v); }
int drv_wrmsr(uint32_t msr, uint64_t v)   { return drv_wrmsr_cpu(msr, -1, v); }

static int ncpu(void)
{
    int n = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n > 0 ? n : 1;
}

int drv_wrmsr_all(uint32_t msr, uint64_t v)
{
    if (lock(LK_NONE)) return -1;
    int rc = -1;
    if (g_drv->wrmsr) {
        rc = 0;
        for (int i = 0, n = ncpu(); i < n; i++)
            if (g_drv->wrmsr(msr, i, v)) { rc = -1; if (i == 0) break; }
    }
    unlock(LK_NONE);
    return rc;
}

int drv_rmw_all(uint32_t msr, uint64_t mask, uint64_t val)
{
    if (lock(LK_NONE)) return -1;
    int rc = -1;
    if (g_drv->rdmsr && g_drv->wrmsr) {
        rc = 0;
        for (int i = 0, n = ncpu(); i < n; i++) {
            uint64_t v;
            if (g_drv->rdmsr(msr, i, &v) || g_drv->wrmsr(msr, i, (v & ~mask) | (val & mask))) {
                rc = -1;
                if (i == 0) break;
            }
        }
    }
    unlock(LK_NONE);
    return rc;
}

int drv_pci_rd(uint32_t bdf, uint32_t off, uint32_t *v)
{
    if (!v || lock(LK_PCI)) return -1;
    int rc = g_drv->pci_rd ? g_drv->pci_rd(bdf, off, v) : -1;
    unlock(LK_PCI);
    return rc;
}

int drv_pci_wr(uint32_t bdf, uint32_t off, uint32_t v)
{
    if (lock(LK_PCI)) return -1;
    int rc = g_drv->pci_wr ? g_drv->pci_wr(bdf, off, v) : -1;
    unlock(LK_PCI);
    return rc;
}

static int mem_rd(uint64_t pa, void *v, uint32_t unit)
{
    if (!v || lock(LK_PCI)) return -1;
    int rc = g_drv->mem_rd ? g_drv->mem_rd(pa, v, unit, 1) : -1;
    unlock(LK_PCI);
    return rc;
}

static int mem_wr(uint64_t pa, const void *v, uint32_t unit)
{
    if (lock(LK_PCI)) return -1;
    int rc = g_drv->mem_wr ? g_drv->mem_wr(pa, v, unit, 1) : -1;
    unlock(LK_PCI);
    return rc;
}

int drv_mem_rd32(uint64_t pa, uint32_t *v) { return mem_rd(pa, v, 4); }
int drv_mem_rd16(uint64_t pa, uint16_t *v) { return mem_rd(pa, v, 2); }
int drv_mem_rd8(uint64_t pa, uint8_t *v)   { return mem_rd(pa, v, 1); }
int drv_mem_wr32(uint64_t pa, uint32_t v)  { return mem_wr(pa, &v, 4); }
int drv_mem_wr16(uint64_t pa, uint16_t v)  { return mem_wr(pa, &v, 2); }
int drv_mem_wr8(uint64_t pa, uint8_t v)    { return mem_wr(pa, &v, 1); }

int drv_io_rd8(uint16_t port, uint8_t *v)
{
    if (!v || lock(LK_EC)) return -1;
    int rc = g_drv->io_rd8 ? g_drv->io_rd8(port, v) : -1;
    unlock(LK_EC);
    return rc;
}

int drv_io_wr8(uint16_t port, uint8_t v)
{
    if (lock(LK_EC)) return -1;
    int rc = g_drv->io_wr8 ? g_drv->io_wr8(port, v) : -1;
    unlock(LK_EC);
    return rc;
}

static int drv_probe(void) { return 1; }

static int drv_init(void)
{
    drv_open();
    return 0;
}

ph_backend bk_drv = {
    "drv",
    L"PawnIO driver",
    BK_DRIVER,
    drv_probe,
    drv_init,
    drv_close,
    NULL,
    NULL,
    0
};
