#include "phawx.h"

/* Controls pinned to the Quick page, in the order they were pinned. Stored as
   [pins] 0=key, 1=key, ... Keys of controls that are not present right now (a
   plugin that is off, hardware that is gone) are kept so the pin comes back. */

#define MAX_PINS 64
#define KEY_LEN  80     /* plugin keys are up to 79 characters */

static char pins[MAX_PINS][KEY_LEN];
static int npins;

/* one write for the whole section instead of one file rewrite per key */
static void pins_save(void)
{
    static wchar_t buf[MAX_PINS * (KEY_LEN + 8) + 2];
    int k = 0;
    if (!npins) { cfg_clear("pins"); return; }
    for (int i = 0; i < npins; i++) {
        k += ph_swprintf(buf + k, PH_ARRAY(buf) - k, L"%d=", i);
        for (const char *s = pins[i]; *s; s++) buf[k++] = (wchar_t)(unsigned char)*s;
        buf[k++] = 0;
    }
    buf[k] = 0;
    WritePrivateProfileSectionW(L"pins", buf, cfg_file());
}

void pins_load(void)
{
    char k[8], v[KEY_LEN];
    npins = 0;
    for (int i = 0; i < MAX_PINS; i++) {
        ph_snprintf(k, sizeof k, "%d", i);
        if (!cfg_get_str("pins", k, v, sizeof v) || !v[0] || pin_has(v)) continue;
        lstrcpynA(pins[npins++], v, KEY_LEN);
    }
}

int pin_count(void) { return npins; }
const char *pin_at(int i) { return (i >= 0 && i < npins) ? pins[i] : NULL; }

int pin_has(const char *key)
{
    if (!key) return 0;
    for (int i = 0; i < npins; i++)
        if (!lstrcmpA(pins[i], key)) return 1;
    return 0;
}

int pin_toggle(const char *key)
{
    if (!key || !key[0] || lstrlenA(key) >= KEY_LEN) return -1;
    for (int i = 0; i < npins; i++) {
        if (lstrcmpA(pins[i], key)) continue;
        for (int j = i; j < npins - 1; j++) lstrcpyA(pins[j], pins[j + 1]);
        npins--;
        pins_save();
        return 0;
    }
    if (npins >= MAX_PINS) return -1;
    lstrcpyA(pins[npins++], key);
    pins_save();
    return 1;
}

int ph_ctl_pinnable(const ph_ctl *c)
{
    if (!c || !c->key || c->page == PG_QUICK || (c->flags & CF_NOPIN) || lstrlenA(c->key) >= KEY_LEN) return 0;
    return c->type == CT_SLIDER || c->type == CT_TOGGLE || c->type == CT_CHOICE || c->type == CT_ACTION;
}
