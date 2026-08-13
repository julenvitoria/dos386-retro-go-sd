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
#include "x86_audio_wave.h"   /* the generator; <stdint.h> only, host-testable */

/* ---------------------------------------------------------------------------
 * PIT (8253) channel 2 and the PC speaker.
 *
 * Channel 2's gate is port 0x61 bit 0 and the speaker's data line is bit 1;
 * sound comes out only when both are set. The tone is the channel's reload
 * divisor against the 1.193182 MHz PIT input.
 */
#define PIT_HZ X86_PIT_HZ

X86IOState x86_io;

/* The last 32 writes that can move the tone, as (port << 24) | value, so a
   guest whose beeps come out at the wrong pitch can be read rather than
   guessed at. scripts/probe.py --dump x86_spk_log:32x4 */
volatile uint32_t x86_spk_log[32];
volatile uint32_t x86_spk_log_idx = 0;

static void pit_write_control(uint8_t v)
{
    unsigned ch = v >> 6;
    if (ch != 2)
        return;                     /* channels 0/1 are the tick, not sound */
    unsigned access = (v >> 4) & 3;
    /* Access field 0 is the COUNTER LATCH command, not an access mode: it
       freezes the count for reading and leaves the programmed mode alone.
       Storing it as the mode put the next 0x42 write through the lo/hi path
       with a stale half-pair flag, which is one way a tone comes out an octave
       or more off. */
    if (access == 0)
        return;
    x86_io.pit2_access = (uint8_t)access;
    x86_io.pit2_latched = 0;        /* a control word resets the pair phase */
}

static void pit_write_ch2(uint8_t v)
{
    switch (x86_io.pit2_access) {
    case 1:                          /* lo byte only: the HIGH byte STAYS */
        x86_io.pit2_reload = (uint16_t)((x86_io.pit2_reload & 0xFF00) | v);
        break;
    case 2:                          /* hi byte only: the LOW byte STAYS  */
        x86_io.pit2_reload = (uint16_t)((v << 8) | (x86_io.pit2_reload & 0xFF));
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

/*
 * The audible PIT divisor right now, or 0 for silence.
 *
 * This -- not a frequency -- is what the renderer and the event ring carry.
 * An integer `PIT_HZ / divisor` loses the pitch before the generator sees it,
 * and the loss grows as the tone falls: it is what put middle C 4.5 cents flat
 * and the octave below it 12.5 (measured, tests/host_audio.c).
 */
uint32_t x86_speaker_divisor(void)
{
    if ((x86_io.port61 & 3) != 3)
        return 0;                    /* gate or data line off: silence */
    /* A reload of 0 means the full 16-bit period, i.e. 65536 -- not 0, which
       the generator reads as "never programmed". */
    return x86_io.pit2_reload ? x86_io.pit2_reload : 65536u;
}

/* Reporting only (probe.py, logs, tier-1 assertions). Never in the signal
   path; see x86_audio_wave.h. */
uint32_t x86_speaker_hz(void)
{
    return x86_spkr_freq_hz(x86_speaker_divisor());
}

uint32_t (*x86_io_time_ms)(void);
volatile uint32_t x86_spk_events = 0;
volatile uint32_t x86_spk_events_dropped = 0;

/*
 * Log a change in the audible frequency, stamped with the host clock.
 *
 * Called after every write that can move it (0x42, 0x43, 0x61). Only CHANGES
 * are recorded, so a guest that rewrites the same divisor costs nothing.
 */
static void spk_event(uint32_t port, uint32_t value)
{
    x86_spk_log[x86_spk_log_idx & 31] = (port << 24) | (value & 0xFFu);
    x86_spk_log_idx++;

    uint32_t div = x86_speaker_divisor();
    if (div == x86_io.spk_last_div)
        return;
    x86_io.spk_last_div = div;

    uint8_t next = (uint8_t)((x86_io.spk_ev_tail + 1u) % X86_SPK_EVENTS);
    if (next == x86_io.spk_ev_head) {
        x86_spk_events_dropped++;    /* loud: the ring is too short for this
                                        guest, and silence would hide it */
        return;
    }
    x86_io.spk_ev_ms[x86_io.spk_ev_tail] = x86_io_time_ms ? x86_io_time_ms() : 0;
    x86_io.spk_ev_div[x86_io.spk_ev_tail] = div;
    x86_io.spk_ev_tail = next;
    x86_spk_events++;
}

/*
 * One constant-divisor run of samples. Phase is carried in x86_io.spk_phase so
 * the waveform does not restart at a buffer or segment boundary, which would
 * put a click at every one.
 *
 * ANTI-CLICK: the phase is zeroed on each RISING edge of the gate -- the
 * transition from "no tone" to "tone" -- so a beep always begins at the same
 * point of the cycle instead of mid-swing. The event ring is exactly where
 * that edge is visible: an event whose divisor goes from 0 (or from a divisor
 * above Nyquist, which is equally silent) to an audible one IS the rising
 * edge, at the sample offset the guest made it. Zeroing on the falling edge
 * instead -- which is what the old renderer did, from inside its silence
 * branch -- gets the same starting phase but only because silence always
 * precedes a beep; it says nothing about the edge and breaks the moment a
 * divisor changes while the gate stays open.
 */
static void spk_run(int16_t *dst, unsigned n, uint32_t divisor,
                    unsigned sample_rate, int16_t amplitude)
{
    if (!n)
        return;
    uint32_t inc = x86_spkr_phase_inc(divisor, sample_rate);
    if (inc == 0) {
        memset(dst, 0, n * sizeof(*dst));
        x86_io.spk_was_on = 0;
        return;                       /* phase deliberately left where it was */
    }
    if (!x86_io.spk_was_on) {
        x86_io.spk_phase = 0;         /* rising edge */
        x86_io.spk_was_on = 1;
    }
    x86_spkr_render(dst, n, inc, amplitude, &x86_io.spk_phase);
}

/*
 * Render `frames` samples covering the real time since the previous call.
 *
 * This used to point-sample the speaker once per frame, which is why beeps
 * went missing: at the measured ~10 fps a tone had to survive 100 ms to be
 * observed at all, and a DOS beep is often 20. Now every frequency change the
 * guest made during the window is placed at its own offset within the buffer,
 * so a 20 ms beep becomes 20 ms of tone in the middle of the buffer instead of
 * either the whole buffer or nothing.
 *
 * With no host clock (the bare-metal harness) every event lands at offset 0
 * and the last one wins, which is the old behaviour exactly.
 */
void x86_speaker_render(int16_t *dst, unsigned frames, unsigned sample_rate,
                        int16_t amplitude)
{
    uint32_t t1 = x86_io_time_ms ? x86_io_time_ms() : 0;
    uint32_t t0 = x86_io.spk_window_ms;
    x86_io.spk_window_ms = t1;
    uint32_t span = t1 - t0;

    unsigned pos = 0;
    while (x86_io.spk_ev_head != x86_io.spk_ev_tail) {
        uint32_t ev_ms = x86_io.spk_ev_ms[x86_io.spk_ev_head];
        uint32_t div   = x86_io.spk_ev_div[x86_io.spk_ev_head];
        x86_io.spk_ev_head =
            (uint8_t)((x86_io.spk_ev_head + 1u) % X86_SPK_EVENTS);

        unsigned idx = 0;
        if (span) {
            uint32_t d = ev_ms - t0;            /* wrap-safe */
            if (d > span)
                d = span;                       /* clamp, never index past */
            idx = (unsigned)(((uint64_t)d * frames) / span);
        }
        if (idx < pos)
            idx = pos;                          /* the ring is in order */
        if (idx > frames)
            idx = frames;
        spk_run(dst + pos, idx - pos, x86_io.spk_render_div,
                sample_rate, amplitude);
        pos = idx;
        x86_io.spk_render_div = div;
    }
    spk_run(dst + pos, frames - pos, x86_io.spk_render_div,
            sample_rate, amplitude);
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

/*
 * What the guest actually got off port 0x60, and how often it asked.
 *
 * The counters upstream of this (input_scancodes, input_irqs, kbd_head/tail)
 * prove a code was QUEUED. They cannot tell you WHICH code the guest read,
 * and "this button does nothing" is exactly the question that turns on which
 * byte arrived -- 0x1C or something else, make or break, once or twice. The
 * log is the last 32 bytes handed out, newest at (idx-1) & 31.
 *
 *   python3 scripts/probe.py --retrogo --wait-magic \
 *       --watch x86_kbd_port60_reads --dump x86_kbd_read_log:32x8
 */
volatile uint32_t x86_kbd_port60_reads = 0;
volatile uint32_t x86_kbd_read_log[32];
volatile uint32_t x86_kbd_read_log_idx = 0;

static uint8_t kbd_read_data(void)
{
    x86_kbd_port60_reads++;
    if (x86_io.kbd_head == x86_io.kbd_tail)
        return x86_io.kbd_last;      /* re-reading holds the last code */
    x86_io.kbd_last = x86_io.kbd_queue[x86_io.kbd_head];
    x86_io.kbd_head = (uint8_t)((x86_io.kbd_head + 1u) & 15u);
    x86_kbd_read_log[x86_kbd_read_log_idx & 31] = x86_io.kbd_last;
    x86_kbd_read_log_idx++;
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
        spk_event(port, value);
        break;
    case 0x43:
        pit_write_control((uint8_t)value);
        spk_event(port, value);
        break;
    case 0x61:
        x86_io.port61 = (uint8_t)value;
        spk_event(port, value);
        break;
    case 0x20:                       /* PIC end-of-interrupt: nothing to model
                                        while only one IRQ source exists */
    case 0x64:                       /* controller commands: accepted, ignored */
        break;

    case 0x3D8:
        x86_io.cga_mode = (uint8_t)value;
        x86_video_log_event(2, 0, value);
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
