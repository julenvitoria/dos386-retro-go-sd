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
#include "x86_probe.h"
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

/*
 * Every port access, as (dir << 28) | (port << 12) | count, with the byte in
 * the low 12 bits of the NEXT word. dir 1 = IN, 2 = OUT.
 *
 * Consecutive accesses to the SAME port in the same direction are COALESCED
 * into one slot with a count, which is the only thing that makes a 32-slot
 * ring useful here: a mode-13h palette upload is 768 consecutive writes to
 * 0x3C9 and would otherwise flush every other port out of the log. What the
 * log is for is the SHAPE of a guest's hardware conversation -- which register
 * files it programs and in what order -- and that is exactly what coalescing
 * preserves. x86_spk_log answers a different question (the tone) and stays.
 *
 * scripts/probe.py --io-log
 */
#define X86_IO_LOG_SLOTS 32
volatile uint32_t x86_io_log[X86_IO_LOG_SLOTS * 2];
volatile uint32_t x86_io_log_idx = 0;
volatile uint32_t x86_io_log_total = 0;

static void io_log(unsigned dir, uint32_t port, uint32_t value)
{
    x86_io_log_total++;
    unsigned prev = (x86_io_log_idx - 1u) & (X86_IO_LOG_SLOTS - 1u);
    uint32_t head = x86_io_log[prev * 2];
    if (x86_io_log_idx && (head >> 28) == dir &&
        ((head >> 12) & 0xFFFFu) == port && (head & 0xFFFu) != 0xFFFu) {
        x86_io_log[prev * 2] = head + 1u;       /* same port again: just count */
        x86_io_log[prev * 2 + 1] = value & 0xFFFu;
        return;
    }
    unsigned slot = x86_io_log_idx & (X86_IO_LOG_SLOTS - 1u);
    x86_io_log[slot * 2] = (dir << 28) | ((port & 0xFFFFu) << 12) | 1u;
    x86_io_log[slot * 2 + 1] = value & 0xFFFu;
    x86_io_log_idx++;
}


/* ---------------------------------------------------------------------------
 * The VGA register FILES: CRTC, Sequencer, Graphics Controller, Attribute.
 *
 * These were not modelled at all -- writes vanished and reads fell through to
 * the 0xFF default. That is not a missing feature a guest degrades around, it
 * is a missing DEVICE, and the way a DOS program finds its display is to WRITE
 * a register and READ IT BACK. WOLF3D does exactly that: `OUT 3D4,0F / OUT
 * 3D5,66 / IN 3D5` and then the same against the mono alias 3B4/3B5
 * (measured, x86_io_log). Both readbacks came back 0xFF, its detection
 * concluded nothing was there, and the engine walked off into its own heap and
 * halted on 0xFF filler -- three subsystems away from anything that looked
 * like video.
 *
 * So this is deliberately a STORE, not an emulation: index latch plus an array
 * per file, written and read back faithfully. That is the whole contract a
 * detection probe tests, and it is honest about what it is -- nothing here
 * changes what the renderer draws. Unchained (Mode Y) addressing, the CRTC
 * start address at 0x0C/0x0D and split-screen still do nothing; they now do
 * nothing VISIBLY, with the values a probe can read, instead of invisibly.
 *
 * 0x3B4/0x3B5 is the mono alias of the CRTC pair and reaches the same file:
 * on a real card the Misc Output register's bit 0 selects which address the
 * CRTC decodes, and a program that probes both must see one of them answer.
 */
#define VGA_CRTC_REGS  32
#define VGA_SEQ_REGS    8
#define VGA_GC_REGS    16
#define VGA_ATTR_REGS  32

static struct {
    uint8_t crtc[VGA_CRTC_REGS], crtc_idx;
    uint8_t seq[VGA_SEQ_REGS],   seq_idx;
    uint8_t gc[VGA_GC_REGS],     gc_idx;
    uint8_t attr[VGA_ATTR_REGS], attr_idx;
    uint8_t attr_flipflop;               /* 0 = next 0x3C0 write is the INDEX */
    uint8_t misc;
} vga_regs = { .misc = 0x67 };

/*
 * Misc Output bit 0 is the I/O ADDRESS SELECT, and it is not decoration: it
 * decides whether the CRTC answers at the colour addresses (0x3D4/0x3D5,
 * status 0x3DA) or the mono ones (0x3B4/0x3B5, status 0x3BA). A card decodes
 * ONE pair; the other is silent.
 *
 * That silence carries information, and answering both is worse than answering
 * neither. WOLF3D probes 0x3D5 and then 0x3B5 to decide which display it has;
 * with both replying it concluded MONO and then polled 0x3BA 32,000 times
 * waiting for a retrace on a card that is not there. Measured -- the first
 * version of this register file did exactly that.
 *
 * 0x67 is what a VGA BIOS leaves after a colour mode set, and it is the reset
 * value here so a guest that never writes 0x3C2 still sees a colour card.
 */
static int vga_mono_decoded(void) { return (vga_regs.misc & 1u) == 0u; }

/*
 * The Attribute controller is one PORT for both halves, sequenced by a
 * flip-flop that a READ OF THE INPUT STATUS REGISTER (0x3DA) resets to
 * "index". A program that is interrupted mid-pair re-syncs by reading 0x3DA,
 * so the reset has to live in the 0x3DA path or the two halves swap roles
 * permanently and every subsequent write lands in the wrong register.
 */
void x86_vga_attr_reset_flipflop(void) { vga_regs.attr_flipflop = 0; }

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
 * Arm IRQ 1 for a queued scancode. THE ONE PLACE that decides it, because the
 * two callers in src/main.c disagreeing about it is the defect this exists to
 * close.
 *
 * Two guards, and the second one is the whole point:
 *
 *  - The pending slot is ONE DEEP -- it is the emulated 8259A's entire model of
 *    one. Writing 0x09 over whatever is in it discards the other interrupt: the
 *    frame loop raises the 18.2 Hz tick as 0x08 later in the same iteration, so
 *    an unguarded store here silently ate keyboard IRQs and timer ticks in
 *    turn. Refuse instead; the queue is still there and the next call re-tries.
 *
 *  - pit_counter = 1 pulls the guest's next instruction boundary in so the IRQ
 *    lands NOW rather than at the end of the slice. That is only useful if the
 *    boundary can actually deliver, i.e. if IF is set. With IF CLEAR -- which
 *    is exactly the state a guest is in while it runs its own INT 09h handler
 *    -- the boundary cannot deliver, so it just ENDS THE SLICE, and a caller
 *    doing this once a frame leaves the guest running ONE INSTRUCTION PER
 *    FRAME. Measured under gwemu with the on-screen keyboard up: 60 IRQ arms a
 *    second, the guest 18 seconds inside one INT 09h handler, five queued
 *    codes it could not reach, every BIOS tick refused (guest_tick_irq_lost
 *    +18/s) -- and all five codes delivered at once the moment the OSK closed
 *    and the queue stopped being refilled. That is the "I had to turn off the
 *    keyboard before it would send the keys" report, in full.
 *
 * eflags is authoritative here: the interpreter caches it in R11 and writes it
 * back on exit, and every caller is outside the interpreter.
 *
 * Returns 1 if the IRQ was armed.
 */
int x86_kbd_raise_irq(X86CPUState *cpu)
{
    if (!x86_kbd_pending() || cpu->hard_int_pending)
        return 0;
    cpu->hard_int_pending = 0x09;
    if (cpu->eflags & 0x200u)
        cpu->pit_counter = 1;
    return 1;
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

static uint32_t port_read_inner(uint32_t port);

#if X86_PROBE
/* Set by the fall-through arm of port_read_inner, cleared before each call.
   The returned VALUE cannot carry this: 0xFF is both "open bus" and a perfectly
   real answer from the DAC mask register, and conflating the two is how a
   report would claim we model a port we have never heard of. */
static unsigned port_read_unmodelled;
#endif

uint32_t x86_port_read(uint32_t port, uint32_t size)
{
    (void)size;
#if X86_PROBE
    port_read_unmodelled = 0;
#endif
    uint32_t v = port_read_inner(port);
    io_log(1, port, v);
    X86_PROBE_PORT(X86_PROBE_KIND_IN, (uint16_t)port, v,
                   port_read_unmodelled);
    return v;
}

static uint32_t port_read_inner(uint32_t port)
{
    switch (port) {
    case 0x42:
        return x86_io.pit2_reload & 0xFF;
    case 0x60:
        return kbd_read_data();
    case 0x64:
        /* Status: bit 0 = output buffer full, bit 2 = self-test passed. Polled
           by code that reads the port without using the interrupt. */
        return (uint32_t)((x86_kbd_pending() ? 1u : 0u) | 4u);
    case 0x61: {
        /*
         * The two bits of 0x61 that a real machine DRIVES rather than stores.
         * Returning the stored byte made both of them constants, and each
         * constant is its own hang:
         *
         *   bit 5, PIT channel 2 OUT. This is how a program CONFIRMS there is
         *   a speaker: program ch2 with a short count, ungate it, and watch
         *   bit 5 toggle. A bit that never moves means either "no timer" or an
         *   unbounded wait, and neither is what this machine is -- the PIT ch2
         *   divisor is modelled, it drives the audio generator, and its output
         *   is the one thing we were not exposing.
         *
         *   bit 4, the DRAM refresh toggle. Used as a cheap microsecond clock
         *   by a great deal of DOS code ("read 0x61, wait for bit 4 to flip").
         *   Stuck, that loop never leaves.
         *
         * Both are derived from the SAME slice counter the 0x3DA retrace bit
         * comes from, for the same reason: the frame loop only advances while
         * the guest is stopped, so anything it pokes reads as a constant from
         * inside the guest. Bit 5 is gated on the speaker actually being
         * enabled and programmed -- an ungated channel 2 holds its output
         * high, which is what a real 8253 does in mode 3.
         */
        uint32_t v = x86_io.port61 & (uint32_t)~0x30u;
        uint32_t reload = cpu_state_ptr->pit_reload;
        uint32_t elapsed = reload ? (reload - cpu_state_ptr->pit_counter)
                                  : ++x86_io.cga_status_reads;
        if (elapsed & 0x10u)
            v |= 0x10u;                    /* refresh toggle */
        if ((x86_io.port61 & 1u) && x86_io.pit2_reload) {
            /* Half the programmed period high, half low -- mode 3, which is
               what every speaker routine programs. The divisor is in PIT ticks
               and `elapsed` is in guest instructions, so this is a SHAPE, not a
               frequency; the audible tone comes from x86_speaker_divisor() and
               is unaffected. What matters to a prober is that it moves, and
               that it moves at a rate that follows the divisor it wrote. */
            uint32_t half = x86_io.pit2_reload >> 1;
            if (half && ((elapsed / half) & 1u))
                v |= 0x20u;
        } else {
            v |= 0x20u;                    /* gate low: OUT idles high */
        }
        return v;
    }

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
    case 0x3BA:
        if (!vga_mono_decoded()) return 0xFFu;
        /* fall through */
    case 0x3DA: {
        unsigned v = 0;
        vga_regs.attr_flipflop = 0;      /* the documented re-sync */
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
    case 0x3B4:
        if (!vga_mono_decoded()) return 0xFFu;
        /* fall through */
    case 0x3D4:
        return vga_regs.crtc_idx;
    case 0x3B5:
        if (!vga_mono_decoded()) return 0xFFu;
        /* fall through */
    case 0x3D5:
        return vga_regs.crtc[vga_regs.crtc_idx & (VGA_CRTC_REGS - 1)];
    case 0x3C4:
        return vga_regs.seq_idx;
    case 0x3C5:
        return vga_regs.seq[vga_regs.seq_idx & (VGA_SEQ_REGS - 1)];
    case 0x3CE:
        return vga_regs.gc_idx;
    case 0x3CF:
        return vga_regs.gc[vga_regs.gc_idx & (VGA_GC_REGS - 1)];
    case 0x3C0:
        return vga_regs.attr_idx;
    case 0x3C1:
        return vga_regs.attr[vga_regs.attr_idx & (VGA_ATTR_REGS - 1)];
    case 0x3CC:
        return vga_regs.misc;
    case 0x3C2:
        /* Input status 0. Bit 4 is the DAC comparator ("switch sense"), which
           a detection routine reads; 0 is the safe answer. */
        return 0x00u;
    case 0x3D8:
        return x86_io.cga_mode;
    case 0x3D9:
        return x86_io.cga_color_select;

    /* VGA DAC. 0x3C9 is read back by fade loops, which read-modify-write the
       palette, so this has to return what the guest last wrote and not a
       constant. */
    case 0x3C6:
        return x86_vga_dac_get_mask();
    case 0x3C9:
        return x86_vga_dac_read();
    }
    /* An unassigned port floats high, which is what a real bus does and what
       DOS code probing for hardware expects to see. */
#if X86_PROBE
    port_read_unmodelled = 1;
#endif
    return 0xFFFFFFFFu;
}

void x86_port_write(uint32_t port, uint32_t value, uint32_t size)
{
    (void)size;
    io_log(2, port, value);
#if X86_PROBE
    unsigned unmodelled = 0;
#endif
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

    case 0x3B4:
        if (!vga_mono_decoded()) break;
        /* fall through */
    case 0x3D4:
        vga_regs.crtc_idx = (uint8_t)value;
        break;
    case 0x3B5:
        if (!vga_mono_decoded()) break;
        /* fall through */
    case 0x3D5:
        vga_regs.crtc[vga_regs.crtc_idx & (VGA_CRTC_REGS - 1)] = (uint8_t)value;
        break;
    case 0x3C4:
        vga_regs.seq_idx = (uint8_t)value;
        break;
    case 0x3C5:
        vga_regs.seq[vga_regs.seq_idx & (VGA_SEQ_REGS - 1)] = (uint8_t)value;
        break;
    case 0x3CE:
        vga_regs.gc_idx = (uint8_t)value;
        break;
    case 0x3CF:
        vga_regs.gc[vga_regs.gc_idx & (VGA_GC_REGS - 1)] = (uint8_t)value;
        break;
    case 0x3C0:
        if (!vga_regs.attr_flipflop)
            vga_regs.attr_idx = (uint8_t)value;
        else
            vga_regs.attr[vga_regs.attr_idx & (VGA_ATTR_REGS - 1)] =
                (uint8_t)value;
        vga_regs.attr_flipflop ^= 1u;
        break;
    case 0x3C2:
        vga_regs.misc = (uint8_t)value;
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

    /*
     * VGA DAC. A hook on the OUT path and not an io_ports[] latch: 0x3C9 is a
     * STREAM. The guest writes the index once to 0x3C8 and then 768
     * consecutive bytes here, three per entry, against a counter that
     * auto-increments. Modelled as a latch, 767 of those 768 are lost and the
     * whole screen comes out one colour.
     */
    case 0x3C6:
        x86_vga_dac_set_mask((uint8_t)value);
        break;
    case 0x3C7:
        x86_vga_dac_set_read_index((uint8_t)value);
        break;
    case 0x3C8:
        x86_vga_dac_set_write_index((uint8_t)value);
        break;
    case 0x3C9:
        x86_vga_dac_write((uint8_t)value);
        break;
    default:
        /* A write to a port nothing models is DROPPED. That is usually right,
           but it is not nothing: a guest programming a sound card or a mouse
           gets silence and no error, and the report has to be able to name it.*/
#if X86_PROBE
        unmodelled = 1;
#endif
        break;
    }
    X86_PROBE_PORT(X86_PROBE_KIND_OUT, (uint16_t)port, value, unmodelled);
}
