/*
 * Guest I/O ports.
 *
 * This replaces a dispatch that indexed three function-pointer tables at
 * HARDCODED AXI addresses (0x2402C000/D000/E000) which nothing in the tree
 * ever initialised -- so an OUT to any port below 1024 loaded a pointer from
 * uninitialised RAM and BLX'd it. The null guard only helped if the RAM
 * happened to be zero. Ports are rare enough in DOS code that a plain C
 * dispatch costs nothing measurable and removes 12 KB of phantom tables.
 */
#include <stdint.h>
#include <string.h>

#include "cutils.h"   /* x86_cpu.h leans on BOOL/TRUE from here */
#include "x86_cpu.h"

/* ---------------------------------------------------------------------------
 * PIT (8253) channel 2 and the PC speaker.
 *
 * Channel 2's gate is port 0x61 bit 0 and the speaker's data line is bit 1;
 * sound comes out only when both are set. The tone is the channel's reload
 * divisor against the 1.193182 MHz PIT input.
 */
#define PIT_HZ 1193182u

X86IOState x86_io;

static void pit_write_control(uint8_t v)
{
    unsigned ch = v >> 6;
    if (ch != 2)
        return;                     /* channels 0/1 are the tick, not sound */
    x86_io.pit2_access = (v >> 4) & 3;
    x86_io.pit2_latched = 0;
}

static void pit_write_ch2(uint8_t v)
{
    switch (x86_io.pit2_access) {
    case 1:                          /* lo byte only */
        x86_io.pit2_reload = v;
        break;
    case 2:                          /* hi byte only */
        x86_io.pit2_reload = (uint16_t)(v << 8);
        break;
    default:                         /* lo then hi */
        if (!x86_io.pit2_latched) {
            x86_io.pit2_reload = (x86_io.pit2_reload & 0xFF00) | v;
            x86_io.pit2_latched = 1;
        } else {
            x86_io.pit2_reload = (uint16_t)((v << 8) | (x86_io.pit2_reload & 0xFF));
            x86_io.pit2_latched = 0;
        }
        break;
    }
}

uint32_t x86_speaker_hz(void)
{
    if ((x86_io.port61 & 3) != 3)
        return 0;                    /* gate or data line off: silence */
    if (x86_io.pit2_reload == 0)
        return PIT_HZ / 65536u;      /* a 0 divisor means the full 16-bit period */
    return PIT_HZ / x86_io.pit2_reload;
}

/*
 * Render `frames` samples of the current tone. Phase is carried across calls in
 * x86_io.spk_phase so the waveform does not restart every audio buffer, which
 * would put a click at the start of each one.
 */
void x86_speaker_render(int16_t *dst, unsigned frames, unsigned sample_rate,
                        int16_t amplitude)
{
    uint32_t hz = x86_speaker_hz();
    if (!hz || !sample_rate) {
        memset(dst, 0, frames * sizeof(*dst));
        x86_io.spk_phase = 0;
        return;
    }
    /* 16.16 phase step: how far through the square wave one sample moves. */
    uint32_t step = (uint32_t)(((uint64_t)hz << 16) / sample_rate);
    uint32_t phase = x86_io.spk_phase;
    for (unsigned i = 0; i < frames; i++) {
        dst[i] = (phase & 0x8000u) ? amplitude : (int16_t)-amplitude;
        phase = (phase + step) & 0xFFFFu;
    }
    x86_io.spk_phase = phase;
}

/* ---------------------------------------------------------------------------
 * Keyboard controller.
 *
 * Queue a make or break code and raise IRQ 1. The interpreter delivers it at
 * the next instruction boundary if IF allows; the host drops pit_counter to 1
 * so that boundary arrives immediately rather than at the end of the slice.
 */
void x86_kbd_scancode(uint8_t code)
{
    uint8_t next = (uint8_t)((x86_io.kbd_tail + 1u) & 15u);
    if (next == x86_io.kbd_head)
        return;                      /* full: the controller drops it too */
    x86_io.kbd_queue[x86_io.kbd_tail] = code;
    x86_io.kbd_tail = next;
}

int x86_kbd_pending(void)
{
    return x86_io.kbd_head != x86_io.kbd_tail;
}

static uint8_t kbd_read_data(void)
{
    if (x86_io.kbd_head == x86_io.kbd_tail)
        return x86_io.kbd_last;      /* re-reading holds the last code */
    x86_io.kbd_last = x86_io.kbd_queue[x86_io.kbd_head];
    x86_io.kbd_head = (uint8_t)((x86_io.kbd_head + 1u) & 15u);
    return x86_io.kbd_last;
}

/* --------------------------------------------------------------------------- */

uint32_t x86_port_read(uint32_t port, uint32_t size)
{
    (void)size;
    switch (port) {
    case 0x42:
        return x86_io.pit2_reload & 0xFF;
    case 0x60:
        return kbd_read_data();
    case 0x64:
        /* Status: bit 0 = output buffer full, bit 2 = self-test passed. Polled
           by code that reads the port without using the interrupt. */
        return (uint32_t)((x86_kbd_pending() ? 1u : 0u) | 4u);
    case 0x61:
        return x86_io.port61;

    /*
     * CGA status (0x3DA). A program polls this to sync to the display, and a
     * benchmark TIMES against it -- counting how much work fits between two
     * retraces and then dividing by the count.
     *
     * That is why the position has to come from the PIT counter rather than
     * from a value the frame loop pokes. The frame loop only moves cga_beam
     * while the guest is STOPPED, so from inside the guest the vertical
     * retrace bit was a constant zero: a wait for vsync-begin never fired, the
     * count came out zero, and TOPBENCH divided by it and took a #DE.
     *
     * The PIT slice IS the frame here, so how far the counter has run down is
     * how far the beam has swept. Vertical retrace covers the last sixteenth,
     * horizontal is a bit of the same counter, and neither costs a divide.
     * Bit 3 is vertical retrace; bit 0 is "display disabled", true during
     * either.
     */
    case 0x3DA: {
        unsigned v = 0;
        uint32_t reload = cpu_state_ptr->pit_reload;
        if (reload) {
            uint32_t elapsed = reload - cpu_state_ptr->pit_counter;
            if (elapsed >= reload - (reload >> 4))
                return 0x08u | 0x01u;           /* vertical retrace */
            if ((elapsed >> 6) & 1u)
                v |= 0x01u;                     /* horizontal retrace */
            return v;
        }
        /* No slice running (the bare-metal harness drives the beam directly). */
        if (x86_io.cga_beam >= 0xD000u)
            v |= 0x08u | 0x01u;
        else if (++x86_io.cga_status_reads & 8u)
            v |= 0x01u;
        return v;
    }
    case 0x3D8:
        return x86_io.cga_mode;
    case 0x3D9:
        return x86_io.cga_color_select;
    }
    /* An unassigned port floats high, which is what a real bus does and what
       DOS code probing for hardware expects to see. */
    return 0xFFFFFFFFu;
}

void x86_port_write(uint32_t port, uint32_t value, uint32_t size)
{
    (void)size;
    switch (port) {
    case 0x42:
        pit_write_ch2((uint8_t)value);
        break;
    case 0x43:
        pit_write_control((uint8_t)value);
        break;
    case 0x61:
        x86_io.port61 = (uint8_t)value;
        break;
    case 0x20:                       /* PIC end-of-interrupt: nothing to model
                                        while only one IRQ source exists */
    case 0x64:                       /* controller commands: accepted, ignored */
        break;

    case 0x3D8:
        x86_io.cga_mode = (uint8_t)value;
        x86_video_refresh_palette();
        break;
    case 0x3D9:
        x86_io.cga_color_select = (uint8_t)value;
        x86_video_refresh_palette();
        break;
    default:
        break;
    }
}
