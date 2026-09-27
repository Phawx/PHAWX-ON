#include "phawx.h"
#include <powrprof.h>

#define DEL 0xFFFFFFFFu
#define UNK 0xFFFFFFFEu

typedef struct { HKEY root; const wchar_t *path, *name; } regval;

typedef struct tweak {
    const char    *key;
    const wchar_t *label;
    uint16_t       flags;
    uint8_t        reboot;
    regval         rv[2];
    const wchar_t *const *choices;   /* NULL = toggle */
    DWORD          vals[4];          /* per position; DEL = value removed */
    DWORD          absent;           /* value assumed when missing; UNK = unknown */
    int            build_min, build_max;
    int          (*avail)(void);
} tweak;

static int ms_avail(void);

static const wchar_t *const kbd_ch[] = { L"Never", L"No keyboard", L"Always", NULL };

#define HKCU HKEY_CURRENT_USER
#define HKLM HKEY_LOCAL_MACHINE

static const tweak tweaks[] = {
    { "sys.gamemode", L"Game Mode", 0, 0,
      { { HKCU, L"Software\\Microsoft\\GameBar", L"AutoGameModeEnabled" } },
      NULL, { 0, 1 }, 1, 0, 0, NULL },
    { "sys.gamecapture", L"Game Bar capture", 0, 0,
      { { HKCU, L"Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR", L"AppCaptureEnabled" },
        { HKCU, L"System\\GameConfigStore", L"GameDVR_Enabled" } },
      NULL, { 0, 1 }, UNK, 0, 0, NULL },
    { "sys.hags", L"GPU hardware scheduling", 0, 1,
      { { HKLM, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode" } },
      NULL, { 1, 2 }, UNK, 19041, 0, NULL },
    { "sys.touchkbd", L"Touch keyboard on tap", 0, 0,
      { { HKCU, L"Software\\Microsoft\\TabletTip\\1.7", L"TouchKeyboardTapInvoke" } },
      kbd_ch, { 0, 1, 2 }, 1, 22000, 0, NULL },
    { "sys.touchkbd", L"Touch keyboard on tap", 0, 0,
      { { HKCU, L"Software\\Microsoft\\TabletTip\\1.7", L"EnableDesktopModeAutoInvoke" } },
      NULL, { 0, 1 }, 0, 0, 21999, NULL },
    { "sys.nomodernstandby", L"Disable Modern Standby", CF_DANGER | CF_ADVANCED, 1,
      { { HKLM, L"SYSTEM\\CurrentControlSet\\Control\\Power", L"PlatformAoAcOverride" } },
      NULL, { DEL, 0 }, DEL, 0, 0, ms_avail },
};

#define NT PH_ARRAY(tweaks)

static ph_ctl ctls[NT + 1];

static int ms_avail(void)
{
    SYSTEM_POWER_CAPABILITIES pc;
    DWORD v;
    if (!reg_get_dword(HKLM, L"SYSTEM\\CurrentControlSet\\Control\\Power", L"PlatformAoAcOverride", &v)) return 1;
    return GetPwrCapabilities(&pc) && pc.spare2[2]; /* AoAc */
}

static int win_build(void)
{
    wchar_t b[16];
    int n = 0;
    if (reg_get_str(HKLM, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber", b, PH_ARRAY(b)))
        return 0;
    for (const wchar_t *p = b; *p >= L'0' && *p <= L'9'; p++) n = n * 10 + (*p - L'0');
    return n;
}

static int npos(const tweak *t)
{
    int n = 0;
    if (!t->choices) return 2;
    while (t->choices[n]) n++;
    return n;
}

static int t_get(ph_ctl *c, int32_t *out)
{
    const tweak *t = c->ctx;
    DWORD v;
    if (reg_get_dword(t->rv[0].root, t->rv[0].path, t->rv[0].name, &v)) v = t->absent;
    if (v == UNK) return -1;
    for (int i = 0, n = npos(t); i < n; i++)
        if (t->vals[i] == v) { *out = i; return 0; }
    if (!t->choices && v != DEL) { *out = v != t->vals[0]; return 0; }
    return -1;
}

static int t_set(ph_ctl *c, int32_t pos)
{
    const tweak *t = c->ctx;
    if (pos < 0 || pos >= npos(t)) return -1;
    int32_t old = -1;
    t_get(c, &old);
    DWORD v = t->vals[pos], cur;
    for (int i = 0; i < 2 && t->rv[i].name; i++) {
        const regval *r = &t->rv[i];
        if (v == DEL) {
            if (reg_del_value(r->root, r->path, r->name) && !reg_get_dword(r->root, r->path, r->name, &cur)) return -1;
        } else if (reg_set_dword(r->root, r->path, r->name, v)) {
            return -1;
        }
    }
    if (old != pos && t->reboot) ui_toast(L"Restart Windows to apply");
    return 0;
}

static int t_probe(void) { return 1; }

static int t_init(void)
{
    int build = win_build(), n = 0;
    ctls[n++] = (ph_ctl){ .key = NULL, .label = L"Windows", .type = CT_HEADER, .page = PG_SYSTEM, .order = 3000,
                          .flags = CF_SECTION };
    for (int i = 0; i < NT; i++) {
        const tweak *t = &tweaks[i];
        if (t->build_min && build && build < t->build_min) continue;
        if (t->build_max && (!build || build > t->build_max)) continue;
        if (t->avail && !t->avail()) continue;
        ctls[n] = (ph_ctl){
            .key = t->key, .label = t->label, .type = t->choices ? CT_CHOICE : CT_TOGGLE,
            .page = PG_SYSTEM, .order = (int16_t)(3001 + i),
            .flags = (uint16_t)(CF_OPTIONAL | CF_NOSAVE | t->flags),
            .choices = t->choices, .get = t_get, .set = t_set, .ctx = (void *)t,
        };
        n++;
    }
    if (n > 1) ph_register_ctls(ctls, n);
    return 0;
}

ph_backend bk_tweaks = {
    "tweaks",
    L"Windows tweaks",
    BK_SYS,
    t_probe,
    t_init,
    NULL,
    NULL,
    NULL,
    0
};
