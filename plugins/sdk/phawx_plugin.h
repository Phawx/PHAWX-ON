/* Phawx ON plugin API, version 1.
 *
 * A plugin is a 64-bit Windows DLL that exports phx_plugin_init. Put it in the
 * plugins\ folder next to PhawxON.exe (or in plugins\<name>\ together with the
 * DLLs it needs) and turn it on in the overlay's Plugins page. See docs/PLUGINS.md.
 *
 * This header is plain C and builds with MinGW-w64, clang and MSVC, from C or C++.
 * The layout of every struct is fixed: members are fixed-width, padding is
 * explicit, and the size checks at the bottom fail the build if a compiler lays
 * them out differently.
 *
 * MIT licensed, like the rest of Phawx ON.
 */
#ifndef PHAWX_PLUGIN_H
#define PHAWX_PLUGIN_H

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#if !defined(_WIN64)
#error "Phawx ON plugins must be built for 64-bit Windows (x64)."
#endif

#pragma pack(push, 8)

#define PHX_API_VERSION 1

/* x64 has one calling convention; the macro documents the boundary. Never let a
   C++ exception or longjmp cross it, and never free memory the other side allocated. */
#define PHX_CALL   __cdecl
#define PHX_EXPORT __declspec(dllexport)
#if defined(_MSC_VER) && !defined(__cplusplus)
#define PHX_INLINE static __inline
#else
#define PHX_INLINE static inline
#endif

/* return values of phx_plugin_init */
#define PHX_OK           0    /* running */
#define PHX_UNSUPPORTED  1    /* not for this machine: shown as "Not running", not an error */
#define PHX_ERROR      (-1)   /* failed; call host->set_status first to say why */
#define PHX_E_VERSION  (-2)   /* this Phawx ON is too old for the plugin */

/* pages of the overlay */
enum {
    PHX_PAGE_QUICK = 0, PHX_PAGE_CPU, PHX_PAGE_POWER, PHX_PAGE_GPU,
    PHX_PAGE_DISPLAY, PHX_PAGE_SYSTEM, PHX_PAGE_PLUGINS
};

/* control types */
enum {
    PHX_SLIDER = 0,   /* min..max in steps of step */
    PHX_TOGGLE,       /* 0 / 1 */
    PHX_CHOICE,       /* index into choices */
    PHX_ACTION,       /* a button; set() runs it */
    PHX_INFO,         /* read-only value from get()/fmt() */
    PHX_HEADER        /* section title */
};

/* control flags */
#define PHX_F_NOSAVE    0x0001u  /* never written to phawx.ini */
#define PHX_F_REAPPLY   0x0002u  /* firmware may reset it (every set value is applied again after sleep and AC/DC changes) */
#define PHX_F_OPTIONAL  0x0004u  /* has a "Default" position that leaves the hardware alone */
#define PHX_F_ADVANCED  0x0008u  /* only shown with Settings > Show advanced options */
#define PHX_F_DANGER    0x0010u  /* asks for confirmation before set() */
#define PHX_F_LIVE      0x0020u  /* set() while a slider is dragged, not only on release */
#define PHX_F_HIDDEN    0x0040u  /* not shown (use when the hardware lacks the feature) */
#define PHX_F_AUTOTDP   0x0080u  /* locked while AutoTDP drives your clock domain */
#define PHX_F_PROFILE   0x0100u  /* saved per game when per-game profiles are on */
#define PHX_F_SIGNED    0x0200u  /* show values with an explicit + sign */

typedef struct phx_plugin phx_plugin;   /* opaque; identifies your plugin to the host */
typedef struct phx_control phx_control;
typedef struct phx_clock phx_clock;
typedef struct phx_fan phx_fan;
typedef struct phx_rgb phx_rgb;

/* ---------- controls ----------
 * One row in the overlay. The host draws it, handles input, saves the value
 * (in [global] or a [game:<exe>] section under "<plugin id>.<key>"), restores it
 * at start and lets the user pin it to the Quick page. set() is only called with
 * values inside min..max.
 */
struct phx_control {
    uint32_t size;                    /* sizeof(phx_control) */
    uint32_t type;                    /* PHX_SLIDER ... */
    const char *key;                  /* "stapm": letters, digits, '_', '-', '.'; NULL for headers and info rows */
    const wchar_t *label;
    const wchar_t *unit;              /* optional, e.g. L"W" */
    const wchar_t *const *choices;    /* PHX_CHOICE: labels, NULL-terminated */
    uint32_t page;                    /* PHX_PAGE_* */
    uint32_t flags;                   /* PHX_F_* */
    int32_t  order;                   /* 0..999, position among your rows on that page */
    int32_t  min, max, step, def;     /* def: the value that "Default" stands for */
    int32_t  reserved0;
    int  (PHX_CALL *get)(phx_control *c, int32_t *out);                       /* optional; 0 = *out is valid */
    int  (PHX_CALL *set)(phx_control *c, int32_t value);                      /* 0 = applied */
    void (PHX_CALL *fmt)(const phx_control *c, int32_t value, wchar_t *buf, int n); /* optional value text */
    void *user;                       /* yours */
    /* kept up to date by the host before it calls you; read-only */
    int32_t  value;                   /* last applied value (def while at Default) */
    int32_t  active;                  /* 1 = the user chose a value, 0 = Default */
};

/* ---------- clock domains ----------
 * A CPU or GPU clock AutoTDP can drive. Among domains of the same kind the one
 * with the highest prio is used. These callbacks run on AutoTDP's worker
 * thread; the host still never runs two of your callbacks at the same time.
 * Keep each call short (well under 50 ms): AutoTDP waits for it.
 */
#define PHX_CLOCK_CPU 0u
#define PHX_CLOCK_GPU 1u

struct phx_clock {
    uint32_t size;                    /* sizeof(phx_clock) */
    uint32_t kind;                    /* PHX_CLOCK_CPU / PHX_CLOCK_GPU */
    const wchar_t *name;              /* shown under GPU > GPU clock control */
    int32_t  min_mhz, max_mhz, step_mhz;
    int32_t  prio;                    /* built-in domains use 10..30 */
    int (PHX_CALL *set_max)(phx_clock *d, int mhz);   /* cap the clock; 0 = ok */
    int (PHX_CALL *set_min)(phx_clock *d, int mhz);   /* optional */
    int (PHX_CALL *reset)(phx_clock *d);              /* remove the cap */
    int (PHX_CALL *cur)(phx_clock *d, int *mhz);      /* optional: current clock */
    int (PHX_CALL *util)(phx_clock *d, int *pct);     /* optional: busy %, improves bottleneck detection */
    void *user;
};

/* ---------- fans ----------
 * You provide the hardware access, the host provides the rows (Fan mode:
 * Default / Auto / Manual / Full speed, a speed slider, the RPM) and hands the
 * fan back to the firmware (set_auto) when the user picks Default, on exit and
 * after a crash.
 */
struct phx_fan {
    uint32_t size;                    /* sizeof(phx_fan) */
    int32_t  min_pct;                 /* lowest manual duty the fan accepts, e.g. 20 */
    const char *id;                   /* "fan" -> keys "<plugin>.fan.mode", "<plugin>.fan.speed" */
    const wchar_t *name;              /* section title, e.g. L"CPU fan" */
    int (PHX_CALL *set_auto)(phx_fan *f);            /* firmware control */
    int (PHX_CALL *set_duty)(phx_fan *f, int pct);   /* manual duty, min_pct..100 */
    int (PHX_CALL *get_rpm)(phx_fan *f, int *rpm);   /* optional */
    void *user;
};

/* ---------- RGB lighting ----------
 * The host provides Lighting (Default / Off / Solid / your effects), Color and
 * Brightness rows and calls apply() with the combined state. restore() hands the
 * LEDs back to the firmware when the user picks Default, on exit and after a crash.
 */
#define PHX_RGB_OFF   0
#define PHX_RGB_SOLID 1               /* effects[i] is mode 2 + i */

#define PHX_RGB_COLOR      0x1u       /* caps: apply() uses r, g, b */
#define PHX_RGB_BRIGHTNESS 0x2u       /* caps: apply() uses brightness */

typedef struct phx_rgb_state {
    int32_t mode;                     /* PHX_RGB_OFF, PHX_RGB_SOLID or 2 + effect index */
    uint8_t r, g, b, reserved;
    int32_t brightness;               /* 0..100 */
} phx_rgb_state;

struct phx_rgb {
    uint32_t size;                    /* sizeof(phx_rgb) */
    uint32_t caps;                    /* PHX_RGB_COLOR | PHX_RGB_BRIGHTNESS */
    const char *id;                   /* "leds" -> keys "<plugin>.leds.mode" ... */
    const wchar_t *name;              /* section title, e.g. L"Stick lights" */
    const wchar_t *const *effects;    /* optional extra modes, NULL-terminated, at most 8 */
    int (PHX_CALL *apply)(phx_rgb *l, const phx_rgb_state *s);
    int (PHX_CALL *restore)(phx_rgb *l);
    void *user;
};

/* ---------- what the host tells you about the machine ---------- */
typedef struct phx_platform {
    uint32_t size;
    uint32_t vendor;                  /* 0 unknown, 1 Intel, 2 AMD */
    uint32_t family, model, stepping; /* CPUID, extended family/model already added */
    int32_t  logical_cpus, cores, hybrid;
    const wchar_t *cpu_name;
    const wchar_t *maker;             /* SMBIOS SystemManufacturer, e.g. L"GPD" */
    const wchar_t *product;           /* SMBIOS SystemProductName */
    const wchar_t *board;             /* SMBIOS BaseBoardProduct */
} phx_platform;

/* ---------- yours: phx_plugin_init points *info at a static one ---------- */
typedef struct phx_info {
    uint32_t size;                    /* sizeof(phx_info) */
    uint32_t reserved0;
    const wchar_t *name;              /* shown on the Plugins page */
    const wchar_t *version;           /* e.g. L"1.0" */
    void (PHX_CALL *shutdown)(void);  /* undo every hardware change; also runs from the crash handler */
    void (PHX_CALL *resume)(void);    /* after sleep; the host then applies every set control again */
    void (PHX_CALL *tick)(void);      /* about once a second while the overlay is open */
} phx_info;

/* ---------- the host API ----------
 * The host calls you on its UI thread (clock callbacks: AutoTDP's thread) and
 * never runs two of your callbacks at once. Call the host back from the thread
 * it called you on; log, ec_* and msr_* may be called from any thread.
 * add_* only work inside phx_plugin_init, and what you add only takes effect if
 * init returns PHX_OK. Everything you pass (structs, strings, choice lists) must
 * stay valid until the process exits: use static storage.
 */
typedef struct phx_host {
    uint32_t size;                    /* sizeof(phx_host) of the host; newer hosts append members */
    uint32_t version;                 /* PHX_API_VERSION of the host */
    const phx_platform *platform;

    int  (PHX_CALL *add_control)(phx_plugin *p, phx_control *c);
    int  (PHX_CALL *add_clock)(phx_plugin *p, phx_clock *d);
    int  (PHX_CALL *add_fan)(phx_plugin *p, phx_fan *f);
    int  (PHX_CALL *add_rgb)(phx_plugin *p, phx_rgb *l);
    void (PHX_CALL *update)(phx_plugin *p, phx_control *c);  /* after changing label, unit, flags, range or choices */

    /* your own section [plugin:<id>] in phawx.ini */
    int  (PHX_CALL *cfg_get_int)(phx_plugin *p, const char *key, int def);
    void (PHX_CALL *cfg_set_int)(phx_plugin *p, const char *key, int value);
    int  (PHX_CALL *cfg_get_str)(phx_plugin *p, const char *key, char *out, int n); /* 1 = found */
    void (PHX_CALL *cfg_set_str)(phx_plugin *p, const char *key, const char *value); /* NULL deletes */

    /* one line to the debug log (phawx.log with PHAWX_LOG=1); see phx_logf below */
    void (PHX_CALL *log)(phx_plugin *p, const char *msg);
    void (PHX_CALL *toast)(phx_plugin *p, const wchar_t *msg);
    void (PHX_CALL *set_status)(phx_plugin *p, const wchar_t *msg); /* shown under your name; NULL clears */
    const wchar_t *(PHX_CALL *plugin_dir)(phx_plugin *p);          /* folder your DLL was loaded from */

    /* any control's state by its full key, e.g. "cpu.tdp"; -1 if absent or hidden */
    int  (PHX_CALL *ctl_get)(phx_plugin *p, const char *key, int32_t *value, int *active);

    /* hardware access through PawnIO's signed modules; -1 when unavailable */
    int  (PHX_CALL *hw_ready)(phx_plugin *p);
    int  (PHX_CALL *ec_read)(phx_plugin *p, uint8_t reg, uint8_t *value);   /* ACPI EC, ports 0x62/0x66 */
    int  (PHX_CALL *ec_write)(phx_plugin *p, uint8_t reg, uint8_t value);
    int  (PHX_CALL *msr_read)(phx_plugin *p, uint32_t msr, int cpu, uint64_t *value); /* cpu -1 = any */
    int  (PHX_CALL *msr_write)(phx_plugin *p, uint32_t msr, int cpu, uint64_t value);
} phx_host;

/* The one export:
 *
 *   PHX_EXPORT int PHX_CALL phx_plugin_init(const phx_host *host, phx_plugin *self,
 *                                           const phx_info **info);
 *
 * Point *info at your static phx_info and return PHX_OK, PHX_UNSUPPORTED or a
 * negative error. On failure, undo whatever you did before returning: the
 * host unloads the DLL. */
typedef int (PHX_CALL *phx_plugin_init_fn)(const phx_host *host, phx_plugin *self, const phx_info **info);
#define PHX_INIT_EXPORT "phx_plugin_init"

/* printf into the host log, formatted by your own C runtime */
PHX_INLINE void phx_logf(const phx_host *host, phx_plugin *self, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    buf[sizeof buf - 1] = 0;
    host->log(self, buf);
}

/* layout checks: a failure here means this compiler would break the ABI */
typedef char phx_check_control[sizeof(phx_control) == 112 ? 1 : -1];
typedef char phx_check_clock[sizeof(phx_clock) == 80 ? 1 : -1];
typedef char phx_check_fan[sizeof(phx_fan) == 56 ? 1 : -1];
typedef char phx_check_rgb_state[sizeof(phx_rgb_state) == 12 ? 1 : -1];
typedef char phx_check_rgb[sizeof(phx_rgb) == 56 ? 1 : -1];
typedef char phx_check_platform[sizeof(phx_platform) == 64 ? 1 : -1];
typedef char phx_check_info[sizeof(phx_info) == 48 ? 1 : -1];

#pragma pack(pop)

#ifdef __cplusplus
}
#endif
#endif
