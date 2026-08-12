/*
 * BIOS services: INT 10h video, INT 16h keyboard, INT 1Ah time.
 *
 * These read and write the same BIOS Data Area a real machine keeps at
 * 0040:0000, because DOS programs read it directly as often as they call the
 * interrupt -- the tick count at 0040:006C and the keyboard buffer at
 * 0040:001E in particular. Servicing the call without maintaining the BDA
 * would work until the first program that peeks instead of calls.
 */
#include <stdint.h>
#include <string.h>

#include "cutils.h"
#include "x86_cpu.h"

#define BDA          0x400u
#define BDA_KB_HEAD  (BDA + 0x1A)
#define BDA_KB_TAIL  (BDA + 0x1C)
#define BDA_KB_BUF   (BDA + 0x1E)   /* 16 word entries */
#define BDA_KB_END   (BDA + 0x3E)
#define BDA_MODE     (BDA + 0x49)
#define BDA_COLS     (BDA + 0x4A)
#define BDA_TICKS    (BDA + 0x6C)

static uint8_t bda_r8(uint32_t lin)
{
    uint8_t *p = x86_guest_ptr(lin, 1);
    return p ? *p : 0;
}
static void bda_w8(uint32_t lin, uint8_t v)
{
    uint8_t *p = x86_guest_ptr(lin, 1);
    if (p) *p = v;
}
static uint16_t bda_r16(uint32_t lin)
{
    uint8_t *p = x86_guest_ptr(lin, 2);
    return p ? (uint16_t)(p[0] | (p[1] << 8)) : 0;
}
static void bda_w16(uint32_t lin, uint16_t v)
{
    uint8_t *p = x86_guest_ptr(lin, 2);
    if (p) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
}
static uint32_t bda_r32(uint32_t lin)
{
    uint8_t *p = x86_guest_ptr(lin, 4);
    return p ? (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)) : 0;
}
static void bda_w32(uint32_t lin, uint32_t v)
{
    uint8_t *p = x86_guest_ptr(lin, 4);
    if (p) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
             p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
}

/* --------------------------------------------------------------------------
 * Keyboard buffer, shared by the INT 09h handler (producer) and INT 16h
 * (consumer). It is the real 16-entry ring at 0040:001E so a program reading
 * the BDA directly sees the same thing INT 16h reports.
 */
void x86_kb_push(uint8_t scancode, uint8_t ascii)
{
    uint16_t tail = bda_r16(BDA_KB_TAIL);
    uint16_t next = (uint16_t)(tail + 2);
    if (next >= (BDA_KB_END - BDA))
        next = (uint16_t)(BDA_KB_BUF - BDA);
    if (next == bda_r16(BDA_KB_HEAD))
        return;                      /* full: a real BIOS beeps and drops */
    bda_w16(BDA + tail, (uint16_t)(ascii | (scancode << 8)));
    bda_w16(BDA_KB_TAIL, next);
}

static int kb_peek(uint16_t *out)
{
    uint16_t head = bda_r16(BDA_KB_HEAD);
    if (head == bda_r16(BDA_KB_TAIL))
        return 0;
    *out = bda_r16(BDA + head);
    return 1;
}

static int kb_pop(uint16_t *out)
{
    uint16_t head = bda_r16(BDA_KB_HEAD);
    if (head == bda_r16(BDA_KB_TAIL))
        return 0;
    *out = bda_r16(BDA + head);
    head = (uint16_t)(head + 2);
    if (head >= (BDA_KB_END - BDA))
        head = (uint16_t)(BDA_KB_BUF - BDA);
    bda_w16(BDA_KB_HEAD, head);
    return 1;
}

void x86_bios_init(void)
{
    /* The BDA fields we maintain. Mode 3 is what a PC boots into. */
    bda_w16(BDA_KB_HEAD, BDA_KB_BUF - BDA);
    bda_w16(BDA_KB_TAIL, BDA_KB_BUF - BDA);
    bda_w8(BDA_MODE, 0x03);
    bda_w16(BDA_COLS, 80);
    bda_w32(BDA_TICKS, 0);
}

/* 18.2 Hz, driven by the frame loop. */
void x86_bios_tick(void)
{
    bda_w32(BDA_TICKS, bda_r32(BDA_TICKS) + 1u);
}

/* --------------------------------------------------------------------------
 * INT 10h
 */
int x86_bios_video(uint8_t func)
{
    switch (func) {
    case 0x00: {                    /* set mode */
        uint8_t mode = (uint8_t)(cpu_state_ptr->regs[0] & 0xFF);
        bda_w8(BDA_MODE, mode);
        x86_video_set_mode(mode);
        return 1;
    }
    case 0x0F:                      /* get mode */
        x86_set_al(bda_r8(BDA_MODE));
        x86_set_ah((uint8_t)bda_r16(BDA_COLS));
        return 1;
    case 0x02:                      /* set cursor position */
        x86_video_set_cursor((uint8_t)(cpu_state_ptr->regs[2] & 0xFF),
                             (uint8_t)((cpu_state_ptr->regs[2] >> 8) & 0xFF));
        return 1;
    case 0x03:                      /* get cursor position */
        x86_set_cx(0x0607);
        x86_set_dx(x86_video_get_cursor());
        return 1;
    case 0x06:                      /* scroll up */
        x86_video_scroll((uint8_t)((cpu_state_ptr->regs[0]) & 0xFF),
                         (uint8_t)((cpu_state_ptr->regs[3] >> 8) & 0xFF));
        return 1;
    case 0x09:                      /* write char + attribute at cursor */
    case 0x0A:
        x86_video_putc_attr((uint8_t)(cpu_state_ptr->regs[0] & 0xFF),
                            (uint8_t)(cpu_state_ptr->regs[3] & 0xFF),
                            (uint16_t)(cpu_state_ptr->regs[1] & 0xFFFF));
        return 1;
    case 0x0E:                      /* teletype output */
        x86_video_teletype((uint8_t)(cpu_state_ptr->regs[0] & 0xFF));
        return 1;
    case 0x0C:                      /* write graphics pixel */
        x86_video_putpixel((uint16_t)(cpu_state_ptr->regs[1] & 0xFFFF),
                           (uint16_t)(cpu_state_ptr->regs[2] & 0xFFFF),
                           (uint8_t)(cpu_state_ptr->regs[0] & 0xFF));
        return 1;
    case 0x0B:                      /* set palette / background */
        x86_video_set_palette_bios(
            (uint8_t)((cpu_state_ptr->regs[3] >> 8) & 0xFF),
            (uint8_t)(cpu_state_ptr->regs[3] & 0xFF));
        return 1;
    case 0x01:                      /* set cursor shape: nothing to model */
    case 0x05:                      /* set active page: single page only */
        return 1;
    default:
        x86_hle_unhandled_vector = 0x10;
        x86_hle_unhandled_func = func;
        return 0;
    }
}

/* --------------------------------------------------------------------------
 * INT 16h. Reads the same ring our INT 09h handler fills, so a game that
 * installs its own INT 09h and never calls INT 16h is unaffected by any of
 * this.
 */
int x86_bios_keyboard(uint8_t func)
{
    uint16_t key;
    switch (func) {
    case 0x00:                      /* wait for a key */
    case 0x10:
        if (kb_pop(&key)) {
            x86_set_ax(key);
            return 1;
        }
        /* Nothing buffered. Rewind so the INT re-executes on the next slice
           rather than returning a key that does not exist -- blocking here
           would stop the frame loop and with it the input that would satisfy
           it. */
        return 2;
    case 0x01:                      /* peek */
    case 0x11:
        if (kb_peek(&key)) {
            x86_set_ax(key);
            x86_hle_set_zf(0);
        } else {
            x86_hle_set_zf(1);
        }
        return 1;
    case 0x02:                      /* shift flags */
        x86_set_al(0);
        return 1;
    default:
        x86_hle_unhandled_vector = 0x16;
        x86_hle_unhandled_func = func;
        return 0;
    }
}

/* --------------------------------------------------------------------------
 * INT 1Ah
 */
int x86_bios_time(uint8_t func)
{
    switch (func) {
    case 0x00: {                    /* read tick count */
        uint32_t t = bda_r32(BDA_TICKS);
        x86_set_cx((uint16_t)(t >> 16));
        x86_set_dx((uint16_t)t);
        x86_set_al(0);
        return 1;
    }
    case 0x01:                      /* set tick count */
        bda_w32(BDA_TICKS, ((uint32_t)(cpu_state_ptr->regs[1] & 0xFFFF) << 16)
                           | (cpu_state_ptr->regs[2] & 0xFFFF));
        return 1;
    default:
        x86_hle_unhandled_vector = 0x1A;
        x86_hle_unhandled_func = func;
        return 0;
    }
}
