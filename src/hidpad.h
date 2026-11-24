#ifndef PH_HIDPAD_H
#define PH_HIDPAD_H
/* Input reports of the gamepads and handheld buttons that are not XInput, as
   input.c reads them over HID. Plain C without Windows headers, so
   tests/hidpad_test.c checks the layouts off Windows. docs/FEATURES.md lists the
   button mapping.

   The result uses XInput's button bits (XINPUT_GAMEPAD_*) plus Guide and the two
   triggers, the set input.c works with, and sticks in XInput's range and sense
   (-32768..32767, up and right positive). */
#include <stdint.h>

#define HP_UP     0x0001u
#define HP_DOWN   0x0002u
#define HP_LEFT   0x0004u
#define HP_RIGHT  0x0008u
#define HP_START  0x0010u   /* Options */
#define HP_BACK   0x0020u   /* Share / Create */
#define HP_LS     0x0040u
#define HP_RS     0x0080u
#define HP_LB     0x0100u
#define HP_RB     0x0200u
#define HP_GUIDE  0x0400u   /* PS button; ROG Ally: Armoury Crate and Command Center */
#define HP_A      0x1000u   /* Cross */
#define HP_B      0x2000u   /* Circle */
#define HP_X      0x4000u   /* Square */
#define HP_Y      0x8000u   /* Triangle */
#define HP_LT     0x10000u
#define HP_RT     0x20000u
#define HP_TRIGGER 100      /* an analog trigger past this (of 255) counts as pressed, as for XInput */

enum { HP_NONE, HP_DS4, HP_DS5, HP_ALLY };

typedef struct hp_state {
    uint32_t btn;
    int32_t  lx, ly;
} hp_state;

/* what a VID/PID is; usage_page and usage name the HID top-level collection to open (usage 0 = any) */
typedef struct hp_dev { uint16_t vid, pid; uint8_t kind; uint16_t usage_page, usage; } hp_dev;

static const hp_dev hp_devs[] = {
    { 0x054C, 0x05C4, HP_DS4, 0x01, 0x05 },     /* DualShock 4 */
    { 0x054C, 0x09CC, HP_DS4, 0x01, 0x05 },     /* DualShock 4 (second version) */
    { 0x054C, 0x0BA0, HP_DS4, 0x01, 0x05 },     /* DualShock 4 USB wireless adapter */
    { 0x054C, 0x0CE6, HP_DS5, 0x01, 0x05 },     /* DualSense */
    { 0x054C, 0x0DF2, HP_DS5, 0x01, 0x05 },     /* DualSense Edge */
    { 0x0B05, 0x1ABE, HP_ALLY, 0xFF31, 0 },     /* ROG Ally: the keyboard-like vendor collection */
    { 0x0B05, 0x1B4C, HP_ALLY, 0xFF31, 0 },     /* ROG Ally X */
};

static inline const hp_dev *hp_match(uint16_t vid, uint16_t pid)
{
    for (unsigned i = 0; i < sizeof hp_devs / sizeof hp_devs[0]; i++)
        if (hp_devs[i].vid == vid && hp_devs[i].pid == pid) return &hp_devs[i];
    return 0;
}

/* a PlayStation hat: 0 = up, clockwise to 7 = up-left, 8 and above = centred */
static inline uint32_t hp_hat(unsigned h)
{
    static const uint8_t m[8] = { 1, 1 | 8, 8, 2 | 8, 2, 2 | 4, 4, 1 | 4 };
    return h < 8 ? m[h] : 0;
}

static inline int32_t hp_axis(uint8_t v, int flip)
{
    int32_t a = ((int32_t)v - 128) * 256;
    if (flip) a = -a;
    return a > 32767 ? 32767 : a;
}

/* b0..b2 are the three button bytes shared by DualShock 4 and DualSense */
static inline uint32_t hp_ps_buttons(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t l2, uint8_t r2)
{
    uint32_t b = hp_hat(b0 & 0x0F);
    if (b0 & 0x10) b |= HP_X;
    if (b0 & 0x20) b |= HP_A;
    if (b0 & 0x40) b |= HP_B;
    if (b0 & 0x80) b |= HP_Y;
    if (b1 & 0x01) b |= HP_LB;
    if (b1 & 0x02) b |= HP_RB;
    if (b1 & 0x10) b |= HP_BACK;
    if (b1 & 0x20) b |= HP_START;
    if (b1 & 0x40) b |= HP_LS;
    if (b1 & 0x80) b |= HP_RS;
    if (b2 & 0x01) b |= HP_GUIDE;
    if (l2 > HP_TRIGGER) b |= HP_LT;
    if (r2 > HP_TRIGGER) b |= HP_RT;
    return b;
}

/* One input report (r[0] is the report id) of a DualShock 4 or DualSense.
   Layouts, from the start of the state after the report's header:
     A: LX LY RX RY  B0 B1 B2  L2 R2          DualShock 4 (USB 0x01, Bluetooth 0x11 at +2,
                                              Bluetooth 0x01), DualSense Bluetooth 0x01
     B: LX LY RX RY  L2 R2 counter  B0 B1 B2  DualSense USB 0x01, Bluetooth 0x31 at +1
   Bluetooth pads send the short 0x01 report until a feature report switches them
   to the full one (Steam does). bt: the pad is on Bluetooth (its input reports are
   not 64 bytes long). Returns 1 and fills *s for a state report, 0 otherwise. */
static inline int hp_ps_parse(int kind, int bt, const uint8_t *r, int n, hp_state *s)
{
    const uint8_t *p;
    int lay_b;
    if (n < 1) return 0;
    if (kind == HP_DS4) {
        if (r[0] == 0x01) p = r + 1;
        else if (r[0] == 0x11 && bt) p = r + 3;
        else return 0;
        lay_b = 0;
    } else if (kind == HP_DS5) {
        if (r[0] == 0x01) { p = r + 1; lay_b = !bt; }
        else if (r[0] == 0x31 && bt) { p = r + 2; lay_b = 1; }
        else return 0;
    } else return 0;
    if ((int)(p - r) + (lay_b ? 10 : 9) > n) return 0;
    if (lay_b) s->btn = hp_ps_buttons(p[7], p[8], p[9], p[4], p[5]);
    else s->btn = hp_ps_buttons(p[4], p[5], p[6], p[7], p[8]);
    s->lx = hp_axis(p[0], 0);
    s->ly = hp_axis(p[1], 1);
    return 1;
}

/* ROG Ally: report 0x5A carries the code of a key the firmware reports as pressed
   (0 when released), as Linux hid-asus maps them: 0x38 Armoury Crate (ROG) key,
   0xA6 Command Center. Long presses (0xA7, 0xA8) and the back buttons are left
   alone. Returns the buttons the report presses; they only last until the next
   poll, because a press and its release can come between two polls. */
static inline uint32_t hp_ally_parse(const uint8_t *r, int n)
{
    if (n < 2 || r[0] != 0x5A) return 0;
    switch (r[1]) {
    case 0x38: case 0xA6: return HP_GUIDE;
    default: return 0;
    }
}

#endif
