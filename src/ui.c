#include "phawx.h"
#include <windowsx.h>
#include <tpcshrd.h>

#define UI_W    380
#define HDR_H   72
#define TAB_H   40
#define FOOT_H  28
#define PAD     16
#define MAXROWS 512

#define IND     20      /* label indent: the pin column */
#define PIN_HIT 22      /* taps left of PAD + this hit the pin */

enum { TM_ANIM = 1, TM_LIVE, TM_COMMIT, TM_TOAST, TM_HOLD, TM_EASE };
enum { WM_UI_SHOW = WM_APP + 40, WM_UI_TOAST, WM_UI_NAV, WM_UI_REFRESH, WM_UI_HOLD };
enum { DR_NONE, DR_TAP, DR_SCROLL, DR_SLIDER, DR_CANCEL, DR_CURVE };   /* DR_CANCEL: slid off its tap */

#define C_BG     RGB(24, 24, 28)
#define C_HDR    RGB(33, 33, 39)
#define C_ROW    RGB(44, 44, 52)
#define C_SEL    RGB(54, 56, 67)
#define C_TEXT   RGB(236, 236, 242)
#define C_DIM    RGB(148, 148, 160)
#define C_FAINT  RGB(96, 96, 108)
#define C_ACC    RGB(64, 214, 128)
#define C_TRACK  RGB(74, 74, 86)
#define C_TRY    RGB(240, 188, 72)      /* a value AutoTDP is trying right now */
#define C_DANGER RGB(240, 98, 86)
#define C_SHADE  RGB(14, 14, 17)
#define C_TOAST  RGB(58, 60, 72)
#define C_ZEBRA  RGB(30, 30, 35)

static const wchar_t *const tab_names[PG_COUNT] = {
    L"Quick", L"CPU", L"GPU", L"Display", L"System", L"Plugins", L"Settings"
};

static HWND hw, toast_hw;
static DWORD ui_tid;
static volatile LONG vis;
static int dpi = 96, font_dpi;
static HFONT f_title, f_body, f_small, f_tab, f_bold;
static HDC bb_dc;
static HBITMAP bb_bmp, bb_old;
static int bb_w, bb_h;
static RECT mon;
static int pw = 1, ph_h = 1, shown_w, anim_dir;

static int page = PG_QUICK;
static int tabs[PG_COUNT], ntabs, tab_l[PG_COUNT], tab_r[PG_COUNT];
static ph_ctl *rows[MAXROWS], *tmp_rows[MAXROWS];
static int nrows, row_y[MAXROWS], row_h[MAXROWS], row_vx[MAXROWS], content_h;
static int32_t hwv[MAXROWS];
static uint8_t hwok[MAXROWS];
static const wchar_t *row_from[MAXROWS];    /* pinned rows: where the control lives */
static int sel = -1, scroll, sel_hint = -1;

#define MAXPINROWS 64
static wchar_t pin_from[MAXPINROWS][48];
static ph_ctl pin_hdr, pin_help;

/* hold-to-reset: sources held (HOLD_*), when it started, how long it takes */
static int hold_src, hold_ms;
static uint64_t hold_t0;
static ph_ctl *hold_ctl;

static ph_ctl *pend, *conf;
static int32_t pend_v, conf_v;
static int conf_btn;
static uint64_t last_live;
static RECT rc_close, rc_yes, rc_no;

static wchar_t toast_msg[160];
static int toast_on, toast_seq, toast_dpi;
static HFONT toast_font;
static wchar_t live[160];

static int drag, down_x, down_y, down_scroll, down_row;

/* rows AutoTDP holds: the knob glides to the value it sets instead of jumping */
static float ease_v[MAXROWS];
static uint8_t ease_ok[MAXROWS];
static int easing, live_ms;
static uint64_t last_tick;

static int ui_scale = 100;              /* Settings -> UI scale, % */

/* the fan curve being edited: its point under the cursor, and whether up / down
   change that point (A starts and ends editing) */
static int cpt = 3;
static ph_ctl *cedit;

static int S(int v) { return MulDiv(v, dpi, 96); }
static int scaled_dpi(HMONITOR m);
static int on_ui(void) { return GetCurrentThreadId() == ui_tid; }
static int top_y(void) { return S(HDR_H + TAB_H); }
static int view_h(void) { return ph_h - S(HDR_H + TAB_H + FOOT_H); }
static void inval(void) { if (hw) InvalidateRect(hw, NULL, FALSE); }

typedef HRESULT (WINAPI *gdfm_fn)(HMONITOR, int, UINT *, UINT *);

static int mon_dpi(HMONITOR m)
{
    static gdfm_fn fn;
    static int tried;
    if (!tried) {
        tried = 1;
        HMODULE h = LoadLibraryExW(L"shcore.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (h) fn = (gdfm_fn)(void *)GetProcAddress(h, "GetDpiForMonitor");
    }
    UINT x = 0, y = 0;
    if (fn && fn(m, 0, &x, &y) == S_OK && x) return (int)x;
    HDC dc = GetDC(NULL);
    int d = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    return d > 0 ? d : 96;
}

static int scaled_dpi(HMONITOR m) { return MulDiv(mon_dpi(m), ui_scale, 100); }

void ui_set_scale(int pct)
{
    pct = PH_CLAMP(pct, 50, 250);
    if (pct == ui_scale) return;
    ui_scale = pct;
    ui_refresh();
}

static HMONITOR target_monitor(void)
{
    HWND fg = GetForegroundWindow();
    if (fg && fg != hw && fg != toast_hw) return MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    POINT p;
    GetCursorPos(&p);
    return MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
}

static HFONT mk_font(int px, int weight)
{
    return CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

static void del_fonts(void)
{
    HFONT *f[] = { &f_title, &f_body, &f_small, &f_tab, &f_bold };
    for (int i = 0; i < PH_ARRAY(f); i++)
        if (*f[i]) { DeleteObject(*f[i]); *f[i] = NULL; }
}

static void make_fonts(void)
{
    if (font_dpi == dpi && f_body) return;
    del_fonts();
    f_title = mk_font(S(24), FW_BOLD);
    f_body = mk_font(S(15), FW_NORMAL);
    f_small = mk_font(S(12), FW_NORMAL);
    f_tab = mk_font(S(13), FW_SEMIBOLD);
    f_bold = mk_font(S(15), FW_SEMIBOLD);
    font_dpi = dpi;
}

static void free_bb(void)
{
    if (!bb_dc) return;
    SelectObject(bb_dc, bb_old);
    DeleteObject(bb_bmp);
    DeleteDC(bb_dc);
    bb_dc = NULL;
    bb_bmp = NULL;
}

static int ensure_bb(void)
{
    if (bb_dc && bb_w == pw && bb_h == ph_h) return 1;
    free_bb();
    HDC s = GetDC(NULL);
    bb_dc = CreateCompatibleDC(s);
    bb_bmp = CreateCompatibleBitmap(s, pw, ph_h);
    ReleaseDC(NULL, s);
    if (!bb_dc || !bb_bmp) {
        if (bb_bmp) DeleteObject(bb_bmp);
        if (bb_dc) DeleteDC(bb_dc);
        bb_dc = NULL;
        bb_bmp = NULL;
        return 0;
    }
    bb_old = (HBITMAP)SelectObject(bb_dc, bb_bmp);
    bb_w = pw;
    bb_h = ph_h;
    return 1;
}

/* ---------- control helpers ---------- */

static int row_visible(const ph_ctl *c)
{
    if (c->flags & CF_HIDDEN) return 0;
    if ((c->flags & CF_ADVANCED) && !ph_advanced()) return 0;
    return 1;
}

static int selectable(const ph_ctl *c) { return c->type != CT_HEADER && c->type != CT_INFO && c->type != CT_STATUS; }
static int locked(const ph_ctl *c) { return c->type != CT_INFO && autotdp_owns(c); }

/* a persisted setting a long press resets; rows without a key (plugin switches,
   links) have nothing to reset */
static int has_default(const ph_ctl *c) { return c->key || c->type == CT_CURVE; }

static int holdable(const ph_ctl *c)
{
    return c && has_default(c) && selectable(c) && c->type != CT_ACTION && !locked(c);
}

/* ... and it is away from Default */
static int resettable(const ph_ctl *c) { return holdable(c) && c->active; }

static int is_section(const ph_ctl *c) { return c->type == CT_HEADER && (c->flags & CF_SECTION); }
static int is_table(const ph_ctl *c) { return c->type == CT_INFO && (c->flags & CF_TABLE); }

static int pin_help_get(ph_ctl *c, int32_t *out) { (void)c; *out = TONE_DIM; return 0; }
static void pin_help_fmt(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    (void)c; (void)v;
    lstrcpynW(b, L"Tap a setting's pin or press Y to add it here", n);
}

static int page_rows(int pg, ph_ctl **out)
{
    int n = 0;
    for (int i = 0, k = ph_ctl_count(); i < k && n < MAXROWS; i++) {
        ph_ctl *c = ph_ctl_at(i);
        if (!c || c->page != pg || !row_visible(c)) continue;
        /* an empty part goes, and an empty section when the next title is a section too */
        if (c->type == CT_HEADER)
            while (n > 0 && out[n - 1]->type == CT_HEADER && (!is_section(out[n - 1]) || is_section(c))) n--;
        out[n++] = c;
    }
    while (n > 0 && out[n - 1]->type == CT_HEADER) n--;
    if (pg != PG_QUICK) return n;
    /* pinned controls follow the built-in Quick rows, in the order they were pinned;
       advanced ones stay because the user asked for them, unavailable ones do not */
    if (!pin_hdr.label) {
        pin_hdr = (ph_ctl){ .label = L"Pinned", .type = CT_HEADER, .page = PG_QUICK };
        pin_help = (ph_ctl){ .label = L"Pins", .type = CT_STATUS, .page = PG_QUICK, .get = pin_help_get, .fmt = pin_help_fmt };
    }
    if (n < MAXROWS) out[n++] = &pin_hdr;
    int shown = 0;
    for (int i = 0; i < pin_count() && n < MAXROWS; i++) {
        ph_ctl *c = ph_ctl_find(pin_at(i));
        if (!c || (c->flags & CF_HIDDEN) || !ph_ctl_pinnable(c)) continue;
        out[n++] = c;
        shown++;
    }
    if (!shown && n < MAXROWS) out[n++] = &pin_help;
    return n;
}

/* "CPU · Power limits": the page and section a pinned control comes from */
static void where_from(const ph_ctl *c, wchar_t *b, int n)
{
    const wchar_t *sec = NULL;
    for (int i = 0, k = ph_ctl_count(); i < k; i++) {
        ph_ctl *o = ph_ctl_at(i);
        if (o == c) break;
        if (o->page == c->page && o->type == CT_HEADER && !(o->flags & CF_HIDDEN)) sec = o->label;
    }
    const wchar_t *pg = c->page < PG_COUNT ? tab_names[c->page] : L"";
    if (sec && lstrcmpiW(sec, pg)) ph_swprintf(b, n, L"%s \x00B7 %s", pg, sec);
    else lstrcpynW(b, pg, n);
}

static int32_t base_val(int i)
{
    ph_ctl *c = rows[i];
    return c->active ? c->val : hwok[i] ? hwv[i] : c->def;
}

static void fmt_val(const ph_ctl *c, int32_t v, wchar_t *b, int n)
{
    b[0] = 0;
    if (c->fmt) { c->fmt(c, v, b, n); return; }
    if (c->type == CT_CHOICE && c->choices) {
        if (v >= 0 && v <= c->max && c->choices[v]) lstrcpynW(b, c->choices[v], n);
        else ph_swprintf(b, n, L"%d", v);
        return;
    }
    if (c->type == CT_TOGGLE) { lstrcpynW(b, v ? L"On" : L"Off", n); return; }
    const wchar_t *u = c->unit ? c->unit : L"";
    const wchar_t *sp = (u[0] && u[0] != L'%') ? L" " : L"";
    ph_swprintf(b, n, ((c->flags & CF_SIGNED) && v > 0) ? L"+%d%s%s" : L"%d%s%s", v, sp, u);
}

static void refresh_hw(int info_only)
{
    for (int i = 0; i < nrows; i++) {
        ph_ctl *c = rows[i];
        int live = c->type == CT_INFO || c->type == CT_STATUS;
        if (info_only && !live) continue;
        if (!c->get || c->type == CT_HEADER || (!live && c->active)) {
            if (!info_only) hwok[i] = 0;
            continue;
        }
        int32_t v = 0;
        hwok[i] = c->get(c, &v) == 0;
        hwv[i] = v;
    }
}

/* a second line under the label: a pinned row's origin, or a status (plugins) */
static int two_line(int i) { return row_from[i] || rows[i]->sub; }

static void layout(void)
{
    int y = 0;
    for (int i = 0; i < nrows; i++) {
        int h;
        switch (rows[i]->type) {
        case CT_HEADER: h = is_section(rows[i]) ? (i == 0 ? 34 : 56) : i == 0 ? 30 : 40; break;
        case CT_SLIDER: h = two_line(i) ? 78 : 64; break;
        case CT_INFO: h = is_table(rows[i]) ? 26 : 36; break;
        case CT_STATUS: h = 38; break;
        case CT_CURVE: h = 204; break;
        default: h = two_line(i) ? 60 : 50; break;
        }
        row_y[i] = y;
        row_h[i] = S(h);
        y += row_h[i];
    }
    content_h = y + S(12);
}

static void clamp_scroll(void)
{
    int mx = content_h - view_h();
    if (mx < 0) mx = 0;
    scroll = PH_CLAMP(scroll, 0, mx);
}

static void ensure_visible(int i)
{
    if (i < 0 || i >= nrows) return;
    int t = row_y[i], b = row_y[i] + row_h[i], vh = view_h();
    if (i > 0 && rows[i - 1]->type == CT_HEADER) t = row_y[i - 1];
    if (b > scroll + vh) scroll = b - vh;
    if (t < scroll) scroll = t;
    clamp_scroll();
}

static int first_sel(int from, int dir)
{
    for (int i = from; i >= 0 && i < nrows; i += dir)
        if (selectable(rows[i])) return i;
    return -1;
}

static void rebuild(void)
{
    ph_ctl *keep = (sel >= 0 && sel < nrows) ? rows[sel] : NULL;
    memset(ease_ok, 0, sizeof ease_ok);
    ntabs = 0;
    for (int pg = 0; pg < PG_COUNT; pg++)
        if (page_rows(pg, tmp_rows) > 0) tabs[ntabs++] = pg;
    int ok = 0;
    for (int i = 0; i < ntabs; i++) if (tabs[i] == page) ok = 1;
    if (!ok && ntabs) { page = tabs[0]; keep = NULL; scroll = 0; }
    nrows = page_rows(page, rows);
    for (int i = 0, np = 0, pinned = 0; i < nrows; i++) {
        row_from[i] = NULL;
        if (rows[i] == &pin_hdr) pinned = 1;
        else if (pinned && ph_ctl_pinnable(rows[i]) && np < MAXPINROWS) {
            where_from(rows[i], pin_from[np], PH_ARRAY(pin_from[np]));
            row_from[i] = pin_from[np++];
        }
    }
    refresh_hw(0);
    sel = -1;
    for (int i = 0; i < nrows; i++) if (rows[i] == keep) sel = i;
    /* the row just went away (unpinned from Quick): stay where it was */
    if (sel < 0 && keep && sel_hint >= 0 && nrows) {
        sel = first_sel(sel_hint < nrows ? sel_hint : nrows - 1, 1);
        if (sel < 0) sel = first_sel(nrows - 1, -1);
    }
    sel_hint = -1;
    if (sel < 0) sel = first_sel(0, 1);
    if (pend) {
        int f = 0;
        for (int i = 0; i < nrows; i++) if (rows[i] == pend) f = 1;
        if (!f) pend = NULL;
    }
    layout();
    clamp_scroll();
}

/* ---------- applying ---------- */

static void apply_val(ph_ctl *c, int32_t v, int confirmed)
{
    if (!c) return;
    /* switching a dangerous toggle off only undoes it, so it needs no confirmation */
    if ((c->flags & (CF_DANGER | CF_CONFIRM)) && !confirmed && !(c->type == CT_TOGGLE && v == 0)) {
        conf = c;
        conf_v = v;
        conf_btn = 0;
        SetRectEmpty(&rc_yes);    /* the buttons move with the message: none until drawn */
        SetRectEmpty(&rc_no);
        inval();
        return;
    }
    int seq = toast_seq;
    int r = ph_ctl_apply(c, v);
    if (r) {
        if (seq == toast_seq && c->type != CT_ACTION) {
            wchar_t m[96];
            ph_swprintf(m, PH_ARRAY(m), L"Could not apply %s", c->label ? c->label : L"setting");
            ui_toast(m);
        }
    } else if (!(c->flags & CF_NOSAVE) || c->type != CT_ACTION) {
        cfg_mark_dirty();
    }
    app_tray_update();
    if (hw && vis) rebuild();
    inval();
}

static void commit_pending(void)
{
    if (!pend) return;
    if (hw) KillTimer(hw, TM_COMMIT);
    ph_ctl *c = pend;
    int32_t v = pend_v;
    pend = NULL;
    if (!c->active || c->val != v) apply_val(c, v, 0);
    inval();
}

static void set_pending(ph_ctl *c, int32_t v, int from_drag)
{
    v = PH_CLAMP(v, c->min, c->max);
    if (pend == c && pend_v == v) return;
    pend = c;
    pend_v = v;
    if ((c->flags & CF_LIVE) && !(c->flags & CF_DANGER)) {
        if (!from_drag || ph_ms() - last_live >= 60) {
            last_live = ph_ms();
            if (ph_ctl_apply(c, v) == 0) cfg_mark_dirty();
        }
    }
    if (!from_drag) SetTimer(hw, TM_COMMIT, 450, NULL);
    inval();
}

static void reset_sel(void)
{
    if (sel < 0 || sel >= nrows) return;
    ph_ctl *c = rows[sel];
    if (!has_default(c) || !selectable(c) || c->type == CT_ACTION || locked(c)) return;
    if (pend == c) { pend = NULL; KillTimer(hw, TM_COMMIT); }
    if (!c->active) return;
    ph_ctl_reset(c);
    cfg_mark_dirty();
    wchar_t m[96];
    ph_swprintf(m, PH_ARRAY(m), L"%s set to Default", c->label ? c->label : L"Setting");
    ui_toast(m);
    rebuild();
    inval();
}

/* ---------- hold to reset ---------- */

static void hold_end(void)
{
    if (!hold_src && !hold_ctl) return;
    hold_src = 0;
    hold_ctl = NULL;
    if (hw) KillTimer(hw, TM_HOLD);
    inval();
}

static ph_ctl *cur(void);

/* A hold belongs to the row that was selected when it started and ends if the
   selection moves, the source is released, a dialog opens or the menu closes. */
static void hold_start(int src, int ms)
{
    ph_ctl *c = cur();
    if (!c || conf) return;
    if (!hold_src) {
        hold_t0 = ph_ms();
        hold_ms = ms;
        hold_ctl = c;
        SetTimer(hw, TM_HOLD, 16, NULL);
    }
    hold_src |= src;
    inval();
}

static void hold_stop(int src)
{
    hold_src &= ~src;
    if (!hold_src) hold_end();
}

static int held;

static void hold_tick(void)
{
    if (!hold_src || !vis || conf || cur() != hold_ctl) { hold_end(); return; }
    if (ph_ms() - hold_t0 < (uint64_t)hold_ms) { inval(); return; }
    int ptr = hold_src & HOLD_PTR;
    ph_ctl *c = hold_ctl;
    hold_end();
    if (ptr) held = 1;    /* the finger or button coming up is not a tap */
    /* a long press also starts on rows at Default, like X or R it then only says so */
    if (resettable(c)) reset_sel();
    else ui_toast(locked(c) ? L"Managed by AutoTDP" : L"Already at Default");
}

/* 0..1000 while the selected row is being held towards Default */
static int hold_progress(void)
{
    if (!hold_src || !hold_ctl || !resettable(hold_ctl) || hold_ms <= 0) return -1;
    uint64_t e = ph_ms() - hold_t0;
    return e >= (uint64_t)hold_ms ? 1000 : (int)(e * 1000 / (uint64_t)hold_ms);
}

void ui_hold(int src, int down)
{
    if (!hw) return;
    if (!on_ui()) { PostMessageW(hw, WM_UI_HOLD, (WPARAM)src, (LPARAM)down); return; }
    if (!down) { hold_stop(src); return; }
    if (!vis || conf) return;
    ph_ctl *c = cur();
    if (!c) return;
    if (!resettable(c)) {
        if (locked(c)) ui_toast(L"Managed by AutoTDP");
        else if (has_default(c) && c->type != CT_ACTION && !c->active) ui_toast(L"Already at Default");
        return;
    }
    hold_start(src, HOLD_MS);
}

/* ---------- pins ---------- */

static void pin_sel(void)
{
    ph_ctl *c = cur();
    if (!c) return;
    if (!ph_ctl_pinnable(c)) {
        if (c->page == PG_QUICK) ui_toast(L"Already on the Quick page");
        else ui_toast(L"This row can't be pinned");
        return;
    }
    int r = pin_toggle(c->key);
    if (r < 0) { ui_toast(L"The Quick page is full of pins"); return; }
    ui_toast(r ? L"Pinned to Quick" : L"Removed from Quick");
    sel_hint = sel;
    rebuild();
    if (sel >= 0) ensure_visible(sel);
    inval();
}

static void toggle_autotdp(void)
{
    ph_ctl *c = ph_ctl_find("auto.on");
    int want = !autotdp_running();
    if (c && !(c->flags & CF_HIDDEN) && c->type == CT_TOGGLE) {
        apply_val(c, want, 1);
    } else {
        if (want) autotdp_start(); else autotdp_stop();
        app_tray_update();
    }
    ui_toast(autotdp_running() ? L"Phawx ON: AutoTDP running" : L"Phawx OFF: AutoTDP stopped");
    if (vis) rebuild();
    inval();
}

static void confirm_close(int yes)
{
    ph_ctl *c = conf;
    conf = NULL;
    if (yes && c) apply_val(c, conf_v, 1);
    else if (vis) rebuild();
    inval();
}

/* ---------- navigation ---------- */

static void move_sel(int d)
{
    commit_pending();
    cedit = NULL;
    if (!nrows) return;
    int n = sel < 0 ? first_sel(d > 0 ? 0 : nrows - 1, d) : first_sel(sel + d, d);
    if (n >= 0) sel = n;
    else if (d < 0) scroll = 0;
    else scroll = content_h;
    clamp_scroll();
    if (n >= 0) ensure_visible(sel);
}

static void switch_tab(int d)
{
    commit_pending();
    cedit = NULL;
    if (ntabs < 1) return;
    int idx = 0;
    for (int i = 0; i < ntabs; i++) if (tabs[i] == page) idx = i;
    idx = (idx + d + ntabs) % ntabs;
    page = tabs[idx];
    sel = -1;
    scroll = 0;
    rebuild();
}

static void goto_tab(int idx)
{
    if (idx < 0 || idx >= ntabs || tabs[idx] == page) return;
    commit_pending();
    cedit = NULL;
    page = tabs[idx];
    sel = -1;
    scroll = 0;
    rebuild();
}

static ph_ctl *cur(void) { return (sel >= 0 && sel < nrows) ? rows[sel] : NULL; }

static void adjust(int d)
{
    ph_ctl *c = cur();
    if (!c) return;
    if (locked(c)) { ui_toast(L"Managed by AutoTDP"); return; }
    int32_t b = pend == c ? pend_v : base_val(sel), v;
    switch (c->type) {
    case CT_SLIDER:
        set_pending(c, b + d * c->step, 0);
        break;
    case CT_TOGGLE:
        v = d > 0 ? 1 : 0;
        if (c->active && c->val == v) return;
        apply_val(c, v, 0);
        break;
    case CT_CHOICE:
        v = b + d;
        if (v < c->min || v > c->max) return;
        apply_val(c, v, 0);
        break;
    case CT_CURVE:
        cpt = PH_CLAMP(cpt + d, 0, FAN_PTS - 1);
        break;
    }
}

/* up / down while a curve point is being edited */
static void curve_step(int d)
{
    ph_fan *f = cedit ? cedit->ctx : NULL;
    if (!f) return;
    fan_curve_point(f, cpt, f->pt[cpt] + d * 5, 1);
}

static void activate(void)
{
    ph_ctl *c = cur();
    if (!c) return;
    if (locked(c)) { ui_toast(L"Managed by AutoTDP"); return; }
    int32_t b = base_val(sel), v;
    switch (c->type) {
    case CT_SLIDER: commit_pending(); break;
    case CT_TOGGLE: apply_val(c, !b, 0); break;
    case CT_CHOICE:
        v = b + 1;
        if (v > c->max) v = c->min;
        apply_val(c, v, 0);
        break;
    case CT_ACTION: apply_val(c, c->val, 0); break;
    case CT_CURVE: cedit = cedit == c ? NULL : c; break;
    }
}

void ui_nav(int a)
{
    if (!hw) return;
    if (!on_ui()) { PostMessageW(hw, WM_UI_NAV, (WPARAM)a, 0); return; }
    if (a == IN_TOGGLE_MENU) { ui_toggle(); return; }
    if (!vis) return;
    if (conf) {
        hold_end();
        switch (a) {
        case IN_LEFT: case IN_UP: conf_btn = 0; break;
        case IN_RIGHT: case IN_DOWN: conf_btn = 1; break;
        case IN_OK: confirm_close(conf_btn); break;
        case IN_BACK: confirm_close(0); break;
        }
        inval();
        return;
    }
    if (cedit && cedit != cur()) cedit = NULL;
    if (cedit && (a == IN_UP || a == IN_DOWN || a == IN_BACK)) {
        if (a == IN_BACK) cedit = NULL;
        else curve_step(a == IN_UP ? 1 : -1);
        inval();
        return;
    }
    switch (a) {
    case IN_UP: move_sel(-1); break;
    case IN_DOWN: move_sel(1); break;
    case IN_LEFT: adjust(-1); break;
    case IN_RIGHT: adjust(1); break;
    case IN_OK: activate(); break;
    case IN_BACK: ui_show(0); break;
    case IN_TAB_PREV: switch_tab(-1); break;
    case IN_TAB_NEXT: switch_tab(1); break;
    case IN_AUX: toggle_autotdp(); break;
    case IN_RESET: reset_sel(); break;
    case IN_PIN: pin_sel(); break;
    }
    inval();
}

/* ---------- live strip ---------- */

static void join(wchar_t *b, int n, const wchar_t *part)
{
    if (!part[0]) return;
    int l = lstrlenW(b);
    if (l) ph_swprintf(b + l, n - l, L"  \x00B7  %s", part);
    else lstrcpynW(b, part, n);
}

static void fmt_clock(wchar_t *b, int n, const wchar_t *tag, int mhz)
{
    b[0] = 0;
    if (mhz <= 0) return;
    if (mhz >= 1000) ph_swprintf(b, n, L"%s %d.%d GHz", tag, mhz / 1000, (mhz % 1000) / 100);
    else ph_swprintf(b, n, L"%s %d MHz", tag, mhz);
}

static void update_live(void)
{
    ph_fps f;
    ph_auto_state st;
    wchar_t t[48];
    int cpu = 0, gpu = 0, run = autotdp_running();
    live[0] = 0;
    if (fps_sample(&f) && f.fps > 0.5f) {
        if (f.low1 > 0.5f) ph_swprintf(t, PH_ARRAY(t), L"%d FPS (1%% %d)", (int)(f.fps + 0.5f), (int)(f.low1 + 0.5f));
        else ph_swprintf(t, PH_ARRAY(t), L"%d FPS", (int)(f.fps + 0.5f));
        join(live, PH_ARRAY(live), t);
    }
    if (run) {
        autotdp_state(&st);
        cpu = st.cpu_mhz;
        gpu = st.gpu_mhz;
    } else {
        ph_clk *d = ph_cpu_clk();
        if (d && d->cur && d->cur(d, &cpu)) cpu = 0;
        d = ph_gpu_clk();
        if (d && d->cur && d->cur(d, &gpu)) gpu = 0;
    }
    fmt_clock(t, PH_ARRAY(t), L"CPU", cpu);
    join(live, PH_ARRAY(live), t);
    fmt_clock(t, PH_ARRAY(t), L"GPU", gpu);
    join(live, PH_ARRAY(live), t);
    if (run && st.target > 0) {
        ph_swprintf(t, PH_ARRAY(t), L"Target %d", st.target);
        join(live, PH_ARRAY(live), t);
    }
    if (run && st.power_mw > 0) {
        ph_swprintf(t, PH_ARRAY(t), L"%d.%d W", st.power_mw / 1000, st.power_mw % 1000 / 100);
        join(live, PH_ARRAY(live), t);
    }
    if (!live[0]) lstrcpynW(live, run ? L"AutoTDP waiting for a game" : L"No game detected", PH_ARRAY(live));
}

/* ---------- drawing ---------- */

static void fill(HDC m, int l, int t, int r, int b, COLORREF c)
{
    RECT rc = { l, t, r, b };
    SetDCBrushColor(m, c);
    FillRect(m, &rc, (HBRUSH)GetStockObject(DC_BRUSH));
}

static void rrect(HDC m, int l, int t, int r, int b, int rad, COLORREF c)
{
    SelectObject(m, GetStockObject(DC_BRUSH));
    SelectObject(m, GetStockObject(DC_PEN));
    SetDCBrushColor(m, c);
    SetDCPenColor(m, c);
    RoundRect(m, l, t, r, b, rad, rad);
}

static void circle(HDC m, int cx, int cy, int rad, COLORREF c)
{
    SelectObject(m, GetStockObject(DC_BRUSH));
    SelectObject(m, GetStockObject(DC_PEN));
    SetDCBrushColor(m, c);
    SetDCPenColor(m, c);
    Ellipse(m, cx - rad, cy - rad, cx + rad + 1, cy + rad + 1);
}

static void text(HDC m, HFONT f, COLORREF c, const wchar_t *s, int l, int t, int r, int b, UINT fl)
{
    RECT rc = { l, t, r, b };
    if (!s || r <= l) return;
    SelectObject(m, f);
    SetTextColor(m, c);
    DrawTextW(m, s, -1, &rc, fl | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

static int text_w(HDC m, HFONT f, const wchar_t *s)
{
    SIZE z = { 0, 0 };
    SelectObject(m, f);
    GetTextExtentPoint32W(m, s, lstrlenW(s), &z);
    return z.cx;
}

static COLORREF value_text(int i, wchar_t *b, int n)
{
    ph_ctl *c = rows[i];
    b[0] = 0;
    if (locked(c)) {
        int32_t lv;
        int l = autotdp_live(c, &lv);
        if (!l) { lstrcpynW(b, L"AutoTDP", n); return C_ACC; }
        wchar_t t[64];
        fmt_val(c, lv, t, PH_ARRAY(t));
        ph_swprintf(b, n, l == 2 ? L"Trying %s" : L"AutoTDP \x00B7 %s", t);
        return l == 2 ? C_TRY : C_ACC;
    }
    if (pend == c) { fmt_val(c, pend_v, b, n); return C_TEXT; }
    if (c->type == CT_INFO) { fmt_val(c, hwok[i] ? hwv[i] : c->val, b, n); return C_TEXT; }
    if (c->type == CT_ACTION) { if (c->fmt) c->fmt(c, c->val, b, n); return C_DIM; }
    if ((c->flags & CF_OPTIONAL) && !c->active) {
        if (hwok[i]) {
            wchar_t t[64];
            fmt_val(c, hwv[i], t, PH_ARRAY(t));
            ph_swprintf(b, n, L"Default (%s)", t);
        } else {
            lstrcpynW(b, L"Default", n);
        }
        return C_DIM;
    }
    fmt_val(c, base_val(i), b, n);
    return C_ACC;
}

/* the slider track; drawing and dragging must agree on it */
static void track_x(int *a, int *b)
{
    *a = S(PAD) + S(IND) + S(8);
    *b = pw - S(PAD) - S(8);
}

static void draw_pin(HDC m, int cx, int cy, int filled, COLORREF col)
{
    /* a push pin: cap, body, flared base, needle (units of 1/96 inch) */
    static const signed char shape[][2] = {
        { -3, -7 }, { 3, -7 }, { 3, -6 }, { 2, -6 }, { 2, -1 }, { 5, 2 }, { 5, 3 },
        { -5, 3 }, { -5, 2 }, { -2, -1 }, { -2, -6 }, { -3, -6 }
    };
    POINT pt[PH_ARRAY(shape)];
    for (int i = 0; i < PH_ARRAY(shape); i++) {
        pt[i].x = cx + S(shape[i][0]);
        pt[i].y = cy + S(shape[i][1]);
    }
    HPEN p = CreatePen(PS_SOLID, S(1) > 0 ? S(1) : 1, col);
    HGDIOBJ op = SelectObject(m, p);
    HGDIOBJ ob = SelectObject(m, GetStockObject(filled ? DC_BRUSH : NULL_BRUSH));
    SetDCBrushColor(m, col);
    Polygon(m, pt, PH_ARRAY(pt));
    MoveToEx(m, cx, cy + S(3), NULL);
    LineTo(m, cx, cy + S(9));
    SelectObject(m, ob);
    SelectObject(m, op);
    DeleteObject(p);
}

static void draw_cross(HDC m, int cx, int cy, COLORREF col)
{
    int r = S(4);
    HPEN p = CreatePen(PS_SOLID, S(2) > 0 ? S(2) : 1, col);
    HGDIOBJ o = SelectObject(m, p);
    MoveToEx(m, cx - r, cy - r, NULL);
    LineTo(m, cx + r + 1, cy + r + 1);
    MoveToEx(m, cx + r, cy - r, NULL);
    LineTo(m, cx - r - 1, cy + r + 1);
    SelectObject(m, o);
    DeleteObject(p);
}

static COLORREF tone_color(int t) { return t == TONE_GOOD ? C_ACC : t == TONE_BAD ? C_DANGER : C_DIM; }

/* ---------- fan curve graph: drawing and dragging agree on these ---------- */

static void curve_box(int y, int h, RECT *g)
{
    g->left = S(PAD) + S(IND) + S(42);
    g->right = pw - S(PAD) - S(8);
    g->top = y + S(48);
    g->bottom = y + h - S(28);
}

static int curve_x(const RECT *g, int k) { return g->left + (g->right - g->left) * k / (FAN_PTS - 1); }
static int curve_y(const RECT *g, int pct) { return g->bottom - (g->bottom - g->top) * PH_CLAMP(pct, 0, 100) / 100; }

static int temp_x(const RECT *g, int t)
{
    int lo = fan_point_temp(0), hi = fan_point_temp(FAN_PTS - 1);
    return g->left + (g->right - g->left) * (PH_CLAMP(t, lo, hi) - lo) / (hi - lo);
}

static void draw_curve(HDC m, int i, int y, int h, int x0, int x1)
{
    ph_ctl *c = rows[i];
    ph_fan *f = c->ctx;
    RECT g;
    wchar_t v[64], t[16];
    int on = i == sel, editing = on && cedit == c;
    curve_box(y, h, &g);

    COLORREF vc = C_DIM;
    if (f->firmware) lstrcpynW(v, L"No CPU temperature: firmware", PH_ARRAY(v));
    else if (f->temp > 0 && f->cur >= 0) { ph_swprintf(v, PH_ARRAY(v), L"%d \x00B0" L"C  \x00B7  %d%%", f->temp, f->cur); vc = C_ACC; }
    else lstrcpynW(v, L"Reading the CPU temperature", PH_ARRAY(v));
    int vw = text_w(m, f_small, v);
    text(m, f_body, C_TEXT, c->label, x0, y + S(8), x1 - vw - S(8), y + S(32), DT_LEFT);
    text(m, f_small, vc, v, x1 - vw, y + S(8), x1, y + S(32), DT_RIGHT);

    /* grid: 0 / 50 / 100 %, one line per point with its temperature under it */
    for (int p = 0; p <= 100; p += 50) {
        int gy = curve_y(&g, p);
        fill(m, g.left, gy, g.right + 1, gy + 1, C_TRACK);
        ph_swprintf(t, PH_ARRAY(t), L"%d%%", p);
        text(m, f_small, C_FAINT, t, x0, gy - S(8), g.left - S(6), gy + S(8), DT_RIGHT);
    }
    for (int k = 0; k < FAN_PTS; k++) {
        int gx = curve_x(&g, k);
        fill(m, gx, g.top, gx + 1, g.bottom, C_ROW);
        ph_swprintf(t, PH_ARRAY(t), L"%d\x00B0", fan_point_temp(k));
        text(m, f_small, on && k == cpt ? C_TEXT : C_FAINT, t, gx - S(20), g.bottom + S(4), gx + S(20), g.bottom + S(22), DT_CENTER);
    }
    if (f->temp > 0) {
        int tx = temp_x(&g, f->temp);
        fill(m, tx, g.top, tx + S(2), g.bottom, C_FAINT);
    }

    POINT pt[FAN_PTS];
    for (int k = 0; k < FAN_PTS; k++) { pt[k].x = curve_x(&g, k); pt[k].y = curve_y(&g, f->pt[k]); }
    HPEN pen = CreatePen(PS_SOLID, S(2) > 0 ? S(2) : 1, f->on ? C_ACC : C_DIM);
    HGDIOBJ op = SelectObject(m, pen);
    Polyline(m, pt, FAN_PTS);
    SelectObject(m, op);
    DeleteObject(pen);
    for (int k = 0; k < FAN_PTS; k++) {
        int cur_pt = on && k == cpt;
        if (cur_pt) circle(m, pt[k].x, pt[k].y, S(9), editing ? C_ACC : C_TEXT);
        circle(m, pt[k].x, pt[k].y, S(5), cur_pt ? C_SEL : C_ACC);
    }
    if (on) {
        ph_swprintf(t, PH_ARRAY(t), L"%d%%", f->pt[cpt]);
        int tx = pt[cpt].x, ty = pt[cpt].y - S(22);
        if (ty < g.top - S(14)) ty = pt[cpt].y + S(8);
        text(m, f_small, editing ? C_ACC : C_TEXT, t, tx - S(24), ty, tx + S(24), ty + S(16), DT_CENTER);
    }
    /* where the fan is now */
    if (f->temp > 0 && f->cur >= 0) circle(m, temp_x(&g, f->temp), curve_y(&g, f->cur), S(4), C_TEXT);
}

static void draw_status(HDC m, int i, int y, int h, int x0, int x1)
{
    ph_ctl *c = rows[i];
    int tone = hwok[i] ? hwv[i] : TONE_DIM;
    wchar_t t[160];
    t[0] = 0;
    if (c->fmt) c->fmt(c, tone, t, PH_ARRAY(t));
    if (!t[0] && c->label) lstrcpynW(t, c->label, PH_ARRAY(t));
    COLORREF col = tone_color(tone);
    if (tone == TONE_BAD) draw_cross(m, S(PAD) + S(5), y + h / 2, col);
    text(m, tone == TONE_DIM ? f_small : f_body, col, t, x0, y, x1, y + h, DT_LEFT);
}

/* the bar along the bottom of a row held towards Default */
static void draw_hold(HDC m, int i, int y, int h)
{
    int p = i == sel ? hold_progress() : -1;
    if (p < 0) return;
    int l = S(18), r = pw - S(18), by = y + h - S(7);
    rrect(m, l, by, r, by + S(3), S(3), C_TRACK);
    rrect(m, l, by, l + (int)((int64_t)(r - l) * p / 1000), by + S(3), S(3), C_ACC);
}

static void draw_row(HDC m, int i, int y)
{
    ph_ctl *c = rows[i];
    int h = row_h[i], x0 = S(PAD) + S(IND), x1 = pw - S(PAD);
    int lk = locked(c);
    wchar_t v[128], sub[160];
    row_vx[i] = x1;
    if (c->type == CT_HEADER) {
        if (is_section(c)) {
            if (i > 0) fill(m, S(PAD), y + S(12), pw - S(PAD), y + S(13), C_ROW);
            text(m, f_bold, C_TEXT, c->label, S(PAD), y + h - S(30), x1, y + h - S(6), DT_LEFT);
        } else {
            text(m, f_small, C_ACC, c->label, x0, y + h - S(24), x1, y + h - S(4), DT_LEFT);
        }
        return;
    }
    if (is_table(c)) {
        if (i & 1) fill(m, S(8), y, pw - S(8), y + h, C_ZEBRA);
        value_text(i, v, PH_ARRAY(v));
        int iw = v[0] ? text_w(m, f_small, v) : 0;
        int vx = x1 - iw;
        if (vx < x0 + (x1 - x0) / 3) vx = x0 + (x1 - x0) / 3;
        text(m, f_small, C_DIM, c->label, x0, y, vx - S(8), y + h, DT_LEFT);
        text(m, f_small, C_TEXT, v, vx, y, x1, y + h, DT_RIGHT);
        return;
    }
    if (i == sel) rrect(m, S(8), y + S(2), pw - S(8), y + h - S(2), S(12), C_SEL);
    if (c->type == CT_STATUS) { draw_status(m, i, y, h, x0, x1); return; }
    if (c->type == CT_CURVE) { draw_curve(m, i, y, h, x0, x1); draw_hold(m, i, y, h); return; }
    int32_t lv_;
    COLORREF lc = lk ? (autotdp_live(c, &lv_) ? C_DIM : C_FAINT) : (c->flags & CF_DANGER) ? C_DANGER : C_TEXT;
    COLORREF vc = value_text(i, v, PH_ARRAY(v));
    int vw = v[0] ? text_w(m, f_body, v) : 0;

    /* the label line, and the line under it for pinned rows and plugin status */
    int tone = TONE_DIM;
    sub[0] = 0;
    if (row_from[i]) lstrcpynW(sub, row_from[i], PH_ARRAY(sub));
    else if (c->sub) tone = c->sub(c, sub, PH_ARRAY(sub));
    int two = two_line(i);
    int lt = c->type == CT_SLIDER ? y + S(8) : two ? y + S(7) : y;
    int lb = c->type == CT_SLIDER ? y + S(32) : two ? y + S(31) : y + h;
    int st = c->type == CT_SLIDER ? y + S(28) : lb - S(2), sb = st + S(18);

    if (ph_ctl_pinnable(c)) {
        int on = pin_has(c->key);
        draw_pin(m, S(PAD) + S(5), (lt + lb) / 2 - S(1), on, on ? C_ACC : i == sel ? C_DIM : C_FAINT);
    }

    switch (c->type) {
    case CT_SLIDER: {
        int vx = x1 - vw;
        if (vx < x0 + (x1 - x0) / 3) vx = x0 + (x1 - x0) / 3;
        row_vx[i] = vx;
        text(m, f_body, lc, c->label, x0, lt, vx - S(8), lb, DT_LEFT);
        text(m, f_body, vc, v, vx, lt, x1, lb, DT_RIGHT);
        if (two) text(m, f_small, tone_color(tone), sub, x0, st, x1, sb, DT_LEFT);
        int32_t cv = pend == c ? pend_v : base_val(i), lv;
        int range = c->max - c->min, kx0, kx1;
        int ty = y + S(two ? 60 : 46);
        int live = lk ? autotdp_live(c, &lv) : 0;
        float fv = (float)cv;
        if (live) {
            /* where AutoTDP has it, gliding there over a few frames */
            float tv = (float)PH_CLAMP(lv, c->min, c->max);
            if (!ease_ok[i]) { ease_v[i] = tv; ease_ok[i] = 1; }
            float d = tv - ease_v[i], eps = range > 0 ? (float)range * 0.003f : 0.5f;
            if (d > eps || d < -eps) { ease_v[i] += d * 0.25f; easing = 1; }
            else ease_v[i] = tv;
            fv = ease_v[i];
        } else {
            ease_ok[i] = 0;
        }
        track_x(&kx0, &kx1);
        int kx = range > 0 ? kx0 + (int)((fv - (float)c->min) * (float)(kx1 - kx0) / (float)range) : kx0;
        kx = PH_CLAMP(kx, kx0, kx1);
        int on = (c->active || pend == c) && !lk;
        COLORREF fillc = live == 2 ? C_TRY : live ? C_ACC : on ? C_ACC : C_FAINT;
        rrect(m, kx0, ty - S(2), kx1, ty + S(2), S(4), C_TRACK);
        rrect(m, kx0, ty - S(2), kx, ty + S(2), S(4), fillc);
        circle(m, kx, ty, S(8), live ? C_TEXT : lk ? C_FAINT : on ? C_TEXT : C_DIM);
        break;
    }
    case CT_TOGGLE: {
        int sw = S(44), sh = S(24), sx = x1 - sw, sy = y + (h - sh) / 2;
        int optdef = (c->flags & CF_OPTIONAL) && !c->active && pend != c;
        int on = base_val(i) != 0;
        int lx = sx - S(8);
        if (optdef || lk) {
            const wchar_t *d = lk ? L"AutoTDP" : L"Default";
            int dw = text_w(m, f_small, d);
            text(m, f_small, lk ? C_ACC : C_DIM, d, lx - dw, y, lx, y + h, DT_RIGHT);
            lx -= dw + S(8);
        }
        row_vx[i] = sx;
        text(m, f_body, lc, c->label, x0, lt, lx, lb, DT_LEFT);
        if (two) text(m, f_small, tone_color(tone), sub, x0, st, lx, sb, DT_LEFT);
        COLORREF tc = on ? ((optdef || lk) ? C_FAINT : C_ACC) : C_TRACK;
        rrect(m, sx, sy, sx + sw, sy + sh, sh, tc);
        int r = sh / 2 - S(3);
        circle(m, on ? sx + sw - sh / 2 : sx + sh / 2, sy + sh / 2, r, optdef ? C_DIM : C_TEXT);
        break;
    }
    case CT_CHOICE: {
        int aw = text_w(m, f_body, L"\x2039 ");
        int total = vw + 2 * aw;
        int vx = x1 - total;
        if (vx < x0 + (x1 - x0) / 3) vx = x0 + (x1 - x0) / 3;
        row_vx[i] = vx;
        text(m, f_body, lc, c->label, x0, lt, vx - S(8), lb, DT_LEFT);
        if (two) text(m, f_small, tone_color(tone), sub, x0, st, vx - S(8), sb, DT_LEFT);
        COLORREF ac = lk ? C_FAINT : C_DIM;
        text(m, f_body, ac, L"\x2039", vx, y, vx + aw, y + h, DT_LEFT);
        text(m, f_body, vc, v, vx + aw, y, x1 - aw, y + h, DT_CENTER);
        text(m, f_body, ac, L"\x203A", x1 - aw, y, x1, y + h, DT_RIGHT);
        break;
    }
    case CT_ACTION: {
        int cw = text_w(m, f_body, L"\x203A");
        int vx = x1 - cw - (vw ? vw + S(10) : 0);
        if (vx < x0 + (x1 - x0) / 2) vx = x0 + (x1 - x0) / 2;
        row_vx[i] = vx;
        text(m, f_bold, lc, c->label, x0, lt, vx - S(8), lb, DT_LEFT);
        if (two) text(m, f_small, tone_color(tone), sub, x0, st, vx - S(8), sb, DT_LEFT);
        if (vw) text(m, f_body, vc, v, vx, y, x1 - cw - S(10), y + h, DT_RIGHT);
        text(m, f_body, C_DIM, L"\x203A", x1 - cw, y, x1, y + h, DT_RIGHT);
        break;
    }
    case CT_INFO: {
        int iw = v[0] ? text_w(m, f_small, v) : 0;
        int vx = x1 - iw;
        if (vx < x0 + (x1 - x0) * 2 / 5) vx = x0 + (x1 - x0) * 2 / 5;
        row_vx[i] = vx;
        text(m, f_small, C_DIM, c->label, x0, y, vx - S(8), y + h, DT_LEFT);
        text(m, f_small, C_TEXT, v, vx, y, x1, y + h, DT_RIGHT);
        break;
    }
    }

    draw_hold(m, i, y, h);
}

static void draw_close(HDC m)
{
    rc_close.left = pw - S(52);
    rc_close.top = S(10);
    rc_close.right = pw - S(8);
    rc_close.bottom = S(54);
    int cx = (rc_close.left + rc_close.right) / 2, cy = (rc_close.top + rc_close.bottom) / 2, r = S(7);
    HPEN p = CreatePen(PS_SOLID, S(2) > 0 ? S(2) : 1, C_DIM);
    HGDIOBJ o = SelectObject(m, p);
    MoveToEx(m, cx - r, cy - r, NULL);
    LineTo(m, cx + r + 1, cy + r + 1);
    MoveToEx(m, cx + r, cy - r, NULL);
    LineTo(m, cx - r - 1, cy + r + 1);
    SelectObject(m, o);
    DeleteObject(p);
}

static void draw_confirm(HDC m)
{
    int t = top_y(), vh = view_h();
    fill(m, 0, t, pw, t + vh, C_SHADE);
    int bx0 = S(20), bx1 = pw - S(20);
    wchar_t v[64], msg[640];
    v[0] = 0;
    if (conf->type == CT_SLIDER || conf->type == CT_CHOICE || conf->type == CT_TOGGLE)
        fmt_val(conf, conf_v, v, PH_ARRAY(v));
    const wchar_t *lbl = conf->label ? conf->label : L"Apply";
    if (conf->desc) lstrcpynW(msg, conf->desc, PH_ARRAY(msg));
    else if (v[0]) ph_swprintf(msg, PH_ARRAY(msg), L"Set %s to %s? This can cause instability.", lbl, v);
    else ph_swprintf(msg, PH_ARRAY(msg), L"%s? This cannot be undone.", lbl);
    /* three lines fit; a longer message (plugin warnings) grows the box, up to the view */
    const UINT fl = DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX;
    RECT mr = { bx0 + S(16), 0, bx1 - S(16), 0 };
    SelectObject(m, f_body);
    DrawTextW(m, msg, -1, &mr, fl | DT_CALCRECT);
    int bh = S(176), grow = mr.bottom - mr.top - S(68);
    if (grow > 0) bh += grow;
    if (grow > 0 && bh > vh - S(16)) bh = vh - S(16) > S(176) ? vh - S(16) : S(176);
    int by = t + (vh - bh) / 2;
    if (by < t) by = t;
    rrect(m, bx0, by, bx1, by + bh, S(16), C_ROW);
    int danger = (conf->flags & CF_DANGER) != 0;
    text(m, f_bold, danger ? C_DANGER : C_TEXT, danger ? L"Confirm change" : L"Are you sure?",
         bx0 + S(16), by + S(12), bx1 - S(16), by + S(40), DT_LEFT);
    SetRect(&mr, bx0 + S(16), by + S(44), bx1 - S(16), by + S(112) + bh - S(176));
    SelectObject(m, f_body);
    SetTextColor(m, C_TEXT);
    DrawTextW(m, msg, -1, &mr, fl | DT_END_ELLIPSIS);
    int gy = by + bh - S(52), gh = S(40), mid = (bx0 + bx1) / 2;
    SetRect(&rc_no, bx0 + S(12), gy, mid - S(6), gy + gh);
    SetRect(&rc_yes, mid + S(6), gy, bx1 - S(12), gy + gh);
    COLORREF yc = danger ? C_DANGER : C_ACC;
    if (conf_btn == 0) rrect(m, rc_no.left - S(2), rc_no.top - S(2), rc_no.right + S(2), rc_no.bottom + S(2), S(12), C_ACC);
    else rrect(m, rc_yes.left - S(2), rc_yes.top - S(2), rc_yes.right + S(2), rc_yes.bottom + S(2), S(12), yc);
    rrect(m, rc_no.left, rc_no.top, rc_no.right, rc_no.bottom, S(10), C_SEL);
    rrect(m, rc_yes.left, rc_yes.top, rc_yes.right, rc_yes.bottom, S(10), C_SEL);
    text(m, f_bold, C_TEXT, L"Cancel", rc_no.left, rc_no.top, rc_no.right, rc_no.bottom, DT_CENTER);
    text(m, f_bold, yc, danger ? L"Apply" : L"Turn on", rc_yes.left, rc_yes.top, rc_yes.right, rc_yes.bottom, DT_CENTER);
}

/* tab widths follow their labels, so eight names fit without ellipses */
static void layout_tabs(HDC m)
{
    int w[PG_COUNT], sum = 0, x = 0;
    for (int k = 0; k < ntabs; k++) {
        w[k] = text_w(m, f_tab, tab_names[tabs[k]]);
        sum += w[k];
    }
    int spare = pw - sum;
    for (int k = 0; k < ntabs; k++) {
        int r = spare > 0 ? x + w[k] + spare * (k + 1) / ntabs - spare * k / ntabs : pw * (k + 1) / ntabs;
        tab_l[k] = x;
        tab_r[k] = k == ntabs - 1 ? pw : r;
        x = tab_r[k];
    }
}

static void render(HDC m)
{
    int t = top_y(), vh = view_h();
    easing = 0;
    SetBkMode(m, TRANSPARENT);
    fill(m, 0, 0, pw, ph_h, C_BG);

    for (int i = 0; i < nrows; i++) {
        int y = t + row_y[i] - scroll;
        if (y + row_h[i] < t || y > t + vh) continue;
        draw_row(m, i, y);
    }
    if (!nrows) text(m, f_body, C_DIM, L"Nothing to adjust here", 0, t, pw, t + S(80), DT_CENTER);
    if (content_h > vh && vh > 0) {
        int th = vh * vh / content_h, ty = t + (int)((int64_t)scroll * (vh - th) / (content_h - vh));
        fill(m, pw - S(4), ty, pw - S(1), ty + th, C_FAINT);
    }

    fill(m, 0, 0, pw, S(HDR_H), C_HDR);
    int run = autotdp_running();
    text(m, f_title, run ? C_ACC : C_TEXT, run ? L"Phawx ON" : L"Phawx OFF", S(PAD), S(8), pw - S(56), S(42), DT_LEFT);
    text(m, f_small, C_DIM, live, S(PAD), S(42), pw - S(56), S(64), DT_LEFT);
    draw_close(m);

    fill(m, 0, S(HDR_H), pw, t, C_HDR);
    fill(m, 0, t - 1, pw, t, C_ROW);
    layout_tabs(m);
    for (int k = 0; k < ntabs; k++) {
        int l = tab_l[k], r = tab_r[k], cur_t = tabs[k] == page;
        const wchar_t *nm = tab_names[tabs[k]];
        text(m, f_tab, cur_t ? C_TEXT : C_DIM, nm, l, S(HDR_H), r, t - S(4), DT_CENTER);
        if (cur_t) {
            int half = text_w(m, f_tab, nm) / 2 + S(4), mid = (l + r) / 2;
            if (half > (r - l) / 2) half = (r - l) / 2;
            rrect(m, mid - half, t - S(4), mid + half, t, S(3), C_ACC);
        }
    }

    int fy = ph_h - S(FOOT_H);
    fill(m, 0, fy, pw, ph_h, C_HDR);
    const wchar_t *hint = L"LB / RB pages  \x00B7  B closes";
    ph_ctl *c = cur();
    int pin = c && ph_ctl_pinnable(c), pinned = pin && pin_has(c->key);
    if (conf) hint = L"A confirms  \x00B7  B cancels";
    else if (c && c->type == CT_CURVE && cedit == c) hint = L"Up / Down: fan speed  \x00B7  A or B: done";
    else if (c && c->type == CT_CURVE && hold_progress() < 0)
        hint = resettable(c) ? L"Left / Right: point  \x00B7  A: edit  \x00B7  Hold X: Default" : L"Left / Right: point  \x00B7  A: edit";
    else if (c && locked(c)) hint = L"Locked while AutoTDP runs";
    else if (hold_progress() >= 0) hint = L"Keep holding to restore Default";
    else if (resettable(c) && pin) hint = pinned ? L"Hold X or R: Default  \x00B7  Y unpins" : L"Hold X or R: Default  \x00B7  Y pins";
    else if (resettable(c)) hint = L"Hold X or R to restore Default";
    else if (pin) hint = pinned ? L"Y removes from Quick  \x00B7  LB / RB pages" : L"Y pins to Quick  \x00B7  LB / RB pages";
    text(m, f_small, C_FAINT, hint, S(PAD), fy, pw - S(PAD), ph_h, DT_CENTER);

    if (conf) draw_confirm(m);

    if (toast_on && toast_msg[0]) {
        int tw = text_w(m, f_body, toast_msg) + S(32);
        if (tw > pw - S(24)) tw = pw - S(24);
        int th = S(40), tx = (pw - tw) / 2, ty = fy - th - S(12);
        rrect(m, tx, ty, tx + tw, ty + th, S(14), C_TOAST);
        text(m, f_body, C_TEXT, toast_msg, tx + S(14), ty, tx + tw - S(14), ty + th, DT_CENTER);
    }
}

static void on_paint(void)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hw, &ps);
    make_fonts();
    if (ensure_bb()) {
        render(bb_dc);
        BitBlt(dc, 0, 0, shown_w, ph_h, bb_dc, 0, 0, SRCCOPY);
    }
    EndPaint(hw, &ps);
    if (easing) SetTimer(hw, TM_EASE, 16, NULL);
    else KillTimer(hw, TM_EASE);
}

/* ---------- show / hide ---------- */

static void place(void)
{
    HMONITOR hm = target_monitor();
    MONITORINFO mi = { sizeof mi };
    if (!GetMonitorInfoW(hm, &mi)) SystemParametersInfoW(SPI_GETWORKAREA, 0, &mi.rcWork, 0);
    mon = mi.rcWork;
    dpi = scaled_dpi(hm);
    make_fonts();
    pw = S(UI_W);
    if (pw > mon.right - mon.left) pw = mon.right - mon.left;
    ph_h = mon.bottom - mon.top;
    if (pw < 1) pw = 1;
    if (ph_h < 1) ph_h = 1;
    layout();
    clamp_scroll();
}

static void set_shown(int w)
{
    if (w < 1) w = 1;
    if (w > pw) w = pw;
    shown_w = w;
    SetWindowPos(hw, HWND_TOPMOST, mon.right - w, mon.top, w, ph_h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void anim_step(void)
{
    int target = anim_dir > 0 ? pw : 0;
    int d = target - shown_w;
    int stepv = d * 35 / 100;
    int mn = S(14);
    if (d > 0 && stepv < mn) stepv = mn < d ? mn : d;
    if (d < 0 && stepv > -mn) stepv = -mn > d ? -mn : d;
    int w = shown_w + stepv;
    if ((anim_dir > 0 && w >= pw) || (anim_dir < 0 && w <= 1) || d == 0) {
        KillTimer(hw, TM_ANIM);
        if (anim_dir > 0) {
            set_shown(pw);
        } else {
            ShowWindow(hw, SW_HIDE);
            shown_w = 0;
            free_bb();
        }
        anim_dir = 0;
    } else {
        set_shown(w);
    }
    RedrawWindow(hw, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
}

static void toast_hide(void)
{
    if (toast_hw) { KillTimer(toast_hw, TM_TOAST); ShowWindow(toast_hw, SW_HIDE); }
}

void ui_show(int show)
{
    if (!hw) return;
    if (!on_ui()) { PostMessageW(hw, WM_UI_SHOW, (WPARAM)(show ? 1 : 0), 0); return; }
    if (show) {
        if (vis) return;
        toast_hide();
        page = PG_QUICK;
        sel = -1;
        scroll = 0;
        place();
        rebuild();
        if (sel >= 0) ensure_visible(sel);
        InterlockedExchange(&vis, 1);
        fps_start();
        input_menu_open(1);
        update_live();
        last_tick = 0;
        live_ms = autotdp_running() ? 250 : 500;
        SetTimer(hw, TM_LIVE, (UINT)live_ms, NULL);
        if (!IsWindowVisible(hw)) shown_w = 0;
        anim_dir = 1;
        if (!shown_w) set_shown(1);
        SetTimer(hw, TM_ANIM, 10, NULL);
    } else {
        if (!vis) return;
        commit_pending();
        conf = NULL;
        cedit = NULL;
        drag = DR_NONE;
        if (GetCapture() == hw) ReleaseCapture();
        hold_end();
        KillTimer(hw, TM_LIVE);
        KillTimer(hw, TM_EASE);
        InterlockedExchange(&vis, 0);
        input_menu_open(0);
        if (!autotdp_running()) fps_stop();
        anim_dir = -1;
        SetTimer(hw, TM_ANIM, 10, NULL);
    }
}

/* open the menu on a given page (after "Restart Phawx ON" from the Plugins page) */
void ui_show_page(int pg)
{
    if (!hw || !on_ui() || pg < 0 || pg >= PG_COUNT) return;
    ui_show(1);
    if (!vis || page == pg) return;
    for (int i = 0; i < ntabs; i++)
        if (tabs[i] == pg) {
            page = pg;
            sel = -1;
            scroll = 0;
            rebuild();
            if (sel >= 0) ensure_visible(sel);
            inval();
        }
}

void ui_toggle(void)
{
    if (!hw) return;
    if (!on_ui()) { PostMessageW(hw, WM_UI_SHOW, 2, 0); return; }
    ui_show(!vis);
}

int ui_visible(void) { return vis != 0; }
HWND ui_hwnd(void) { return hw; }

void ui_refresh(void)
{
    if (!hw) return;
    if (!on_ui()) { PostMessageW(hw, WM_UI_REFRESH, 0, 0); return; }
    if (!vis) return;
    if (!anim_dir) {
        RECT old = mon;
        int od = dpi;
        place();
        if (!EqualRect(&old, &mon) || od != dpi) set_shown(pw);
    }
    rebuild();
    update_live();
    inval();
}

/* ---------- toasts ---------- */

static void show_toast_window(void)
{
    if (!toast_hw) return;
    HMONITOR hm = target_monitor();
    MONITORINFO mi = { sizeof mi };
    if (!GetMonitorInfoW(hm, &mi)) return;
    int d = scaled_dpi(hm);
    if (!toast_font || toast_dpi != d) {
        if (toast_font) DeleteObject(toast_font);
        toast_font = mk_font(MulDiv(15, d, 96), FW_NORMAL);
        toast_dpi = d;
    }
    HDC dc = GetDC(toast_hw);
    HGDIOBJ o = SelectObject(dc, toast_font);
    SIZE z = { 0, 0 };
    GetTextExtentPoint32W(dc, toast_msg, lstrlenW(toast_msg), &z);
    SelectObject(dc, o);
    ReleaseDC(toast_hw, dc);
    int mw = mi.rcWork.right - mi.rcWork.left;
    int w = z.cx + MulDiv(40, d, 96), h = MulDiv(44, d, 96);
    if (w > mw - MulDiv(32, d, 96)) w = mw - MulDiv(32, d, 96);
    if (w < h) w = h;
    int x = mi.rcWork.left + (mw - w) / 2, y = mi.rcWork.bottom - h - MulDiv(56, d, 96);
    int rad = MulDiv(16, d, 96);
    SetWindowRgn(toast_hw, CreateRoundRectRgn(0, 0, w + 1, h + 1, rad, rad), FALSE);
    SetWindowPos(toast_hw, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(toast_hw, NULL, TRUE);
    SetTimer(toast_hw, TM_TOAST, 2800, NULL);
}

void ui_toast(const wchar_t *msg)
{
    if (!msg) return;
    if (!hw) { ph_log("toast: %ls", msg); return; }
    if (!on_ui()) {
        int n = lstrlenW(msg) + 1;
        wchar_t *c = ph_alloc((size_t)n * sizeof(wchar_t));
        if (!c) return;
        lstrcpyW(c, msg);
        if (!PostMessageW(hw, WM_UI_TOAST, 0, (LPARAM)c)) ph_free(c);
        return;
    }
    lstrcpynW(toast_msg, msg, PH_ARRAY(toast_msg));
    toast_seq++;
    if (vis) {
        toast_on = 1;
        SetTimer(hw, TM_TOAST, 2500, NULL);
        inval();
    } else {
        show_toast_window();
    }
}

static LRESULT CALLBACK toast_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_POINTERACTIVATE: return PA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONUP: toast_hide(); return 0;
    case WM_TIMER:
        if (w == TM_TOAST) toast_hide();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        SetDCBrushColor(dc, C_TOAST);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(DC_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, C_TEXT);
        HGDIOBJ o = SelectObject(dc, toast_font ? (HGDIOBJ)toast_font : GetStockObject(DEFAULT_GUI_FONT));
        InflateRect(&rc, -MulDiv(12, toast_dpi ? toast_dpi : 96, 96), 0);
        DrawTextW(dc, toast_msg, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, o);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

/* ---------- pointer ---------- */

static int hit_row(int y)
{
    int cy = y - top_y() + scroll;
    for (int i = 0; i < nrows; i++)
        if (cy >= row_y[i] && cy < row_y[i] + row_h[i]) return i;
    return -1;
}

static void slider_to(int i, int x)
{
    ph_ctl *c = rows[i];
    int kx0, kx1;
    track_x(&kx0, &kx1);
    int range = c->max - c->min;
    if (range <= 0 || kx1 <= kx0) return;
    int px = PH_CLAMP(x, kx0, kx1) - kx0;
    int st = c->step > 0 ? c->step : 1;
    int64_t raw = (int64_t)px * range / (kx1 - kx0);
    int32_t v = c->min + (int32_t)((raw + st / 2) / st) * st;
    set_pending(c, v, 1);
}

/* the lower part of a slider row is its track: pressing there drags the knob */
static int in_track(int i, int y)
{
    return rows[i]->type == CT_SLIDER && y >= top_y() + row_y[i] - scroll + S(two_line(i) ? 46 : 32);
}

static int on_pin(int i, int x, int y)
{
    return x < S(PAD) + S(PIN_HIT) && !in_track(i, y) && ph_ctl_pinnable(rows[i]);
}

/* the graph of a curve row; above it is the row's label line */
static int in_graph(int i, int y)
{
    return rows[i]->type == CT_CURVE && y >= top_y() + row_y[i] - scroll + S(40);
}

/* touch and mouse: the point nearest the press follows the finger up and down */
static void curve_drag(int i, int y, int save)
{
    RECT g;
    ph_fan *f = rows[i]->ctx;
    curve_box(top_y() + row_y[i] - scroll, row_h[i], &g);
    if (g.bottom <= g.top) return;
    int pct = (g.bottom - y) * 100 / (g.bottom - g.top);
    pct = (PH_CLAMP(pct, 0, 100) + 2) / 5 * 5;
    fan_curve_point(f, cpt, pct, save);
    inval();
}

static void curve_pick(int i, int x)
{
    RECT g;
    curve_box(top_y() + row_y[i] - scroll, row_h[i], &g);
    int w = g.right - g.left;
    if (w > 0) cpt = PH_CLAMP(((x - g.left) * (FAN_PTS - 1) + w / 2) / w, 0, FAN_PTS - 1);
}

/* a press that moves further than this is no longer a tap or a hold */
static int moved(int x, int y)
{
    int sl = S(10);
    return x - down_x > sl || down_x - x > sl || y - down_y > sl || down_y - y > sl;
}

/* hold_ms: HOLD_MS for the mouse, HOLD_TOUCH_MS for touch and pen */
static void press(int x, int y, int hold)
{
    hold_stop(HOLD_PTR);
    held = 0;
    down_x = x;
    down_y = y;
    down_scroll = scroll;
    down_row = -1;
    drag = DR_TAP;
    if (conf || y < top_y() || y >= ph_h - S(FOOT_H)) return;
    int i = hit_row(y);
    down_row = i;
    if (i < 0 || !selectable(rows[i])) return;
    if (sel != i) commit_pending();
    /* committing the old row can ask for confirmation: this press then only closes over it */
    if (conf) { down_row = -1; drag = DR_NONE; inval(); return; }
    sel = i;
    ph_ctl *c = rows[i];
    if (in_graph(i, y)) {
        drag = DR_CURVE;
        cedit = NULL;
        curve_pick(i, x);
        curve_drag(i, y, 0);
    } else if (on_pin(i, x, y)) {
        /* the pin reacts to a tap; holding it does not reset anything */
    } else if (c->type == CT_SLIDER && !locked(c) && in_track(i, y)) {
        drag = DR_SLIDER;
        KillTimer(hw, TM_COMMIT);
        slider_to(i, x);
    } else if (holdable(c)) {
        hold_start(HOLD_PTR, hold);
    }
    inval();
}

static void move(int x, int y)
{
    if (drag == DR_SLIDER && sel >= 0 && sel < nrows) { slider_to(sel, x); return; }
    if (drag == DR_CURVE && sel >= 0 && sel < nrows) { curve_drag(sel, y, 0); return; }
    /* sliding sideways cancels the tap and the hold; up or down scrolls the list */
    if ((drag == DR_TAP || drag == DR_CANCEL) && moved(x, y)) {
        hold_stop(HOLD_PTR);
        drag = down_row >= 0 && (y - down_y > S(10) || down_y - y > S(10)) ? DR_SCROLL : DR_CANCEL;
    }
    if (drag == DR_SCROLL) {
        scroll = down_scroll - (y - down_y);
        clamp_scroll();
        inval();
    }
}

static int in_rc(const RECT *r, int x, int y) { return x >= r->left && x < r->right && y >= r->top && y < r->bottom; }

static void tap(int x, int y)
{
    if (conf) {
        if (in_rc(&rc_yes, x, y)) confirm_close(1);
        else if (in_rc(&rc_no, x, y)) confirm_close(0);
        return;
    }
    if (y < S(HDR_H)) {
        if (in_rc(&rc_close, x, y)) ui_show(0);
        return;
    }
    if (y < top_y()) {
        for (int k = 0; k < ntabs; k++)
            if (x >= tab_l[k] && x < tab_r[k]) { goto_tab(k); break; }
        return;
    }
    if (y >= ph_h - S(FOOT_H)) return;
    int i = hit_row(y);
    if (i < 0 || i != down_row || i != sel) return;
    ph_ctl *c = rows[i];
    if (on_pin(i, x, y)) { pin_sel(); return; }
    if (locked(c)) { ui_toast(L"Managed by AutoTDP"); return; }
    if (c->type == CT_CHOICE) {
        int aw = S(24);
        if (x >= row_vx[i] - S(8) && x < row_vx[i] + aw) {
            int32_t v = base_val(i) - 1;
            if (v < c->min) v = c->max;
            apply_val(c, v, 0);
        } else {
            activate();
        }
    } else if (c->type == CT_TOGGLE || c->type == CT_ACTION) {
        activate();
    }
}

static void release(int x, int y)
{
    hold_stop(HOLD_PTR);
    int d = drag;
    drag = DR_NONE;
    if (d == DR_SLIDER) commit_pending();
    else if (d == DR_CURVE && sel >= 0 && sel < nrows) curve_drag(sel, y, 1);
    else if (d == DR_TAP && !held && !moved(x, y)) tap(down_x, down_y);   /* acts on what was pressed */
    inval();
}

/* a drag that ended without its release: keep the curve as it is now */
static void curve_save(void)
{
    ph_ctl *c = cur();
    if (c && c->type == CT_CURVE) {
        ph_fan *f = c->ctx;
        fan_curve_point(f, cpt, f->pt[cpt], 1);
    }
}

static void pt_client(LPARAM l, int *x, int *y)
{
    POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
    ScreenToClient(hw, &p);
    *x = p.x;
    *y = p.y;
}

static void key(WPARAM vk, LPARAM l)
{
    int shift = GetKeyState(VK_SHIFT) < 0;
    switch (vk) {
    case 'R': if (!(l & (1 << 30))) ui_hold(HOLD_KEY, 1); break;   /* bit 30: autorepeat */
    case VK_UP: ui_nav(IN_UP); break;
    case VK_DOWN: ui_nav(IN_DOWN); break;
    case VK_LEFT: ui_nav(IN_LEFT); break;
    case VK_RIGHT: ui_nav(IN_RIGHT); break;
    case VK_RETURN: case VK_SPACE: ui_nav(IN_OK); break;
    case VK_ESCAPE: ui_nav(IN_BACK); break;
    case VK_TAB: ui_nav(shift ? IN_TAB_PREV : IN_TAB_NEXT); break;
    case VK_PRIOR: ui_nav(IN_TAB_PREV); break;
    case VK_NEXT: ui_nav(IN_TAB_NEXT); break;
    case VK_DELETE: case VK_BACK: ui_nav(IN_RESET); break;
    case VK_F2: ui_nav(IN_AUX); break;
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    int x, y;
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_POINTERACTIVATE: return PA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: on_paint(); return 0;
    case WM_DPICHANGED: return 0;
    case WM_LBUTTONDOWN:
        SetCapture(h);
        press(GET_X_LPARAM(l), GET_Y_LPARAM(l), HOLD_MS);
        return 0;
    case WM_MOUSEMOVE:
        if (drag != DR_NONE && (w & MK_LBUTTON)) move(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        return 0;
    case WM_LBUTTONUP:
        if (drag != DR_NONE) release(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (GetCapture() == h) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        if ((HWND)l != h && drag != DR_NONE) {
            if (drag == DR_SLIDER) commit_pending();
            if (drag == DR_CURVE) curve_save();
            drag = DR_NONE;
            hold_stop(HOLD_PTR);
        }
        return 0;
    case WM_POINTERDOWN: {
        if (!IS_POINTER_PRIMARY_WPARAM(w)) return 0;
        POINTER_INPUT_TYPE pt = PT_TOUCH;
        GetPointerType(GET_POINTERID_WPARAM(w), &pt);
        pt_client(l, &x, &y);
        press(x, y, pt == PT_MOUSE || pt == PT_TOUCHPAD ? HOLD_MS : HOLD_TOUCH_MS);
        return 0;
    }
    case WM_POINTERUPDATE:
        if (!IS_POINTER_PRIMARY_WPARAM(w)) return 0;
        if (IS_POINTER_INCONTACT_WPARAM(w) && drag != DR_NONE) {
            pt_client(l, &x, &y);
            move(x, y);
        }
        return 0;
    case WM_POINTERUP:
        if (!IS_POINTER_PRIMARY_WPARAM(w)) return 0;
        if (drag != DR_NONE) {
            pt_client(l, &x, &y);
            release(x, y);
        }
        return 0;
    case WM_POINTERCAPTURECHANGED:
        if (drag == DR_SLIDER) commit_pending();
        if (drag == DR_CURVE) curve_save();
        drag = DR_NONE;
        hold_stop(HOLD_PTR);
        return 0;
    case WM_TABLET_QUERYSYSTEMGESTURESTATUS:   /* no press-and-hold right click, no flicks */
        return TABLET_DISABLE_PRESSANDHOLD | TABLET_DISABLE_PENTAPFEEDBACK | TABLET_DISABLE_PENBARRELFEEDBACK |
               TABLET_DISABLE_FLICKS;
    case WM_MOUSEWHEEL:
        if (vis && !conf) {
            scroll -= GET_WHEEL_DELTA_WPARAM(w) * S(60) / WHEEL_DELTA;
            clamp_scroll();
            inval();
        }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (m == WM_KEYDOWN || w == VK_F10) { key(w, l); return 0; }
        break;
    case WM_KEYUP:
        if (w == 'R') ui_hold(HOLD_KEY, 0);
        break;
    case WM_TIMER:
        switch (w) {
        case TM_ANIM: anim_step(); break;
        case TM_COMMIT: commit_pending(); break;
        case TM_TOAST: KillTimer(h, TM_TOAST); toast_on = 0; inval(); break;
        case TM_HOLD: hold_tick(); break;
        case TM_LIVE: {
            if (!vis) { KillTimer(h, TM_LIVE); break; }
            /* faster while AutoTDP runs, so its sliders follow it closely */
            int want = autotdp_running() ? 250 : 500;
            if (want != live_ms) { live_ms = want; SetTimer(h, TM_LIVE, (UINT)want, NULL); }
            update_live();
            uint64_t now = ph_ms();
            if (!last_tick || now - last_tick >= 950) {
                last_tick = now;
                ph_backends_tick();
                refresh_hw(1);
            }
            inval();
            break;
        }
        case TM_EASE:
            if (!vis) { KillTimer(h, TM_EASE); break; }
            inval();
            break;
        }
        return 0;
    case WM_UI_SHOW:
        if (w == 2) ui_toggle(); else ui_show((int)w);
        return 0;
    case WM_UI_NAV: ui_nav((int)w); return 0;
    case WM_UI_HOLD: ui_hold((int)w, (int)l); return 0;
    case WM_UI_REFRESH: ui_refresh(); return 0;
    case WM_UI_TOAST:
        if (l) { ui_toast((const wchar_t *)l); ph_free((void *)l); }
        return 0;
    case WM_DISPLAYCHANGE: ui_refresh(); return 0;
    case WM_CLOSE: ui_show(0); return 0;
    case WM_DESTROY:
        KillTimer(h, TM_ANIM);
        KillTimer(h, TM_LIVE);
        KillTimer(h, TM_COMMIT);
        KillTimer(h, TM_TOAST);
        KillTimer(h, TM_HOLD);
        free_bb();
        del_fonts();
        if (toast_hw) { DestroyWindow(toast_hw); toast_hw = NULL; }
        if (toast_font) { DeleteObject(toast_font); toast_font = NULL; }
        InterlockedExchange(&vis, 0);
        hw = NULL;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void ui_init(HINSTANCE hi)
{
    ui_tid = GetCurrentThreadId();
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"PhawxON.panel";
    RegisterClassW(&wc);
    wc.lpfnWndProc = toast_proc;
    wc.lpszClassName = L"PhawxON.toast";
    RegisterClassW(&wc);
    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    hw = CreateWindowExW(ex, L"PhawxON.panel", PH_APPNAME_W, WS_POPUP, 0, 0, 1, 1, NULL, NULL, hi, NULL);
    /* a 2 s touch or pen hold resets a row; Windows' own hold ring and right tap would get in the way */
    typedef BOOL (WINAPI *wfs_fn)(HWND, FEEDBACK_TYPE, DWORD, UINT32, const VOID *);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    wfs_fn wfs = u32 ? (wfs_fn)(void *)GetProcAddress(u32, "SetWindowFeedbackSetting") : NULL;
    if (hw && wfs) {
        static const FEEDBACK_TYPE fb[] = { FEEDBACK_TOUCH_PRESSANDHOLD, FEEDBACK_TOUCH_RIGHTTAP, FEEDBACK_PEN_PRESSANDHOLD,
                                            FEEDBACK_PEN_RIGHTTAP, FEEDBACK_PEN_BARRELVISUALIZATION };
        BOOL off = FALSE;
        for (int i = 0; i < PH_ARRAY(fb); i++) wfs(hw, fb[i], 0, sizeof off, &off);
    }
    toast_hw = CreateWindowExW(ex, L"PhawxON.toast", PH_APPNAME_W, WS_POPUP, 0, 0, 1, 1, NULL, NULL, hi, NULL);
    lstrcpynW(live, L"", PH_ARRAY(live));
}
