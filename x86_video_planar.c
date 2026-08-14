/*
 * The planar VGA adapter -- EGA mode 0Dh and unchained 13h ("Mode Y").
 *
 * Every guest STORE that lands in the 64 KB aperture at 0xA0000 arrives here
 * instead of being written by the interpreter -- see the VGA_ST macro in
 * cpu_exec.S. The hook REPLACES the store; it is never a notification after
 * one. That is load-bearing: in an unchained write the byte the guest presents
 * is not the byte that reaches any plane, so letting the raw store happen first
 * would destroy a pixel written four stores earlier.
 *
 * ===========================================================================
 * WHAT THIS IS A PORT OF, AND THE TWO PLACES IT IMPROVES ON THE ORIGINAL
 * ===========================================================================
 *
 * The model, the arithmetic and every "do not re-propose this" note come from
 * references/8086tiny (dos_ega_cell, dos_ega_store, dos_vga_set_unchained) and
 * its design records, which are the thing to read before changing anything
 * here:
 *
 *   references/8086tiny/docs/video/11-ega-planar.md   the 0Dh write pipeline
 *   references/8086tiny/docs/video/15-mode-y.md       the unchained model
 *   docs/08_vga_mode13h.md                            our side of it
 *
 * TWO THINGS ARE BETTER HERE THAN IN THE REFERENCE, and both fall out of the
 * hook's shape rather than from cleverness:
 *
 *  1. THE LATCHES ARE EXACT FOR PLAIN READS, not only for read-modify-writes.
 *     The reference is a C interpreter whose read path never surfaced a plain
 *     load, so it could only latch where it could see a read: inside a
 *     read-modify-write store, from the DESTINATION. Our VGA_LATCH macro sits
 *     on 47 real load sites and records the window offset of the last guest
 *     read in `cpu->vga_latch`, so `mov al,[si] / mov [di],al` -- the write
 *     mode 1 block copy that is most of the reason to unchain at all -- latches
 *     from SI, which is where the hardware latches from.
 *
 *  2. THERE IS NO SAVE/RESTORE DANCE. 15-mode-y.md section 5 ("the one thing
 *     with no analogue in 0Dh") describes saving the aperture bytes a raw store
 *     is about to destroy and putting them back, because in Mode Y the shadow
 *     address IS plane storage. We never write the raw byte at all -- the hook
 *     replaces the store -- so `dos_ega_wr_save[]` has no analogue here and is
 *     not needed. Do not reintroduce it.
 *
 * ===========================================================================
 * THE ADDRESS MODEL
 * ===========================================================================
 *
 * Planes are INTERLEAVED: plane p at plane offset a lives at `cell[4*a + p]`.
 * That is the QEMU/TinyEMU convention, reached via tiny386's vga.c, and it
 * earns its place twice -- you allocate four times what is USED rather than
 * four times the window, and all four planes of one group are a single aligned
 * 32-bit load, which is what makes the blit cheap on a Cortex-M7.
 *
 * Mode 0Dh: window offset == plane offset. A guest read is served from the
 * bottom 16 KB of the aperture, held permanently equal to what a read must
 * return (the "read shadow"), so reads cost nothing on the hot path.
 *
 * Mode Y: pixel (x,y) is plane x&3 at plane offset y*80 + (x>>2), and
 *
 *     4 * (y*80 + (x>>2)) + (x&3)  ==  320*y + x
 *
 * so the interleaved store IS what a chained blit already wanted, and the
 * de-interleaving blit is the identity at the standard geometry. The four
 * squashed copies were never in the blit; they were the core storing plane
 * data through a chained write path.
 *
 * A MODE-Y PAGE IS STILL 64,000 BYTES. Unchaining does not shrink a page, it
 * makes three more of them addressable -- and that is the thing that does not
 * fit. Two real pages is 195,072 bytes; a full card is 262,144. We have the
 * 64 KB aperture and 32 KB of arena. So plane offsets are FOLDED into one
 * page's worth of storage and a page flip is a no-op; see the fold in
 * vga_cell() and the limits recorded in docs/08_vga_mode13h.md section 4.
 *
 * DO NOT "FIX" THE ALIASING WITHOUT BRINGING ~192 KB WITH YOU.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "cutils.h"
#include "x86_cpu.h"
#include "x86_video_planar.h"

/* ---------------------------------------------------------------------------
 * NEGATIVE CONTROLS. Each one compiles a specific bug back in and each is
 * REQUIRED to fail tests/test_video.py's Mode Y arm -- a check that still
 * passes with the bug present is not a check. Defaults are all 0; the two that
 * live in the blit are in x86_video.c.
 */
#ifndef X86_MODEY_CORE_NEGCTL
#define X86_MODEY_CORE_NEGCTL 0     /* chain-4 inert: the pre-fix core        */
#endif
#ifndef X86_MODEY_PAGE_NEGCTL
#define X86_MODEY_PAGE_NEGCTL 0     /* fixed 16 KB mask, cache inside the     */
#endif                              /* alias -- the state before the stride   */
                                    /* work                                   */
#ifndef X86_MODEY_TIMER_NEGCTL
#define X86_MODEY_TIMER_NEGCTL 0    /* the frame loop paints on a TIMER, with */
#endif                              /* no test for whether the guest had      */
                                    /* finished drawing -- the shipped bug    */

/* Plane offsets alias into one 16 KB page: 4 * 0x4000 == 0x10000, the aperture
   exactly. A 320x200 page needs 16,000 of those 16,384, so the top 384 are
   never displayed and act as scratch. */
#define X86_MODEY_PLANE  X86_MODEY_PLANE_OFFSETS
#define X86_MODEY_MASK   (X86_MODEY_PLANE - 1u)

/*
 * THE PAGE STRIDE IS NOT 16 KB, AND ASSUMING IT WAS IS WHAT BROKE WOLF3D.
 *
 * `a & 0x3FFF` aliases pages only if the guest spaces them 16,384 plane offsets
 * apart. Measured on the reference port, WOLF3D does not: its display start
 * cycles 0x0000 -> 0x4100 -> 0x8200, so its page is 80 x 208 = 16,640 offsets
 * -- the id Galaxy engine's SCREENSIZE -- and 0x4100 & 0x3FFF == 0x0100. Under
 * a mask every page lands 256 offsets (3.2 scanlines) off its neighbour, which
 * is both the "first frame was perfect and then it overwrote itself" report
 * (a full-page latch copy becomes a copy of the page onto itself, shifted) and
 * the "stepped vertical slabs" one (consecutive frames fold differently).
 *
 * So the stride is LEARNED from the only place a guest ever states it: the
 * distance between two display starts it programs. Bounded on both sides,
 * because a Sequencer-walking probe and a smooth-scroll title write that
 * register too. A page must hold the 200 displayed rows (16,000 offsets), and
 * past 20,480 we could not store the displayed part anyway -- which is what
 * rejects WOLF3D's own 0x0000 <-> 0x8000 signon pair while accepting 0x4100.
 * The SMALLEST accepted distance wins, so a first wrong guess is corrected
 * rather than latched.
 */
#define X86_MODEY_PGMIN  16000u     /* 200 rows x 80 offsets                  */
#define X86_MODEY_PGMAX  20480u     /* 256 rows; past this we could not store */

/*
 * THE OFF-SCREEN CACHE BASE IS A CONSTANT, AND THAT IS DELIBERATE.
 *
 * 0xC300 is 3 x 16,640 -- the first offset past a three-page 80x208 layout,
 * the number the Galaxy engines call FREESTART, and exactly where WOLF3D's
 * off-screen writes start. It is NOT derived from the learned stride: that was
 * built in the reference and it MANGLES THE STATUS BAR, because the tiles are
 * cached before the first flip and read back after, so the base moves under
 * stored data. And it is not 0xC000 either -- page 2 of a 16,640 layout runs to
 * 0xC300, so a cache starting at 0xC000 eats that page's last four displayed
 * rows, which the reference's own test caught as 512 wrong pixels.
 */
#define X86_MODEY_CACHE  0xC300u

/*
 * THE STRIDE ASSUMED UNTIL THE GUEST FLIPS, AND WHY IT IS 16,640.
 *
 * Learning is TOO LATE FOR THE FURNITURE. WOLF3D's DrawPlayScreen paints the
 * border and the status bar into all three pages BEFORE PlayLoop flips for the
 * first time, so whatever stride is assumed at that moment is baked into stored
 * pixels that are never redrawn. Measured on the reference: at 16,384 the
 * status bar comes out as three copies stepped 3 rows and 64 pixels apart and
 * stays that way for the level; at 16,640 the frame is pixel-identical to a
 * 256 KB reference card. That is a measurement, not a principle -- a title
 * spacing its pages 16,384 apart is displaced until its first flip corrects the
 * value downward. -DX86_MODEY_PAGE0=<n> overrides it, and the POSITIVE control
 * in tests/test_video.py builds 16384 to prove the learning actually runs.
 */
#ifndef X86_MODEY_PAGE0
#define X86_MODEY_PAGE0 16640u
#endif

/* ---- state ---------------------------------------------------------------
 *
 * Small on purpose: this is .bss on a target with ~4.5 KB of headroom.
 */

/* Host base of the guest 0xA0000 aperture, or NULL. Owned here rather than in
   x86_video.c so the hook has no dependency on the renderer, but SET there --
   x86_video_set_aperture() is the single place the aperture is decided. */
uint8_t *x86_vga_planar_aperture;

volatile uint32_t x86_vga_planar_stores;
volatile uint32_t x86_vga_planar_lost;
volatile uint32_t x86_vga_modey_flips;
volatile uint32_t x86_vga_modey_page = X86_MODEY_PAGE0;
volatile uint32_t x86_vga_modey_armed;
volatile uint32_t x86_vga_ega_armed;
volatile uint32_t x86_vga_ega_refused;
volatile uint32_t x86_vga_latch_loads;
volatile uint32_t x86_vga_latch_miss;

static uint8_t  *ega_planes;        /* 0Dh interleaved plane storage (arena) */
static uint32_t  ega_plane_size;    /* plane OFFSETS the card has fitted     */
static uint8_t  *modey_cache;       /* Mode Y off-screen store (arena)       */
static uint32_t  modey_cache_offs;  /* plane offsets it holds                */

static uint32_t  modey_prev_start;
static int       modey_learned;

/* The Graphics Controller file, mirrored here because the write pipeline reads
   it on every store and an accessor call per byte is not free. x86_io.c remains
   the register file a GUEST reads back; this is fed from it and never the other
   way round, so there is one writer and no possibility of disagreement. */
static uint8_t   gc[16];
static uint8_t   map_mask = 0x0F;   /* Sequencer index 2                     */
static uint8_t   latch[4];

#define GC_SET_RESET     gc[0]
#define GC_EN_SET_RESET  gc[1]
#define GC_COLOR_CMP     gc[2]
#define GC_ROTATE        gc[3]
#define GC_READ_MAP      gc[4]
#define GC_MODE          gc[5]
#define GC_COLOR_DC      gc[7]
#define GC_BIT_MASK      gc[8]

int x86_vga_is_unchained(void)   { return (int)x86_vga_modey_armed; }
int x86_vga_planar_active(void)  { return (int)(x86_vga_modey_armed | x86_vga_ega_armed); }

const uint8_t *x86_vga_ega_planes(void)      { return ega_planes; }
uint32_t       x86_vga_ega_plane_offsets(void) { return ega_plane_size; }

void x86_vga_ega_set_planes(void *p, uint32_t plane_offsets)
{
    ega_planes = (uint8_t *)p;
    ega_plane_size = p ? plane_offsets : 0u;
}

void x86_vga_modey_set_cache(void *p, uint32_t offsets)
{
    modey_cache = (uint8_t *)p;
    modey_cache_offs = p ? offsets : 0u;
}

/* ---------------------------------------------------------------------------
 * THE ONE PLACE THAT SAYS WHERE A PLANE BYTE LIVES.
 *
 * Aperture, off-screen cache, or nowhere. Every consumer -- the store, the
 * latch reload and the blit (through x86_vga_modey_row) -- goes through this,
 * so they cannot disagree about where a row is. The reference reached the same
 * shape only after deleting two rival helpers that had drifted apart.
 *
 * Returns a pointer to the FOUR interleaved plane bytes of `a`, or NULL when
 * this card has no memory there. NULL is not an error: a missing off-screen
 * tile must draw nothing, where an aliased one corrupts the picture.
 */
static uint8_t *vga_cell(uint32_t a)
{
    uint32_t p;

    if (!x86_vga_modey_armed) {
        if (a < ega_plane_size)
            return ega_planes + 4u * a;
        return 0;                       /* past the fitted card: discarded */
    }

#if X86_MODEY_PAGE_NEGCTL
    return x86_vga_planar_aperture + 4u * (a & X86_MODEY_MASK);
#else
    if (a >= X86_MODEY_CACHE) {
        a -= X86_MODEY_CACHE;
        if (a >= modey_cache_offs)
            return 0;                   /* no storage: draws nothing */
        return modey_cache + 4u * a;
    }
    /* Page-relative against the LEARNED stride, not an AND mask. Three pages,
       because that is what a Galaxy-engine layout uses; a title that spaces
       them differently loses its cache to the discard path above rather than
       corrupting a page, which is the direction to fail in. */
    p = x86_vga_modey_page;
    if (a >= 2u * p)      a -= 2u * p;
    else if (a >= p)      a -= p;
    if (a >= X86_MODEY_PLANE)
        return 0;                       /* page tail, rows 205+: never shown */
    return x86_vga_planar_aperture + 4u * a;
#endif
}

uint32_t x86_vga_modey_row(uint32_t a)
{
    const uint8_t *base = x86_vga_planar_aperture;
    const uint8_t *c;

    if (!base)
        return ~0u;
    c = vga_cell(a);
    if (!c || c < base || c >= base + X86_VGA_APERTURE_BYTES)
        return ~0u;
    return (uint32_t)(c - base) / 4u;
}

/* ---------------------------------------------------------------------------
 * READS (mode 0Dh only).
 *
 * What a guest read at plane offset `a` must return, for the CURRENT read mode.
 * This is the ONLY definition of what the shadow holds, so the shadow is
 * correct under read mode 1 (colour compare) rather than silently handing back
 * plane bytes.
 *
 * MODE Y HAS NO SHADOW and cannot have one: the planes ARE the aperture. A
 * guest data read of A000:a there returns aperture[a], i.e. plane a&3 at plane
 * offset a>>2 -- the wrong plane, by design and for want of 64 KB. Latch loads
 * stay exact, and latch copies are the idiom that matters; a title that reads
 * pixels back for collision detection would not be served.
 */
static uint8_t ega_read_byte(uint32_t a)
{
    static const uint8_t nocell[4] = { 0, 0, 0, 0 };
    const uint8_t *p = vga_cell(a);

    if (!p)
        p = nocell;
    if (GC_MODE & 8u) {
        /* Read mode 1: each returned bit is 1 where every plane selected by
           Colour Don't Care matches the corresponding bit of Colour Compare. */
        uint8_t r = 0xFF;
        for (int i = 0; i < 4; i++)
            if (GC_COLOR_DC & (1u << i))
                r &= (GC_COLOR_CMP & (1u << i)) ? p[i] : (uint8_t)~p[i];
        return r;
    }
    return p[GC_READ_MAP & 3u];
}

/*
 * Rebuild the whole read shadow -- the bottom 16 KB of the aperture, which is
 * what a guest read of A000:xxxx lands on.
 *
 * THE SHADOW IS AT THE BOTTOM OF THE APERTURE AND THAT IS LOAD-BEARING. A guest
 * access to the window has to reach it with NO change to the address fold, on
 * the single hottest path in the emulator. Putting the planes there instead
 * would make every guest memory access consult a global. Do not reorder this.
 *
 * Called only when something that changes what a read returns changes -- Read
 * Map Select, read mode, Colour Compare, Colour Don't Care -- and guarded on
 * the value ACTUALLY changing by its caller. The guard matters: the rebuild is
 * ega_plane_size byte computations, and in read mode 1 each is a four-plane
 * loop. On the reference this went unnoticed at 8 KB and then wedged a device
 * run at 64 KB.
 */
#define EGA_SHADOW_OFFSETS  0x4000u     /* 16 KB, the CPU-visible window */

static void ega_shadow_rebuild(void)
{
    uint8_t *sh = x86_vga_planar_aperture;
    uint32_t a;

    /* Mode Y has no shadow: "rebuilding" it would overwrite the picture with
       itself decoded as something else. */
    if (!sh || x86_vga_modey_armed || !x86_vga_ega_armed)
        return;
    for (a = 0; a < ega_plane_size && a < EGA_SHADOW_OFFSETS; a++)
        sh[a] = ega_read_byte(a);
    /* Offsets past the modelled planes read as zero rather than as stale bytes
       from whatever mode ran before. */
    for (; a < EGA_SHADOW_OFFSETS; a++)
        sh[a] = 0;
}

/* ---------------------------------------------------------------------------
 * THE LATCHES.
 *
 * Reloaded from the offset of the last guest read that landed in the aperture,
 * which cpu->vga_latch carries exactly (see the header comment). The reference
 * could only do this on a read-modify-write; here it is right for a plain
 * `mov al,[si]` too, which is the whole of the write-mode-1 block copy.
 */
static void latch_reload(void)
{
    const X86CPUState *cpu = cpu_state_ptr;
    const uint8_t *p;

    if (!cpu)
        return;
    x86_vga_latch_loads++;
    /* The recorded offset is a WINDOW offset. In 0Dh that is the plane offset;
       in Mode Y the window offset and the plane offset are the same number too,
       because the guest addresses planes directly there. */
    p = vga_cell(cpu->vga_latch);
    if (p) {
        latch[0] = p[0]; latch[1] = p[1];
        latch[2] = p[2]; latch[3] = p[3];
        return;
    }
    x86_vga_latch_miss++;
    if (x86_vga_modey_armed) {
        /* A latch load from an offset with no storage is a MISS on the
           off-screen cache, and the copy that follows must draw NOTHING rather
           than replicate whatever tile was copied last. 0Dh keeps the latches
           unchanged instead, deliberately -- that is the reference's behaviour
           and its tests pin it. */
        latch[0] = latch[1] = latch[2] = latch[3] = 0;
    }
}

/* Data Rotate register bits 4-3: how the source is combined with the latch. */
static inline uint8_t ega_alu(uint8_t src, uint8_t lat)
{
    switch ((GC_ROTATE >> 3) & 3u) {
    case 1:  return (uint8_t)(src & lat);
    case 2:  return (uint8_t)(src | lat);
    case 3:  return (uint8_t)(src ^ lat);
    default: return src;
    }
}

/* Apply one guest byte write at window offset `a`. The latches have already
   been reloaded for this instruction. */
static void planar_byte(uint32_t a, uint8_t v)
{
    uint8_t *p = vga_cell(a);
    uint8_t out[4], rot, bitmask;
    int i;

    if (!p) {
        x86_vga_planar_lost++;
        return;                         /* card has no memory there */
    }

    switch (GC_MODE & 3u) {
    case 1:
        /* Write mode 1: the latches go back untouched. This is the fast block
           copy every planar title leans on and the reason the latch
           bookkeeping above has to be right. */
        out[0] = latch[0]; out[1] = latch[1];
        out[2] = latch[2]; out[3] = latch[3];
        break;

    case 2:
        /* Write mode 2: the low nibble of the CPU byte is a colour; bit i of it
           fills plane i, under the Bit Mask. */
        bitmask = GC_BIT_MASK;
        for (i = 0; i < 4; i++) {
            uint8_t src = (v & (1u << i)) ? 0xFFu : 0x00u;
            src = ega_alu(src, latch[i]);
            out[i] = (uint8_t)((src & bitmask) | (latch[i] & (uint8_t)~bitmask));
        }
        break;

    case 3:
        /* Write mode 3: Set/Reset supplies the colour and the rotated CPU byte
           ANDs into the Bit Mask. */
        rot = GC_ROTATE & 7u;
        v = (uint8_t)((v >> rot) | (v << (8 - rot)));
        bitmask = (uint8_t)(GC_BIT_MASK & v);
        for (i = 0; i < 4; i++) {
            uint8_t src = (GC_SET_RESET & (1u << i)) ? 0xFFu : 0x00u;
            out[i] = (uint8_t)((src & bitmask) | (latch[i] & (uint8_t)~bitmask));
        }
        break;

    default:
        /* Write mode 0: rotate, optionally substitute Set/Reset per plane,
           combine with the latch, then merge under the Bit Mask. */
        rot = GC_ROTATE & 7u;
        v = (uint8_t)((v >> rot) | (v << (8 - rot)));
        bitmask = GC_BIT_MASK;
        for (i = 0; i < 4; i++) {
            uint8_t src = (GC_EN_SET_RESET & (1u << i))
                        ? ((GC_SET_RESET & (1u << i)) ? 0xFFu : 0x00u)
                        : v;
            src = ega_alu(src, latch[i]);
            out[i] = (uint8_t)((src & bitmask) | (latch[i] & (uint8_t)~bitmask));
        }
        break;
    }

    /* Map Mask decides which planes the write actually reaches. */
    for (i = 0; i < 4; i++)
        if (map_mask & (1u << i))
            p[i] = out[i];

    /* Keep the read shadow true. This is what buys the free read path.
       NOT IN MODE Y: there the shadow address IS plane storage -- byte `a` of
       the aperture is plane a&3 at plane offset a>>2 -- so this would scribble
       a decoded byte over a pixel four stores younger than itself. */
    if (!x86_vga_modey_armed && a < EGA_SHADOW_OFFSETS)
        x86_vga_planar_aperture[a] = ega_read_byte(a);
}

void x86_vga_planar_store(uint32_t off, uint32_t val, uint32_t size)
{
    uint8_t *ap = x86_vga_planar_aperture;
    uint32_t i;

    x86_vga_planar_stores++;
    if (!ap)
        return;                         /* armed with no storage: drop it */
    off &= (X86_VGA_APERTURE_BYTES - 1u);

    if (!x86_vga_planar_active()) {
        /* Belt and braces. Nothing arms cpu->vga_base outside a planar mode, so
           a chained 13h store never reaches here at all and mode 13h keeps its
           original zero-cost store path -- but if it ever did, a plain store is
           what it must do. */
        for (i = 0; i < size; i++)
            ap[(off + i) & (X86_VGA_APERTURE_BYTES - 1u)] = (uint8_t)(val >> (8u * i));
        return;
    }

    latch_reload();

    /* FAST PATH: write mode 1 with every plane enabled, one byte. That is the
       page-copy idiom -- `mov al,[si] / mov [di],al` -- and it is what a Mode Y
       title spends its frame doing, so it is one 32-bit copy of the latched
       group rather than twenty instructions of register file. */
    if (size == 1u && (GC_MODE & 3u) == 1u && map_mask == 0x0Fu) {
        uint8_t *p = vga_cell(off);
        if (!p) {
            x86_vga_planar_lost++;
            return;
        }
        p[0] = latch[0]; p[1] = latch[1];
        p[2] = latch[2]; p[3] = latch[3];
        if (!x86_vga_modey_armed && off < EGA_SHADOW_OFFSETS)
            ap[off] = ega_read_byte(off);
        return;
    }

    /* A word or dword store covers CONSECUTIVE window offsets, and each of them
       is a separate adapter operation with its own cell, its own Map Mask pass
       and its own Bit Mask merge. Folding them into one is how a `stosw` comes
       out half right. */
    for (i = 0; i < size; i++)
        planar_byte((off + i) & (X86_VGA_APERTURE_BYTES - 1u),
                    (uint8_t)(val >> (8u * i)));
}

/* ---------------------------------------------------------------------------
 * ARMING
 */

/* The hook is armed by giving the interpreter the aperture base; 0 disarms it,
   and 0 is safe because host pointers are 0x24xxxxxx. One store, no special
   case anywhere else. */
static void hook_set(int on)
{
    if (cpu_state_ptr)
        cpu_state_ptr->vga_base =
            (on && x86_vga_planar_aperture)
                ? (uint32_t)(uintptr_t)x86_vga_planar_aperture : 0u;
}

/*
 * Unchained 13h. Driven from Sequencer index 4 bit 3 by x86_io.c.
 *
 * Unlike 0Dh this is NOT keyed off the mode set -- it is the real register the
 * real hardware uses, because unchaining is something software does to itself
 * long after the BIOS mode set is over, without asking anyone. That is also why
 * the CALLER gates it on BDA 0040:0049 already being 0x13: a blind I/O probe
 * walking the Sequencer would otherwise reinterpret whatever mode is running as
 * planar.
 */
void x86_vga_modey_arm(int on)
{
#if X86_MODEY_CORE_NEGCTL
    (void)on;
    return;                             /* chain-4 inert: the pre-Mode-Y core */
#else
    if (!on) {
        if (!x86_vga_modey_armed)
            return;
        x86_vga_modey_armed = 0;
        hook_set(x86_vga_ega_armed);
        return;
    }
    if (x86_vga_modey_armed)
        return;                         /* idempotent */

    /* Mode 13h's mode set leaves the Graphics Controller alone -- it has no
       reason to touch it -- so a game that unchains and draws immediately would
       otherwise write through a ZERO Bit Mask and paint nothing at all. Put the
       register file in the state a planar mode set guarantees. */
    memset(gc, 0, sizeof gc);
    GC_BIT_MASK = 0xFF;
    GC_COLOR_DC = 0x0F;
    map_mask = 0x0F;
    latch[0] = latch[1] = latch[2] = latch[3] = 0;

    /* The planes are NOT cleared. They ARE the aperture, and the mode 13h set
       that just ran already cleared it; clearing again is 64 KB of work to
       reach the state we are in. It also lets a title that sets its screen up
       chained and then unchains keep what it drew. */

    /* The stride is re-learned per arming: it is a property of the software
       that unchained, not of the adapter. */
    x86_vga_modey_page = X86_MODEY_PAGE0;
    modey_prev_start = x86_vga_crtc_start();
    modey_learned = 0;

    /* The off-screen cache is separate storage and, unlike the aperture,
       nothing has just cleared it. Its contents as tiles would be noise. */
    if (modey_cache)
        memset(modey_cache, 0, 4u * modey_cache_offs);

    /* The presentation policy is per-arming state too: a stale quiet run from
       whatever ran before must not authorise a paint of a page this mode has
       not drawn yet. */
    x86_video_modey_policy_reset();

    x86_vga_modey_armed = 1;
    hook_set(1);
#endif
}

/*
 * EGA 0Dh. Keyed off the MODE SET, not off a write to Miscellaneous Output.
 *
 * The reference armed 0Dh by having its BIOS write port 0x3C2, and its own
 * design record calls that an abuse of a real register -- with the known limit
 * that a game programming the adapter directly never arms planar, and the
 * measured accident that FreeDOS walking 0x3C0-0x3CF with 0xFF armed it and
 * wiped VRAM. We have a mode table; the mode set is the honest edge, and it is
 * still before the first store.
 *
 * Returns 0 when there is no plane storage, so the mode can be REFUSED rather
 * than adopted-and-not-rendered.
 */
int x86_vga_planar_arm(int on)
{
    if (!on) {
        if (!x86_vga_ega_armed)
            return 1;
        x86_vga_ega_armed = 0;
        hook_set(x86_vga_modey_armed);
        return 1;
    }
    if (x86_vga_ega_armed)
        return 1;                       /* idempotent */
    if (!ega_planes || !ega_plane_size || !x86_vga_planar_aperture) {
        x86_vga_ega_refused++;
        return 0;
    }

    /* Any 0Dh arm ends an unchained 13h; the two are mutually exclusive and
       share the arena block. */
    x86_vga_modey_armed = 0;

    memset(gc, 0, sizeof gc);
    GC_BIT_MASK = 0xFF;
    GC_COLOR_DC = 0x0F;
    map_mask = 0x0F;
    latch[0] = latch[1] = latch[2] = latch[3] = 0;
    memset(ega_planes, 0, 4u * ega_plane_size);
    /* The WHOLE aperture, not just the shadow. An EGA in a planar mode decodes
       all 64 KB at A000:0000 and software uses it -- Keen 4's first act after
       the mode set is a `rep stosw` with CX=8000h over the lot. Only the bottom
       16 KB is the shadow; the rest must read back as ZERO, because a write up
       there reaches a plane offset this card has not fitted and is discarded.
       Leaving stale bytes would have the guest read back data it never stored,
       which is exactly the `z`-marker failure tega2.asm exists to catch. */
    memset(x86_vga_planar_aperture, 0, X86_VGA_APERTURE_BYTES);

    x86_vga_ega_armed = 1;
    ega_shadow_rebuild();
    hook_set(1);
    return 1;
}

/* ---------------------------------------------------------------------------
 * REGISTER WRITES WITH AN EFFECT. x86_io.c owns the files; these are the
 * writes that change what the adapter DOES.
 */

void x86_vga_planar_seq_write(unsigned idx, uint8_t val)
{
    if ((idx & 7u) == 2u) {
        map_mask = (uint8_t)(val & 0x0Fu);
        return;
    }
    if ((idx & 7u) == 4u) {
        /* Bit 3 is Chain-4. Clearing it is Mode Y; setting it is chained 13h.
           The CALLER has already checked we are in mode 13h. */
        x86_vga_modey_arm((val & 0x08u) == 0u);
    }
}

void x86_vga_planar_gc_write(unsigned idx, uint8_t val)
{
    uint8_t before;

    idx &= 15u;
    before = gc[idx];
    gc[idx] = val;
    if (before == val)
        return;
    /* Only four registers change what a READ returns, and the rebuild is
       expensive enough that the guard is the point -- see ega_shadow_rebuild().
       2 Colour Compare, 4 Read Map Select, 5 Mode, 7 Colour Don't Care. */
    if (idx == 2u || idx == 4u || idx == 5u || idx == 7u)
        ega_shadow_rebuild();
}

/*
 * The display start has been reprogrammed. Two consequences, and they are
 * separate: the stride learning, and the present edge.
 */
void x86_vga_planar_start_write(void)
{
    uint32_t s, d;

    if (!x86_vga_modey_armed)
        return;
    s = x86_vga_crtc_start();
    if (s == modey_prev_start)
        return;

    d = (s > modey_prev_start) ? s - modey_prev_start : modey_prev_start - s;
    modey_prev_start = s;
    if (d >= X86_MODEY_PGMIN && d <= X86_MODEY_PGMAX
        && (!modey_learned || d < x86_vga_modey_page)) {
        modey_learned = 1;
        x86_vga_modey_page = d;
    }

    /*
     * PRESENT ON THE GUEST'S PAGE FLIP.
     *
     * Aliasing the pages means the single page of storage holds, at any
     * instant, whatever the guest is drawing RIGHT NOW. Blitting at the
     * emulator's own frame boundary composites a finished frame with a
     * half-drawn one -- measured on the reference against a 256 KB card, and
     * reported as "vertical slabs from different frames". A display-start
     * change is the guest stating that the page it just left is COMPLETE, which
     * is the only instant an aliased page is known good.
     */
    x86_vga_modey_flips++;
    x86_video_page_flip();
    x86_video_modey_presented(1);
}

/* ---------------------------------------------------------------------------
 * WHEN THE FRAME LOOP MAY PAINT A MODE Y SCREEN BY ITSELF.
 *
 * The escape hatch this policy replaces was a plain "paint anyway every 8
 * frames without a flip", and it is what put the reported artefacts on the
 * panel: the ONE page of storage holds whatever the guest is drawing RIGHT NOW,
 * so a timed blit lands mid-redraw. WOLF3D flips once per GUEST frame and the
 * guest runs far below the panel rate, so the hatch fired between essentially
 * every pair of flips -- half a raycast viewport in gameplay, an erase-then-draw
 * menu highlight caught between its two halves in the menus.
 *
 * The fix is not a longer timer, it is a better question. A guest that is
 * drawing is STORING; the store counter is already maintained on the hot path.
 * So the frame loop may paint only when the aperture has been QUIET for two
 * whole guest slices -- which is a completed picture by construction, and is
 * exactly the case the hatch exists for (a menu that draws once and then waits
 * for a key, a title screen, a pause). A guest that stores forever and never
 * flips would otherwise never be shown at all, so a long stale bound remains;
 * at one second it cannot fire inside a title that is flipping, because every
 * flip resets it.
 *
 * ===========================================================================
 * AND WHY A FLAT ONE-SECOND BOUND WAS STILL THE GAMEPLAY BUG
 * ===========================================================================
 *
 * The quiet rule fixed the MENUS, which do go quiet, and left GAMEPLAY exactly
 * as broken as it was. A raycaster never goes quiet: it stores in every frame
 * of its redraw, so `modey_quiet` never reaches 2 and the ONLY rule that could
 * ever fire was the 60-frame staleness bound -- which by construction fires 60
 * frames after the last flip, i.e. at an arbitrary instant in the middle of the
 * next redraw. A WOLF3D gameplay frame on the device takes far longer than 60
 * frame-loop iterations, so the bound fired inside essentially every guest
 * frame. That is the SAME artefact as the 8-frame hatch, at 60 frames instead
 * of 8: a split viewport, and a house of mirrors while turning.
 *
 * So the bound is no longer a constant. Its only job is the ANTI-FREEZE
 * property -- a title that stops flipping must not leave a dead panel -- and a
 * title that IS flipping is not frozen, however slowly it flips. It is
 * therefore scaled to what this guest has been OBSERVED to do: for a title that
 * has flipped, it may not fire until the screen has stood unpainted for several
 * times the longest gap between flips that title has ever shown, so a flip
 * always gets there first. A title that has never flipped has no gap, keeps the
 * flat one-second bound, and is the only kind of title the bound now serves. A
 * flipping title that later stops is still rescued -- after a delay measured in
 * ITS OWN frame times rather than in ours, which is the only unit that means
 * anything here.
 *
 * AND THE BOUND BACKS OFF WHEN IT IS WRONG, because the worst gap seen SO FAR
 * is a lower bound on the gap, never the gap. WOLF3D's menus flip fast and its
 * gameplay flips slowly, so entering a level the estimate is stale by an order
 * of magnitude and the bound fires once inside the first slow frame. There is
 * no signal that separates "drawing a slow frame" from "stopped flipping and
 * still drawing" -- they are the same observation -- so the bound cannot be
 * made right first time. It is made SELF-CORRECTING instead: every stale paint
 * inside a flipping title doubles the floor, so a wrong estimate costs at most
 * a couple of frames and then converges above the guest's real frame time and
 * stays there. The tier-1 model measures exactly that: the transition is
 * allowed one artefact, and the run after it must be clean.
 */
#define X86_MODEY_IDLE_FRAMES   2u    /* frames since the last present         */
#define X86_MODEY_QUIET_FRAMES  2u    /* consecutive frames with NO store      */
#define X86_MODEY_STALE_FRAMES  60u   /* ~1 s: nothing may freeze indefinitely */
#define X86_MODEY_FLIP_SLACK    8u    /* x the worst flip gap, once it flips   */
#define X86_MODEY_GAP_CLAMP     0x100000u  /* keeps the product in range       */

volatile uint32_t x86_modey_fallbacks;    /* quiet-page paints by the loop    */
volatile uint32_t x86_modey_stale_paints; /* the last-resort bound firing     */
volatile uint32_t x86_modey_flip_gap;     /* frames between the last 2 flips  */
volatile uint32_t x86_modey_flip_gap_max;
/*
 * THE DEFECT COUNTER, and it is the whole point of the instrumentation. A paint
 * the LOOP took inside a title that flips, with stores banked since that flip,
 * IS a mid-draw paint -- the split viewport, counted, with the store count
 * saying how half-drawn the page was. It must stay at zero during gameplay.
 */
volatile uint32_t x86_modey_mid_draw;     /* loop paints with stores pending  */
volatile uint32_t x86_modey_mid_draw_max; /* worst store count at such a paint*/

static uint32_t modey_idle;        /* frames since the last present           */
static uint32_t modey_stale;       /* frames since the last paint of any kind */
static uint32_t modey_quiet;       /* consecutive frames with no planar store */
static uint32_t modey_stores_seen; /* x86_vga_planar_stores a frame ago       */
static uint32_t modey_dirty;       /* stores since the last frame put on show */
static uint32_t modey_flipper;     /* this title has flipped at least once    */
static uint32_t modey_stores_flip; /* x86_vga_planar_stores at the last flip  */
static uint32_t modey_stale_floor = X86_MODEY_STALE_FRAMES;

/* How long the screen must have stood unpainted before the staleness bound may
   fire. Flat until this title flips; afterwards the larger of a multiple of its
   own worst flip gap and the floor the back-off has reached, so that a flip
   always beats it to the paint. */
static uint32_t modey_stale_limit(void)
{
    uint32_t gap;

    if (!modey_flipper)
        return X86_MODEY_STALE_FRAMES;
    gap = x86_modey_flip_gap_max;
    if (gap > X86_MODEY_GAP_CLAMP)
        gap = X86_MODEY_GAP_CLAMP;
    gap *= X86_MODEY_FLIP_SLACK;
    return (gap > modey_stale_floor) ? gap : modey_stale_floor;
}

/* Per-ARMING policy state. The witnesses above are cumulative and host-visible;
   this is not. A stale quiet run -- or a stale "this title flips" -- left by
   whatever ran before must not authorise a paint of a page this mode has not
   drawn yet. */
void x86_video_modey_policy_reset(void)
{
    modey_flipper = 0;
    modey_stores_flip = x86_vga_planar_stores;
    modey_stale_floor = X86_MODEY_STALE_FRAMES;
    x86_video_modey_presented(0);
}

void x86_video_modey_presented(int from_flip)
{
    if (from_flip) {
        x86_modey_flip_gap = modey_idle;
        if (modey_idle > x86_modey_flip_gap_max)
            x86_modey_flip_gap_max = modey_idle;
        modey_flipper = 1;
        modey_stores_flip = x86_vga_planar_stores;
    }
    modey_idle = 0;
    modey_stale = 0;
    modey_quiet = 0;
    modey_dirty = 0;
    modey_stores_seen = x86_vga_planar_stores;
}

int x86_video_modey_paint_due(void)
{
    uint32_t stores;

    /* Every other mode owes a paint every frame: its storage is not aliased,
       so there is no half-drawn page to catch. */
    if (!x86_vga_modey_armed)
        return 1;

    stores = x86_vga_planar_stores;
    if (stores == modey_stores_seen) {
        modey_quiet++;
    } else {
        modey_quiet = 0;
        modey_dirty = 1;    /* there is something NEW to show, once it settles */
    }
    modey_stores_seen = stores;

    modey_idle++;
    modey_stale++;

#if X86_MODEY_TIMER_NEGCTL
    /* THE NEGATIVE CONTROL: the escape hatch as it shipped -- a timer, with no
       question asked about whether the guest had finished drawing. This is the
       artefact, compiled back in, and the presentation test must fail here or
       it is grading nothing. */
    if (modey_idle >= 8u) {
        x86_modey_fallbacks++;
        modey_idle = 0;
        modey_stale = 0;
        modey_dirty = 0;
        return 1;
    }
    return 0;
#endif

    /* Nothing has been drawn since the last present: both LCD buffers already
       hold it, so there is nothing to paint and repainting it every other frame
       is the whole frame budget spent on a static picture. */
    if (!modey_dirty)
        return 0;

    if (modey_quiet >= X86_MODEY_QUIET_FRAMES
        && modey_idle >= X86_MODEY_IDLE_FRAMES) {
        /* A quiet page is a finished page whatever else is true of it, so this
           is never a mid-draw paint and never evidence about the flip rate. */
        x86_modey_fallbacks++;
        modey_idle = 0;
        modey_stale = 0;
        modey_dirty = 0;
        return 1;
    }
    if (modey_stale < modey_stale_limit())
        return 0;
    x86_modey_stale_paints++;
    /* The bound fired. If this title flips and has stored since its last flip,
       what we are about to put on the panel is a HALF-DRAWN page -- the split
       viewport. Count it, and how far into the redraw it was. */
    if (modey_flipper && stores != modey_stores_flip) {
        uint32_t n = stores - modey_stores_flip;
        x86_modey_mid_draw++;
        if (n > x86_modey_mid_draw_max)
            x86_modey_mid_draw_max = n;
        /* BACK OFF. This title flips, and we painted it mid-draw anyway, so the
           estimate of its frame time was too short -- double it. Two or three
           of these and the bound is above the guest's real frame and silent for
           the rest of the level; without it, a menu-to-gameplay transition
           re-arms the artefact for good. */
        if (modey_stale_floor < X86_MODEY_GAP_CLAMP)
            modey_stale_floor *= 2u;
    }
    modey_idle = 0;
    modey_stale = 0;
    modey_dirty = 0;
    return 1;
}

/* "The page you were displaying is finished; show it." Overridden by the
   frame loop on the device; a no-op in the tier-1 harness and anywhere else
   with no LCD, so every link resolves. */
__attribute__((weak)) void x86_video_page_flip(void) { }
