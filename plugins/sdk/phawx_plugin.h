/* Phawx ON plugin API, version 1.
 *
 * A plugin is a 64-bit Windows DLL that exports phx_plugin_init (declared at the
 * bottom). Put it in the plugins\ folder next to PhawxON.exe, or in plugins\<name>\
 * together with the DLLs it needs, turn it on in the overlay's Plugins page and
 * restart Phawx ON. The guide is PLUGINS.md: docs\PLUGINS.md in the repository,
 * sdk\PLUGINS.md in the release zip.
 *
 * This header is plain C and builds with MinGW-w64, clang and MSVC, from C or C++.
 * The layout of every struct is fixed: members are fixed-width, there is no
 * implicit padding, and the size checks at the bottom fail the build if a compiler
 * lays a struct out differently.
 *
 * Conventions
 * - char strings are UTF-8, wchar_t strings UTF-16; both NUL-terminated.
 * - Return values: 0 (PHX_OK) is success, anything else a failure, unless a
 *   comment says otherwise.
 * - Never let a C++ exception or longjmp cross into Phawx ON, and never free
 *   memory the other side allocated.
 *
 * What the host keeps and what it copies
 * - Kept: the host holds pointers to every phx_control, phx_clock, phx_fan and
 *   phx_rgb you add, and to their label, unit, choices (the array and its
 *   strings), name, effects and desc, until the process exits. Use static
 *   storage and do not change them except as described for host->update.
 *   phx_control must not be const: the host writes value and active into it.
 * - Copied: key and id (turned into "<plugin id>.<key>"); your phx_info and its
 *   name and version strings (copied right after phx_plugin_init returns, so
 *   *info must still be valid then: make it static); the strings you pass to
 *   set_status, toast, log and the cfg_* functions (only needed during the call).
 * - The host's: host, host->platform and its strings, self, and the string
 *   plugin_dir() returns belong to Phawx ON and stay valid for the whole process.
 *
 * Threads
 * "UI thread" is Phawx ON's main thread; it also runs start-up and normal exit.
 *   phx_plugin_init            UI thread, once, at start
 *   control get, fmt           UI thread
 *   control set                UI thread: when the user changes a row, and when
 *                              saved values are applied again (see PHX_F_REAPPLY)
 *   clock set_max, reset       AutoTDP's worker while AutoTDP runs; the UI thread
 *                              for the CPU/GPU max clock rows, a GPU domain switch
 *                              and when AutoTDP stops; the exit thread
 *   clock cur                  UI thread, every 0.5 s while the overlay is open
 *                              and AutoTDP is off
 *   clock util                 AutoTDP's worker, every 250 ms while a game runs
 *   fan and RGB callbacks      UI thread, and the exit thread
 *   info.tick, info.resume     UI thread
 *   info.shutdown              the exit thread
 * The exit thread is the UI thread on a normal exit, logoff or restart, and the
 * crash handler's own thread after a crash elsewhere in the process.
 * - One lock per plugin: the host never runs two of your callbacks at the same
 *   time, so state they share needs no lock of your own. Only while restoring
 *   after a crash does it wait at most about 300 ms for a callback still running
 *   on another thread; then it makes its reset, fan, light and shutdown calls
 *   anyway.
 * - Callbacks never nest on one thread. If a callback pumps messages (a message
 *   box, STA COM), the host does not call you again on that thread until it
 *   returns: nested get, fmt, tick, resume, clock, fan and RGB calls are skipped
 *   (fmt shows no value) and a nested set() fails ("Could not apply ...").
 * - AutoTDP's worker has a 64 KiB stack: keep big buffers off the stack in
 *   set_max, reset and util.
 * - Keep every callback short (well under 50 ms): the overlay or AutoTDP waits.
 * - log, toast, set_status, update, ctl_get, plugin_dir, hw_ready, ec_* and msr_*
 *   may be called from any thread, your own included. Call cfg_* from inside a
 *   callback or phx_plugin_init, and add_* only inside phx_plugin_init.
 * - Crashes: see PLUGINS.md. A plugin blamed for a crash is never called again,
 *   not even its shutdown, and is turned off at the next start.
 */
#ifndef PHAWX_PLUGIN_H
#define PHAWX_PLUGIN_H

#include <stddef.h>
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

/* x64 has one calling convention; the macro marks the boundary */
#define PHX_CALL   __cdecl
#define PHX_EXPORT __declspec(dllexport)
#if defined(_MSC_VER) && !defined(__cplusplus)
#define PHX_INLINE static __inline
#else
#define PHX_INLINE static inline
#endif

/* ---------- struct sizes ----------
 * Set every size member to sizeof(the struct) as this header declares it. The
 * host accepts a struct whose size is at least the V1 size and never reads or
 * writes past the size you give, so plugins built with a newer header keep
 * working, and newer hosts keep accepting v1 plugins. phx_info may be smaller:
 * the host reads only the whole members that fit. The host's own structs grow
 * the same way: check PHX_HOST_HAS before using a member added after version 1.
 */
#define PHX_CONTROL_SIZE_V1   120u
#define PHX_CLOCK_SIZE_V1      80u
#define PHX_FAN_SIZE_V1        56u
#define PHX_RGB_SIZE_V1        56u
#define PHX_INFO_SIZE_V1       48u
#define PHX_PLATFORM_SIZE_V1   64u
#define PHX_HOST_SIZE_V1      168u

/* return values of phx_plugin_init (and PHX_OK / PHX_ERROR of host functions) */
#define PHX_OK           0    /* running */
#define PHX_UNSUPPORTED  1    /* not for this machine: "Not running · not for this device" in grey, or your set_status text */
#define PHX_ERROR      (-1)   /* failed; call host->set_status first to say why. Any value other than 0 and 1 counts as failed */
#define PHX_E_VERSION  (-2)   /* this Phawx ON is too old: "Not running · needs a newer Phawx ON" */

/* pages of the overlay. There is no Power tab any more: PHX_PAGE_POWER rows are shown
   on the System page, like PHX_PAGE_SYSTEM ones. */
enum {
    PHX_PAGE_QUICK = 0, PHX_PAGE_CPU, PHX_PAGE_POWER, PHX_PAGE_GPU,
    PHX_PAGE_DISPLAY, PHX_PAGE_SYSTEM, PHX_PAGE_PLUGINS
};

/* control types */
enum {
    PHX_SLIDER = 0,   /* min..max in steps of step */
    PHX_TOGGLE,       /* 0 / 1 */
    PHX_CHOICE,       /* index into choices (at most 64) */
    PHX_ACTION,       /* a button; set() runs it */
    PHX_INFO,         /* read-only value from get()/fmt(), refreshed about once a second */
    PHX_HEADER        /* section title */
};

/* control flags; other bits are ignored */
#define PHX_F_NOSAVE    0x0001u  /* never written to or read from phawx.ini, so never applied at start */
#define PHX_F_REAPPLY   0x0002u  /* reserved, no effect: every row away from Default is applied again at
                                    start, after sleep, on AC/DC changes and on per-game profile switches,
                                    so set() must be idempotent */
#define PHX_F_OPTIONAL  0x0004u  /* has a "Default" position that leaves the hardware alone (see def) */
#define PHX_F_ADVANCED  0x0008u  /* only shown with Settings > Show advanced options */
#define PHX_F_DANGER    0x0010u  /* red label; asks for confirmation (desc) before set(), except when a
                                    toggle is turned off; not applied while dragging even with PHX_F_LIVE */
#define PHX_F_LIVE      0x0020u  /* set() while a slider is dragged (at most every 60 ms), not only on release */
#define PHX_F_HIDDEN    0x0040u  /* not shown, not applied, and ctl_get reports it as absent */
#define PHX_F_AUTOTDP   0x0080u  /* locked and not applied while AutoTDP runs (whichever domains it drives);
                                    applied again when AutoTDP stops */
#define PHX_F_PROFILE   0x0100u  /* saved per game when per-game profiles are on */
#define PHX_F_SIGNED    0x0200u  /* show positive values with a + sign; ignored when fmt is set */

typedef struct phx_plugin phx_plugin;   /* opaque; identifies your plugin to the host */
typedef struct phx_control phx_control;
typedef struct phx_clock phx_clock;
typedef struct phx_fan phx_fan;
typedef struct phx_rgb phx_rgb;

/* ---------- controls ----------
 * One row in the overlay. The host draws it, handles input, saves the value (in
 * [global] or a [game:<exe>] section under "<plugin id>.<key>"), applies it
 * again at start and lets the user pin it to the Quick page and hold it to go
 * back to Default. set() is only called with values inside min..max.
 * Limits: 48 controls per plugin; order is clamped to 0..999, and your rows come
 * after Phawx ON's own rows on every page.
 */
struct phx_control {
    uint32_t size;                    /* sizeof(phx_control), at least PHX_CONTROL_SIZE_V1 */
    uint32_t type;                    /* PHX_SLIDER ... */
    const char *key;                  /* "stapm": 1..40 letters, digits, '_', '-', '.'; unique in your plugin;
                                         required for sliders, toggles, choices and actions; copied */
    const wchar_t *label;             /* required */
    const wchar_t *unit;              /* optional, e.g. L"W"; ignored when fmt is set */
    const wchar_t *const *choices;    /* PHX_CHOICE: labels, NULL-terminated */
    uint32_t page;                    /* PHX_PAGE_* */
    uint32_t flags;                   /* PHX_F_* */
    int32_t  order;                   /* 0..999, position among your rows on that page */
    int32_t  min, max, step, def;     /* def: the value "Default" stands for without PHX_F_OPTIONAL */
    int32_t  reserved0;
    int  (PHX_CALL *get)(phx_control *c, int32_t *out);  /* optional: read the hardware; 0 = *out is valid */
    int  (PHX_CALL *set)(phx_control *c, int32_t value); /* 0 = applied. When a change the user made fails,
                                                            they see "Could not apply <label>" (not for actions,
                                                            and not if you showed a toast yourself) */
    void (PHX_CALL *fmt)(const phx_control *c, int32_t value, wchar_t *buf, int n); /* optional value text; n is
                                                            the size of buf in wchar_t including the terminator */
    void *user;                       /* yours */
    /* written by the host before every call into your plugin; read-only for you */
    int32_t  value;                   /* the row's value as the host has it (def while active is 0) */
    int32_t  active;                  /* 1 = the user chose a value, 0 = Default. Both change only after
                                         set() returns 0, so inside set() they still show the old state */
    const wchar_t *desc;              /* optional: the confirmation text of a PHX_F_DANGER row, in place of
                                         "Set <label> to <value>? This can cause instability."; kept */
};

/* ---------- clock domains ----------
 * A CPU or GPU clock AutoTDP can drive. For the CPU, AutoTDP uses the domain with
 * the highest prio; the user picks the GPU domain under Quick > GPU clock control
 * (listed only when there are two or more), and the highest prio is the default.
 * Limits: 4 clock domains per plugin, 8 CPU and 8 GPU domains in all.
 */
#define PHX_CLOCK_CPU 0u
#define PHX_CLOCK_GPU 1u

struct phx_clock {
    uint32_t size;                    /* sizeof(phx_clock), at least PHX_CLOCK_SIZE_V1 */
    uint32_t kind;                    /* PHX_CLOCK_CPU / PHX_CLOCK_GPU */
    const wchar_t *name;              /* shown in GPU clock control and the log; NULL = your plugin's name */
    int32_t  min_mhz, max_mhz, step_mhz; /* 0 <= min < max <= 20000; step 0 = 50 */
    int32_t  prio;                    /* clamped to 0..100; built-in domains use 10..30 */
    int (PHX_CALL *set_max)(phx_clock *d, int mhz);   /* cap the clock; required */
    int (PHX_CALL *set_min)(phx_clock *d, int mhz);   /* reserved: never called; leave NULL */
    int (PHX_CALL *reset)(phx_clock *d);              /* optional: remove the cap. Without it the host
                                                         calls set_max(max_mhz) instead */
    int (PHX_CALL *cur)(phx_clock *d, int *mhz);      /* optional: current clock, for the overlay's live line */
    int (PHX_CALL *util)(phx_clock *d, int *pct);     /* optional: busy %, improves bottleneck detection */
    void *user;
};

/* ---------- fans ----------
 * You provide the hardware access, the host provides the rows in the System page's
 * Fan control section (Fan mode: Default / Auto / Manual / Full speed / Curve, a
 * Manual fan speed slider, the fan curve graph, the RPM) under the keys
 * "<plugin>.<id>.mode" and ".speed", and ".curve" for the curve. In Curve mode the
 * host calls set_duty about once a second on the UI thread, from the CPU
 * temperature, and set_auto when it has no temperature. It calls set_auto when the
 * user picks Auto or Default, and at exit and after a crash elsewhere if it had set
 * a duty; it does not call a plugin that crashed itself. Limit: 4 fans per plugin.
 */
struct phx_fan {
    uint32_t size;                    /* sizeof(phx_fan), at least PHX_FAN_SIZE_V1 */
    int32_t  min_pct;                 /* lowest manual duty the fan accepts, e.g. 20 */
    const char *id;                   /* "fan", same rules as control keys; copied */
    const wchar_t *name;              /* section title, e.g. L"CPU fan" */
    int (PHX_CALL *set_auto)(phx_fan *f);            /* firmware control; required */
    int (PHX_CALL *set_duty)(phx_fan *f, int pct);   /* manual duty, min_pct..100 (Full speed = 100); required */
    int (PHX_CALL *get_rpm)(phx_fan *f, int *rpm);   /* optional */
    void *user;
};

/* ---------- RGB lighting ----------
 * The host provides Lighting (Default / Off / Solid / your effects), Color and
 * Brightness rows on the System page and calls apply() with the combined state.
 * restore() hands the LEDs back to the firmware when the user picks Default, at
 * exit and after a crash elsewhere. Limit: 4 lights per plugin.
 */
#define PHX_RGB_OFF   0
#define PHX_RGB_SOLID 1               /* effects[i] is mode 2 + i */

#define PHX_RGB_COLOR      0x1u       /* caps: apply() uses r, g, b (the Color row is shown) */
#define PHX_RGB_BRIGHTNESS 0x2u       /* caps: apply() uses brightness (the Brightness row is shown) */

typedef struct phx_rgb_state {
    int32_t mode;                     /* PHX_RGB_OFF (turn them off), PHX_RGB_SOLID or 2 + effect index */
    uint8_t r, g, b, reserved;
    int32_t brightness;               /* 0..100 */
} phx_rgb_state;

struct phx_rgb {
    uint32_t size;                    /* sizeof(phx_rgb), at least PHX_RGB_SIZE_V1 */
    uint32_t caps;                    /* PHX_RGB_COLOR | PHX_RGB_BRIGHTNESS */
    const char *id;                   /* "leds" -> keys "<plugin>.leds.mode", ".color", ".bright"; copied */
    const wchar_t *name;              /* section title, e.g. L"Stick lights" */
    const wchar_t *const *effects;    /* optional extra modes, NULL-terminated, at most 8 */
    int (PHX_CALL *apply)(phx_rgb *l, const phx_rgb_state *s);   /* required */
    int (PHX_CALL *restore)(phx_rgb *l);                         /* required */
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
    const wchar_t *name;              /* shown on the Plugins page while you run (else FileDescription) */
    const wchar_t *version;           /* e.g. L"1.0"; not shown yet */
    void (PHX_CALL *shutdown)(void);  /* undo every hardware change: at exit, logoff, restart and after a
                                         crash elsewhere, on the exit thread. The host has already reset
                                         your capped clocks and handed fans and lights back. Also called
                                         right after init if the host has no room for your rows */
    void (PHX_CALL *resume)(void);    /* after sleep; the host then applies every row away from Default again */
    void (PHX_CALL *tick)(void);      /* about once a second while the overlay is open, and whenever saved
                                         values are applied again (also with the overlay closed) */
} phx_info;

/* ---------- the host API ----------
 * add_* only work inside phx_plugin_init, and what you add only takes effect if
 * it returns PHX_OK. They return PHX_OK or PHX_ERROR, and write the reason to the
 * log ("plugin <id>: <what> rejected (<why>)").
 */
typedef struct phx_host {
    uint32_t size;                    /* sizeof(phx_host) of the host; newer hosts append members */
    uint32_t version;                 /* PHX_API_VERSION of the host */
    const phx_platform *platform;

    int  (PHX_CALL *add_control)(phx_plugin *p, phx_control *c);
    int  (PHX_CALL *add_clock)(phx_plugin *p, phx_clock *d);
    int  (PHX_CALL *add_fan)(phx_plugin *p, phx_fan *f);
    int  (PHX_CALL *add_rgb)(phx_plugin *p, phx_rgb *l);
    /* after changing label, unit, desc, flags, choices (choice rows), min/max/step
       (sliders) or def (sliders and choices) of a control you added. From another
       thread the change is applied on the UI thread a moment later, so leave the new
       values in the struct. Type, page, order, key and the callbacks cannot change. */
    void (PHX_CALL *update)(phx_plugin *p, phx_control *c);

    /* Your own section [plugin:<id>] in phawx.ini, for settings that are not controls.
       Keys follow the control key rules and, like all ini keys, ignore case. Values
       are UTF-8, at most 511 characters and without line breaks; quotes and spaces
       around a value are stripped when it is read back. */
    int  (PHX_CALL *cfg_get_int)(phx_plugin *p, const char *key, int def);  /* def if absent or not a number */
    int  (PHX_CALL *cfg_set_int)(phx_plugin *p, const char *key, int value); /* PHX_OK, or PHX_ERROR if refused */
    int  (PHX_CALL *cfg_get_str)(phx_plugin *p, const char *key, char *out, int n); /* 1 = found, cut after the
                                                            last whole UTF-8 character that fits n bytes; 0 = "" */
    int  (PHX_CALL *cfg_set_str)(phx_plugin *p, const char *key, const char *value); /* NULL deletes; PHX_OK, or
                                                            PHX_ERROR if refused (then nothing is written) */

    /* one line to the log: "plugin <id>: <msg>", cut at 400 bytes. It always goes to
       OutputDebugString, and to phawx.log next to phawx.ini when the environment
       variable PHAWX_LOG is set. See phx_logf below. */
    void (PHX_CALL *log)(phx_plugin *p, const char *msg);
    void (PHX_CALL *toast)(phx_plugin *p, const wchar_t *msg);        /* a short message on screen */
    void (PHX_CALL *set_status)(phx_plugin *p, const wchar_t *msg);   /* at most 127 characters, shown under your
                                                                         name on the Plugins page; NULL clears */
    const wchar_t *(PHX_CALL *plugin_dir)(phx_plugin *p);             /* the folder your DLL was loaded from,
                                                                         without a trailing backslash */

    /* any control's state by its full key, e.g. "cpu.tdp": 0 with *value (def at Default) and
       *active, or -1 if it is absent or hidden. Inside phx_plugin_init saved values are not
       loaded yet (you get def and 0), and AutoTDP's, Settings' and later plugins' rows do not
       exist yet. */
    int  (PHX_CALL *ctl_get)(phx_plugin *p, const char *key, int32_t *value, int *active);

    /* hardware access through PawnIO's signed modules */
    int  (PHX_CALL *hw_ready)(phx_plugin *p);                                /* 1 = PawnIO is running, 0 = not */
    int  (PHX_CALL *ec_read)(phx_plugin *p, uint8_t reg, uint8_t *value);    /* ACPI EC, ports 0x62/0x66; -1 = failed */
    int  (PHX_CALL *ec_write)(phx_plugin *p, uint8_t reg, uint8_t value);
    int  (PHX_CALL *msr_read)(phx_plugin *p, uint32_t msr, int cpu, uint64_t *value); /* cpu -1 = any; -1 = failed */
    int  (PHX_CALL *msr_write)(phx_plugin *p, uint32_t msr, int cpu, uint64_t value);
} phx_host;

/* does this host have a member added after version 1? e.g. PHX_HOST_HAS(host, new_fn) */
#define PHX_HOST_HAS(h, member) \
    ((size_t)(h)->size >= offsetof(phx_host, member) + sizeof((h)->member))

/* ---------- the one export ----------
 * Point *info at your static phx_info and return PHX_OK, PHX_UNSUPPORTED or an
 * error. On any other return than PHX_OK, undo whatever you did first (stop your
 * threads): the host drops your registrations and unloads the DLL.
 * The declaration gives it C linkage in C++ (an undecorated export) and makes a
 * definition with another signature an error: at compile time in C, at link time
 * in C++ (where other parameters would make a separate, mangled overload). Code
 * that includes this header but is not a plugin, like Phawx ON itself, defines
 * PHX_HOST first.
 */
typedef int (PHX_CALL *phx_plugin_init_fn)(const phx_host *host, phx_plugin *self, const phx_info **info);
#define PHX_INIT_EXPORT "phx_plugin_init"

#ifndef PHX_HOST
PHX_EXPORT int PHX_CALL phx_plugin_init(const phx_host *host, phx_plugin *self, const phx_info **info);
#if defined(__cplusplus) && defined(_MSC_VER) && !defined(__GNUC__)
#pragma comment(linker, "/include:phx_plugin_init")
#elif defined(__cplusplus) && defined(__GNUC__)
static const phx_plugin_init_fn phx_init_check __attribute__((used)) = &phx_plugin_init;
#endif
#endif

/* printf into the host log, formatted by your own C runtime */
#if defined(__GNUC__) && defined(__MINGW_PRINTF_FORMAT)
#define PHX_PRINTF(f, a) __attribute__((format(__MINGW_PRINTF_FORMAT, f, a)))
#else
#define PHX_PRINTF(f, a)
#endif

PHX_INLINE void phx_logf(const phx_host *host, phx_plugin *self, const char *fmt, ...) PHX_PRINTF(3, 4);
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
typedef char phx_check_control[sizeof(phx_control) == PHX_CONTROL_SIZE_V1 ? 1 : -1];
typedef char phx_check_clock[sizeof(phx_clock) == PHX_CLOCK_SIZE_V1 ? 1 : -1];
typedef char phx_check_fan[sizeof(phx_fan) == PHX_FAN_SIZE_V1 ? 1 : -1];
typedef char phx_check_rgb_state[sizeof(phx_rgb_state) == 12 ? 1 : -1];
typedef char phx_check_rgb[sizeof(phx_rgb) == PHX_RGB_SIZE_V1 ? 1 : -1];
typedef char phx_check_platform[sizeof(phx_platform) == PHX_PLATFORM_SIZE_V1 ? 1 : -1];
typedef char phx_check_info[sizeof(phx_info) == PHX_INFO_SIZE_V1 ? 1 : -1];
typedef char phx_check_host[sizeof(phx_host) == PHX_HOST_SIZE_V1 ? 1 : -1];

#pragma pack(pop)

#ifdef __cplusplus
}
#endif
#endif
