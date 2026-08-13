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

/*
 * The two BDA fields the renderer selects on: 0040:0049 mode, 0040:004A
 * columns. Exported because the video layer must read what the BIOS PUBLISHES
 * rather than a copy of its own -- a program is free to set the mode and then
 * be believed, and the previous port's whole mode-detection design is "read the
 * BDA, never the 0x3D8 register" (references/8086tiny/docs/video/03).
 *
 * Returns 0 if the BDA page is not backed, which is a real possibility on
 * silicon (every bda_w* here already tolerates it), and the caller then keeps
 * whatever mode it last had rather than believing a zero.
 */
int x86_bios_bda_video(uint8_t *mode, uint16_t *cols)
{
    uint8_t *p = x86_guest_ptr(BDA_MODE, 3);
    if (!p)
        return 0;
    *mode = p[0];
    *cols = (uint16_t)(p[1] | (p[2] << 8));
    return 1;
}

/* --------------------------------------------------------------------------
 * Keyboard buffer, shared by the INT 09h handler (producer) and INT 16h
 * (consumer). It is the real 16-entry ring at 0040:001E so a program reading
 * the BDA directly sees the same thing INT 16h reports.
 */
/* Modifier state as the controller sees it. Not derived from the BDA: a guest
   may write 0040:0017 itself, and a real 8042 does not read the guest's mind. */
static uint8_t kbd_flags;         /* bit0 RSHIFT, 1 LSHIFT, 2 CTRL, 3 ALT */

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
    x86_kb_pushes++;
}

/* Ring bookkeeping, host-readable. "The guest polls INT 16h forever" has two
   very different causes -- nothing was ever queued, or something is taking the
   key out from under it -- and only a count of what the PRODUCER and each
   CONSUMER did can tell them apart. */
volatile uint32_t x86_kb_pushes = 0;      /* keys queued by INT 09h        */
volatile uint32_t x86_kb_pops = 0;        /* keys dequeued (AH=00, DOS)    */
volatile uint32_t x86_kb_peek_hits = 0;   /* peeks that found a key        */

static int kb_peek(uint16_t *out)
{
    uint16_t head = bda_r16(BDA_KB_HEAD);
    if (head == bda_r16(BDA_KB_TAIL))
        return 0;
    *out = bda_r16(BDA + head);
    x86_kb_peek_hits++;
    return 1;
}

static int kb_pop(uint16_t *out)
{
    uint16_t head = bda_r16(BDA_KB_HEAD);
    if (head == bda_r16(BDA_KB_TAIL))
        return 0;
    x86_kb_pops++;
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
    kbd_flags = 0;
    bda_w8(BDA + 0x17, 0);
    /* Keep the renderer and the BDA agreeing. They did not: the BDA said mode
       3 (text, what a PC boots into) while the renderer stayed on mode 4, so a
       guest that never calls INT 10h AH=00 had its text cells drawn as 2bpp
       graphics. */
    x86_video_set_mode(0x03);
    bda_w32(BDA_TICKS, 0);
}

/* 18.2 Hz, driven by the frame loop. */
void x86_bios_tick(void)
{
    bda_w32(BDA_TICKS, bda_r32(BDA_TICKS) + 1u);
}

/* --------------------------------------------------------------------------
 * INT 09h -- the BIOS keyboard interrupt.
 *
 * Only reached while the guest is still using the BIOS vector. A game that
 * installs its own INT 09h replaces the IVT entry, our stub is never called,
 * and it reads port 0x60 itself -- which is the whole reason the keyboard is
 * modelled at the port level rather than as an INT 16h shim.
 */
static const uint8_t sc_to_ascii[128] = {
    0,    27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8,  9,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 13,  0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', 39, '`',
    0,   92,  'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,  '*',
    0,   ' ',
};

/*
 * The SHIFTED half of the same table, as pairs rather than as a second 128-byte
 * array: 21 pairs is 43 bytes of rodata against 128, and the pairing is exactly
 * what is printed on the key.
 *
 * This exists because the on-screen keyboard needs it. Without a shift decode
 * the BDA ring receives '1' for a key labelled '!' and 'a' for 'A' -- the same
 * class of silent wrong answer the previous port's osk_needs_shift() comment
 * calls "the single most likely thing to get silently wrong here", only on our
 * side of the wire. The OSK sends a real Left-Shift make/break around the key
 * (x86_osk.c), so the decode has to be here, in the handler that reads the
 * port, and not in the OSK.
 */
static const char sc_shift_pairs[] =
    "1!2@3#4$5%6^7&8*9(0)-_=+[{]}" ";:" "'\"" "`~" "\\|" ",<.>/?";

uint8_t x86_kbd_ascii(uint8_t sc, int shift)
{
    uint8_t base = (sc < sizeof(sc_to_ascii)) ? sc_to_ascii[sc] : 0;
    if (!shift || !base)
        return base;
    if (base >= 'a' && base <= 'z')
        return (uint8_t)(base - 'a' + 'A');
    for (unsigned i = 0; sc_shift_pairs[i]; i += 2)
        if ((uint8_t)sc_shift_pairs[i] == base)
            return (uint8_t)sc_shift_pairs[i + 1];
    return base;
}

volatile uint32_t x86_bios_kbd_irqs = 0;     /* INT 09h entries served */
volatile uint32_t x86_bios_int16_calls = 0;  /* guest polls of INT 16h */

int x86_bios_kbd_irq(void)
{
    x86_bios_kbd_irqs++;
    /* Read the controller exactly as a real handler does, so the queue drains
       through the same path whichever handler is installed. */
    uint8_t sc = (uint8_t)x86_port_read(0x60, 1);
    uint8_t make = (uint8_t)(sc & 0x7F);
    uint8_t bit = 0;

    /* Modifiers are STATE, not characters: a real BIOS updates 0040:0017 and
       buffers nothing, on make and on break alike. Handling the break half is
       what stops a Shift from latching down forever after one shifted key. */
    switch (make) {
    case 0x2A: bit = 0x02; break;    /* left shift  */
    case 0x36: bit = 0x01; break;    /* right shift */
    case 0x1D: bit = 0x04; break;    /* ctrl        */
    case 0x38: bit = 0x08; break;    /* alt         */
    default:   break;
    }
    if (bit) {
        if (sc & 0x80)
            kbd_flags &= (uint8_t)~bit;
        else
            kbd_flags |= bit;
        /* The BDA copy, which is what INT 16h AH=02 and every program that
           peeks 0040:0017 reads. */
        bda_w8(BDA + 0x17, kbd_flags);
        return 1;
    }

    if (sc & 0x80)
        return 1;                    /* break code: nothing to buffer */
    x86_kb_push(sc, x86_kbd_ascii(sc, (kbd_flags & 0x03) != 0));
    return 1;
}

/* --------------------------------------------------------------------------
 * INT 10h
 */
int x86_bios_video(uint8_t func)
{
    switch (func) {
    case 0x00: {                    /* set mode */
        /* Bit 7 of AL is "do not clear the display" (EGA and later); the mode
           number is the low seven bits. */
        uint8_t mode = (uint8_t)(cpu_state_ptr->regs[0] & 0x7F);
        x86_video_set_mode(mode);
        /* Publish what the renderer actually adopted, not what was asked for:
           a mode we cannot render leaves the previous one up, and the BDA has
           to agree with the screen or the next frame's BDA re-read undoes it. */
        bda_w8(BDA_MODE, x86_video_mode());
        bda_w16(BDA_COLS, x86_video_text_cols());
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
    case 0x11:                      /* character generator */
        /* AL=30h: return a pointer to a font. A CGA BIOS has no AH=11h at all,
           so leaving it unanswered left ES:BP holding the caller's own garbage
           and a program that reads through it walked into memory it does not
           own. We do have an 8x8 font and it is mapped at F000:F000. */
        if ((cpu_state_ptr->regs[0] & 0xFF) == 0x30) {
            cpu_state_ptr->segs[0].sel  = 0xF000;
            cpu_state_ptr->segs[0].base = 0xF0000u;
            cpu_state_ptr->segs[0].limit = 0xFFFF;
            cpu_state_ptr->regs[5] = 0xF000;        /* BP: F000:F000 */
            x86_set_cx(8);                          /* bytes per character */
            cpu_state_ptr->regs[2] =
                (cpu_state_ptr->regs[2] & 0xFFFFFF00u) | 24;   /* DL = rows-1 */
            return 1;
        }
        x86_hle_unhandled_vector = 0x10;
        x86_hle_unhandled_func = func;
        return 0;
    case 0x08:                      /* read char + attribute at cursor */
        /* Unimplemented, this returned nothing and left AX holding whatever
           the caller had -- and a program that probes the display this way
           then believes the garbage. */
        cpu_state_ptr->regs[0] = (cpu_state_ptr->regs[0] & 0xFFFF0000u)
                               | x86_video_read_cell();
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
    /*
     * AH=10h -- VGA palette / DAC. Implemented as calls into the SAME DAC the
     * 0x3C8/0x3C9 ports drive, so there is one palette model and not two that
     * can disagree about what colour an index is. A title that loads its
     * palette through the BIOS and one that streams it to the ports must end
     * up in the same place.
     */
    case 0x10: {
        unsigned al = cpu_state_ptr->regs[0] & 0xFF;
        if (al == 0x10) {           /* set one DAC register: BX=index, DH/CH/CL */
            uint8_t rgb[3];
            rgb[0] = (uint8_t)((cpu_state_ptr->regs[2] >> 8) & 0xFF);   /* DH R */
            rgb[1] = (uint8_t)((cpu_state_ptr->regs[1] >> 8) & 0xFF);   /* CH G */
            rgb[2] = (uint8_t)( cpu_state_ptr->regs[1]       & 0xFF);   /* CL B */
            x86_vga_dac_set_block(cpu_state_ptr->regs[3] & 0xFF, 1, rgb);
            return 1;
        }
        if (al == 0x12) {           /* set a BLOCK: BX=first, CX=count, ES:DX */
            unsigned first = cpu_state_ptr->regs[3] & 0xFFFF;
            unsigned count = cpu_state_ptr->regs[1] & 0xFFFF;
            uint32_t lin = cpu_state_ptr->segs[0].base
                         + (cpu_state_ptr->regs[2] & 0xFFFF);
            uint8_t *p;
            if (first > 255u) return 1;
            if (count > 256u - first) count = 256u - first;
            p = x86_guest_ptr(lin, count * 3u);
            if (p)
                x86_vga_dac_set_block(first, count, p);
            return 1;
        }
        if (al == 0x15 || al == 0x17) {   /* read one / read a block */
            /* Reads go through the port side's own index+stream so a
               read-modify-write fade sees exactly what it wrote. */
            unsigned first = (al == 0x15) ? (cpu_state_ptr->regs[3] & 0xFF)
                                          : (cpu_state_ptr->regs[3] & 0xFFFF);
            unsigned count = (al == 0x15) ? 1u
                                          : (cpu_state_ptr->regs[1] & 0xFFFF);
            if (first > 255u) return 1;
            if (count > 256u - first) count = 256u - first;
            x86_vga_dac_set_read_index((uint8_t)first);
            if (al == 0x15) {
                uint8_t r = x86_vga_dac_read();
                uint8_t g = x86_vga_dac_read();
                uint8_t b = x86_vga_dac_read();
                cpu_state_ptr->regs[2] =
                    (cpu_state_ptr->regs[2] & 0xFFFF00FFu) | ((uint32_t)r << 8);
                cpu_state_ptr->regs[1] =
                    (cpu_state_ptr->regs[1] & 0xFFFF0000u)
                    | ((uint32_t)g << 8) | b;
            } else {
                uint32_t lin = cpu_state_ptr->segs[0].base
                             + (cpu_state_ptr->regs[2] & 0xFFFF);
                uint8_t *p = x86_guest_ptr(lin, count * 3u);
                if (p)
                    for (unsigned i = 0; i < count * 3u; i++)
                        p[i] = x86_vga_dac_read();
            }
            return 1;
        }
        /* AL=00/01/02/03 are the EGA 16-colour palette-register calls. Mode
           13h indexes the DAC directly and does not go through them, and we
           have no attribute controller, so they are accepted and ignored
           rather than left unanswered -- an unanswered call in the middle of a
           sequence is indistinguishable, to the guest, from broken hardware.
           That is the TOPBENCH AH=43h shape, and it cost a session. */
        return 1;
    }
    /*
     * The EGA/VGA detection cascade. Neither of these exists on a CGA BIOS,
     * where the vector simply IRETs -- and answering them that way is not
     * pedantry, it is the difference between a program deciding "no EGA" and a
     * program walking off a pointer. Leaving AH=12h unanswered is what stopped
     * TOPBENCH one second into its run: its detection unit called it, got no
     * answer, kept whatever ES:DI it had, read through it and took a memory
     * fault on an unmapped page. Exactly the shape AH=11h above was fixed for.
     */
    case 0x12:
        /* AH=12h/BL=10h asks for EGA configuration; a machine without EGA
           returns BL UNCHANGED, which is precisely "do nothing". */
        return 1;
    case 0x1A:
        /* AH=1Ah is VGA's display-combination call. It reports support by
           returning AL=1Ah, so AL=0 is the documented "not supported". */
        x86_set_al(0);
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
/* The keyboard ring, for the DOS layer. INT 21h's console input functions read
   the same buffer INT 16h does -- on a real machine DOS goes through the BIOS,
   and a program that mixes the two must see one queue, not two. */
volatile uint32_t x86_kb_get_calls = 0;   /* DOS/BIOS ring consumers */
volatile uint32_t x86_kb_peek_calls = 0;

int x86_kb_get(uint16_t *key)  { x86_kb_get_calls++;  return kb_pop(key); }
int x86_kb_peek(uint16_t *key) { x86_kb_peek_calls++; return kb_peek(key); }

int x86_bios_keyboard(uint8_t func)
{
    uint16_t key;
    x86_bios_int16_calls++;
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
        /* The real state, not a hardcoded 0. A program that asks whether Shift
           is down and is always told "no" cannot be given a shifted key by any
           amount of correctness elsewhere. */
        x86_set_al(bda_r8(BDA + 0x17));
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
