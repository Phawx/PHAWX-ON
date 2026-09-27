#include "phawx.h"
#include <string.h>

/* Fan curves. An owner (the GPD backend, the plugin host) registers a fan with a
   "Curve" mode; while that mode is selected, fans_tick sets the duty from the CPU
   temperature. Up at once, down in steps with a small dead band so the fan does
   not hunt. No temperature for a few seconds: the fan goes back to the firmware,
   and the curve takes over again when readings return. */

#define MAX_FANS   16
#define LOST_TICKS 5         /* seconds without a temperature before the firmware takes over */
#define DOWN_BAND  3         /* % the duty must drop by before the fan slows down */
#define DOWN_STEP  5         /* % per second at most when slowing down */
#define REMIND_MS  5000      /* some ECs go back to automatic unless the duty is written again */
#define HOT_C      95        /* at or above this the fan runs at 100 %, whatever the curve says */

static const uint8_t def_pts[FAN_PTS] = { 20, 25, 35, 50, 65, 85, 100 };

static ph_fan *fans[MAX_FANS];
static int nfans;
static volatile LONG stopped;
static int (*temp_fn)(int *);

int fan_point_temp(int i) { return 30 + 10 * i; }

void ph_set_cpu_temp_reader(int (*fn)(int *celsius)) { if (!temp_fn) temp_fn = fn; }

int ph_cpu_temp(int *c)
{
    int t = 0;
    if (!temp_fn || temp_fn(&t) || t <= 0 || t >= 125) return -1;
    *c = t;
    return 0;
}

int fan_curve_eval(const ph_fan *f, int t)
{
    int d;
    if (t >= HOT_C) d = 100;
    else if (t <= fan_point_temp(0)) d = f->pt[0];
    else if (t >= fan_point_temp(FAN_PTS - 1)) d = f->pt[FAN_PTS - 1];
    else {
        int i = (t - fan_point_temp(0)) / 10, t0 = fan_point_temp(i);
        d = f->pt[i] + (f->pt[i + 1] - f->pt[i]) * (t - t0) / 10;
    }
    return PH_CLAMP(d, f->min_pct, 100);
}

static int is_default(const ph_fan *f)
{
    for (int i = 0; i < FAN_PTS; i++)
        if (f->pt[i] != def_pts[i]) return 0;
    return 1;
}

static void save(ph_fan *f)
{
    char s[64];
    int n = 0;
    for (int i = 0; i < FAN_PTS; i++) n += ph_snprintf(s + n, (int)sizeof s - n, i ? ",%u" : "%u", f->pt[i]);
    cfg_set_str("global", f->cfg_key, is_default(f) ? NULL : s);
    f->row.active = !is_default(f);
}

static void load(ph_fan *f)
{
    char s[64];
    int v[FAN_PTS], n = 0;
    memcpy(f->pt, def_pts, sizeof f->pt);
    if (!cfg_get_str("global", f->cfg_key, s, sizeof s)) return;
    for (const char *p = s; *p && n < FAN_PTS;) {
        int x = 0, digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 4) { x = x * 10 + (*p++ - '0'); digits++; }
        if (!digits || x > 100) return;
        v[n++] = x;
        if (*p == ',') p++;
        else if (*p) return;
    }
    if (n != FAN_PTS) return;
    for (int i = 0; i < FAN_PTS; i++) f->pt[i] = (uint8_t)v[i];
}

/* keeps the curve rising: moving one point drags its neighbours along */
void fan_curve_point(ph_fan *f, int i, int pct, int save_now)
{
    if (i < 0 || i >= FAN_PTS) return;
    pct = PH_CLAMP(pct, 0, 100);
    f->pt[i] = (uint8_t)pct;
    for (int j = i + 1; j < FAN_PTS; j++) if (f->pt[j] < pct) f->pt[j] = (uint8_t)pct;
    for (int j = i - 1; j >= 0; j--) if (f->pt[j] > pct) f->pt[j] = (uint8_t)pct;
    f->row.active = !is_default(f);
    if (save_now) save(f);
}

static void tick_one(ph_fan *f)
{
    int t, want, d;
    if (stopped || !f->on) return;
    if (ph_cpu_temp(&t)) {
        f->temp = -1;
        if (++f->fails >= LOST_TICKS && !f->firmware) {
            f->autom(f);
            f->firmware = 1;
            f->cur = -1;
            ph_log("fans: no CPU temperature, %ls is back on the firmware", f->name ? f->name : L"fan");
        }
        return;
    }
    f->fails = 0;
    f->firmware = 0;
    f->temp = t;
    want = fan_curve_eval(f, t);
    if (f->cur < 0 || want > f->cur) d = want;
    else if (f->cur - want >= DOWN_BAND) d = f->cur - want > DOWN_STEP ? f->cur - DOWN_STEP : want;
    else d = f->cur;
    uint64_t now = ph_ms();
    if (d == f->cur && now - f->last_set < REMIND_MS) return;
    if (stopped) return;
    if (f->duty(f, d) == 0) {
        f->cur = d;
        f->last_set = now;
    }
}

static int row_set(ph_ctl *c, int32_t v)
{
    ph_fan *f = c->ctx;
    if (v == c->def) {
        memcpy(f->pt, def_pts, sizeof f->pt);
        save(f);
        f->cur = -1;
        tick_one(f);
    }
    return 0;
}

void fan_curve_add(ph_fan *f, const char *cfg_key, int16_t order, uint16_t flags)
{
    if (!f || nfans >= MAX_FANS) return;
    lstrcpynA(f->cfg_key, cfg_key, (int)sizeof f->cfg_key);
    load(f);
    f->on = 0;
    f->cur = f->temp = -1;
    f->row = (ph_ctl){ .label = L"Fan curve", .type = CT_CURVE, .page = PG_SYSTEM, .order = order,
                       .flags = (uint16_t)(flags | CF_HIDDEN | CF_NOSAVE | CF_NOPIN), .set = row_set, .ctx = f };
    f->row.active = !is_default(f);
    fans[nfans++] = f;
    ph_register_ctl(&f->row);
}

void fan_curve_use(ph_fan *f, int on)
{
    if (!f || !f->cfg_key[0]) return;
    on = on != 0;
    if (on) f->row.flags &= (uint16_t)~CF_HIDDEN;
    else f->row.flags |= CF_HIDDEN;
    if (on == f->on) return;
    f->on = on;
    f->cur = -1;
    f->fails = f->firmware = 0;
    f->last_set = 0;
    if (on) tick_one(f);
}

void fans_tick(void)
{
    for (int i = 0; i < nfans; i++) tick_one(fans[i]);
}

void fans_stop(void) { InterlockedExchange(&stopped, 1); }
