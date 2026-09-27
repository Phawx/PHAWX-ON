#include "phawx.h"
#include <wctype.h>

/* ---------- embedded controller access (through drv_io_*) ---------- */

enum { EC_ACPI, EC_ITE };

typedef struct ec_bus { uint8_t kind; uint16_t idx, dat; } ec_bus;

static HANDLE mx_ec, mx_isa;

static HANDLE ec_mutex(const ec_bus *b)
{
    HANDLE *h = b->kind == EC_ACPI ? &mx_ec : &mx_isa;
    if (!*h) {
        const wchar_t *n = b->kind == EC_ACPI ? L"Global\\Access_EC" : L"Global\\Access_ISABUS.HTP.Method";
        *h = CreateMutexW(NULL, FALSE, n);
        if (!*h) *h = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, n);
    }
    return *h;
}

static int ec_lock(const ec_bus *b)
{
    HANDLE h = ec_mutex(b);
    if (!h) return 0;
    DWORD w = WaitForSingleObject(h, 500);
    return (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) ? 0 : -1;
}

static void ec_unlock(const ec_bus *b)
{
    HANDLE h = b->kind == EC_ACPI ? mx_ec : mx_isa;
    if (h) ReleaseMutex(h);
}

#define ACPI_DATA 0x62
#define ACPI_CMD  0x66
#define OBF 0x01
#define IBF 0x02

static int acpi_wait(uint8_t mask, uint8_t want)
{
    for (int i = 0; i < 5000; i++) {
        uint8_t s;
        if (drv_io_rd8(ACPI_CMD, &s)) return -1;
        if ((s & mask) == want) return 0;
        if (i > 100) Sleep(0);
    }
    return -1;
}

static int acpi_rd(uint8_t addr, uint8_t *v)
{
    if (acpi_wait(IBF, 0) || drv_io_wr8(ACPI_CMD, 0x80) || acpi_wait(IBF, 0) ||
        drv_io_wr8(ACPI_DATA, addr) || acpi_wait(OBF, OBF))
        return -1;
    return drv_io_rd8(ACPI_DATA, v);
}

static int acpi_wr(uint8_t addr, uint8_t v)
{
    if (acpi_wait(IBF, 0) || drv_io_wr8(ACPI_CMD, 0x81) || acpi_wait(IBF, 0) ||
        drv_io_wr8(ACPI_DATA, addr) || acpi_wait(IBF, 0) || drv_io_wr8(ACPI_DATA, v))
        return -1;
    return acpi_wait(IBF, 0);
}

static int ite_sel(const ec_bus *b, uint16_t off)
{
    return drv_io_wr8(b->idx, 0x2E) || drv_io_wr8(b->dat, 0x11) ||
           drv_io_wr8(b->idx, 0x2F) || drv_io_wr8(b->dat, (uint8_t)(off >> 8)) ||
           drv_io_wr8(b->idx, 0x2E) || drv_io_wr8(b->dat, 0x10) ||
           drv_io_wr8(b->idx, 0x2F) || drv_io_wr8(b->dat, (uint8_t)off) ||
           drv_io_wr8(b->idx, 0x2E) || drv_io_wr8(b->dat, 0x12) ||
           drv_io_wr8(b->idx, 0x2F) ? -1 : 0;
}

static int ec_rd(const ec_bus *b, uint16_t off, uint8_t *v)
{
    if (ec_lock(b)) return -1;
    int rc = b->kind == EC_ACPI ? (off > 0xFF ? -1 : acpi_rd((uint8_t)off, v))
                                : (ite_sel(b, off) ? -1 : drv_io_rd8(b->dat, v));
    ec_unlock(b);
    return rc;
}

static int ec_wr(const ec_bus *b, uint16_t off, uint8_t v)
{
    if (ec_lock(b)) return -1;
    int rc = b->kind == EC_ACPI ? (off > 0xFF ? -1 : acpi_wr((uint8_t)off, v))
                                : (ite_sel(b, off) ? -1 : drv_io_wr8(b->dat, v));
    ec_unlock(b);
    return rc;
}

/* shared with plugins: the ACPI EC through the same lock and protocol as the fan code */
static const ec_bus acpi_ec = { EC_ACPI, ACPI_CMD, ACPI_DATA };

int ph_ec_read(uint8_t reg, uint8_t *v)
{
    if (!v || !drv_ok()) return -1;
    return ec_rd(&acpi_ec, reg, v);
}

int ph_ec_write(uint8_t reg, uint8_t v)
{
    if (!drv_ok()) return -1;
    return ec_wr(&acpi_ec, reg, v);
}

/* ---------- device plugin table ---------- */

typedef struct dev_def dev_def;
typedef struct dev_ops {
    int  (*init)(const dev_def *d);
    void (*shutdown)(void);
    void (*resume)(void);
    void (*tick)(void);
} dev_ops;

struct dev_def {
    const wchar_t *maker;    /* substring of SystemManufacturer, NULL = any */
    const wchar_t *product;  /* prefix of SystemProductName, NULL = any */
    const wchar_t *board;    /* prefix of BaseBoardProduct, NULL = any */
    const wchar_t *name;
    const dev_ops *ops;
    const void    *cfg;
    uint16_t       flags;    /* extra CF_* for this device's controls */
};

/* ---------- generic EC fan plugin ---------- */

typedef struct fan_def {
    ec_bus   bus;
    uint16_t reg_pwm;
    uint16_t reg_rpm_hi, reg_rpm_lo;  /* 0 = no tachometer */
    uint16_t reg_en;                  /* 0 = pwm value 0 means automatic */
    uint8_t  en_manual, en_auto;
    uint8_t  pwm_max;
} fan_def;

static const fan_def fan_gpd_mini_ite = {
    { EC_ITE, 0x4E, 0x4F }, 0x047A, 0x0478, 0x0479, 0, 0, 0, 244
};

static const fan_def fan_gpd_mini_acpi = {
    { EC_ACPI, ACPI_CMD, ACPI_DATA }, 0x7A, 0x78, 0x79, 0, 0, 0, 244
};

static const fan_def *fan;
static int fan_touched;

enum { FM_AUTO, FM_MANUAL, FM_FULL };
static const wchar_t *const fan_modes[] = { L"Auto", L"Manual", L"Full speed", NULL };

enum { F_HDR, F_MODE, F_SPEED, F_RPM, F_N };
static ph_ctl fan_ctls[F_N];

static int fan_write_auto(void)
{
    if (fan->reg_en) return ec_wr(&fan->bus, fan->reg_en, fan->en_auto);
    return ec_wr(&fan->bus, fan->reg_pwm, 0);
}

static int fan_write_duty(int pct)
{
    int d = (pct * fan->pwm_max + 50) / 100;
    d = PH_CLAMP(d, 1, fan->pwm_max);
    if (fan->reg_en && ec_wr(&fan->bus, fan->reg_en, fan->en_manual)) return -1;
    if (ec_wr(&fan->bus, fan->reg_pwm, (uint8_t)d)) return -1;
    fan_touched = 1;
    return 0;
}

static int fan_apply(int mode, int pct)
{
    if (mode == FM_AUTO) {
        int rc = fan_write_auto();
        if (!rc) fan_touched = 0;
        return rc;
    }
    return fan_write_duty(mode == FM_FULL ? 100 : pct);
}

static int set_mode(ph_ctl *c, int32_t v)
{
    (void)c;
    return fan_apply(v, fan_ctls[F_SPEED].val);
}

static int set_speed(ph_ctl *c, int32_t v)
{
    (void)c;
    ph_ctl *m = &fan_ctls[F_MODE];
    if (m->active && m->val == FM_MANUAL) return fan_write_duty(v);
    return 0;
}

static int get_rpm(ph_ctl *c, int32_t *out)
{
    (void)c;
    uint8_t hi, lo;
    if (!fan->reg_rpm_hi || ec_rd(&fan->bus, fan->reg_rpm_hi, &hi) || ec_rd(&fan->bus, fan->reg_rpm_lo, &lo)) return -1;
    *out = (hi << 8) | lo;
    return 0;
}

static int fan_init(const dev_def *d)
{
    fan = d->cfg;
    uint8_t a, b;
    if (ec_rd(&fan->bus, fan->reg_pwm, &a)) return -1;
    if (fan->reg_rpm_hi) {
        if (ec_rd(&fan->bus, fan->reg_rpm_hi, &a) || ec_rd(&fan->bus, fan->reg_rpm_lo, &b)) return -1;
        if (a == 0xFF && b == 0xFF) return -1;
    }
    uint16_t fl = CF_OPTIONAL | CF_REAPPLY | CF_PROFILE | d->flags;
    fan_ctls[F_HDR] = (ph_ctl){ .key = NULL, .label = d->name, .type = CT_HEADER, .page = PG_SYSTEM, .order = 200, .flags = d->flags };
    fan_ctls[F_MODE] = (ph_ctl){ .key = "dev.fanmode", .label = L"Fan mode", .type = CT_CHOICE, .page = PG_SYSTEM,
                                 .order = 201, .flags = fl, .choices = fan_modes, .set = set_mode };
    fan_ctls[F_SPEED] = (ph_ctl){ .key = "dev.fanspeed", .label = L"Manual fan speed", .type = CT_SLIDER, .page = PG_SYSTEM,
                                  .order = 202, .flags = (uint16_t)(CF_PROFILE | d->flags), .min = 10, .max = 100, .step = 5,
                                  .def = 50, .unit = L"%", .fmt = fmt_pct, .set = set_speed };
    fan_ctls[F_RPM] = (ph_ctl){ .key = NULL, .label = L"Fan speed", .type = CT_INFO, .page = PG_SYSTEM, .order = 203,
                                .flags = d->flags, .unit = L"RPM", .get = get_rpm };
    if (!fan->reg_rpm_hi) fan_ctls[F_RPM].flags |= CF_HIDDEN;
    ph_register_ctls(fan_ctls, F_N);
    return 0;
}

static void fan_shutdown(void)
{
    if (fan && fan_touched) fan_apply(FM_AUTO, 0);
}

static void fan_tick(void)
{
    if (fan && fan_touched && !fan_ctls[F_MODE].active) fan_apply(FM_AUTO, 0);
}

static const dev_ops ec_fan_ops = { fan_init, fan_shutdown, NULL, fan_tick };

static const dev_def devs[] = {
    { L"GPD", L"G1617-", NULL, L"GPD Win Mini", &ec_fan_ops, &fan_gpd_mini_ite, 0 },
    { L"GPD", L"G1617-", NULL, L"GPD Win Mini (ACPI EC)", &ec_fan_ops, &fan_gpd_mini_acpi, CF_ADVANCED },
};

/* ---------- backend ---------- */

static const dev_def *active;

static int ieq_prefix(const wchar_t *s, const wchar_t *p)
{
    for (; *p; s++, p++)
        if (!*s || towupper(*s) != towupper(*p)) return 0;
    return 1;
}

static int icontains(const wchar_t *s, const wchar_t *p)
{
    for (; *s; s++)
        if (ieq_prefix(s, p)) return 1;
    return !*p;
}

static int matches(const dev_def *d)
{
    return (!d->maker || icontains(g_plat.maker, d->maker)) &&
           (!d->product || ieq_prefix(g_plat.product, d->product)) &&
           (!d->board || ieq_prefix(g_plat.board, d->board));
}

static int dv_probe(void)
{
    for (int i = 0; i < PH_ARRAY(devs); i++)
        if (matches(&devs[i])) return 1;
    return 0;
}

static int dv_init(void)
{
    for (int i = 0; i < PH_ARRAY(devs); i++) {
        const dev_def *d = &devs[i];
        if (!matches(d) || !d->ops->init) continue;
        if (d->ops->init(d) == 0) {
            active = d;
            ph_log("device: %ls", d->name);
            return 0;
        }
    }
    return -1;
}

static void dv_shutdown(void)
{
    if (active && active->ops->shutdown) active->ops->shutdown();
    if (mx_ec) { CloseHandle(mx_ec); mx_ec = NULL; }
    if (mx_isa) { CloseHandle(mx_isa); mx_isa = NULL; }
}

static void dv_resume(void)
{
    if (active && active->ops->resume) active->ops->resume();
}

static void dv_tick(void)
{
    if (active && active->ops->tick) active->ops->tick();
}

ph_backend bk_devices = {
    "devices",
    L"Device plugins",
    BK_DEVICE,
    dv_probe,
    dv_init,
    dv_shutdown,
    dv_resume,
    dv_tick,
    0
};
