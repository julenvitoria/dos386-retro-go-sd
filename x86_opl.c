/*
 * x86_opl.c -- YM3812 (OPL2) FM tone generation.
 *
 * ---------------------------------------------------------------------------
 * PROVENANCE. This is a hand-reduced OPL2 of Nuked-OPL3, read out of
 * references/dosbox-x/src/hardware/nukedopl.cpp. Every table and every piece
 * of arithmetic below is cited to that file by line; NOTHING here was derived
 * from first principles, because the envelope curve, the log-sine ROM and the
 * exponent ROM are silicon and there is no way to re-derive them that ends up
 * sounding right.
 *
 *   logsinrom[256]   nukedopl.cpp:105-133   the quarter log-sine ROM
 *   exprom[256]      nukedopl.cpp:201-232   PRE-DOUBLED (see below)
 *   mt[16]           nukedopl.cpp:242       frequency multiple, x2
 *   kslrom[16]       nukedopl.cpp:250       key scale level
 *   kslshift[4]      nukedopl.cpp:254
 *   eg_incstep[4][4] nukedopl.cpp:262
 *   envelope generator      nukedopl.cpp:337-460  (OPL3_EnvelopeCalc)
 *   phase generator         nukedopl.cpp:487-520  (OPL3_PhaseUpdateInc)
 *   operator output         nukedopl.cpp:655-680  (OPL3_SlotGenerate)
 *   feedback                nukedopl.cpp:682-695  (OPL3_SlotCalcFB)
 *   2-op connection modes   nukedopl.cpp:940-970  (OPL3_ChannelSetupAlgBody)
 *   per-sample housekeeping nukedopl.cpp:1519-1612 (OPL3_Generate's tail)
 *   register decode         nukedopl.cpp:1770-1870 (OPL3_WriteReg)
 *
 * The exprom in that file is ALREADY DOUBLED relative to upstream Nuked (its
 * header says so, and exprom[0] reads 0xff4 where upstream reads 0x7fa), so
 * the output step here is `exprom[level & 0xff] >> (level >> 8)` with no <<1.
 * Copying upstream's table into this expression is a silent 6 dB loss.
 *
 * WHERE THE REFERENCES DISAGREE. references/tiny386/fmopl.c is the other OPL2
 * in the tree (MAME 0.37a, Tatsuyuki Satoh). It is a fine core and it was NOT
 * followed, for a reason that is about this target and not about accuracy: its
 * tables are TL_TABLE at 2 x 8192 int32 (64 KB) plus a 2048-entry table of
 * POINTERS into it (8 KB), and it builds them at run time with pow() and
 * sin(). Nuked's formulation needs two 256-entry uint16 ROMs -- 1,024 bytes,
 * const, in flash -- and no float anywhere. On a part where one guest page
 * frame costs 4,104 bytes, that is not a close call. Where the two disagree on
 * behaviour (MAME's envelope is a 4096-step curve table, Nuked's is the chip's
 * own shift-and-add state machine) Nuked is the one that was reverse
 * engineered from die photographs, so it is the one followed.
 *
 * ---------------------------------------------------------------------------
 * THE RATE QUESTION, WHICH IS THE WHOLE DESIGN.
 *
 * docs/03 sized this as 18 operators x 48,000 Hz = 864,000 operator updates
 * per second and concluded "it does not fit at full rate". THAT SIZING USED
 * THE WRONG UNIT. It compared operator updates against ~2.8 M guest
 * INSTRUCTIONS per second, but a guest instruction on this part costs ~57
 * cycles (CLAUDE.md), so 2.8 M insn/s is ~160 M cycles/s of a 280 MHz budget.
 * The comparison to make is against CYCLES:
 *
 *   chip rate 49,716 Hz / 60 fps      = 829 chip samples per frame
 *   x 18 operators                    = 14,916 operator updates per frame
 *   frame budget at 280 MHz, 60 fps   = 4,666,666 cycles
 *
 * so an operator update may cost 60 cycles before FM reaches 20% of a frame,
 * and ~31 cycles to sit at 10%. That is a normal C operator update, not a
 * heroic one -- which is why this file renders at the chip's OWN rate and
 * loses no quality at all, rather than at 12 kHz with the aliasing that would
 * bring. The measured share of frame time is in docs/03 4g and was taken on
 * SILICON with X86_ZONES=1; it is a tier-3 number and nothing else is quoted.
 *
 * TWO THINGS MAKE THAT BUDGET REAL RATHER THAN NOMINAL:
 *
 *   1. AN IDLE CHANNEL COSTS NOTHING. A channel whose two slots are both keyed
 *      off and fully decayed is retired from the loop and is skipped whole --
 *      not skipped per operator, skipped before either operator is touched.
 *      Real AdLib music sounds 2-6 of the 9 channels at a time, so the typical
 *      cost is a third of the 18-operator worst case. x86_opl_slots_run counts
 *      the updates actually performed, so the worst case is observable and not
 *      assumed.
 *   2. NOTHING RUNS WHEN NO MUSIC IS PLAYING. x86_opl_active() is one load and
 *      a test and the render call early-outs on it, so every title that never
 *      writes an FM register pays exactly what it paid before this file
 *      existed.
 *
 * THE RESAMPLER IS THE ONE THE DMA PATH ALREADY HAS. 49,716 Hz into 48,000 Hz
 * is a 16.16 zero-order-hold accumulator carried across frame boundaries,
 * identical in form to x86_sb_render_mix()'s -- one add and one compare per
 * output sample, a chip step only when it crosses. There is no second
 * resampler in this core and this file does not add one.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS DELIBERATELY NOT HERE.
 *
 *   * PERCUSSION MODE (register BDh bit 5). Not synthesised. It needs the
 *     chip's 23-bit noise LFSR plus four special phase paths, and the titles
 *     this core exists for (WOLF3D, KEEN4 -- id's IMF format) are melodic.
 *     When a guest turns it on, channels 6, 7 and 8 are MUTED and
 *     x86_opl_rhythm_muted counts it. Muting is the deliberate choice over
 *     ignoring the bit: with rhythm on, those channels' registers no longer
 *     describe notes, so playing them melodically emits three wrong voices --
 *     which sounds like a broken emulator rather than a missing feature.
 *   * CSM mode (register 08h bit 7), which key-ons every channel off timer 1.
 *     Vanishingly rare and it interacts with the timer model in x86_io.c;
 *     register 08h's NTS bit IS honoured because it changes key scaling on
 *     every note.
 *   * OPL3 ANYTHING. We report YM3812 (x86_io.c returns 0x06 in the status low
 *     bits) and this file must not become a chip we do not claim to be.
 */

#include <string.h>
#include "x86_opl.h"

/* Published counters. */
volatile uint32_t x86_opl_samples;
volatile uint32_t x86_opl_keyons;
volatile uint32_t x86_opl_chip_steps;
volatile uint32_t x86_opl_slots_run;
volatile uint32_t x86_opl_clips;
volatile uint32_t x86_opl_rhythm_muted;

/* ---------------------------------------------------------------------------
 * THE ROMs. `const`, so they are in FLASH and cost no RAM_EMU at all -- 1,024
 * bytes of the guest's 8 MB would have been a quarter of a page frame.
 */

/* nukedopl.cpp:105 -- log2(sin) of the first quarter wave, 4.8 fixed point in
   1/256 dB units, 0x859 at the zero crossing down to 0x000 at the peak. */
static const uint16_t logsinrom[256] = {
    0x859, 0x6c3, 0x607, 0x58b, 0x52e, 0x4e4, 0x4a6, 0x471,
    0x443, 0x41a, 0x3f5, 0x3d3, 0x3b5, 0x398, 0x37e, 0x365,
    0x34e, 0x339, 0x324, 0x311, 0x2ff, 0x2ed, 0x2dc, 0x2cd,
    0x2bd, 0x2af, 0x2a0, 0x293, 0x286, 0x279, 0x26d, 0x261,
    0x256, 0x24b, 0x240, 0x236, 0x22c, 0x222, 0x218, 0x20f,
    0x206, 0x1fd, 0x1f5, 0x1ec, 0x1e4, 0x1dc, 0x1d4, 0x1cd,
    0x1c5, 0x1be, 0x1b7, 0x1b0, 0x1a9, 0x1a2, 0x19b, 0x195,
    0x18f, 0x188, 0x182, 0x17c, 0x177, 0x171, 0x16b, 0x166,
    0x160, 0x15b, 0x155, 0x150, 0x14b, 0x146, 0x141, 0x13c,
    0x137, 0x133, 0x12e, 0x129, 0x125, 0x121, 0x11c, 0x118,
    0x114, 0x10f, 0x10b, 0x107, 0x103, 0x0ff, 0x0fb, 0x0f8,
    0x0f4, 0x0f0, 0x0ec, 0x0e9, 0x0e5, 0x0e2, 0x0de, 0x0db,
    0x0d7, 0x0d4, 0x0d1, 0x0cd, 0x0ca, 0x0c7, 0x0c4, 0x0c1,
    0x0be, 0x0bb, 0x0b8, 0x0b5, 0x0b2, 0x0af, 0x0ac, 0x0a9,
    0x0a7, 0x0a4, 0x0a1, 0x09f, 0x09c, 0x099, 0x097, 0x094,
    0x092, 0x08f, 0x08d, 0x08a, 0x088, 0x086, 0x083, 0x081,
    0x07f, 0x07d, 0x07a, 0x078, 0x076, 0x074, 0x072, 0x070,
    0x06e, 0x06c, 0x06a, 0x068, 0x066, 0x064, 0x062, 0x060,
    0x05e, 0x05c, 0x05b, 0x059, 0x057, 0x055, 0x053, 0x052,
    0x050, 0x04e, 0x04d, 0x04b, 0x04a, 0x048, 0x046, 0x045,
    0x043, 0x042, 0x040, 0x03f, 0x03e, 0x03c, 0x03b, 0x039,
    0x038, 0x037, 0x035, 0x034, 0x033, 0x031, 0x030, 0x02f,
    0x02e, 0x02d, 0x02b, 0x02a, 0x029, 0x028, 0x027, 0x026,
    0x025, 0x024, 0x023, 0x022, 0x021, 0x020, 0x01f, 0x01e,
    0x01d, 0x01c, 0x01b, 0x01a, 0x019, 0x018, 0x017, 0x017,
    0x016, 0x015, 0x014, 0x014, 0x013, 0x012, 0x011, 0x011,
    0x010, 0x00f, 0x00f, 0x00e, 0x00d, 0x00d, 0x00c, 0x00c,
    0x00b, 0x00a, 0x00a, 0x009, 0x009, 0x008, 0x008, 0x007,
    0x007, 0x007, 0x006, 0x006, 0x005, 0x005, 0x005, 0x004,
    0x004, 0x004, 0x003, 0x003, 0x003, 0x002, 0x002, 0x002,
    0x002, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000
};

/* nukedopl.cpp:201 -- 2^(-x) over one octave, and PRE-DOUBLED there, so the
   output expression below carries no <<1. See the provenance note. */
static const uint16_t exprom[256] = {
    0xff4, 0xfea, 0xfde, 0xfd4, 0xfc8, 0xfbe, 0xfb4, 0xfa8,
    0xf9e, 0xf92, 0xf88, 0xf7e, 0xf72, 0xf68, 0xf5c, 0xf52,
    0xf48, 0xf3e, 0xf32, 0xf28, 0xf1e, 0xf14, 0xf08, 0xefe,
    0xef4, 0xeea, 0xee0, 0xed4, 0xeca, 0xec0, 0xeb6, 0xeac,
    0xea2, 0xe98, 0xe8e, 0xe84, 0xe7a, 0xe70, 0xe66, 0xe5c,
    0xe52, 0xe48, 0xe3e, 0xe34, 0xe2a, 0xe20, 0xe16, 0xe0c,
    0xe04, 0xdfa, 0xdf0, 0xde6, 0xddc, 0xdd2, 0xdca, 0xdc0,
    0xdb6, 0xdac, 0xda4, 0xd9a, 0xd90, 0xd88, 0xd7e, 0xd74,
    0xd6a, 0xd62, 0xd58, 0xd50, 0xd46, 0xd3c, 0xd34, 0xd2a,
    0xd22, 0xd18, 0xd10, 0xd06, 0xcfe, 0xcf4, 0xcec, 0xce2,
    0xcda, 0xcd0, 0xcc8, 0xcbe, 0xcb6, 0xcae, 0xca4, 0xc9c,
    0xc92, 0xc8a, 0xc82, 0xc78, 0xc70, 0xc68, 0xc60, 0xc56,
    0xc4e, 0xc46, 0xc3c, 0xc34, 0xc2c, 0xc24, 0xc1c, 0xc12,
    0xc0a, 0xc02, 0xbfa, 0xbf2, 0xbea, 0xbe0, 0xbd8, 0xbd0,
    0xbc8, 0xbc0, 0xbb8, 0xbb0, 0xba8, 0xba0, 0xb98, 0xb90,
    0xb88, 0xb80, 0xb78, 0xb70, 0xb68, 0xb60, 0xb58, 0xb50,
    0xb48, 0xb40, 0xb38, 0xb32, 0xb2a, 0xb22, 0xb1a, 0xb12,
    0xb0a, 0xb02, 0xafc, 0xaf4, 0xaec, 0xae4, 0xade, 0xad6,
    0xace, 0xac6, 0xac0, 0xab8, 0xab0, 0xaa8, 0xaa2, 0xa9a,
    0xa92, 0xa8c, 0xa84, 0xa7c, 0xa76, 0xa6e, 0xa68, 0xa60,
    0xa58, 0xa52, 0xa4a, 0xa44, 0xa3c, 0xa36, 0xa2e, 0xa28,
    0xa20, 0xa18, 0xa12, 0xa0c, 0xa04, 0x9fe, 0x9f6, 0x9f0,
    0x9e8, 0x9e2, 0x9da, 0x9d4, 0x9ce, 0x9c6, 0x9c0, 0x9b8,
    0x9b2, 0x9ac, 0x9a4, 0x99e, 0x998, 0x990, 0x98a, 0x984,
    0x97c, 0x976, 0x970, 0x96a, 0x962, 0x95c, 0x956, 0x950,
    0x948, 0x942, 0x93c, 0x936, 0x930, 0x928, 0x922, 0x91c,
    0x916, 0x910, 0x90a, 0x904, 0x8fc, 0x8f6, 0x8f0, 0x8ea,
    0x8e4, 0x8de, 0x8d8, 0x8d2, 0x8cc, 0x8c6, 0x8c0, 0x8ba,
    0x8b4, 0x8ae, 0x8a8, 0x8a2, 0x89c, 0x896, 0x890, 0x88a,
    0x884, 0x87e, 0x878, 0x872, 0x86c, 0x866, 0x860, 0x85a,
    0x854, 0x850, 0x84a, 0x844, 0x83e, 0x838, 0x832, 0x82c,
    0x828, 0x822, 0x81c, 0x816, 0x810, 0x80c, 0x806, 0x800,
};

/* nukedopl.cpp:242 -- frequency multiple, doubled so 0 can mean 1/2. */
static const uint8_t mt[16] = {
    1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30
};
/* nukedopl.cpp:250/254 -- key scale LEVEL and its per-KSL shift. */
static const uint8_t kslrom[16] = {
    0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64
};
static const uint8_t kslshift[4] = { 8, 1, 2, 0 };
/* nukedopl.cpp:262 -- the sub-sample phase pattern for the fast envelope
   rates (rate_hi >= 12), indexed [rate_lo][eg_timer & 3]. */
static const uint8_t eg_incstep[4][4] = {
    { 0, 0, 0, 0 },
    { 1, 0, 0, 0 },
    { 1, 0, 1, 0 },
    { 1, 1, 1, 0 }
};
/* nukedopl.cpp:273 -- register offset (low 5 bits) -> operator 0..17. The two
   holes at x6/x7 and xE/xF are the chip's, not a table typo: a write there
   reaches no operator at all. */
static const int8_t ad_slot[0x20] = {
    0, 1, 2, 3, 4, 5, -1, -1, 6, 7, 8, 9, 10, 11, -1, -1,
    12, 13, 14, 15, 16, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};

/* Channel c's first operator. Operator n and operator n+3 are a pair, which is
   the 0x00/0x03 offset pattern every AdLib driver writes. */
static const uint8_t ch_op0[9] = { 0, 1, 2, 6, 7, 8, 12, 13, 14 };

enum { EG_ATTACK = 0, EG_DECAY = 1, EG_SUSTAIN = 2, EG_RELEASE = 3 };

/* ---------------------------------------------------------------------------
 * STATE. Sized deliberately: this is RAM_EMU and RAM_EMU is guest page frames.
 * 18 operators x 40 B + 9 channels x 8 B + ~32 B of chip = 824 bytes, which is
 * a fifth of one 4,104-byte frame. The raw 256-byte register file a real card
 * would need is NOT stored -- every write is parsed into the fields below at
 * OUT time, and the port is write-only so nothing can ever read it back.
 */
typedef struct {
    uint32_t pg_phase;        /* 10.22 phase accumulator                    */
    uint32_t pg_inc;          /* per-chip-sample increment, no vibrato      */
    uint16_t eg_rout;         /* 9-bit envelope attenuation, 0 = loudest    */
    uint16_t eg_tl_ksl;       /* (tl << 2) + (ksl >> kslshift), cached      */
    uint16_t eg_out;          /* eg_rout + eg_tl_ksl + tremolo, this sample */
    int16_t  out, prout, fbmod;
    uint8_t  eg_rates[4];     /* AR, DR, SR(=RR or 0), RR                   */
    uint8_t  eg_rate_hi[4], eg_rate_lo[4];
    uint8_t  eg_gen, eg_ks, eg_ksl, pg_reset;
    uint8_t  key;             /* key-on, non-zero while the note is held    */
    uint8_t  reg_mult, reg_ksr, reg_type, reg_vib, reg_am;
    uint8_t  reg_ksl, reg_tl, reg_sl, reg_ar, reg_dr, reg_rr, reg_wf;
    uint8_t  ch;              /* owning channel, 0..8                       */
} opl_slot_t;

typedef struct {
    uint16_t f_num;
    uint8_t  block, ksv, fb, con, keyon, active;
} opl_chan_t;

static struct {
    opl_slot_t slot[18];
    opl_chan_t chan[9];
    uint64_t eg_timer;
    uint32_t timer;
    uint32_t frac;            /* the 16.16 resampler accumulator            */
    int16_t  last;            /* the held chip sample                       */
    uint8_t  eg_add, eg_state, eg_timerrem, eg_timer_lo;
    uint8_t  tremolo, tremolopos, tremoloshift;
    uint8_t  vibpos, vibshift;
    uint8_t  nts;             /* register 08h bit 6, key-split select       */
    uint8_t  wavesel;         /* register 01h bit 5                         */
    uint8_t  rhythm;          /* register BDh low 6 bits                    */
    uint8_t  any_active;      /* the render call's one-load early-out       */
    uint8_t  inited;          /* see the lazy reset in x86_opl_write()      */
} opl;

/* The E0h byte AS WRITTEN, before register 01h's waveform-select enable gates
   it. Kept apart from opl.slot[].reg_wf so that setting the enable bit AFTER
   the waveforms still brings them into effect, which is an order some drivers
   use; see wf_refresh(). */
static uint8_t slot_wf_raw[18];

/* ---------------------------------------------------------------------------
 * Derived-parameter refresh. Each is called from exactly the register writes
 * that can invalidate it, which is why nothing in the per-sample loop below
 * recomputes any of them.
 */

/* nukedopl.cpp:303 -- key scale LEVEL, plus the cached tl+ksl sum. */
static void eg_update_ksl(opl_slot_t *s)
{
    const opl_chan_t *c = &opl.chan[s->ch];
    int16_t ksl = (int16_t)((kslrom[c->f_num >> 6] << 2)
                            - ((0x08 - c->block) << 5));
    if (ksl < 0)
        ksl = 0;
    s->eg_ksl = (uint8_t)ksl;
    s->eg_tl_ksl = (uint16_t)((s->reg_tl << 2)
                              + (s->eg_ksl >> kslshift[s->reg_ksl]));
}

/* nukedopl.cpp:320 -- key scale RATE. Four envelope stages, each resolved to
   the (hi, lo) pair the per-sample stepper indexes with. */
static void eg_update_rate(opl_slot_t *s)
{
    const opl_chan_t *c = &opl.chan[s->ch];
    s->eg_ks = (uint8_t)(c->ksv >> ((s->reg_ksr ^ 1) << 1));
    for (unsigned i = 0; i < 4; i++) {
        uint8_t rate = (uint8_t)(s->eg_ks + (s->eg_rates[i] << 2));
        uint8_t hi = (uint8_t)(rate >> 2);
        if (hi & 0x10)
            hi = 0x0F;
        s->eg_rate_hi[i] = hi;
        s->eg_rate_lo[i] = (uint8_t)(rate & 0x03);
    }
}

/* nukedopl.cpp:487 -- the phase increment, vibrato excluded (it is applied per
   sample against the live LFO position). */
static void pg_update_inc(opl_slot_t *s)
{
    const opl_chan_t *c = &opl.chan[s->ch];
    uint32_t base = ((uint32_t)c->f_num << c->block) >> 1;
    s->pg_inc = (base * mt[s->reg_mult]) >> 1;
}

static void chan_refresh(unsigned ci)
{
    opl_chan_t *c = &opl.chan[ci];
    c->ksv = (uint8_t)((c->block << 1)
                       | ((c->f_num >> (0x09 - opl.nts)) & 0x01));
    for (unsigned k = 0; k < 2; k++) {
        opl_slot_t *s = &opl.slot[ch_op0[ci] + 3 * k];
        eg_update_ksl(s);
        eg_update_rate(s);
        pg_update_inc(s);
    }
}

/* ---------------------------------------------------------------------------
 * WHICH CHANNELS RUN. A channel is retired when both its operators are keyed
 * off AND fully attenuated; it is revived by a key-on. Retiring is per
 * CHANNEL, not per operator, because the pair share the feedback and
 * modulation wiring and half a retired channel is not a state this can be in.
 */
static void chan_wake(unsigned ci)
{
    if (opl.rhythm & 0x20u) {
        /* Percussion mode owns channels 6-8; see the header. */
        if (ci >= 6)
            return;
    }
    opl.chan[ci].active = 1;
    opl.any_active = 1;
}

static void chan_retire(unsigned ci)
{
    opl_chan_t *c = &opl.chan[ci];
    c->active = 0;
    for (unsigned k = 0; k < 2; k++) {
        opl_slot_t *s = &opl.slot[ch_op0[ci] + 3 * k];
        s->out = s->prout = s->fbmod = 0;
    }
    for (unsigned i = 0; i < 9; i++)
        if (opl.chan[i].active)
            return;
    opl.any_active = 0;
}

/* ---------------------------------------------------------------------------
 * THE ENVELOPE GENERATOR. nukedopl.cpp:337-460, transcribed. This is the
 * chip's own shift-and-add state machine rather than a curve table, so the
 * attack shape and the rate-vs-key-scale interaction come out right without
 * a 16 KB table to hold them.
 */
static void eg_calc(opl_slot_t *s)
{
    uint8_t rate_hi, rate_lo, reg_rate, eg_shift, shift = 0, eg_off = 0;
    uint8_t reset = 0, nonzero;
    uint16_t eg_rout;
    int16_t eg_inc = 0;

    s->eg_out = (uint16_t)(s->eg_rout + s->eg_tl_ksl
                           + (s->reg_am ? opl.tremolo : 0));

    if (s->key && s->eg_gen == EG_RELEASE) {
        reset = 1;
        reg_rate = s->eg_rates[0];
        rate_hi = s->eg_rate_hi[0];
        rate_lo = s->eg_rate_lo[0];
    } else {
        reg_rate = s->eg_rates[s->eg_gen];
        rate_hi = s->eg_rate_hi[s->eg_gen];
        rate_lo = s->eg_rate_lo[s->eg_gen];
    }
    s->pg_reset = reset;
    nonzero = (uint8_t)(reg_rate != 0);

    eg_shift = (uint8_t)(rate_hi + opl.eg_add);
    if (nonzero) {
        if (rate_hi < 12) {
            if (opl.eg_state) {
                switch (eg_shift) {
                case 12: shift = 1; break;
                case 13: shift = (uint8_t)((rate_lo >> 1) & 1); break;
                case 14: shift = (uint8_t)(rate_lo & 1); break;
                default: break;
                }
            }
        } else {
            shift = (uint8_t)((rate_hi & 3) + eg_incstep[rate_lo][opl.eg_timer_lo]);
            if (shift & 4)
                shift = 3;
            if (!shift)
                shift = opl.eg_state;
        }
    }

    eg_rout = s->eg_rout;
    if (reset && rate_hi == 0x0F)          /* attack rate 15: instant */
        eg_rout = 0;
    if ((s->eg_rout & 0x1F8) == 0x1F8)
        eg_off = 1;
    if (s->eg_gen != EG_ATTACK && !reset && eg_off)
        eg_rout = 0x1FF;

    switch (s->eg_gen) {
    case EG_ATTACK:
        if (!s->eg_rout)
            s->eg_gen = EG_DECAY;
        else if (s->key && shift > 0 && rate_hi != 0x0F)
            eg_inc = (int16_t)(~s->eg_rout >> (4 - shift));
        break;
    case EG_DECAY:
        if ((s->eg_rout >> 4) == s->reg_sl)
            s->eg_gen = EG_SUSTAIN;
        else if (!eg_off && !reset && shift > 0)
            eg_inc = (int16_t)(1 << (shift - 1));
        break;
    default:                                /* sustain and release */
        if (!eg_off && !reset && shift > 0)
            eg_inc = (int16_t)(1 << (shift - 1));
        break;
    }
    s->eg_rout = (uint16_t)((eg_rout + eg_inc) & 0x1FF);
    if (reset)
        s->eg_gen = EG_ATTACK;
    if (!s->key)
        s->eg_gen = EG_RELEASE;
}

/* nukedopl.cpp:518 -- phase accumulate. Vibrato is computed inline from the
   live LFO position rather than from a per-slot 8-entry table: the table is
   nukedopl's speed/space trade and 18 x 8 x 4 bytes of RAM_EMU is the wrong
   side of it here. Returns the 10-bit phase THIS sample uses, which is the
   value BEFORE the increment -- the chip outputs the old phase and then
   advances, and swapping those detunes everything by one sample. */
static uint16_t pg_generate(opl_slot_t *s)
{
    uint32_t inc;
    uint16_t phase = (uint16_t)(s->pg_phase >> 9);

    if (s->reg_vib) {
        const opl_chan_t *c = &opl.chan[s->ch];
        uint16_t fn = c->f_num;
        int8_t range = (int8_t)((fn >> 7) & 7);
        if (!(opl.vibpos & 3))
            range = 0;
        else if (opl.vibpos & 1)
            range = (int8_t)(range >> 1);
        range = (int8_t)(range >> opl.vibshift);
        if (opl.vibpos & 4)
            range = (int8_t)(-range);
        fn = (uint16_t)(fn + range);
        inc = ((((uint32_t)fn << c->block) >> 1) * mt[s->reg_mult]) >> 1;
    } else {
        inc = s->pg_inc;
    }
    if (s->pg_reset)
        s->pg_phase = 0;
    s->pg_phase += inc;
    return phase;
}

/* nukedopl.cpp:655 -- the operator output. `mod` is the modulator's previous
   output (or this slot's own feedback), added to the phase in the chip's
   10-bit phase space and NOT scaled: that unscaled add is what makes an OPL
   sound like an OPL.
 *
 * The OPL2 has four waveforms and they are selected only when register 01h
 * bit 5 is set; before that every operator is a plain sine, which is what an
 * AdLib-era driver expects and what a YM3812 does. `0x1000` in the log domain
 * is "silent" -- it drives the exponent lookup past its own range and reads
 * through to zero, which is how the half/quarter waves get their flat parts
 * for free. */
static void slot_generate(opl_slot_t *s, uint16_t pg_out, int16_t mod)
{
    uint16_t phase = (uint16_t)((pg_out + (uint16_t)mod) & 0x3FFu);
    uint16_t neg = 0, ls;
    uint32_t level;

    switch (s->reg_wf) {
    default:
    case 0:                                    /* full sine                 */
        if (phase & 0x200)
            neg = 0xFFFFu;
        ls = (phase & 0x100) ? logsinrom[(phase & 0xFF) ^ 0xFF]
                             : logsinrom[phase & 0xFF];
        break;
    case 1:                                    /* half sine                 */
        if (phase & 0x200)
            ls = 0x1000u;
        else
            ls = (phase & 0x100) ? logsinrom[(phase & 0xFF) ^ 0xFF]
                                 : logsinrom[phase & 0xFF];
        break;
    case 2:                                    /* absolute sine             */
        ls = (phase & 0x100) ? logsinrom[(phase & 0xFF) ^ 0xFF]
                             : logsinrom[phase & 0xFF];
        break;
    case 3:                                    /* pulse (quarter) sine      */
        ls = (phase & 0x100) ? 0x1000u : logsinrom[phase & 0xFF];
        break;
    }

    level = (uint32_t)ls + ((uint32_t)s->eg_out << 3);
    if (level > 0x1FFFu)
        level = 0x1FFFu;
    s->out = (int16_t)((exprom[level & 0xFFu] >> (level >> 8)) ^ neg);
}

/* nukedopl.cpp:682 -- operator 1's feedback, taken from the average of its
   last two outputs. `fb` is 0..7 and 0 means none, which is a different thing
   from a shift of 9 and is why the test is written out. */
static void slot_calc_fb(opl_slot_t *s, uint8_t fb)
{
    s->fbmod = fb ? (int16_t)((s->prout + s->out) >> (9 - fb)) : 0;
    s->prout = s->out;
}

/* ---------------------------------------------------------------------------
 * ONE CHIP SAMPLE at X86_OPL_CLOCK_HZ. Everything above is called from here
 * and from nowhere else.
 */
static int32_t opl_chip_step(void)
{
    int32_t mix = 0;
    unsigned ci;
    uint32_t ran = 0;

    for (ci = 0; ci < 9; ci++) {
        opl_chan_t *c = &opl.chan[ci];
        opl_slot_t *s0, *s1;
        uint16_t p0, p1;

        if (!c->active)
            continue;
        s0 = &opl.slot[ch_op0[ci]];
        s1 = s0 + 3;

        /* Operator 1: modulated by its OWN feedback, which is last sample's.
           Operator 2 then sees operator 1's output from THIS sample -- the
           order of these two blocks is the connection. */
        eg_calc(s0);
        p0 = pg_generate(s0);
        slot_generate(s0, p0, s0->fbmod);
        slot_calc_fb(s0, c->fb);

        eg_calc(s1);
        p1 = pg_generate(s1);
        slot_generate(s1, p1, c->con ? 0 : s0->out);

        /* nukedopl.cpp:940 -- CON=0 is FM (operator 2 alone is heard),
           CON=1 is AM (both operators are heard in parallel). */
        mix += c->con ? ((int32_t)s0->out + s1->out) : (int32_t)s1->out;
        ran += 2;

        if (!s0->key && !s1->key
            && (s0->eg_rout & 0x1F8) == 0x1F8
            && (s1->eg_rout & 0x1F8) == 0x1F8)
            chan_retire(ci);
    }
    x86_opl_slots_run += ran;
    x86_opl_chip_steps++;

    /* nukedopl.cpp:1519 -- the per-sample housekeeping, in the chip's order.
       The tremolo LFO is a 210-step triangle clocked every 64 samples and the
       vibrato LFO an 8-position ring clocked every 1024; the envelope timer
       advances on alternate samples and its trailing-zero count is what makes
       one envelope rate sixteen times another. */
    if ((opl.timer & 0x3F) == 0x3F) {
        opl.tremolopos++;
        if (opl.tremolopos == 210)
            opl.tremolopos = 0;
    }
    opl.tremolo = (uint8_t)(opl.tremolopos < 105
                            ? (opl.tremolopos >> opl.tremoloshift)
                            : ((210 - opl.tremolopos) >> opl.tremoloshift));
    if ((opl.timer & 0x3FF) == 0x3FF)
        opl.vibpos = (uint8_t)((opl.vibpos + 1) & 7);
    opl.timer++;

    if (opl.eg_state) {
        uint32_t low = (uint32_t)opl.eg_timer & 0x1FFFu;
        opl.eg_add = low ? (uint8_t)(__builtin_ctz(low) + 1) : 0;
        opl.eg_timer_lo = (uint8_t)(opl.eg_timer & 3u);
    }
    if (opl.eg_timerrem || opl.eg_state) {
        if (opl.eg_timer == UINT64_C(0xFFFFFFFFF)) {
            opl.eg_timer = 0;
            opl.eg_timerrem = 1;
        } else {
            opl.eg_timer++;
            opl.eg_timerrem = 0;
        }
    }
    opl.eg_state ^= 1u;

    return mix;
}

/* ---------------------------------------------------------------------------
 * Public: register writes. nukedopl.cpp:1770, OPL2 half only.
 */
void x86_opl_reset(void)
{
    memset(&opl, 0, sizeof(opl));
    memset(slot_wf_raw, 0, sizeof(slot_wf_raw));
    opl.tremoloshift = 4;
    opl.vibshift = 1;
    opl.frac = 0x10000u;                 /* step on the first output sample */
    for (unsigned i = 0; i < 18; i++) {
        opl_slot_t *s = &opl.slot[i];
        s->ch = (uint8_t)((i / 6) * 3 + (i % 3));
        s->eg_gen = EG_RELEASE;
        s->eg_rout = 0x1FF;
    }
    for (unsigned i = 0; i < 9; i++)
        chan_refresh(i);
    opl.inited = 1;
}

int x86_opl_active(void)
{
    return opl.any_active;
}

/* Effective waveform. Register 01h bit 5 is the WAVEFORM SELECT ENABLE and a
   YM3812 with it clear is a pure sine generator whatever E0h says -- an
   AdLib-era driver that never sets it must not get the OPL2's extra three
   waveforms by accident. The raw E0h value is kept in reg_wf's low bits and
   gated here so that setting the enable bit LATER brings the already-written
   waveforms into effect, which is the order some drivers use. */
static void wf_refresh(void)
{
    for (unsigned i = 0; i < 18; i++)
        opl.slot[i].reg_wf = opl.wavesel ? slot_wf_raw[i] : 0u;
}

void x86_opl_write(uint8_t reg, uint8_t val)
{
    int8_t sn;
    unsigned ci;

    /* BSS ZERO IS NOT THIS CHIP'S RESET STATE -- the operator-to-channel map,
       the two LFO depths and "every envelope is fully released" are all
       non-zero -- and x86_io.c has no init entry point to hang a call off.
       One predictable-not-taken branch on a path that runs a few hundred times
       a second is the cheap way to be right; it must NOT become a pattern on
       the per-sample path below. */
    if (!opl.inited)
        x86_opl_reset();

    /* THE 0xB0 RANGE IS DECODED FIRST AND ON PURPOSE. `reg & 0xE0` folds
       0xB0-0xBF onto 0xA0, so a switch on it alone reads a key-on as an f-num
       write to channel 0 -- every note in the tune landing on one voice at the
       wrong pitch. Peel the range off before the fold rather than trying to
       unpick it inside the case. */
    if (reg == 0xBDu) {
        uint8_t was = opl.rhythm;
        opl.tremoloshift = (uint8_t)(((((unsigned)val >> 7) ^ 1u) << 1) + 2u);
        opl.vibshift     = (uint8_t)((((unsigned)val >> 6) & 1u) ^ 1u);
        opl.rhythm       = (uint8_t)(val & 0x3Fu);
        if ((opl.rhythm & 0x20u) && !(was & 0x20u)) {
            /* Entering percussion mode. Channels 6-8 stop being notes; see
               the header for why they are muted rather than left running. */
            for (ci = 6; ci < 9; ci++)
                if (opl.chan[ci].active)
                    chan_retire(ci);
            x86_opl_rhythm_muted++;
        }
        return;
    }
    if ((reg & 0xF0u) == 0xB0u) {
        uint8_t keyon;
        if ((reg & 0x0Fu) > 8u) return;
        ci = reg & 0x0Fu;
        opl.chan[ci].f_num = (uint16_t)((opl.chan[ci].f_num & 0xFFu)
                                        | ((unsigned)(val & 0x03u) << 8));
        opl.chan[ci].block = (uint8_t)((val >> 2) & 7u);
        chan_refresh(ci);
        keyon = (uint8_t)((val >> 5) & 1u);
        if (keyon != opl.chan[ci].keyon) {
            opl.chan[ci].keyon = keyon;
            opl.slot[ch_op0[ci]].key = keyon;
            opl.slot[ch_op0[ci] + 3].key = keyon;
            if (keyon) {
                chan_wake(ci);
                x86_opl_keyons++;
            }
        }
        return;
    }

    switch (reg & 0xE0u) {
    case 0x00:
        if (reg == 0x01) {
            opl.wavesel = (uint8_t)((val >> 5) & 1u);
            wf_refresh();
        } else if (reg == 0x08) {
            uint8_t nts = (uint8_t)((val >> 6) & 1u);
            if (nts != opl.nts) {
                opl.nts = nts;
                for (ci = 0; ci < 9; ci++)
                    chan_refresh(ci);
            }
        }
        /* 02h/03h/04h are the TIMERS and belong to x86_io.c; this file must
           not keep a second copy of the state the status byte is built from. */
        return;

    case 0x20:                                   /* AM / VIB / EG / KSR / MUL */
        sn = ad_slot[reg & 0x1Fu];
        if (sn < 0) return;
        {
            opl_slot_t *s = &opl.slot[sn];
            s->reg_am   = (uint8_t)((val >> 7) & 1u);
            s->reg_vib  = (uint8_t)((val >> 6) & 1u);
            s->reg_type = (uint8_t)((val >> 5) & 1u);
            s->reg_ksr  = (uint8_t)((val >> 4) & 1u);
            s->reg_mult = (uint8_t)(val & 0x0Fu);
            /* EG-TYPE: 1 = sustaining, and a sustaining operator's SUSTAIN
               stage rate is zero (it holds) rather than the release rate. */
            s->eg_rates[2] = s->reg_type ? 0u : s->reg_rr;
            eg_update_rate(s);
            pg_update_inc(s);
        }
        return;

    case 0x40:                                   /* KSL / TL                  */
        sn = ad_slot[reg & 0x1Fu];
        if (sn < 0) return;
        opl.slot[sn].reg_ksl = (uint8_t)((val >> 6) & 3u);
        opl.slot[sn].reg_tl  = (uint8_t)(val & 0x3Fu);
        eg_update_ksl(&opl.slot[sn]);
        return;

    case 0x60:                                   /* AR / DR                   */
        sn = ad_slot[reg & 0x1Fu];
        if (sn < 0) return;
        {
            opl_slot_t *s = &opl.slot[sn];
            s->reg_ar = (uint8_t)((val >> 4) & 0x0Fu);
            s->reg_dr = (uint8_t)(val & 0x0Fu);
            s->eg_rates[0] = s->reg_ar;
            s->eg_rates[1] = s->reg_dr;
            eg_update_rate(s);
        }
        return;

    case 0x80:                                   /* SL / RR                   */
        sn = ad_slot[reg & 0x1Fu];
        if (sn < 0) return;
        {
            opl_slot_t *s = &opl.slot[sn];
            s->reg_sl = (uint8_t)((val >> 4) & 0x0Fu);
            if (s->reg_sl == 0x0Fu)
                s->reg_sl = 0x1Fu;               /* SL 15 is -93 dB, not -45  */
            s->reg_rr = (uint8_t)(val & 0x0Fu);
            s->eg_rates[2] = s->reg_type ? 0u : s->reg_rr;
            s->eg_rates[3] = s->reg_rr;
            eg_update_rate(s);
        }
        return;

    case 0xA0:                                   /* F-NUM low                 */
        if ((reg & 0x0Fu) > 8u) return;
        ci = reg & 0x0Fu;
        opl.chan[ci].f_num = (uint16_t)((opl.chan[ci].f_num & 0x300u) | val);
        chan_refresh(ci);
        return;

    case 0xC0:                                   /* FEEDBACK / CONNECTION     */
        if ((reg & 0x0Fu) > 8u) return;
        ci = reg & 0x0Fu;
        opl.chan[ci].fb  = (uint8_t)((val >> 1) & 7u);
        opl.chan[ci].con = (uint8_t)(val & 1u);
        return;

    case 0xE0:                                   /* WAVEFORM SELECT           */
        sn = ad_slot[reg & 0x1Fu];
        if (sn < 0) return;
        slot_wf_raw[sn] = (uint8_t)(val & 0x03u);   /* OPL2: four waveforms   */
        opl.slot[sn].reg_wf = opl.wavesel ? slot_wf_raw[sn] : 0u;
        return;

    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Public: the render. The ONLY clock -- see the header. Called once per frame
 * from src/main.c's audio phase, after the speaker and beside the Sound
 * Blaster, and the three MIX rather than overwrite because a title may sound
 * all three at once.
 */
volatile uint32_t x86_opl_frame_cycles;

/* See x86_opl.h. A sustaining patch (EG-TYPE 1) at total level 0 on both
   operators of all nine channels, keyed on and never released: the loop below
   then never retires a channel and the render does 18 operator updates per
   chip sample for the whole run. Deliberately the WORST case and not a
   plausible tune -- a bound is only useful if nothing can exceed it. */
void x86_opl_bench_all_on(void)
{
    x86_opl_reset();
    x86_opl_write(0x01, 0x20);
    for (unsigned c = 0; c < 9; c++) {
        uint8_t o0 = (uint8_t)((c / 3) * 8 + (c % 3));   /* the .00 offset */
        uint8_t o1 = (uint8_t)(o0 + 3);
        for (unsigned k = 0; k < 2; k++) {
            uint8_t o = k ? o1 : o0;
            x86_opl_write((uint8_t)(0x20 + o), 0x21);    /* sustain, mult 1 */
            x86_opl_write((uint8_t)(0x40 + o), 0x00);    /* total level 0   */
            x86_opl_write((uint8_t)(0x60 + o), 0xF0);    /* AR 15, DR 0     */
            x86_opl_write((uint8_t)(0x80 + o), 0x00);    /* SL 0, RR 0      */
        }
        x86_opl_write((uint8_t)(0xC0 + c), 0x01);        /* AM: both heard  */
        x86_opl_write((uint8_t)(0xA0 + c), (uint8_t)(0x40 + c * 8));
        x86_opl_write((uint8_t)(0xB0 + c), (uint8_t)(0x20 | (4 << 2) | 1));
    }
}

unsigned x86_opl_render_mix(int16_t *dst, unsigned frames,
                            unsigned sample_rate, int16_t amplitude)
{
    uint32_t step;
    unsigned n;

    if (!opl.any_active || !frames || !sample_rate || !dst) {
        opl.last = 0;                  /* never hold a DC offset over silence */
        return 0;
    }
    step = (uint32_t)(((uint64_t)X86_OPL_CLOCK_HZ << 16) / sample_rate);
    if (!step)
        step = 1;

    for (n = 0; n < frames; n++) {
        int32_t v, o;
        while (opl.frac >= 0x10000u) {
            int32_t raw;
            opl.frac -= 0x10000u;
            raw = opl_chip_step();
            if (raw > 32767) raw = 32767;
            else if (raw < -32768) raw = -32768;
            opl.last = (int16_t)raw;
        }
        v = ((int32_t)opl.last * amplitude) >> 13;
        o = (int32_t)dst[n] + v;
        if (o > 32767) { o = 32767; x86_opl_clips++; }
        else if (o < -32768) { o = -32768; x86_opl_clips++; }
        dst[n] = (int16_t)o;
        opl.frac += step;
    }
    x86_opl_samples += frames;
    return frames;
}
