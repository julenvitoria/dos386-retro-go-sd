/*
 * x86_mouse.c -- INT 33h mouse driver. Contract, scope and what is NOT done:
 * x86_mouse.h. Ported from the gw-dos (XTulator) frontend's mouse_hle.c.
 *
 * COLD PATH: reached only through the HLE stub (a guest's INT 33h) and from the
 * per-frame input code, a few calls per frame at most.
 */
#include <stdint.h>
#include <string.h>

#include "cutils.h"
#include "x86_cpu.h"
#include "x86_mouse.h"

uint8_t x86_mouse_enabled;
volatile uint32_t x86_mouse_calls;
volatile uint32_t x86_mouse_resets;

static int16_t mx, my, xmin, xmax, ymin, ymax;
static int16_t shown = -1;
static uint8_t buttons, in_use;
static int16_t mick_x, mick_y;
static uint16_t press_count[2], release_count[2];
static int16_t press_x[2], press_y[2], release_x[2], release_y[2];
static int16_t frac_y;                 /* y scaling remainder, so slow moves move */

#define GR(n) (cpu_state_ptr->regs[n])
#define AX ((uint16_t)GR(0))
#define CX ((uint16_t)GR(1))
#define DX ((uint16_t)GR(2))
#define BX ((uint16_t)GR(3))

/* screen lines of the current BIOS mode, from the BDA */
static int vheight(void)
{
    const uint8_t *m = (const uint8_t *)x86_guest_ptr(0x449, 1);
    uint8_t mode = m ? (uint8_t)(*m & 0x7F) : 3;
    if (mode == 0x0F || mode == 0x10) return 350;
    if (mode == 0x11 || mode == 0x12) return 480;
    return 200;
}

static int16_t clampi(int v, int lo, int hi) { return (int16_t)(v < lo ? lo : (v > hi ? hi : v)); }

static void defaults(void)
{
    int h = vheight();
    xmin = 0; xmax = 639; ymin = 0; ymax = (int16_t)(h - 1);
    mx = 320; my = (int16_t)(h / 2);
    shown = -1; mick_x = mick_y = 0; frac_y = 0;
    press_count[0] = press_count[1] = release_count[0] = release_count[1] = 0;
}

void x86_mouse_reset_state(void)
{
    defaults();
    buttons = 0;
    in_use = 0;
}

int x86_mouse_in_use(void) { return x86_mouse_enabled && in_use; }

void x86_mouse_move(int dx, int dy)
{
    int h = vheight(), sy;
    /* y arrives in 200-line units; scale to the mode, carrying the remainder so
       a one-unit tap in a 350/480-line mode still moves */
    frac_y = (int16_t)(frac_y + dy * h);
    sy = frac_y / 200;
    frac_y = (int16_t)(frac_y - sy * 200);
    /* mickeys: 8 per 8 pixels horizontally, 16 per 8 vertically (driver
       defaults), counted on the 640-wide virtual grid */
    mick_x = (int16_t)(mick_x + dx);
    mick_y = (int16_t)(mick_y + sy * 2);
    mx = clampi(mx + dx, xmin, xmax);
    my = clampi(my + sy, ymin, ymax);
}

void x86_mouse_buttons(unsigned b)
{
    for (unsigned i = 0; i < 2; i++) {
        unsigned m = 1u << i;
        if ((b & m) && !(buttons & m)) { press_count[i]++; press_x[i] = mx; press_y[i] = my; }
        if (!(b & m) && (buttons & m)) { release_count[i]++; release_x[i] = mx; release_y[i] = my; }
    }
    buttons = (uint8_t)(b & 3u);
}

void x86_mouse_button(unsigned bit, int down)
{
    unsigned b = buttons;
    if (bit > 1)
        return;
    if (down) b |= 1u << bit; else b &= ~(1u << bit);
    x86_mouse_buttons(b);
}

int x86_mouse_cursor(int *x, int *y)
{
    *x = mx / 2;
    *y = my * 200 / vheight();
    return x86_mouse_in_use() && shown >= 0;
}

int x86_mouse_int33(void)
{
    unsigned b;
    if (!x86_mouse_enabled)
        return 1;              /* the bare-IRET behaviour: registers untouched */
    x86_mouse_calls++;
    switch (AX) {
    case 0x00:                 /* reset + detect */
    case 0x21:                 /* software reset */
        defaults();
        in_use = 1;
        x86_mouse_resets++;
        x86_set_ax(0xFFFF);
        x86_set_bx(2);
        break;
    case 0x01: if (shown < 0) shown++; break;
    case 0x02: shown--; break;
    case 0x03:
        x86_set_bx(buttons); x86_set_cx((uint16_t)mx); x86_set_dx((uint16_t)my);
        break;
    case 0x04:
        mx = clampi((int16_t)CX, xmin, xmax); my = clampi((int16_t)DX, ymin, ymax);
        break;
    case 0x05:
        b = BX < 2 ? BX : 0;
        x86_set_ax(buttons); x86_set_bx(press_count[b]);
        x86_set_cx((uint16_t)press_x[b]); x86_set_dx((uint16_t)press_y[b]);
        press_count[b] = 0;
        break;
    case 0x06:
        b = BX < 2 ? BX : 0;
        x86_set_ax(buttons); x86_set_bx(release_count[b]);
        x86_set_cx((uint16_t)release_x[b]); x86_set_dx((uint16_t)release_y[b]);
        release_count[b] = 0;
        break;
    case 0x07: {
        int a = (int16_t)CX, c = (int16_t)DX;
        xmin = (int16_t)(a < c ? a : c); xmax = (int16_t)(a < c ? c : a);
        mx = clampi(mx, xmin, xmax);
        break;
    }
    case 0x08: {
        int a = (int16_t)CX, c = (int16_t)DX;
        ymin = (int16_t)(a < c ? a : c); ymax = (int16_t)(a < c ? c : a);
        my = clampi(my, ymin, ymax);
        break;
    }
    case 0x0B:                 /* motion counters since the last call */
        x86_set_cx((uint16_t)mick_x); x86_set_dx((uint16_t)mick_y);
        mick_x = mick_y = 0;
        break;
    case 0x15: x86_set_bx(16); break;                  /* state buffer size  */
    case 0x1B: x86_set_bx(50); x86_set_cx(50); x86_set_dx(50); break;
    case 0x24: x86_set_bx(0x0626); x86_set_cx(0x0400); break;  /* v6.26, PS/2 */
    default:
        /* cursor shapes (09h/0Ah), handlers (0Ch/14h/18h), ratios (0Fh/13h/
           1Ah), light pen: accepted, nothing to report. x86_mouse.h says why
           handlers are never called. */
        break;
    }
    return 1;
}
