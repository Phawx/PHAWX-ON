#include "smu.h"

#define NB_SMN_INDEX 0xB8
#define NB_SMN_DATA  0xBC

enum { RSP_OK = 0x01, RSP_FAILED = 0xFF, RSP_UNKNOWN = 0xFE, RSP_PREREQ = 0xFD, RSP_BUSY = 0xFC };
enum { MB_MP1, MB_PSMU };

typedef struct { uint32_t msg, rsp, arg; } mbox;
typedef struct { mbox m[2]; } mset;

static const mset SET1 = {{{0x03B10528, 0x03B10564, 0x03B10998}, {0x03B10A20, 0x03B10A80, 0x03B10A88}}};
static const mset SET2 = {{{0x03B10528, 0x03B10578, 0x03B10998}, {0x03B10A20, 0x03B10A80, 0x03B10A88}}};
static const mset SET3 = {{{0x03B10928, 0x03B10978, 0x03B10998}, {0x03B10A20, 0x03B10A80, 0x03B10A88}}};
static const mset SET4 = {{{0x03B10530, 0x03B1057C, 0x03B109C4}, {0x03B10524, 0x03B10570, 0x03B10A40}}};

typedef struct { uint8_t fam, model; int8_t af; const mset *mb; } cpu_id;
static const cpu_id ids[] = {
    {0x17, 0x11, AF_RAVEN,     &SET1}, {0x17, 0x12, AF_RAVEN, &SET1},
    {0x17, 0x18, AF_PICASSO,   &SET1},
    {0x17, 0x20, AF_DALI,      &SET1},
    {0x17, 0x60, AF_RENOIR,    &SET1},
    {0x17, 0x68, AF_LUCIENNE,  &SET1},
    {0x17, 0x90, AF_VANGOGH,   &SET2}, {0x17, 0x91, AF_VANGOGH, &SET2},
    {0x17, 0xA0, AF_MENDOCINO, &SET2},
    {0x19, 0x50, AF_CEZANNE,   &SET1},
    {0x19, 0x40, AF_REMBRANDT, &SET2}, {0x19, 0x44, AF_REMBRANDT, &SET2},
    {0x19, 0x61, AF_DRAGON,    &SET4},
    {0x19, 0x74, AF_PHOENIX,   &SET2}, {0x19, 0x78, AF_PHOENIX, &SET2},
    {0x19, 0x75, AF_HAWKPOINT, &SET2}, {0x19, 0x7C, AF_HAWKPOINT, &SET2},
    {0x1A, 0x20, AF_STRIX,     &SET3}, {0x1A, 0x24, AF_STRIX, &SET3},
    {0x1A, 0x44, AF_FIRE,      &SET4},
    {0x1A, 0x60, AF_KRACKAN,   &SET3}, {0x1A, 0x68, AF_KRACKAN, &SET3},
    {0x1A, 0x70, AF_STRIXHALO, &SET3},
};

#define B(f)  (1u << (f))
#define G_RV  (B(AF_RAVEN) | B(AF_PICASSO) | B(AF_DALI))
#define G_RN  (B(AF_RENOIR) | B(AF_LUCIENNE) | B(AF_CEZANNE))
#define G_VG  (B(AF_VANGOGH))
#define G_RMB (B(AF_REMBRANDT))
#define G_MDN (B(AF_MENDOCINO))
#define G_PHX (B(AF_PHOENIX) | B(AF_HAWKPOINT))
#define G_STX (B(AF_KRACKAN) | B(AF_STRIX) | B(AF_STRIXHALO))
#define G_DR  (B(AF_DRAGON) | B(AF_FIRE))
#define G_MOB (G_RN | G_VG | G_RMB | G_MDN | G_PHX | G_STX)

typedef struct { uint8_t s, mb; uint16_t msg; uint32_t fams; } smu_cmd;
static const smu_cmd cmds[] = {
    {S_STAPM_LIMIT,    MB_MP1,  0x1A, G_RV}, {S_STAPM_LIMIT, MB_MP1, 0x14, G_MOB},
    {S_STAPM_LIMIT,    MB_PSMU, 0x31, G_MOB}, {S_STAPM_LIMIT, MB_MP1, 0x4F, G_DR},
    {S_FAST_LIMIT,     MB_MP1,  0x1B, G_RV}, {S_FAST_LIMIT, MB_MP1, 0x15, G_MOB},
    {S_FAST_LIMIT,     MB_MP1,  0x3E, G_DR},
    {S_SLOW_LIMIT,     MB_MP1,  0x1C, G_RV}, {S_SLOW_LIMIT, MB_MP1, 0x16, G_MOB},
    {S_SLOW_LIMIT,     MB_MP1,  0x5F, G_DR},
    {S_SLOW_TIME,      MB_MP1,  0x1D, G_RV}, {S_SLOW_TIME, MB_MP1, 0x17, G_MOB},
    {S_SLOW_TIME,      MB_MP1,  0x60, G_DR},
    {S_STAPM_TIME,     MB_MP1,  0x1E, G_RV}, {S_STAPM_TIME, MB_MP1, 0x18, G_MOB},
    {S_STAPM_TIME,     MB_MP1,  0x4E, G_DR},
    {S_TCTL_TEMP,      MB_MP1,  0x1F, G_RV}, {S_TCTL_TEMP, MB_MP1, 0x19, G_MOB},
    {S_TCTL_TEMP,      MB_MP1,  0x3F, G_DR},
    {S_APU_SLOW_LIMIT, MB_MP1,  0x21, G_RN}, {S_APU_SLOW_LIMIT, MB_MP1, 0x23, G_RMB | G_PHX | G_STX},
    {S_MAX_GFXCLK,     MB_MP1,  0x46, G_RV | B(AF_LUCIENNE)},
    {S_MIN_GFXCLK,     MB_MP1,  0x47, G_RV | B(AF_LUCIENNE)},
};

static int fam = AF_UNKNOWN;
static const mset *mb;
static int mb_ok[2];
static uint32_t ver;
static uint8_t dead[PH_ARRAY(cmds)];
static CRITICAL_SECTION cs;
static int cs_ready;
static HANDLE mx;
static uint64_t pm_addr;
static uint16_t pm_xfer_msg;
static uint32_t pm_xfer_arg;
static int pm_ok;
static double pm_last;

static int smn_rd(uint32_t a, uint32_t *v)
{
    if (drv_pci_wr(0, NB_SMN_INDEX, a & ~3u)) return -1;
    return drv_pci_rd(0, NB_SMN_DATA, v);
}

static int smn_wr(uint32_t a, uint32_t v)
{
    if (drv_pci_wr(0, NB_SMN_INDEX, a)) return -1;
    return drv_pci_wr(0, NB_SMN_DATA, v);
}

static int lock(void)
{
    if (!cs_ready) return -1;
    EnterCriticalSection(&cs);
    if (mx) {
        DWORD w = WaitForSingleObject(mx, 500);
        if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) { LeaveCriticalSection(&cs); return -1; }
    }
    return 0;
}

static void unlock(void)
{
    if (mx) ReleaseMutex(mx);
    LeaveCriticalSection(&cs);
}

static int wait_rsp(const mbox *m, uint32_t *r)
{
    double t0 = ph_qpc_ms();
    unsigned spin = 0;
    for (;;) {
        if (smn_rd(m->rsp, r)) return -1;
        if (*r) return 0;
        if (ph_qpc_ms() - t0 > 200.0) return -1;
        if (++spin > 32) Sleep(1);
        else if (spin > 4) SwitchToThread();
    }
}

static int send(int w, uint32_t id, uint32_t a[6])
{
    if (!mb) return -1;
    const mbox *m = &mb->m[w];
    uint32_t r;
    int rc = -1;
    if (lock()) return -1;
    if (wait_rsp(m, &r) || smn_wr(m->rsp, 0)) goto out;
    for (int i = 0; i < 6; i++)
        if (smn_wr(m->arg + 4u * i, a[i])) goto out;
    if (smn_wr(m->msg, id) || wait_rsp(m, &r)) goto out;
    if (r != RSP_OK) { rc = (int)r; goto out; }
    for (int i = 0; i < 6; i++)
        if (smn_rd(m->arg + 4u * i, &a[i])) goto out;
    rc = 0;
out:
    unlock();
    return rc;
}

static int send1(int w, uint32_t id, uint32_t a0)
{
    uint32_t a[6] = { a0, 0, 0, 0, 0, 0 };
    return send(w, id, a);
}

int smu_detect(void)
{
    if (g_plat.vendor != VENDOR_AMD) return AF_UNKNOWN;
    for (int i = 0; i < PH_ARRAY(ids); i++)
        if (ids[i].fam == g_plat.family && ids[i].model == g_plat.model) return ids[i].af;
    return AF_UNKNOWN;
}

static const mset *detect_set(void)
{
    for (int i = 0; i < PH_ARRAY(ids); i++)
        if (ids[i].fam == g_plat.family && ids[i].model == g_plat.model) return ids[i].mb;
    return NULL;
}

static int pm_read_raw(uint32_t off, float *v)
{
    uint32_t u;
    if (!pm_addr || drv_mem_rd32(pm_addr + off, &u)) return -1;
    memcpy(v, &u, sizeof u);
    return 0;
}

static int pm_xfer(void)
{
    int rc = send1(MB_PSMU, pm_xfer_msg, pm_xfer_arg);
    if (rc == RSP_PREREQ) { Sleep(10); rc = send1(MB_PSMU, pm_xfer_msg, pm_xfer_arg); }
    pm_last = ph_qpc_ms();
    return rc == 0 ? 0 : -1;
}

static void pm_setup(void)
{
    uint32_t f = B(fam), a[6] = { 0 };
    int wide = 0;
    uint16_t get;
    if (!mb_ok[MB_PSMU]) return;
    if (f & G_RV) { get = 0x0B; a[0] = 3; pm_xfer_msg = 0x3D; pm_xfer_arg = 3; }
    else if (f & (G_RN | G_RMB | G_PHX | G_STX)) { get = 0x66; a[0] = a[1] = 1; pm_xfer_msg = 0x65; wide = 1; }
    else return;
    if (send(MB_PSMU, get, a) != 0) return;
    pm_addr = wide ? ((uint64_t)a[1] << 32) | a[0] : a[0];
    if (!pm_addr) return;
    for (int tries = 0; tries < 2; tries++) {
        float lim;
        if (pm_xfer() == 0 && pm_read_raw(0, &lim) == 0 && lim >= 1.0f && lim <= 300.0f) { pm_ok = 1; return; }
        Sleep(10);
    }
    pm_addr = 0;
}

int smu_init(void)
{
    if (!cs_ready) { InitializeCriticalSection(&cs); cs_ready = 1; }
    fam = smu_detect();
    mb = detect_set();
    mb_ok[0] = mb_ok[1] = 0;
    pm_ok = 0;
    pm_addr = 0;
    ver = 0;
    memset(dead, 0, sizeof dead);
    if (fam == AF_UNKNOWN || !mb || !drv_ok()) { mb = NULL; return -1; }
    if (!mx) {
        mx = CreateMutexW(NULL, FALSE, L"Global\\Access_PCI");
        if (!mx) mx = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, L"Global\\Access_PCI");
    }

    uint32_t v = 0;
    int probe = -1;
    if (lock() == 0) {
        if (smn_wr(mb->m[MB_MP1].arg, 0x47) == 0 && smn_rd(mb->m[MB_MP1].arg, &v) == 0 && v == 0x47) probe = 0;
        unlock();
    }
    if (probe) { ph_log("smu: SMN access blocked"); mb = NULL; return -1; }
    if (send1(MB_MP1, 0x01, 0) != 0) { ph_log("smu: MP1 test failed"); mb = NULL; return -1; }
    mb_ok[MB_MP1] = 1;
    if (send1(MB_PSMU, 0x01, 0) == 0) mb_ok[MB_PSMU] = 1;

    uint32_t a[6] = { 1, 0, 0, 0, 0, 0 };
    if (send(MB_MP1, 0x02, a) == 0) ver = a[0];
    ph_log("smu: family %d, fw %u.%u.%u, psmu %d", fam,
           (ver >> 16) & 0xFF, (ver >> 8) & 0xFF, ver & 0xFF, mb_ok[MB_PSMU]);
    pm_setup();
    return 0;
}

int smu_family(void) { return fam; }
uint32_t smu_version(void) { return ver; }
int smu_pm_ok(void) { return pm_ok; }

int smu_has(int s)
{
    if (!mb || fam == AF_UNKNOWN) return 0;
    for (int i = 0; i < PH_ARRAY(cmds); i++)
        if (cmds[i].s == s && (cmds[i].fams & B(fam)) && mb_ok[cmds[i].mb] && !dead[i]) return 1;
    return 0;
}

int smu_set(int s, uint32_t v)
{
    int tried = 0;
    if (!mb || fam == AF_UNKNOWN) return -1;
    for (int i = 0; i < PH_ARRAY(cmds); i++) {
        if (cmds[i].s != s || !(cmds[i].fams & B(fam)) || !mb_ok[cmds[i].mb] || dead[i]) continue;
        int rc = send1(cmds[i].mb, cmds[i].msg, v);
        if (rc == 0) return 0;
        if (rc == RSP_UNKNOWN) dead[i] = 1;
        else tried = 1;
    }
    return tried ? -1 : 1;
}

int smu_pm_refresh(void)
{
    if (!pm_ok) return -1;
    if (ph_qpc_ms() - pm_last < 500.0) return 0;
    return pm_xfer();
}

int smu_pm_get(int field, float *v)
{
    int rv = fam == AF_RAVEN || fam == AF_PICASSO || fam == AF_DALI;
    int t58 = rv || fam == AF_STRIXHALO;
    int off;
    if (!pm_ok) return -1;
    switch (field) {
    case PM_STAPM_LIM: off = 0x00; break;
    case PM_STAPM_VAL: off = 0x04; break;
    case PM_FAST_LIM:  off = 0x08; break;
    case PM_FAST_VAL:  off = 0x0C; break;
    case PM_SLOW_LIM:  off = 0x10; break;
    case PM_SLOW_VAL:  off = 0x14; break;
    case PM_APU_SLOW_LIM: if (rv) return -1; off = 0x18; break;
    case PM_TCTL_LIM:  off = t58 ? 0x58 : 0x40; break;
    case PM_TCTL_VAL:  off = t58 ? 0x5C : 0x44; break;
    default: return -1;
    }
    return pm_read_raw((uint32_t)off, v);
}
