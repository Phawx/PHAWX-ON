#ifndef PHAWX_H
#define PHAWX_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <stdint.h>

#include "version.h"
#define PH_APPNAME_W L"Phawx ON"

/* ---------- pages shown in the overlay ---------- */
/* Windows power settings, fans, lights and hardware info are sections of System */
enum {
    PG_QUICK,
    PG_CPU,
    PG_GPU,
    PG_DISPLAY,
    PG_SYSTEM,
    PG_PLUGINS,
    PG_SETTINGS,
    PG_COUNT
};

/* ---------- controls: every tunable is one of these ---------- */
/* CT_STATUS: full-width line; get() returns its tone (TONE_*), fmt() its text.
   CT_CURVE: a fan curve graph (fans.c); ctx is its ph_fan, it has no config key. */
enum { CT_SLIDER, CT_TOGGLE, CT_CHOICE, CT_ACTION, CT_INFO, CT_HEADER, CT_STATUS, CT_CURVE };
enum { TONE_DIM, TONE_GOOD, TONE_BAD };

enum {
    CF_NOSAVE    = 1 << 0,  /* never persisted */
    CF_REAPPLY   = 1 << 1,  /* only for ph_apply_all(1), which nothing calls: ph_apply_all(0) re-applies every active row */
    CF_OPTIONAL  = 1 << 2,  /* "Default" position = leave hardware untouched */
    CF_ADVANCED  = 1 << 3,  /* hidden unless advanced mode */
    CF_DANGER    = 1 << 4,  /* confirm before applying */
    CF_LIVE      = 1 << 5,  /* apply while sliding, not only on release */
    CF_HIDDEN    = 1 << 6,  /* not rendered (probe failed / n/a) */
    CF_AUTOTDP   = 1 << 7,  /* locked while AutoTDP owns it */
    CF_PROFILE   = 1 << 8,  /* saved per game profile */
    CF_SIGNED    = 1 << 9,  /* display with explicit sign */
    CF_FLICKER   = 1 << 10, /* screen may flicker when changed */
    CF_NOPIN     = 1 << 11, /* cannot be pinned to the Quick page */
    CF_CONFIRM   = 1 << 12, /* asks first (message in desc) without being shown as dangerous */
    CF_SECTION   = 1 << 13, /* CT_HEADER: a page section; the plain headers under it are its parts */
    CF_TABLE     = 1 << 14  /* CT_INFO: a compact line of the Hardware info table */
};

typedef struct ph_ctl ph_ctl;
typedef int  (*ph_get_fn)(ph_ctl *c, int32_t *out);
typedef int  (*ph_set_fn)(ph_ctl *c, int32_t v);
typedef void (*ph_fmt_fn)(const ph_ctl *c, int32_t v, wchar_t *buf, int n);
typedef int  (*ph_sub_fn)(const ph_ctl *c, wchar_t *buf, int n);   /* second line; returns TONE_* */

struct ph_ctl {
    const char        *key;       /* config key "cpu.pl1"; NULL for headers/info */
    const wchar_t     *label;
    uint8_t            type;
    uint8_t            page;
    uint16_t           flags;
    int32_t            min, max, step, def;
    const wchar_t     *unit;
    const wchar_t     *desc;      /* optional: the confirmation text of a CF_DANGER / CF_CONFIRM row (not shown otherwise) */
    const wchar_t *const *choices; /* CT_CHOICE labels, NULL terminated */
    ph_get_fn          get;       /* optional: read current hardware value */
    ph_set_fn          set;       /* apply value; returns 0 on success */
    ph_fmt_fn          fmt;       /* optional custom value formatter */
    void              *ctx;
    int32_t            arg;       /* backend private (plane id, index...) */
    /* runtime */
    int32_t            val;
    uint8_t            active;    /* user chose a value (not "Default") */
    uint8_t            dirty;
    int16_t            order;     /* sort key within page, lower first */
    ph_sub_fn          sub;       /* optional status line under the label */
    void             (*release)(ph_ctl *c);  /* optional, CF_OPTIONAL: it went back to Default */
};

/* ---------- clock domains used by AutoTDP ---------- */
typedef struct ph_clk {
    const wchar_t *name;
    int  min_mhz, max_mhz, step_mhz;
    int  (*set_max)(struct ph_clk *d, int mhz);   /* cap; returns 0 on success */
    int  (*set_min)(struct ph_clk *d, int mhz);   /* optional */
    int  (*reset)(struct ph_clk *d);              /* remove cap */
    int  (*cur)(struct ph_clk *d, int *mhz);      /* optional telemetry */
    int  (*util)(struct ph_clk *d, int *pct);     /* optional busy % */
    void *ctx;
    int  prio;                                    /* higher wins among same kind */
    int  plugin;                                  /* a plugin's: after a crash the plugin host resets it */
} ph_clk;

/* ---------- backends (plugins) ---------- */
enum { BK_DRIVER, BK_CPU, BK_POWER, BK_GPU, BK_SYS, BK_DEVICE, BK_PLUGIN };

typedef struct ph_backend {
    const char    *id;
    const wchar_t *name;
    uint8_t        kind;
    int  (*probe)(void);          /* nonzero if this backend applies to this machine */
    int  (*init)(void);           /* register controls/clock domains; 0 = ok */
    void (*shutdown)(void);       /* restore anything that must not persist */
    void (*resume)(void);         /* optional: after sleep */
    void (*tick)(void);           /* optional: called ~1 Hz while menu visible, and at the end of ph_apply_all */
    volatile LONG alive;
} ph_backend;

/* registry.c */
void     ph_register_ctl(ph_ctl *c);
void     ph_register_ctls(ph_ctl *c, int n);
void     ph_register_cpu_clk(ph_clk *d);
void     ph_register_gpu_clk(ph_clk *d);
int      ph_ctl_count(void);
int      ph_ctl_room(void);           /* rows that can still be registered */
ph_ctl  *ph_ctl_at(int i);
ph_ctl  *ph_ctl_find(const char *key);
int      ph_ctl_apply(ph_ctl *c, int32_t v);
void     ph_ctl_reset(ph_ctl *c);
void     ph_apply_all(int reapply_only);
ph_clk  *ph_cpu_clk(void);
ph_clk  *ph_gpu_clk(void);
ph_clk  *ph_gpu_clk_at(int i);
int      ph_gpu_clk_count(void);
void     ph_select_gpu_clk(int i);
void     ph_backends_init(void);
void     ph_backends_shutdown(void);  /* also from the crash handler: only backends still alive */
void     ph_backends_resume(void);
void     ph_backends_tick(void);
extern ph_backend *const ph_backends[];

/* ---------- low level hardware access (hw/drv.c) ---------- */
typedef struct ph_drv {
    const char *id;
    int  (*open)(void);
    void (*close)(void);
    int  (*rdmsr)(uint32_t msr, int cpu, uint64_t *v);   /* cpu -1 = current */
    int  (*wrmsr)(uint32_t msr, int cpu, uint64_t v);
    int  (*pci_rd)(uint32_t bdf, uint32_t off, uint32_t *v);
    int  (*pci_wr)(uint32_t bdf, uint32_t off, uint32_t v);
    int  (*mem_rd)(uint64_t pa, void *buf, uint32_t unit, uint32_t count);
    int  (*mem_wr)(uint64_t pa, const void *buf, uint32_t unit, uint32_t count);
    int  (*io_rd8)(uint16_t port, uint8_t *v);
    int  (*io_wr8)(uint16_t port, uint8_t v);
} ph_drv;

#define PH_BDF(b, d, f) (((uint32_t)(b) << 8) | ((uint32_t)(d) << 3) | (uint32_t)(f))

enum { DRV_MISSING, DRV_STOPPED, DRV_NOMODULES, DRV_OK };
int  drv_open(void);
int  drv_ok(void);
int  drv_state(void);                 /* DRV_OK, or why drv_ok() is false */
const char *drv_name(void);
const char *drv_version(void);        /* "2.0", "" if unknown */
const char *drv_missing(void);        /* CPU modules that failed to load, "" if none */
int  drv_rdmsr(uint32_t msr, uint64_t *v);
int  drv_wrmsr(uint32_t msr, uint64_t v);
int  drv_rdmsr_cpu(uint32_t msr, int cpu, uint64_t *v);
int  drv_wrmsr_cpu(uint32_t msr, int cpu, uint64_t v);
int  drv_wrmsr_all(uint32_t msr, uint64_t v);
int  drv_rmw_all(uint32_t msr, uint64_t mask, uint64_t val);
int  drv_pci_rd(uint32_t bdf, uint32_t off, uint32_t *v);
int  drv_pci_wr(uint32_t bdf, uint32_t off, uint32_t v);
int  drv_mem_rd32(uint64_t pa, uint32_t *v);
int  drv_mem_wr32(uint64_t pa, uint32_t v);
int  drv_mem_rd16(uint64_t pa, uint16_t *v);
int  drv_mem_wr16(uint64_t pa, uint16_t v);
int  drv_mem_rd8(uint64_t pa, uint8_t *v);
int  drv_mem_wr8(uint64_t pa, uint8_t v);
int  drv_io_rd8(uint16_t port, uint8_t *v);
int  drv_io_wr8(uint16_t port, uint8_t v);
void drv_close(void);
extern const ph_drv drv_pawnio;

/* ---------- platform info (sys/platform.c) ---------- */
enum { VENDOR_UNKNOWN, VENDOR_INTEL, VENDOR_AMD };
typedef struct ph_platform {
    int      vendor;
    uint32_t family, model, stepping;
    wchar_t  cpu_name[64];
    wchar_t  maker[64];
    wchar_t  product[64];
    wchar_t  board[64];
    int      nlogical, ncores;
    int      hybrid;                 /* has more than one efficiency class */
    int      n_class0, n_class1;     /* logical cpus in low (E) / high (P) efficiency class */
    uint8_t  cls[256];               /* efficiency class per logical processor */
    uint8_t  core_of[256];           /* physical core index per logical processor */
    int      max_class;
    int      hwp, epp, cppc;         /* capability flags */
    int      on_ac;
} ph_platform;
extern ph_platform g_plat;
void platform_detect(void);
int  platform_on_ac(void);
void cpuidex(uint32_t leaf, uint32_t sub, uint32_t r[4]);

/* ---------- Windows power settings (sys/winpower.c) ---------- */
int  wp_read(const GUID *sub, const GUID *set, int dc, DWORD *v);
int  wp_write(const GUID *sub, const GUID *set, DWORD ac, DWORD dc);
int  wp_write_both(const GUID *sub, const GUID *set, DWORD v);
void wp_commit(void);                 /* PowerSetActiveScheme(current) once after batch */
int  wp_set_overlay(const GUID *ov);  /* power mode slider */
extern const GUID GUID_SUB_PROCESSOR_, GUID_SUB_NONE_;
int  wp_set_epp(int cls, int pct);             /* cls 0 = base (E or all), 1 = "...1" (P), -1 = both */
int  wp_get_epp(int cls);                      /* -1 unknown */
int  wp_set_cores(int cls, int min_pct, int max_pct); /* core parking CPMIN/CPMAXCORES(1) */
int  wp_set_freq_cap(int cls, int mhz);        /* PROCFREQMAX(1), 0 = none */

/* ---------- display (sys/display.c) ---------- */
int  display_refresh(void);                    /* current refresh of the foreground monitor */

/* ---------- FPS (fps.c) ---------- */
typedef struct ph_fps {
    DWORD  pid;
    float  fps;          /* over ~0.5 s */
    float  fps_fast;     /* over ~150 ms */
    float  low1;         /* 1% low over ~3 s */
    float  frametime_ms; /* latest */
    float  ft_p99_ms;
    float  gpu_busy_ms;  /* PresentMon only, else 0 */
    float  cpu_busy_ms;  /* PresentMon only, else 0 */
    uint32_t frames;     /* monotonically increasing */
    int    source;       /* 0 none, 1 PresentMon, 2 ETW */
    wchar_t exe[64];
} ph_fps;
int  fps_start(void);                  /* 0 ok, -1 PresentMon unavailable */
int  fps_status(void);                 /* 0 missing, 1 service unreachable, 2 ok, -1 first check running */
const wchar_t *fps_version(void);      /* version of the loaded PresentMon, L"" if unknown */
void fps_stop(void);
int  fps_sample(ph_fps *out);          /* foreground presenting process */
HANDLE fps_event(void);                /* signalled on new frames (throttled) */

/* ---------- AutoTDP (autotdp.c) ---------- */
enum { EPP_LEAVE, EPP_ZERO, EPP_TUNE };   /* what AutoTDP does with EPP */
typedef struct ph_auto_cfg {
    int target_fps;       /* 0 = match refresh rate */
    int tolerance_pct;    /* dead band below target, default 3 */
    int raise_aggr;       /* 1..5 */
    int lower_aggr;       /* 1..5 */
    int settle_ms;        /* stable time before probing down */
    int cpu_floor_mhz, cpu_ceil_mhz;
    int gpu_floor_mhz, gpu_ceil_mhz;
    int manage_cores;     /* look for the fewest unparked cores that hold the target */
    int epp_mode;         /* EPP_* */
    int half_refresh;
} ph_auto_cfg;
typedef struct ph_auto_state {
    int   running;
    int   cpu_mhz, gpu_mhz;
    int   target;
    float fps;
    int   cpu_util, gpu_util;
    int   bottleneck;     /* 0 none, 1 cpu, 2 gpu */
    int   power_mw;       /* what the machine draws, -1 unknown */
    int   power_src;      /* PSRC_* */
    int   cores, cores_max;   /* cores AutoTDP lets Windows use, of cores_max (0 = not managed) */
    int   cores_on;       /* cores Windows has unparked now, -1 unknown */
    int   epp;            /* EPP AutoTDP set, -1 = Windows' own */
    int   trial;          /* what it is trying now, 0 = nothing (autotdp.c) */
} ph_auto_state;
extern ph_auto_cfg g_auto;
int  autotdp_start(void);
void autotdp_stop(void);
void autotdp_crash_release(DWORD crashed_tid);   /* crash path: uncap clocks without joining the worker */
int  autotdp_running(void);
void autotdp_state(ph_auto_state *s);
void autotdp_register_ctls(void);
void autotdp_cfg_loaded(void);                   /* after cfg_load: find the saved GPU domain by name */

/* ---------- config (config.c) ---------- */
void cfg_load(void);
void cfg_save(void);
int  cfg_get_int(const char *sec, const char *key, int def);
void cfg_set_int(const char *sec, const char *key, int v);
int  cfg_get_str(const char *sec, const char *key, char *out, int n); /* 1 = found; cut at a UTF-8 character to fit */
int  cfg_set_str(const char *sec, const char *key, const char *v);   /* NULL deletes the key; -1 if too long (511) */
void cfg_clear(const char *sec);                                    /* delete a whole section */
const wchar_t *cfg_file(void);
void cfg_load_profile(const wchar_t *exe);  /* NULL = global */
void cfg_save_profile(const wchar_t *exe);
int  cfg_has_profile(const wchar_t *exe);
int  cfg_any_profile(void);                 /* any [game:*] section saved */
const wchar_t *cfg_dir(void);
void cfg_mark_dirty(void);

/* ---------- input (input.c) ---------- */
/* IN_RESET resets at once (Delete key); the pad's X and keyboard R reset only when
   held, reported through ui_hold. IN_PIN is the pad's Y. */
enum {
    IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT, IN_OK, IN_BACK, IN_TAB_PREV, IN_TAB_NEXT,
    IN_TOGGLE_MENU, IN_AUX, IN_RESET, IN_PIN, IN_NAV_COUNT
};
enum { HOLD_PAD = 1, HOLD_KEY = 2, HOLD_PTR = 4 };  /* sources of a hold-to-reset */
#define HOLD_MS       500
#define HOLD_TOUCH_MS 2000
void input_init(HWND owner);
void input_menu_open(int open);         /* raise poll rate while visible */
void input_shutdown(void);
void input_capture_combo(void);         /* next held combo becomes the menu combo */
int  input_capturing(void);
void input_on_rawinput(HWND h, LPARAM lp);
void input_on_timer(void);
void input_on_devchange(void);
uint32_t input_combo(void);
void input_set_combo(uint32_t mask);
void combo_to_text(uint32_t mask, wchar_t *buf, int n);

/* ---------- UI (ui.c) ---------- */
void ui_init(HINSTANCE hi);
void ui_toggle(void);
void ui_show(int show);
void ui_show_page(int pg);
int  ui_visible(void);
void ui_nav(int action);
void ui_hold(int src, int down);        /* HOLD_* source pressed / released */
void ui_refresh(void);
HWND ui_hwnd(void);
void ui_toast(const wchar_t *msg);

/* ---------- pins on the Quick page (pins.c) ---------- */
void pins_load(void);
int  pin_count(void);
const char *pin_at(int i);
int  pin_has(const char *key);
int  pin_toggle(const char *key);       /* returns 1 if now pinned, 0 if removed, -1 if full */
int  ph_ctl_pinnable(const ph_ctl *c);

/* ---------- plugins (plugins.c) ---------- */
extern ph_backend bk_plugins;
int  plugins_restart_pending(void);
void plugins_started(void);                  /* the first ph_apply_all after loading has returned */
void plugins_update(void *plugin, void *control);  /* WM_PH_UPDATE: host->update from another thread */
void plugins_crash_note(EXCEPTION_POINTERS *ep);   /* crash filter, on the faulting thread: which plugin faulted */

/* ---------- tray / app (main.c) ---------- */
#define WM_PH_TRAY     (WM_APP + 1)
#define WM_PH_TOGGLE   (WM_APP + 2)
#define WM_PH_NAV      (WM_APP + 3)
#define WM_PH_REFRESH  (WM_APP + 4)
#define WM_PH_FGCHANGE (WM_APP + 5)
#define WM_PH_HOLD     (WM_APP + 6)     /* wParam HOLD_* source, lParam 1 down / 0 up */
#define WM_PH_UPDATE   (WM_APP + 7)     /* wParam plugin, lParam its phx_control */
extern HWND g_main;
extern HINSTANCE g_inst;
void app_tray_update(void);
void app_quit(void);
int  app_restart(void);                 /* relaunch; the new instance waits for this one to exit */
int  app_autostart_get(void);
int  app_autostart_set(int on);
int  ph_advanced(void);

/* ---------- utilities (util.c) ---------- */
void  ph_log(const char *fmt, ...);
int   ph_snprintf(char *b, int n, const char *fmt, ...);
int   ph_swprintf(wchar_t *b, int n, const wchar_t *fmt, ...);
uint64_t ph_ms(void);
double   ph_qpc_ms(void);
int   ph_run(const wchar_t *cmdline, int wait_ms);   /* hidden process, returns exit code or -1 */
int   reg_get_dword(HKEY root, const wchar_t *path, const wchar_t *name, DWORD *v);
int   reg_set_dword(HKEY root, const wchar_t *path, const wchar_t *name, DWORD v);
int   reg_del_value(HKEY root, const wchar_t *path, const wchar_t *name);
int   reg_get_str(HKEY root, const wchar_t *path, const wchar_t *name, wchar_t *out, DWORD cch);
int   reg_set_str(HKEY root, const wchar_t *path, const wchar_t *name, const wchar_t *v);
int   reg_set_bin(HKEY root, const wchar_t *path, const wchar_t *name, const void *d, DWORD n);
int   gpu_class_keys(const wchar_t *ven, wchar_t keys[][128], int max);  /* display class driver keys for PCI\VEN_xxxx */
int   ph_exe_dir(wchar_t *out, int n);                  /* folder of PhawxON.exe, no trailing slash */
int   ph_path_protected(const wchar_t *path);           /* inside Program Files (admin-only writes) */
int   ph_file_version(const wchar_t *path, int ver[3], wchar_t *desc, int ndesc); /* product version + FileDescription */
int   ph_file_string(const wchar_t *path, const wchar_t *name, wchar_t *out, int n); /* a StringFileInfo value */
int   ph_program_files(wchar_t *out, int n);            /* the real Program Files, not %ProgramFiles% */
void  ph_fmt_version(wchar_t *out, int n, const int ver[3]);                  /* "2.0" or "2.0.1" */
int   ph_open_url(const wchar_t *target);               /* open in the user's (unelevated) shell */
void *ph_alloc(size_t n);
void  ph_free(void *p);

/* ---------- power draw (power.c) ---------- */
/* sys_mw is the whole machine, from the battery while it discharges off AC; the
   parts come from readers the backends register, and parts_mw is their sum when
   every one of them answered. -1 = unknown. */
typedef struct ph_power { int sys_mw, pkg_mw, gpu_mw, parts_mw; } ph_power;
enum { PWR_PKG, PWR_GPU, PWR_KINDS };          /* CPU package or APU; a discrete GPU */
enum { PSRC_NONE, PSRC_PARTS, PSRC_SYSTEM };    /* where a total came from */
void ph_set_power_reader(int kind, int (*fn)(int *mw));   /* the first one per kind; NULL removes it */
int  ph_power_read(ph_power *p);                /* 0 if anything was read */
int  ph_battery_mw(void);                       /* the battery's discharge rate alone, -1 on AC */
int  ph_power_total(const ph_power *p, int *src);   /* battery, else package + GPU; -1 */

/* ---------- fan curves (fans.c) ---------- */
/* A fan whose owner (the GPD backend, the plugin host) offers a "Curve" mode. The
   curve sets the duty from the CPU temperature about once a second on the main
   thread; it hands the fan back to the firmware when the temperature cannot be read. */
#define FAN_PTS 7                           /* curve points at 30, 40 ... 90 °C */
typedef struct ph_fan ph_fan;
struct ph_fan {
    /* the owner fills these */
    const wchar_t *name;
    int   min_pct;
    int  (*duty)(ph_fan *f, int pct);       /* manual duty; 0 = ok */
    int  (*autom)(ph_fan *f);               /* back to the firmware */
    void *ctx;
    /* fans.c */
    char     cfg_key[80];                   /* [global] key holding the curve, "20,25,..." */
    uint8_t  pt[FAN_PTS];                   /* duty % at each point */
    int      on, cur, fails, firmware, temp;
    uint64_t last_set;
    ph_ctl   row;                           /* the CT_CURVE row, shown in Curve mode */
};
int  fan_point_temp(int i);                 /* °C of point i */
void fan_curve_add(ph_fan *f, const char *cfg_key, int16_t order, uint16_t flags);  /* registers f->row */
void fan_curve_use(ph_fan *f, int on);      /* the owner's Fan mode went to Curve (1) or away (0) */
int  fan_curve_eval(const ph_fan *f, int temp);
void fan_curve_point(ph_fan *f, int i, int pct, int save);   /* an edit from the UI */
void fans_tick(void);                       /* about once a second, main thread */
void fans_stop(void);                       /* exit and crash: the curves never touch a fan again */
void ph_set_cpu_temp_reader(int (*fn)(int *celsius));        /* by the CPU backend that can read it */
int  ph_cpu_temp(int *celsius);             /* 0 = ok */

/* ---------- embedded controller (sys/devices.c) ---------- */
int   ph_ec_read(uint8_t reg, uint8_t *v);              /* ACPI EC 0x62/0x66 through PawnIO */
int   ph_ec_write(uint8_t reg, uint8_t v);
#define PH_ARRAY(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define PH_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

/* formatting helpers shared by backends */
void fmt_watts_mw(const ph_ctl *c, int32_t v, wchar_t *b, int n);
void fmt_mhz(const ph_ctl *c, int32_t v, wchar_t *b, int n);
void fmt_mv(const ph_ctl *c, int32_t v, wchar_t *b, int n);
void fmt_pct(const ph_ctl *c, int32_t v, wchar_t *b, int n);
void fmt_onoff(const ph_ctl *c, int32_t v, wchar_t *b, int n);

#endif
