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
#include "x86_video_planar.h"
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

/* ---------------------------------------------------------------------------
 * A PIT WHOSE COUNTERS CAN BE READ BACK, AND WHICH ARE COUNTING.
 *
 * Until this existed, `IN 0x40` was not in the read switch at all (it fell
 * through to the open-bus 0xFF) and `IN 0x42` returned the static reload byte.
 * A clock that never moves is not a small inaccuracy: Michael Abrash's
 * Precision Zen Timer -- which is what TOPBENCH times every one of its five
 * suites with (references/TOPBENCH/_MBLOCK.INC:13, _EA.INC:8,
 * _OPCODES.INC:27, _VIDMEM.INC:14, _3DGAMES.INC:107) -- programs CHANNEL 0 to
 * mode 2 with a count of 0, latches it with `OUT 43h,00h`, and reads the two
 * bytes back off port 0x40 (references/TOPBENCH/ZTIMER11/PZTIMER.ASM:246-266).
 * Alley Cat reads port 0x40 two million times in 28 seconds for the same
 * reason. This is a general gap, not a benchmark's quirk.
 *
 * WHAT TIME IS, HERE. The interpreter has no cycle counter; what it has is
 * `pit_counter`, the per-slice INSTRUCTION budget it decrements once per
 * instruction. So guest time is instructions converted at the emulated CPU's
 * declared speed -- guest_kips, the same knob the pacing loop uses (main.c
 * "TWO PACING MODES"). In a paced profile that is exact by construction: the
 * machine claims to be a 386DX-33 and its PIT agrees. Unthrottled (kips == 0)
 * there is no declared speed, so a fixed nominal stands in; see below.
 *
 * The alternative -- driving the counter off HOST wall-clock time -- was
 * rejected. The Zen Timer overflows past 54.9 ms and reports a sentinel, and
 * an emulated suite that takes a real 386 two milliseconds takes this machine
 * far longer than that. Wall-clock would have been "honest" only in the sense
 * of reporting how slow the EMULATOR is, which is not what a guest benchmark
 * is asking. What it asks is how fast the machine it is running on claims to
 * be, and guest_kips is that number.
 */
#define PIT_NOMINAL_KIPS 5500u  /* measured silicon rate; see main.c KIPS_* */

uint32_t x86_pit_kips = 0;      /* published by main.c from guest_kips      */

static uint64_t pit_frac;           /* insns*PIT_HZ not yet worth a tick    */
static uint32_t pit_now;            /* free-running 1.193182 MHz tick count */
static uint32_t pit_last_counter;   /* pit_counter at the last accounting   */

/*
 * Fold everything the guest has executed since the last call into `pit_now`.
 *
 * The delta has to tolerate a RELOAD, because the interpreter reloads
 * pit_counter from pit_reload in-guest when it delivers an IRQ
 * (cpu_exec.S .handle_pit_or_irq) and carries on inside the same slice. A
 * counter that went UP therefore means "it wrapped", and the instructions run
 * are what was left before the wrap plus what has gone since it.
 *
 * Every host write to pit_counter that is NOT the guest spending its budget --
 * arming an IRQ drops it to 1 -- must go through x86_pit_counter_forced(),
 * or that drop is counted as a whole slice of guest time that never ran.
 */
void x86_pit_sync(void)
{
    uint32_t cur    = cpu_state_ptr->pit_counter;
    uint32_t reload = cpu_state_ptr->pit_reload;
    uint32_t d;

    if (cur <= pit_last_counter)
        d = pit_last_counter - cur;
    else if (reload >= cur)
        d = pit_last_counter + (reload - cur);
    else
        d = 0;                       /* incoherent: charge nothing */
    pit_last_counter = cur;
    if (!d)
        return;
    /* A slice is bounded (main.c's MAX_SLICE_CEIL); this bound only stops a
       stale reload of 0xFFFFFFFF at reset banking an hour of guest time. */
    if (d > 1000000u)
        d = 1000000u;

    uint32_t kips = x86_pit_kips ? x86_pit_kips : PIT_NOMINAL_KIPS;
    if (kips < 100u)     kips = 100u;
    if (kips > 100000u)  kips = 100000u;

    pit_frac += (uint64_t)d * (uint64_t)PIT_HZ;
    uint64_t den = (uint64_t)kips * 1000u;
    pit_now += (uint32_t)(pit_frac / den);
    pit_frac %= den;
}

void x86_pit_counter_forced(uint32_t v)
{
    x86_pit_sync();
    cpu_state_ptr->pit_counter = v;
    pit_last_counter = v;
}

uint32_t x86_pit_ticks(void)
{
    x86_pit_sync();
    return pit_now;
}

/* Per-channel counter state. Channel 1 (DRAM refresh) is carried so the array
   indexes by channel number and nothing has to remember that 2 is really 1. */
static uint32_t pit_ch_start[3];    /* pit_now when the count was loaded   */
static uint16_t pit_ch_latch[3];    /* the frozen count from OUT 43h,xx00b */
static uint8_t  pit_ch_latched[3];  /* a latch is pending                  */
static uint8_t  pit_ch_rdphase[3];  /* lo/hi READ phase (its own, not the
                                       write phase -- a guest may be part way
                                       through a write pair and still latch) */
/*
 * A BOOTED MACHINE, not a blank one. These defaults are load-bearing: a guest
 * that never writes a control word -- Alley Cat reads port 0x40 six million
 * times in one run and programs it never -- still expects the counter the BIOS
 * left running. Zeroed, pit_ch_mode[0] would be mode 0, the one-shot: the
 * count would reach terminal count after one period and read 0 forever after,
 * which is a clock that stopped rather than one that never started, and just
 * as useless. Channel 0 is mode 3 at 65536, channel 1 is the mode 2 refresh
 * counter, and channel 2 is stopped because its gate is (port 0x61 bit 0).
 */
static uint8_t  pit_ch_mode[3]    = { 3, 2, 3 };
static uint8_t  pit_ch_running[3] = { 1, 1, 0 };
static uint16_t pit_ch_frozen[3]  = { 0xFFFF, 0xFFFF, 0xFFFF };

static uint32_t pit_ch_reload(unsigned ch)
{
    if (ch == 0)
        return x86_io.pit0_reload ? x86_io.pit0_reload : 65536u;
    return x86_io.pit2_reload ? x86_io.pit2_reload : 65536u;
}

/*
 * The counter as the guest would read it right now.
 *
 * Modes 0 and 1 are one-shots: they count down to zero and STOP there, which
 * is the terminal count software polls for. Everything else is treated as
 * periodic (mode 2/3), which is what a divisor is for -- the count wraps back
 * to the reload value and keeps going.
 *
 * THE RANGE IS R-1 DOWN TO 0, not R down to 1, and that is not a rounding
 * choice. A reload of 0 means 65536, so "R" is 0x0000 in sixteen bits -- and
 * a guest that latches immediately after loading the count would read 0x0000
 * and then, a few instructions later, 0xFC9A. To software the counter appears
 * to have jumped UP by 64666, which is precisely the "counter going backwards"
 * a Zen Timer cannot survive. The part loads the count on the first CLK after
 * the write and the first value software can observe is already one below it,
 * so R-1 is both what hardware shows and the value that has no wrap artefact.
 */
static uint16_t pit_count(unsigned ch)
{
    uint32_t R = pit_ch_reload(ch);

    if (!pit_ch_running[ch])
        return pit_ch_frozen[ch];

    uint32_t el = pit_now - pit_ch_start[ch];
    if (pit_ch_mode[ch] == 0 || pit_ch_mode[ch] == 1) {
        if (el >= R - 1u)
            return 0;                /* terminal count, and it stays there */
        return (uint16_t)((R - 1u) - el);
    }
    return (uint16_t)((R - 1u) - (el % R));
}

/* A completed count load restarts the channel. */
static void pit_ch_loaded(unsigned ch)
{
    x86_pit_sync();
    pit_ch_start[ch] = pit_now;
    pit_ch_frozen[ch] = (uint16_t)((pit_ch_reload(ch) - 1u) & 0xFFFFu);
    pit_ch_running[ch] = (ch == 2) ? (uint8_t)(x86_io.port61 & 1u) : 1u;
}

static uint32_t pit_read_data(unsigned ch)
{
    x86_pit_sync();
    uint16_t c = pit_ch_latched[ch] ? pit_ch_latch[ch] : pit_count(ch);
    uint8_t acc = (ch == 0) ? x86_io.pit0_access : x86_io.pit2_access;
    uint8_t v;
    int done;

    if (acc == 1) {
        v = (uint8_t)(c & 0xFFu); done = 1;
    } else if (acc == 2) {
        v = (uint8_t)(c >> 8); done = 1;
    } else if (!pit_ch_rdphase[ch]) {
        v = (uint8_t)(c & 0xFFu); pit_ch_rdphase[ch] = 1; done = 0;
    } else {
        v = (uint8_t)(c >> 8); pit_ch_rdphase[ch] = 0; done = 1;
    }
    if (done)
        pit_ch_latched[ch] = 0;
    return v;
}

/* Channel 2's gate, driven by port 0x61 bit 0. A rising edge restarts the
   count (which is what a real 8253 does in the modes a speaker uses); a
   falling edge freezes it where it stands, so a guest that gates the channel
   off and reads it back sees a stable value rather than a running one. */
static void pit_ch2_gate(unsigned on)
{
    on = on ? 1u : 0u;
    if (on == pit_ch_running[2])
        return;
    x86_pit_sync();
    if (on) {
        pit_ch_start[2] = pit_now;
        pit_ch_running[2] = 1;
    } else {
        pit_ch_frozen[2] = pit_count(2);
        pit_ch_running[2] = 0;
    }
}

/*
 * The 8259A, as far as a READ of it is concerned.
 *
 * This is the other half of the 65535 story and, on its own, the whole of it.
 * PZTimerOff decides whether the timed interval OVERFLOWED by asking the PIC
 * whether IRQ 0 is pending: `OUT 20h,0Ah` selects the Interrupt Request
 * register and `IN AL,20h` reads it (PZTIMER.ASM:238-247). Port 0x20 was not
 * in the read switch, so it floated to 0xFF, bit 0 came back set, and
 * PZTimerCount() took its overflow path and returned 0xFFFFFFFF
 * (PZTIMER.ASM:488-493) -- which TOPBENCH stores into a `word`
 * (TOPB_DAT.PAS:31-35) and prints as 65535, for every suite, always.
 *
 * The IMR at 0x21 is stored and returned for a different reason: code that
 * saves the mask, masks an IRQ and restores it reads 0x21 first, and 0xFF told
 * it every line was already masked -- which it then wrote back. It is NOT
 * enforced here; delivery is still the one-deep hard_int_pending slot.
 */
static uint8_t pic_ocw3_read = 2;   /* 2 = IRR, 3 = ISR */
static uint8_t pic_imr = 0xB8;      /* the mask a booted PC/AT BIOS leaves  */

static uint32_t pic_read_irr(void)
{
    uint32_t pending = cpu_state_ptr->hard_int_pending;
    uint32_t r = 0;
    if (pending == 0x08u) r |= 1u;
    if (pending == 0x09u) r |= 2u;
    return r;
}

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

/* ---------------------------------------------------------------------------
 * WHAT THE RENDERER AND THE PLANAR ADAPTER READ OUT OF THESE FILES.
 *
 * Accessors, not copies. The whole reason the register file is a faithful store
 * is that ONE place holds each value; a blit that kept its own idea of the row
 * pitch is how a write path and a renderer come to disagree about the same
 * screen (references/8086tiny/docs/video/11-ega-planar.md).
 */

/* Display start, CRTC 0x0C (high) / 0x0D (low), in PLANE OFFSETS. */
unsigned x86_vga_crtc_start(void)
{
    return ((unsigned)vga_regs.crtc[0x0C] << 8) | vga_regs.crtc[0x0D];
}

/*
 * ROW PITCH = 2 x CRTC OFFSET, because the Offset register counts WORDS.
 *
 * A real EGA BIOS writes 0x14 (20) for mode 0Dh and 0Dh is 40 bytes per plane
 * per row; it writes 0x28 (40) for a Mode Y page whose row is 80 plane offsets.
 * This has been got wrong before, in a way that was self-consistent (Offset as
 * BYTES in the C with a compensating value in the BIOS) and was settled only by
 * rendering a frame and looking at it -- an autocorrelation "measurement" had
 * confidently agreed with the mistake. See 11-ega-planar.md, "The Offset unit".
 *
 * A zero Offset would collapse the picture to one row, so a guest that never
 * programs it gets the mode's default rather than nothing; x86_vga_crtc_preset()
 * plants that at the mode set and this is the belt-and-braces half.
 */
unsigned x86_vga_row_pitch(void)
{
    unsigned off = vga_regs.crtc[0x13];
    return off ? off * 2u : 80u;
}

/* Attribute Controller index 0x13, low 3 bits: the horizontal pel pan, in
   pixels. A scrolling EGA title reprograms it every frame. */
unsigned x86_vga_pel_panning(void) { return vga_regs.attr[0x13] & 7u; }

/* ATC palette registers 0x00-0x0F: pixel value -> DAC entry. */
const uint8_t *x86_vga_attr_palette(void) { return vga_regs.attr; }

/*
 * Plant the CRTC geometry a real VGA BIOS writes for `mode`.
 *
 * Chained 13h does not need it -- its blit is a straight copy. A title that
 * unchains without programming the Offset itself does, or it inherits mode
 * 0Dh's 20 and renders at half the stride. Values are the ones a VGA BIOS
 * leaves behind: 0x28 (40 words = 80 bytes) for 13h, 0x14 (20 words = 40
 * bytes) for 0Dh, and a display start of zero for both.
 */
void x86_vga_crtc_preset(uint8_t mode)
{
    vga_regs.crtc[0x0C] = 0;
    vga_regs.crtc[0x0D] = 0;
    vga_regs.crtc[0x13] = (mode == 0x13) ? 0x28 : 0x14;
    /* Identity attribute palette. Pixel value n -> DAC entry n, which in a VGA
       DAC IS the sixteen EGA colours -- IBM's 00 01 02 03 04 05 14 07 38.. are
       indices into a 64-entry EGA DAC layout and would recolour every EGA-16
       title against our 256-colour table. A deviation from IBM's NUMBERS, not
       from the hardware; it is also verbatim what a real VGA reads back in
       mode 13h. */
    for (unsigned i = 0; i < 16; i++)
        vga_regs.attr[i] = (uint8_t)i;
    vga_regs.attr[0x13] = 0;            /* no pel panning until asked */
}

/*
 * The Sequencer's Chain-4 bit may only be believed in mode 13h.
 *
 * Unchaining is something software does to itself long after the BIOS mode set,
 * so it has to be driven off the real register -- but a blind I/O probe walking
 * the Sequencer (FreeDOS does exactly this at boot, writing 0xFF everywhere)
 * would otherwise reinterpret whatever mode is running as planar. The BDA is
 * the authority because it is what the mode set publishes; the renderer's own
 * mode byte is the fallback for a link with no BDA page backed.
 */
static int vga_in_mode13(void)
{
    uint8_t bmode; uint16_t bcols;

    if (x86_bios_bda_video(&bmode, &bcols))
        return bmode == 0x13;
    return x86_video_mode() == 0x13;
}

static void pit_write_control(uint8_t v)
{
    unsigned ch = v >> 6;
    if (ch != 0 && ch != 2)
        return;                     /* channel 1 is DRAM refresh: nothing here */
    unsigned access = (v >> 4) & 3;
    /* Access field 0 is the COUNTER LATCH command, not an access mode: it
       freezes the count for reading and leaves the programmed mode alone.
       Storing it as the mode put the next 0x42 write through the lo/hi path
       with a stale half-pair flag, which is one way a tone comes out an octave
       or more off.

       LATCHING IS WHAT MAKES THE COUNTER READABLE AT ALL. The two reads that
       follow must see ONE consistent 16-bit value, not a counter that moved
       between the low byte and the high byte -- a Zen Timer reading a lo/hi
       pair straddling a borrow computes a count that is off by 256. A second
       latch before the pair has been consumed is ignored, as on the part. */
    if (access == 0) {
        x86_pit_sync();
        if (!pit_ch_latched[ch]) {
            pit_ch_latch[ch] = pit_count(ch);
            pit_ch_latched[ch] = 1;
            pit_ch_rdphase[ch] = 0;
        }
        return;
    }
    /* Modes 6 and 7 alias 2 and 3 on the part. */
    unsigned mode = (v >> 1) & 7u;
    if (mode > 5u)
        mode -= 4u;
    pit_ch_mode[ch] = (uint8_t)mode;
    pit_ch_rdphase[ch] = 0;
    pit_ch_latched[ch] = 0;
    if (ch == 0) {
        x86_io.pit0_access = (uint8_t)access;
        x86_io.pit0_latched = 0;
        return;
    }
    x86_io.pit2_access = (uint8_t)access;
    x86_io.pit2_latched = 0;        /* a control word resets the pair phase */
}

/* Channel 0's data port. Same three access modes as channel 2 and for the same
   reasons; the divisor it leaves behind is read by the frame loop, which is
   where the guest's tick rate actually comes from. */
static void pit_write_ch0(uint8_t v)
{
    int loaded = 1;                  /* the count is complete after this byte */
    switch (x86_io.pit0_access) {
    case 1:                          /* lo byte only: the HIGH byte STAYS */
        x86_io.pit0_reload = (uint16_t)((x86_io.pit0_reload & 0xFF00) | v);
        break;
    case 2:                          /* hi byte only: the LOW byte STAYS  */
        x86_io.pit0_reload = (uint16_t)((v << 8) | (x86_io.pit0_reload & 0xFF));
        break;
    default:                         /* lo then hi */
        if (!x86_io.pit0_latched) {
            x86_io.pit0_reload = (uint16_t)((x86_io.pit0_reload & 0xFF00) | v);
            x86_io.pit0_latched = 1;
            loaded = 0;              /* half a count is not a count */
        } else {
            x86_io.pit0_reload = (uint16_t)((v << 8) | (x86_io.pit0_reload & 0xFF));
            x86_io.pit0_latched = 0;
        }
        break;
    }
    if (loaded)
        pit_ch_loaded(0);
    x86_pit0_writes++;
}

/*
 * The system timer's divisor right now. A reload of 0 means the full 16-bit
 * period, i.e. 65536 -- which is also the boot value, so a guest that never
 * touches channel 0 gets exactly the 18.2065 Hz it always got.
 */
volatile uint32_t x86_pit0_writes = 0;

uint32_t x86_pit0_divisor(void)
{
    return x86_io.pit0_reload ? x86_io.pit0_reload : 65536u;
}

static void pit_write_ch2(uint8_t v)
{
    int loaded = 1;
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
            loaded = 0;
        } else {
            x86_io.pit2_reload = (uint16_t)((v << 8) | (x86_io.pit2_reload & 0xFF));
            x86_io.pit2_latched = 0;
        }
        break;
    }
    if (loaded)
        pit_ch_loaded(2);
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
        x86_pit_counter_forced(1);   /* NOT a raw store: see x86_pit_sync() */
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

/* ---------------------------------------------------------------------------
 * SOUND: the OPL2 (AdLib) timer registers, and the Sound Blaster DSP's RESET
 * HANDSHAKE. THIS IS A DETECTION MODEL AND NOTHING ELSE -- IT PRODUCES NO
 * SAMPLES, and every arm below is written so a guest can tell that.
 *
 * WHAT THE TITLES ACTUALLY PROBE (measured, X86_PROBE=1 + probe_report.py,
 * gwemu, this tree -- not inferred from a spec):
 *
 *   WOLF3D   IN 0x388 x348, OUT 0x388/0x389 x6 each.  NOTHING on 0x22x.
 *   KEEN4    IN 0x388 x348, OUT 0x388/0x389 x6 each.  NOTHING on 0x22x.
 *   POP      the full SB BASE SCAN: OUT 0x226 x3 (DSP reset), IN 0x22E x32
 *            (data-available poll), IN 0x22A x33 (the 0xAA), IN 0x228 x94 and
 *            OUT 0x220/0x221/0x228/0x229 (the FM aliases), then 0x240, 0x280
 *            and 0x2C0 in turn when 0x220 does not answer.
 *   ALLEYCAT, SIMCITY, TOPBENCH, JAZZ, TYRIAN, MORTAL_KOMBAT, EPIC_PINBALL
 *            NOTHING -- the last four die before they get anywhere near it.
 *
 * So the shape is: 348 status reads is an AdLib TIMER test spinning on a port
 * that floated 0xFF, and 32 polls plus 33 data reads is a DSP reset handshake
 * that never saw its 0xAA. Both are answered here.
 *
 * THE FM REGISTER FILE IS DELIBERATELY NOT STORED. On a real YM3812 those
 * registers are WRITE-ONLY -- there is no readback -- so dropping them loses
 * nothing a guest can observe, and storing them would only invite someone to
 * believe a synthesiser is implied. The two TIMERS are different: their expiry
 * is readable at 0x388 bit 6/5 and IS the detection test.
 *
 * ONE CARD, AT ONE BASE. 0x240/0x280/0x2C0 stay absent so a scanning title
 * finds exactly one, at 0x220, the address every DOS title defaults to.
 *
 * WHAT IS NOT HERE, AND WHY IT IS ABSENT RATHER THAN FAKED: the mixer chip
 * (0x224/0x225), DMA, and the IRQ. We cannot deliver a DMA-completion IRQ 5,
 * so a transfer would be started and never finish. The DSP therefore ACCEPTS
 * a transfer command's bytes -- refusing them mid-command desynchronises the
 * command stream, which is worse -- and counts them in x86_sb_dma_refused.
 * That counter is the honest statement "a title asked for digitised audio and
 * got silence"; read it before believing any title is audio-clean.
 *
 * AND `BLASTER` IS DELIBERATELY NOT IN THE ENVIRONMENT (x86_dos.c builds
 * "PATH=\0COMSPEC=..." and nothing else). Wolfenstein 3D reads BLASTER to find
 * the card and does not probe 0x22x without it -- which is exactly what the
 * measurement above shows. Setting it would make WOLF3D start a DMA transfer
 * and wait for an IRQ that cannot arrive. Adding BLASTER is the LAST step of
 * sound support, not the first.
 */
#define SB_BASE       0x220u
#define OPL_T1_STEP   96u    /* 80 us at 1.193182 MHz -- 95.45, rounded  */
#define OPL_T2_STEP   382u   /* 320 us                                   */

/* Published for the tests and for probe.py --watch. */
volatile uint32_t x86_opl_writes;        /* register writes accepted        */
volatile uint32_t x86_opl_status_reads;  /* status-port reads answered      */
volatile uint32_t x86_opl_timer_expiry;  /* times a timer flag came up set  */
volatile uint32_t x86_sb_resets;         /* completed 1-then-0 handshakes   */
volatile uint32_t x86_sb_cmds;           /* DSP command bytes accepted      */
volatile uint32_t x86_sb_cmds_unknown;   /* accepted, no reply, no effect   */
volatile uint32_t x86_sb_dma_refused;    /* transfers asked for, none run   */
volatile uint32_t x86_sb_reads_empty;    /* data reads with nothing queued  */

static struct {
    uint8_t  idx;             /* the register index latch                 */
    uint8_t  t1_preset, t2_preset;
    uint8_t  ctrl;            /* register 4 as last written (minus 0x80)  */
    uint8_t  t1_run, t2_run;
    uint32_t t1_start, t2_start;   /* x86_pit_ticks() when started        */
    uint8_t  status_sticky;   /* flags latched until an IRQ-reset write   */
} opl;

static struct {
    uint8_t  reset_armed;     /* a write with bit 0 set is pending a 0    */
    uint8_t  out[8], out_head, out_tail;
    uint8_t  cmd, args_left, arg0;
    uint8_t  test_reg, speaker_on;
} dsp;

static void opl_push_status(void)
{
    uint32_t now = x86_pit_ticks();
    uint8_t  f   = 0;

    /* A timer's flag comes up when its period has elapsed AND it is unmasked:
       control bit 6 masks timer 1, bit 5 masks timer 2. The flags are STICKY
       -- only a write of 0x80 to register 4 clears them -- because the whole
       detection sequence is "start it, spin, read it back". */
    if (opl.t1_run && !(opl.ctrl & 0x40u) &&
        (uint32_t)(now - opl.t1_start) >=
            (uint32_t)(256u - opl.t1_preset) * OPL_T1_STEP)
        f |= 0x40u;
    if (opl.t2_run && !(opl.ctrl & 0x20u) &&
        (uint32_t)(now - opl.t2_start) >=
            (uint32_t)(256u - opl.t2_preset) * OPL_T2_STEP)
        f |= 0x20u;
    if (f && !(opl.status_sticky & f))
        x86_opl_timer_expiry++;
    opl.status_sticky |= f;
}

static uint32_t opl_read_status(void)
{
    x86_opl_status_reads++;
    opl_push_status();
    /* Bit 7 mirrors "some timer fired". The low bits read 0x06 on a YM3812 and
       0x00 on a YMF262 -- that pair IS the OPL2/OPL3 test, and we are an OPL2,
       so 0x06 is the answer that matches what the rest of this claims. */
    return (opl.status_sticky ? (0x80u | opl.status_sticky) : 0u) | 0x06u;
}

static void opl_write(uint32_t port, uint8_t v)
{
    if (!(port & 1u)) {              /* 0x388 / 0x220 / 0x228: index latch */
        opl.idx = v;
        return;
    }
    x86_opl_writes++;
    switch (opl.idx) {
    case 0x02: opl.t1_preset = v; break;
    case 0x03: opl.t2_preset = v; break;
    case 0x04:
        if (v & 0x80u) {             /* IRQ RESET: clears the flags, only  */
            opl.status_sticky = 0;
            return;
        }
        opl.ctrl = v;
        if ((v & 0x01u) && !opl.t1_run) opl.t1_start = x86_pit_ticks();
        if ((v & 0x02u) && !opl.t2_run) opl.t2_start = x86_pit_ticks();
        opl.t1_run = (uint8_t)(v & 0x01u);
        opl.t2_run = (uint8_t)((v >> 1) & 0x01u);
        break;
    default:
        /* Write-only on the real part; see the header. Counted, not stored. */
        break;
    }
}

static void dsp_push(uint8_t v)
{
    uint8_t next = (uint8_t)((dsp.out_tail + 1u) & 7u);
    if (next == dsp.out_head)
        return;
    dsp.out[dsp.out_tail] = v;
    dsp.out_tail = next;
}

static uint32_t dsp_read_data(void)
{
    if (dsp.out_head == dsp.out_tail) {
        x86_sb_reads_empty++;
        return 0xFFu;                /* an empty DSP buffer floats high    */
    }
    uint8_t v = dsp.out[dsp.out_head];
    dsp.out_head = (uint8_t)((dsp.out_head + 1u) & 7u);
    return v;
}

/*
 * How many ARGUMENT bytes follow a command. Getting this wrong is the one way
 * this block can wedge a guest: an argument mistaken for a command makes every
 * later byte a command, and a command mistaken for an argument swallows the
 * next one. Only the commands a DSP 2.01 answers are listed; anything else
 * takes no arguments, which is true of every remaining 2.x command.
 */
static uint8_t dsp_cmd_args(uint8_t c)
{
    switch (c) {
    case 0x10:                        /* direct DAC output                 */
    case 0x40:                        /* time constant                     */
    case 0xE0:                        /* DSP identify                      */
    case 0xE4:                        /* write test register               */
        return 1;
    case 0x14: case 0x15:             /* 8-bit single-cycle DMA output     */
    case 0x16: case 0x17:             /* 2-bit ADPCM                       */
    case 0x24:                        /* 8-bit DMA input                   */
    case 0x48:                        /* set DMA block size                */
        return 2;
    default:
        return 0;
    }
}

static void dsp_exec(uint8_t c, uint8_t a0)
{
    (void)a0;
    switch (c) {
    case 0xE1:                        /* DSP version. 2.01: a plain SB 2.0 */
        dsp_push(2); dsp_push(1);
        break;
    case 0xE0:                        /* identify: the argument, inverted  */
        dsp_push((uint8_t)~dsp.arg0);
        break;
    case 0xE4: dsp.test_reg = dsp.arg0; break;
    case 0xE8: dsp_push(dsp.test_reg); break;
    case 0xE3:
        /* The copyright string. A DSP 2.01 does not answer this at all, but a
           reader loops until it sees a NUL and silence there is a HANG, so we
           answer with the terminator and no string. Nothing can be computed
           from an empty string; something can be computed from an invented
           one, which is why there is not one here. */
        dsp_push(0);
        break;
    case 0xD1: dsp.speaker_on = 1; break;
    case 0xD3: dsp.speaker_on = 0; break;
    case 0xD8: dsp_push(dsp.speaker_on ? 0xFFu : 0x00u); break;
    case 0x10: case 0x14: case 0x15: case 0x16: case 0x17: case 0x24:
        x86_sb_dma_refused++;         /* accepted, consumed, NOT PLAYED    */
        break;
    case 0x40: case 0x48:             /* rate/block size: no audio, no-op  */
    case 0xD0: case 0xD4:             /* pause/continue a transfer         */
        break;
    default:
        x86_sb_cmds_unknown++;
        break;
    }
}

static void dsp_write_cmd(uint8_t v)
{
    if (dsp.args_left) {
        if (dsp.args_left == dsp_cmd_args(dsp.cmd))
            dsp.arg0 = v;
        if (--dsp.args_left == 0)
            dsp_exec(dsp.cmd, dsp.arg0);
        return;
    }
    x86_sb_cmds++;
    dsp.cmd = v;
    dsp.args_left = dsp_cmd_args(v);
    if (!dsp.args_left)
        dsp_exec(v, 0);
}

static void dsp_write_reset(uint8_t v)
{
    if (v & 1u) {
        dsp.reset_armed = 1;
        return;
    }
    if (!dsp.reset_armed)
        return;                       /* a lone 0 is not a reset           */
    dsp.reset_armed = 0;
    dsp.out_head = dsp.out_tail = 0;
    dsp.args_left = 0;
    dsp.speaker_on = 0;
    dsp_push(0xAAu);                  /* THE handshake: "a card is here"   */
    x86_sb_resets++;
}

/* 0x388/0x389 is the AdLib pair; 0x2x0/0x2x1 and 0x2x8/0x2x9 are its aliases
   at the SB base, which is why POP probes 0x220, 0x221, 0x228 and 0x229. */
static int opl_port(uint32_t p)
{
    return p == 0x388u || p == 0x389u ||
           p == SB_BASE || p == SB_BASE + 1u ||
           p == SB_BASE + 8u || p == SB_BASE + 9u;
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
    /* The counters. Not the reload value: see the PIT clock block above. */
    case 0x40:
        return pit_read_data(0);
    case 0x42:
        return pit_read_data(2);
    case 0x20:
        return (pic_ocw3_read == 3) ? 0u : pic_read_irr();
    case 0x21:
        return pic_imr;
    case 0x60:
        /* 0x64 command 0xD0 is "read output port", and its answer comes back
           HERE, not on 0x64. A guest that reads the port, flips bit 1 and
           writes it back through 0xD1 is the classic A20 sequence; answering
           it with a scancode makes the guest write a scancode into the output
           port and mask A20 by accident. */
        if (x86_io.kbd_cmd == 0xD0u) {
            x86_io.kbd_cmd = 0;
            /* Bit 1 is answered from the GATE, not from the latch, so the
               0xDD/0xDF and port 0x92 doors are visible through this one. Two
               places that can move A20 and one that reports a stale copy of it
               is how a guest concludes its write did not take. */
            return (uint32_t)((x86_io.kbd_outport & (uint32_t)~0x03u) |
                              (x86_io.a20_open ? 0x02u : 0x00u) | 0x01u);
        }
        return kbd_read_data();
    case 0x92:
        /* System Control Port A. Bit 1 IS the fast A20 gate, and it is the
           door most 386-era code uses because it costs two instructions
           instead of an 8042 handshake. */
        return (uint32_t)((x86_io.port92 & (uint32_t)~0x02u) |
                          (x86_io.a20_open ? 0x02u : 0x00u));
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
            /* Now taken from the CHANNEL 2 COUNTER rather than from a shape
               fitted to the instruction budget: OUT is high for the first half
               of the period in mode 3, and the counter that decides which half
               we are in is the same one the guest can read back on 0x42. A
               detection routine that ungates the channel and waits for bit 5
               to toggle is therefore watching the real thing. */
            x86_pit_sync();
            uint32_t R = pit_ch_reload(2);
            if (pit_count(2) > (R >> 1))
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
    /* ---- sound: see the block above. Detection only, no audio. -------- */
    case 0x388: case 0x220: case 0x228:
        return opl_read_status();
    case 0x389: case 0x221: case 0x229:
        return 0xFFu;                /* FM data is WRITE-ONLY on a YM3812 */
    case SB_BASE + 0xA:              /* DSP read data                     */
        return dsp_read_data();
    case SB_BASE + 0xC:              /* write-buffer status: b7=0 = ready */
        return 0x7Fu;
    case SB_BASE + 0xE:              /* read-buffer status: b7=1 = data   */
        return (dsp.out_head != dsp.out_tail) ? 0xFFu : 0x7Fu;

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

/*
 * A WORD `OUT dx, ax` TO AN INDEX/DATA PAIR IS TWO WRITES, AND DROPPING AH IS
 * THE SINGLE MOST EXPENSIVE BUG THIS FILE CAN HAVE.
 *
 * Real software almost never writes these registers as two byte OUTs. It writes
 * `mov ax,0x0604 / out dx,ax` -- index in AL, data in AH, one instruction. The
 * reference port shipped a byte-only path and measured the cost when it finally
 * built a test that drove the adapter the way software does: dropping AH failed
 * 56,767 of 64,000 pixels. Wolfenstein 3-D unchains with exactly this form.
 *
 * Only the four index/data pairs are split. The DAC stream at 0x3C9 must NOT
 * be, and neither must 0x3C0, which is one port sequenced by a flip-flop rather
 * than two ports.
 */
static int vga_index_port(uint32_t port)
{
    return port == 0x3C4u || port == 0x3CEu || port == 0x3D4u || port == 0x3B4u;
}

void x86_port_write(uint32_t port, uint32_t value, uint32_t size)
{
    if (size >= 2u && vga_index_port(port)) {
        x86_port_write(port, value & 0xFFu, 1);
        x86_port_write(port + 1u, (value >> 8) & 0xFFu, 1);
        return;
    }
    (void)size;
    io_log(2, port, value);
#if X86_PROBE
    unsigned unmodelled = 0;
#endif
    switch (port) {
    case 0x40:
        pit_write_ch0((uint8_t)value);
        break;
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
        pit_ch2_gate(value & 1u);    /* bit 0 IS channel 2's gate */
        spk_event(port, value);
        break;
    case 0x20:
        /* EOI still has nothing to model while delivery is a one-deep slot.
           OCW3 does: bit 3 set with bit 4 clear selects which register the
           NEXT read of this port returns, and reading the IRR is how the Zen
           Timer detects overflow. */
        if ((value & 0x18u) == 0x08u)
            pic_ocw3_read = (uint8_t)(value & 3u);
        break;
    case 0x21:                       /* IMR: stored and read back, not enforced */
        pic_imr = (uint8_t)value;
        break;

    /*
     * The 8042, as far as the A20 GATE is concerned -- and no further.
     *
     * Keyboard commands are still accepted and ignored, exactly as before;
     * what is modelled here is the output port, because bit 1 of it is A20 and
     * A20 decides whether this machine has memory above 1 MB. Three commands
     * matter and all three are in real BIOSes and real extenders:
     *
     *   0xD0  read output port  -> the answer arrives on 0x60 (see the read side)
     *   0xD1  write output port -> the NEXT byte written to 0x60 is the value
     *   0xDD / 0xDF             -> disable / enable A20 outright
     *
     * Anything else clears the pending-argument latch rather than leaving it
     * armed: a stale 0xD1 would turn the guest's next keyboard command byte
     * into an output-port write, which silently masks A20 mid-run.
     */
    case 0x64:
        switch (value & 0xFFu) {
        case 0xD0:
        case 0xD1:
            x86_io.kbd_cmd = (uint8_t)value;
            break;
        case 0xDD:
            x86_io.kbd_cmd = 0;
            x86_a20_set(0);
            break;
        case 0xDF:
            x86_io.kbd_cmd = 0;
            x86_a20_set(1);
            break;
        default:
            x86_io.kbd_cmd = 0;      /* controller command: accepted, ignored */
            break;
        }
        break;

    case 0x60:
        /* Only meaningful as the argument to a pending 0xD1. A bare write to
           0x60 is a keyboard command (set LEDs, set typematic) and stays
           dropped -- modelling those is a separate piece of work with its own
           ACK protocol, and half-modelling it is worse than not. */
        if (x86_io.kbd_cmd == 0xD1u) {
            x86_io.kbd_cmd = 0;
            x86_io.kbd_outport = (uint8_t)value;
            x86_a20_set((value & 0x02u) != 0u);
        }
        break;

    case 0x92:
        /* The fast A20 gate. Bit 0 is FAST RESET and is deliberately NOT
           modelled as a reset -- a guest that pulses it expects to reboot, and
           silently rebooting the emulator is a far worse answer than ignoring
           it. It is stored so a read-back sees what was written. */
        x86_io.port92 = (uint8_t)value;
        x86_a20_set((value & 0x02u) != 0u);
        break;

    /* ---- sound: see the block above. Detection only, no audio. -------- */
    case 0x388: case 0x389:
    case 0x220: case 0x221: case 0x228: case 0x229:
        opl_write(port, (uint8_t)value);
        break;
    case SB_BASE + 6:                /* DSP reset: 1 then 0 -> 0xAA       */
        dsp_write_reset((uint8_t)value);
        break;
    case SB_BASE + 0xC:              /* DSP command / data                */
        dsp_write_cmd((uint8_t)value);
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
    case 0x3D5: {
        unsigned idx = vga_regs.crtc_idx & (VGA_CRTC_REGS - 1);
        vga_regs.crtc[idx] = (uint8_t)value;
        /* The display start pair is the only CRTC write with a side effect: it
           feeds the Mode Y page-stride learning and it is the present edge. */
        if ((idx & 0x1Eu) == 0x0Cu)
            x86_vga_planar_start_write();
        break;
    }
    case 0x3C4:
        vga_regs.seq_idx = (uint8_t)value;
        break;
    case 0x3C5: {
        unsigned idx = vga_regs.seq_idx & (VGA_SEQ_REGS - 1);
        vga_regs.seq[idx] = (uint8_t)value;
        /* Index 2 is the Map Mask and index 4 bit 3 is Chain-4. The adapter
           keeps no second copy of either -- it is fed from here. */
        if (idx == 2u || (idx == 4u && vga_in_mode13()))
            x86_vga_planar_seq_write(idx, (uint8_t)value);
        break;
    }
    case 0x3CE:
        vga_regs.gc_idx = (uint8_t)value;
        break;
    case 0x3CF: {
        unsigned idx = vga_regs.gc_idx & (VGA_GC_REGS - 1);
        vga_regs.gc[idx] = (uint8_t)value;
        x86_vga_planar_gc_write(idx, (uint8_t)value);
        break;
    }
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
