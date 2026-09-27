/* Example Phawx ON plugin: a simulated fan, simulated RGB lights and one control.
 *
 * It touches no hardware, so it is safe to try: turn it on in the Plugins page,
 * restart Phawx ON, and the rows show up under System and Plugins. Use it as a
 * template: replace the sim_* functions with real EC or HID access.
 *
 *   x86_64-w64-mingw32-gcc -shared -O2 -I../sdk example.c -o example.dll
 *   cl /LD /O2 /I..\sdk example.c
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "phawx_plugin.h"

static const phx_host *H;
static phx_plugin *SELF;

/* ---------- the pretend hardware ---------- */

static int sim_duty = -1;          /* -1 = firmware (automatic) */
static phx_rgb_state sim_led;
static int sim_led_custom;

static int PHX_CALL fan_auto(phx_fan *f)
{
    (void)f;
    sim_duty = -1;
    phx_logf(H, SELF, "fan: automatic");
    return 0;
}

static int PHX_CALL fan_duty(phx_fan *f, int pct)
{
    (void)f;
    sim_duty = pct;
    phx_logf(H, SELF, "fan: %d%%", pct);
    return 0;
}

static int PHX_CALL fan_rpm(phx_fan *f, int *rpm)
{
    (void)f;
    *rpm = sim_duty < 0 ? 2400 : 900 + sim_duty * 45;
    return 0;
}

static phx_fan fan = {
    .size = sizeof(phx_fan), .min_pct = 20, .id = "fan", .name = L"Example fan (simulated)",
    .set_auto = fan_auto, .set_duty = fan_duty, .get_rpm = fan_rpm,
};

static int PHX_CALL led_apply(phx_rgb *l, const phx_rgb_state *s)
{
    (void)l;
    sim_led = *s;
    sim_led_custom = 1;
    phx_logf(H, SELF, "leds: mode %d color %02x%02x%02x brightness %d", s->mode, s->r, s->g, s->b, s->brightness);
    return 0;
}

static int PHX_CALL led_restore(phx_rgb *l)
{
    (void)l;
    sim_led_custom = 0;
    phx_logf(H, SELF, "leds: back to firmware");
    return 0;
}

static const wchar_t *const effects[] = { L"Breathe", L"Rainbow", NULL };

static phx_rgb leds = {
    .size = sizeof(phx_rgb), .caps = PHX_RGB_COLOR | PHX_RGB_BRIGHTNESS, .id = "leds",
    .name = L"Example lights (simulated)", .effects = effects, .apply = led_apply, .restore = led_restore,
};

/* ---------- a plain control and an info row ---------- */

static int PHX_CALL quiet_set(phx_control *c, int32_t v)
{
    (void)c;
    phx_logf(H, SELF, "quiet mode %s", v ? "on" : "off");
    H->cfg_set_int(SELF, "last_quiet", v);     /* the plugin's own [plugin:example] section */
    return 0;
}

static void PHX_CALL state_fmt(const phx_control *c, int32_t v, wchar_t *b, int n)
{
    (void)c; (void)v;
    if (sim_duty < 0) lstrcpynW(b, L"Fan auto", n);
    else _snwprintf(b, (size_t)n, L"Fan %d%%", sim_duty);
}

static phx_control ctls[] = {
    { .size = sizeof(phx_control), .type = PHX_HEADER, .label = L"Example plugin", .page = PHX_PAGE_PLUGINS, .order = 0 },
    { .size = sizeof(phx_control), .type = PHX_TOGGLE, .key = "quiet", .label = L"Quiet mode (does nothing)",
      .page = PHX_PAGE_PLUGINS, .order = 1, .flags = PHX_F_PROFILE, .set = quiet_set },
    { .size = sizeof(phx_control), .type = PHX_INFO, .label = L"Simulated state", .page = PHX_PAGE_PLUGINS,
      .order = 2, .fmt = state_fmt },
};

/* ---------- lifetime ---------- */

static void PHX_CALL tick(void)
{
    /* runs about once a second while the overlay is open */
}

static void PHX_CALL on_shutdown(void)
{
    /* The host already handed the fan and lights back (set_auto / restore) before
       calling this. Undo anything else you changed here. */
    phx_logf(H, SELF, "shutdown");
}

static const phx_info info = {
    .size = sizeof(phx_info), .name = L"Example device", .version = L"1.0", .shutdown = on_shutdown, .tick = tick,
};

PHX_EXPORT int PHX_CALL phx_plugin_init(const phx_host *host, phx_plugin *self, const phx_info **out)
{
    H = host;
    SELF = self;
    if (host->version < PHX_API_VERSION) return PHX_E_VERSION;
    /* a real plugin checks for its device first, e.g.
       if (lstrcmpiW(host->platform->maker, L"GPD")) return PHX_UNSUPPORTED; */
    for (int i = 0; i < (int)(sizeof ctls / sizeof ctls[0]); i++)
        if (host->add_control(self, &ctls[i])) return PHX_ERROR;
    if (host->add_fan(self, &fan) || host->add_rgb(self, &leds)) return PHX_ERROR;
    host->set_status(self, L"simulated hardware");
    phx_logf(host, self, "started on %ls %ls", host->platform->maker, host->platform->product);
    *out = &info;
    return PHX_OK;
}
