/* The HID gamepad report layouts (src/hidpad.h), checked with hand-made reports.
   Plain C: make test builds it with the host compiler and runs it. */

#include "../src/hidpad.h"
#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* a DualShock 4 style state at p: sticks, then buttons b0..b2, then the triggers */
static void put_a(uint8_t *p, uint8_t lx, uint8_t ly, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t l2, uint8_t r2)
{
    p[0] = lx; p[1] = ly; p[2] = 128; p[3] = 128;
    p[4] = b0; p[5] = b1; p[6] = b2; p[7] = l2; p[8] = r2;
}

/* a DualSense full state at p: sticks, triggers, counter, then buttons */
static void put_b(uint8_t *p, uint8_t lx, uint8_t ly, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t l2, uint8_t r2)
{
    p[0] = lx; p[1] = ly; p[2] = 128; p[3] = 128;
    p[4] = l2; p[5] = r2; p[6] = 0x5A;
    p[7] = b0; p[8] = b1; p[9] = b2;
}

static void ds4(void)
{
    uint8_t r[64];
    hp_state s;
    memset(r, 0, sizeof r);
    r[0] = 0x01;
    put_a(r + 1, 128, 128, 0x08 | 0x20, 0, 0, 0, 0);          /* hat centred, Cross */
    CHECK(hp_ps_parse(HP_DS4, 0, r, 64, &s));
    CHECK(s.btn == HP_A);
    CHECK(s.lx == 0 && s.ly == 0);

    put_a(r + 1, 255, 0, 0x02 | 0x80, 0x01 | 0x20, 0x01, 255, 50);   /* right, Triangle, L1, Options, PS, L2 */
    CHECK(hp_ps_parse(HP_DS4, 0, r, 64, &s));
    CHECK(s.btn == (HP_RIGHT | HP_Y | HP_LB | HP_START | HP_GUIDE | HP_LT));
    CHECK(s.lx > 32000 && s.ly > 32000);                       /* right and up */

    /* Bluetooth full report 0x11: two header bytes more */
    uint8_t bt[78];
    memset(bt, 0, sizeof bt);
    bt[0] = 0x11;
    put_a(bt + 3, 0, 255, 0x06 | 0x40, 0x10 | 0x80, 0, 0, 200);      /* left, Circle, Share, R3, R2 */
    CHECK(hp_ps_parse(HP_DS4, 1, bt, 78, &s));
    CHECK(s.btn == (HP_LEFT | HP_B | HP_BACK | HP_RS | HP_RT));
    CHECK(s.lx == -32768 && s.ly == -32512);                   /* left and down */

    /* the short Bluetooth report has the USB layout */
    memset(bt, 0, sizeof bt);
    bt[0] = 0x01;
    put_a(bt + 1, 128, 128, 0x08 | 0x10, 0x02, 0, 0, 0);       /* Square, R1 */
    CHECK(hp_ps_parse(HP_DS4, 1, bt, 78, &s));
    CHECK(s.btn == (HP_X | HP_RB));

    /* not a state report */
    r[0] = 0x05;
    CHECK(!hp_ps_parse(HP_DS4, 0, r, 64, &s));
    bt[0] = 0x11;
    CHECK(!hp_ps_parse(HP_DS4, 1, bt, 5, &s));                 /* too short */
}

static void ds5(void)
{
    uint8_t r[64];
    hp_state s;
    memset(r, 0, sizeof r);
    r[0] = 0x01;
    put_b(r + 1, 128, 128, 0x00 | 0x20, 0x10, 0x01, 0, 0);     /* up, Cross, Create, PS */
    CHECK(hp_ps_parse(HP_DS5, 0, r, 64, &s));
    CHECK(s.btn == (HP_UP | HP_A | HP_BACK | HP_GUIDE));

    put_b(r + 1, 128, 128, 0x08, 0x40, 0x02 | 0x04, 101, 100); /* L3; touchpad and mute map to nothing */
    CHECK(hp_ps_parse(HP_DS5, 0, r, 64, &s));
    CHECK(s.btn == (HP_LS | HP_LT));

    /* Bluetooth 0x31: one header byte more */
    uint8_t bt[78];
    memset(bt, 0, sizeof bt);
    bt[0] = 0x31;
    bt[1] = 0x10;
    put_b(bt + 2, 128, 128, 0x05 | 0x80, 0x02 | 0x20, 0, 0, 0);    /* down-left, Triangle, R1, Options */
    CHECK(hp_ps_parse(HP_DS5, 1, bt, 78, &s));
    CHECK(s.btn == (HP_DOWN | HP_LEFT | HP_Y | HP_RB | HP_START));

    /* the short Bluetooth 0x01 report has the DualShock 4 layout */
    memset(bt, 0, sizeof bt);
    bt[0] = 0x01;
    put_a(bt + 1, 128, 128, 0x08 | 0x40, 0, 0x01, 0, 0);       /* Circle, PS */
    CHECK(hp_ps_parse(HP_DS5, 1, bt, 78, &s));
    CHECK(s.btn == (HP_B | HP_GUIDE));

    /* 0x31 over USB does not happen: ignored */
    r[0] = 0x31;
    CHECK(!hp_ps_parse(HP_DS5, 0, r, 64, &s));
}

static void hat(void)
{
    CHECK(hp_hat(0) == HP_UP);
    CHECK(hp_hat(1) == (HP_UP | HP_RIGHT));
    CHECK(hp_hat(2) == HP_RIGHT);
    CHECK(hp_hat(3) == (HP_DOWN | HP_RIGHT));
    CHECK(hp_hat(4) == HP_DOWN);
    CHECK(hp_hat(6) == HP_LEFT);
    CHECK(hp_hat(7) == (HP_UP | HP_LEFT));
    CHECK(hp_hat(8) == 0 && hp_hat(15) == 0);
}

static void ally(void)
{
    uint8_t r[64];
    memset(r, 0, sizeof r);
    r[0] = 0x5A;
    r[1] = 0x38;
    CHECK(hp_ally_parse(r, 64) == HP_GUIDE);
    r[1] = 0xA6;
    CHECK(hp_ally_parse(r, 64) == HP_GUIDE);
    r[1] = 0xA7;
    CHECK(hp_ally_parse(r, 64) == 0);
    r[1] = 0;
    CHECK(hp_ally_parse(r, 64) == 0);
    r[0] = 0x5D;
    r[1] = 0x38;
    CHECK(hp_ally_parse(r, 64) == 0);
}

static void match(void)
{
    CHECK(hp_match(0x054C, 0x0CE6) && hp_match(0x054C, 0x0CE6)->kind == HP_DS5);
    CHECK(hp_match(0x054C, 0x09CC) && hp_match(0x054C, 0x09CC)->kind == HP_DS4);
    CHECK(hp_match(0x0B05, 0x1ABE) && hp_match(0x0B05, 0x1ABE)->usage_page == 0xFF31);
    CHECK(!hp_match(0x045E, 0x028E));
}

int main(void)
{
    ds4();
    ds5();
    hat();
    ally();
    match();
    if (fails) { printf("hidpad: %d failed\n", fails); return 1; }
    printf("hidpad: all passed\n");
    return 0;
}
