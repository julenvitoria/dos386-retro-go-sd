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
#ifndef X86_MODEY_QUIET_NEGCTL
#define X86_MODEY_QUIET_NEGCTL 0    /* "a quiet page is a finished page": the */
#endif                              /* flat two-frame quiet rule, with no     */
                                    /* account taken of a raycaster's         */
                                    /* WITHIN-FRAME store-free gaps. The      */
                                    /* state hardware was captured in         */
#ifndef X86_MODEY_BACKOFF_NEGCTL
#define X86_MODEY_BACKOFF_NEGCTL 0  /* the DOUBLING back-off that replaced it */
#endif                              /* and froze the intro/menus for ~0.5 s:  */
                                    /* monotonic, so the menus paid           */
                                    /* gameplay's bill. Must fail the         */
                                    /* PROMPTNESS arm                         */

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
/*
 * THE DISPLAY START IS 16 BITS PROGRAMMED AS TWO BYTES, AND HALF OF IT IS NOT
 * AN ADDRESS.
 *
 * Every VGA program writes CRTC 0x0C (high) and 0x0D (low) as two separate
 * OUTs. Between them the register file holds the new half of the page being
 * flipped to and the OLD half of the page being flipped from -- an address the
 * guest never asked for. Recomputing `(crtc[0x0C] << 8) | crtc[0x0D]` on each
 * write and acting on it therefore:
 *
 *   - counts TWO flips per real flip whenever both halves change, which is
 *     twice the blits and twice the present edges;
 *   - paints a whole frame sourced from the hybrid address, i.e. rows fetched
 *     from an offset the guest never programmed. The correct paint follows one
 *     instruction later, so the hybrid is not what settles on the panel -- but
 *     it is a full 2 x 64,000-pixel blit thrown away, from inside the guest
 *     slice, on every single page flip;
 *   - and, worst, feeds the STRIDE LEARNING a distance measured between a real
 *     start and a hybrid one. The learner takes the SMALLEST accepted distance
 *     and latches it for the arming, so one hybrid inside [16000, 20480] mislays
 *     every page for the rest of the title.
 *
 * Real hardware does not have this problem: the start register pair is latched
 * as a unit at vertical retrace, never mid-pair. So we latch too. A half is
 * recorded as it arrives and the value is committed when the PAIR is complete,
 * in either order. A guest that writes only one half (a coarse vertical scroll)
 * still gets its start -- committed at the next frame boundary, one loop frame
 * late, which is invisible and is the only case that costs anything at all.
 */
#ifndef X86_MODEY_START_NEGCTL
#define X86_MODEY_START_NEGCTL 0    /* 1: act on each byte, hybrids and all */
#endif

#define MODEY_START_HI  1u
#define MODEY_START_LO  2u
#define MODEY_START_BOTH (MODEY_START_HI | MODEY_START_LO)

volatile uint32_t x86_modey_start_partial;
static uint8_t modey_start_seen;

static void modey_start_commit(void)
{
    uint32_t s, d;

    modey_start_seen = 0;
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

void x86_vga_planar_start_write(unsigned idx)
{
    if (!x86_vga_modey_armed)
        return;
#if X86_MODEY_START_NEGCTL
    /* THE NEGATIVE CONTROL: recompute and act on every byte, hybrid or not.
       This is the code the hardware capture was taken against, and the Mode Y
       arm's flip log must fail here or it is grading nothing. */
    (void)idx;
    modey_start_commit();
#else
    modey_start_seen |= (idx == 0x0Du) ? MODEY_START_LO : MODEY_START_HI;
    if (modey_start_seen == MODEY_START_BOTH) {
        modey_start_commit();
        return;
    }
    /* One half so far. Hold it: the other one is almost always the very next
       instruction, and if it never comes the frame boundary commits it. */
    x86_modey_start_partial++;
#endif
}

/* A half-written start that no second half ever completed. Committed at the
   frame boundary so a guest that programs only one half is at most one loop
   frame late, and never so that a hybrid is acted on. */
static void modey_start_settle(void)
{
    if (modey_start_seen)
        modey_start_commit();
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
/*
 * ===========================================================================
 * AND WHY "A QUIET PAGE IS A FINISHED PAGE" WAS STILL THE GAMEPLAY BUG
 * ===========================================================================
 *
 * The staleness back-off above made the STALE bound silent during gameplay, and
 * hardware still showed the house of mirrors. `x86_modey_mid_draw` agreed with
 * the code and said zero -- because it was only ever incremented on the stale
 * path. The counter that was actually moving was `x86_modey_fallbacks`: 62 of
 * them in a hardware capture whose flip count only reached 94.
 *
 * Those are QUIET-page paints, and the rule that authorises them asserts, in a
 * comment, that "a quiet page is a finished page whatever else is true of it".
 * THAT ASSERTION IS FALSE FOR A RAYCASTER. A guest frame is not one unbroken
 * run of stores: WOLF3D casts rays, sorts its vislist, pulls texture pages
 * through its page manager and waits on the tic counter, all with the aperture
 * untouched. Any of those gaps that spans two frame-loop slices reads exactly
 * like a menu that has finished drawing, so the loop painted the one aliased
 * page with the redraw partway across it -- the split viewport, and, caught on
 * successive gaps at successive rotations, the scene repeated across the
 * viewport.
 *
 * There is NO signal that separates "quiet because finished" from "quiet in the
 * middle of a frame", so the rule cannot be right first time and has to learn.
 *
 * ===========================================================================
 * AND WHY THE FIRST VERSION OF THAT LEARNING BROKE THE INTRO AND THE MENUS
 * ===========================================================================
 *
 * It was a DOUBLING BACK-OFF, in the staleness bound's shape: every quiet paint
 * taken mid-redraw doubled the quiet run required for the next one, clamped at
 * 32 frames. It fixed gameplay and was reported from hardware as a regression:
 *
 *   "there's an issue in the intro/menu. If you press a button to skip, it just
 *    freezes the image for a moment and then jumps to where you tried to get
 *    to."
 *
 * That is the clamp, seen from the sofa. ~0.5 s of paint latency on a screen
 * that is finished, so the states between are never shown and the panel appears
 * to teleport. Three things made it inevitable and each is worth stating:
 *
 *  1. WOLF3D'S INTRO AND MENUS DO NOT FLIP. Measured, and it is the whole
 *     reason they depend on this rule at all: `flips=2` across the entire
 *     signon/title/menu phase (docs/08 section 4, and the hardware capture that
 *     motivated the gameplay fix -- 2 flips at t=0, 94 by the end of a level).
 *     They draw into the page they are already displaying and call
 *     VW_SetScreen with an UNCHANGED value, which x86_vga_planar_start_write()
 *     correctly ignores. So a flip cannot rescue them: the quiet rule IS their
 *     presentation path, and its latency is their frame time.
 *  2. `modey_flipper` is sticky for the whole arming, so the two early flips
 *     were enough to put every later menu under the raycaster's rule.
 *  3. The back-off was MONOTONIC. It converged on the worst thing the guest had
 *     ever done and then stayed there, so the menus paid gameplay's bill.
 *
 * TWO THINGS CHANGED, and (2) is the one that answers the report.
 *
 * (1) The requirement is no longer a back-off, it is a MEASUREMENT: the longest
 * store-free gap this guest has been SEEN to take in the middle of a redraw --
 * a quiet run with unpainted stores banked behind it that ended because the
 * guest STORED AGAIN rather than because we painted it. A gap that ends in more
 * stores is, by construction, a gap inside a frame; nothing else observable
 * says so. The loop may take a quiet paint mid-redraw only when the aperture
 * has been quiet for LONGER than any such gap, which is the smallest rule that
 * excludes every gap this guest has actually shown -- and, crucially, is a
 * property of the raycaster's own behaviour rather than a number that only ever
 * grows. WOLF3D's in-frame gaps are a few frames, so a menu is painted a few
 * frames after it goes quiet instead of half a second.
 *
 * Two details that are not decoration:
 *
 *  - The measurement is taken ONLY while `modey_dirty`. The long quiet after a
 *    flip -- game logic, AI, the tic wait -- has nothing banked behind it and is
 *    not evidence about redraws. Counting it would learn most of a guest frame
 *    and put us straight back at the clamp, which is the regression again.
 *  - It is still CLAMPED, at the same 32 frames, but now as a bound on how bad
 *    the measurement is allowed to get rather than as the destination.
 *
 * (2) `modey_flipper` DECAYS -- see modey_is_flipping(). Being a title that
 * flips is not a permanent property earned by two flips during the signon; it
 * is a description of what the guest is doing NOW. WOLF3D's intro and menus
 * draw straight into the page they are displaying, and on real hardware you
 * WATCH them being drawn. Holding the old screen until the new one is finished
 * is the "freezes and then jumps" report almost word for word, and it is not a
 * latency after the guest goes quiet -- it is the whole DRAW that is hidden.
 * Once the guest has not flipped for several times its own worst flip gap, the
 * flat two-frame rule comes back and the intro is progressively revealed again,
 * which is exactly the behaviour it had before any of this work.
 *
 * A title that has never flipped AT ALL was always untouched, and now a title
 * that has stopped flipping rejoins it.
 *
 * THE TEST SUITE HAD THE SAME BLIND SPOT AS THE CODE, and that is why this
 * shipped: every presentation arm asserted that paints did NOT happen, and not
 * one asserted that a paint happened SOON. `which == 6` is the promptness arm,
 * and it fails against the doubling back-off.
 */
#define X86_MODEY_IDLE_FRAMES   2u    /* frames since the last present         */
#define X86_MODEY_QUIET_FRAMES  2u    /* consecutive frames with NO store      */
#define X86_MODEY_QUIET_CLAMP   32u   /* ~0.5 s: the measurement's upper bound */
#define X86_MODEY_STALE_FRAMES  60u   /* ~1 s: nothing may freeze indefinitely */
#define X86_MODEY_FLIP_SLACK    8u    /* x the flip cadence, for ANTI-FREEZE   */
/*
 * AND A MUCH TIGHTER ONE FOR "IS THIS GUEST FLIPPING NOW?".
 *
 * The two windows were the same number and they are not the same question. The
 * anti-freeze bound must be generous -- being late to rescue a dead panel costs
 * nothing, being early puts a half-drawn page on it -- so eight times the
 * cadence is right there. RECENCY is the opposite trade: while it says "this
 * guest flips", every quiet screen is judged by the raycaster's rule and its
 * whole draw is hidden. At 8 x a hardware-measured 34-frame gap that is 272
 * loop frames, ~4.5 s, which is exactly the reported "pressing escape back to
 * the menu holds a frame... it does fix itself given some time though, it like
 * caught up". A guest that has not flipped in TWO of its own frame times has
 * stopped flipping; there is no evidence in a third.
 */
#define X86_MODEY_FLIP_RECENT   2u    /* x the flip cadence, for RECENCY       */
#define X86_MODEY_RECENT_MIN    8u    /* ...never shorter than the quiet rule  */
#define X86_MODEY_GAP_CLAMP     0x100000u  /* keeps the product in range       */

volatile uint32_t x86_modey_fallbacks;    /* quiet-page paints by the loop    */
volatile uint32_t x86_modey_stale_paints; /* the last-resort bound firing     */
volatile uint32_t x86_modey_flip_gap;     /* frames between the last 2 flips  */
volatile uint32_t x86_modey_flip_gap_max; /* WITNESS ONLY -- see _est below   */
/*
 * THE CADENCE THE WINDOWS ARE SCALED BY, AND WHY IT IS NOT `_max`.
 *
 * `x86_modey_flip_gap_max` is a HIGH-WATER MARK: it can only grow, for the whole
 * arming. Once WOLF3D's gameplay pushes it to 34 every quiet screen after it --
 * every menu, every intro, every pause -- is judged against 34 for the rest of
 * the title, however fast the guest is flipping by then. A title that flipped
 * slowly ONCE permanently penalises everything that follows it, and nothing ever
 * takes the penalty off.
 *
 * So the windows are scaled by an estimate that can come DOWN: it jumps
 * immediately to a longer gap (being late is the safe direction, and a
 * raycaster's first slow frame must widen the window at once) and eases towards
 * a shorter one, halving the error each flip. `_max` stays as a witness because
 * probes and hardware captures quote it.
 */
volatile uint32_t x86_modey_flip_gap_est;
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
static uint32_t modey_flip_age;    /* frames since the last flip -- the decay */
static uint32_t modey_stores_flip; /* x86_vga_planar_stores at the last flip  */
static uint32_t modey_stale_floor = X86_MODEY_STALE_FRAMES;
/*
 * The longest store-free gap this guest has been SEEN to take in the middle of
 * a redraw -- quiet, with unpainted stores banked, ended by MORE STORES rather
 * than by a paint. A mid-redraw quiet paint needs a longer run than this. It is
 * host-visible because it is the number that decides menu latency, and reading
 * it off a target is how "the menus are slow" stops being a guess.
 */
volatile uint32_t x86_modey_inframe_gap;
/* The doubling back-off, kept only for X86_MODEY_BACKOFF_NEGCTL. */
static uint32_t modey_quiet_floor = X86_MODEY_QUIET_FRAMES;
/* Consecutive quiet frames, reset ONLY by the guest storing again -- never by a
   paint, which is what makes a run we painted inside still measurable. */
static uint32_t modey_gap;

/* How long the screen must have stood unpainted before the staleness bound may
   fire. Flat until this title flips; afterwards the larger of a multiple of its
   own worst flip gap and the floor the back-off has reached, so that a flip
   always beats it to the paint. */
/*
 * IS THIS GUEST FLIPPING *NOW*?
 *
 * `modey_flipper` is sticky for the whole arming, and that is what put
 * WOLF3D's intro and menus under the raycaster's rule: they inherit two flips
 * made during the signon and then never flip again. A title that last presented
 * a page thousands of frames ago is not "a title that flips" -- it is a title
 * that draws straight into the page it is displaying, which is what an intro
 * screen and a menu do, and on real hardware you WATCH those being drawn.
 * Suppressing their intermediate states is not more correct, it is less: it is
 * the reported "freezes the image for a moment and then jumps to where you
 * tried to get to", which is the panel holding the OLD screen for the whole
 * time the new one is being painted into memory.
 *
 * So the evidence decays, against the same threshold the anti-freeze bound
 * already uses: a title that has not flipped in several times its own worst
 * flip gap has stopped flipping, whatever it did earlier. Gameplay flips every
 * 20-34 frames on hardware and is never near it; the intro crosses it in a few
 * seconds and gets the flat two-frame rule back -- exactly the behaviour it had
 * before any of this, which is the behaviour that was not complained about.
 */
#ifndef X86_MODEY_RECENCY_NEGCTL
#define X86_MODEY_RECENCY_NEGCTL 0
#endif

static uint32_t modey_stale_limit(void);
static uint32_t modey_flip_recent(void);

static int modey_is_flipping(void)
{
#if X86_MODEY_BACKOFF_NEGCTL
    /* THE NEGATIVE CONTROL, and it is the OTHER half of what regressed: being a
       title that flips was a permanent property earned by two flips during the
       signon. With this and the doubling above, the policy is byte for byte the
       one the owner reported the intro freeze against, and the PROMPTNESS arm
       must fail here or it is grading nothing. */
    return (int)modey_flipper;
#else
    /* The anti-freeze bound's own threshold: it is already this file's
       definition of "long enough without a flip that something must be
       wrong". Reusing it means there is ONE number for "this guest has
       stopped flipping" rather than two that can disagree. */
#if X86_MODEY_RECENCY_NEGCTL
    /* THE NEGATIVE CONTROL for symptom B: recency off the HIGH-WATER MARK and
       the anti-freeze slack, byte for byte the rule the escape-to-menu hold was
       reported against. The escape arm must fail here. */
    {
        uint32_t gap = x86_modey_flip_gap_max;
        if (gap > X86_MODEY_GAP_CLAMP)
            gap = X86_MODEY_GAP_CLAMP;
        gap *= X86_MODEY_FLIP_SLACK;
        if (gap < modey_stale_floor)
            gap = modey_stale_floor;
        return modey_flipper && modey_flip_age < gap;
    }
#else
    return modey_flipper && modey_flip_age < modey_flip_recent();
#endif
#endif
}

static uint32_t modey_stale_limit(void)
{
    uint32_t gap;

    if (!modey_flipper)
        return X86_MODEY_STALE_FRAMES;
    gap = x86_modey_flip_gap_est;
    if (gap > X86_MODEY_GAP_CLAMP)
        gap = X86_MODEY_GAP_CLAMP;
    gap *= X86_MODEY_FLIP_SLACK;
    return (gap > modey_stale_floor) ? gap : modey_stale_floor;
}

/* How recently this guest must have flipped for its quiet gaps to be read as
   gaps in a REDRAW rather than as a finished screen. See X86_MODEY_FLIP_RECENT:
   short, because everything this window covers is a screen held back. */
static uint32_t modey_flip_recent(void)
{
    uint32_t gap = x86_modey_flip_gap_est;

    if (gap > X86_MODEY_GAP_CLAMP)
        gap = X86_MODEY_GAP_CLAMP;
    gap *= X86_MODEY_FLIP_RECENT;
    return (gap > X86_MODEY_RECENT_MIN) ? gap : X86_MODEY_RECENT_MIN;
}

/* Per-ARMING policy state. The witnesses above are cumulative and host-visible;
   this is not. A stale quiet run -- or a stale "this title flips" -- left by
   whatever ran before must not authorise a paint of a page this mode has not
   drawn yet. */
void x86_video_modey_policy_reset(void)
{
    modey_start_seen = 0;
    x86_modey_flip_gap_est = 0;
    modey_flipper = 0;
    modey_flip_age = 0;
    modey_stores_flip = x86_vga_planar_stores;
    modey_stale_floor = X86_MODEY_STALE_FRAMES;
    modey_quiet_floor = X86_MODEY_QUIET_FRAMES;
    /* Per-arming, like the rest of the policy state: the in-frame gaps of
       whatever ran before are not evidence about this guest. */
    x86_modey_inframe_gap = 0;
    modey_gap = 0;
    x86_video_modey_presented(0);
}

static void modey_gap_observe(uint32_t gap);

void x86_video_modey_presented(int from_flip)
{
    if (from_flip) {
        x86_modey_flip_gap = modey_idle;
        if (modey_idle > x86_modey_flip_gap_max)
            x86_modey_flip_gap_max = modey_idle;
        modey_gap_observe(modey_idle);
        modey_flipper = 1;
        modey_flip_age = 0;
        modey_stores_flip = x86_vga_planar_stores;
    }
    modey_idle = 0;
    modey_stale = 0;
    modey_quiet = 0;
    modey_dirty = 0;
    modey_stores_seen = x86_vga_planar_stores;
}

/* The flip cadence the windows are scaled by. Jumps UP, eases DOWN. */
static void modey_gap_observe(uint32_t gap)
{
    uint32_t est = x86_modey_flip_gap_est;

    if (gap > X86_MODEY_GAP_CLAMP)
        gap = X86_MODEY_GAP_CLAMP;
    if (est == 0u || gap >= est)
        est = gap;
    else
        est -= (est - gap) >> 1;
    x86_modey_flip_gap_est = est;
}

int x86_video_modey_paint_due(void)
{
    uint32_t stores;

    /* Every other mode owes a paint every frame: its storage is not aliased,
       so there is no half-drawn page to catch. */
    if (!x86_vga_modey_armed)
        return 1;

    /* A half-written start held from the guest slice: commit it here, where the
       pair is demonstrably not coming. */
    modey_start_settle();

    stores = x86_vga_planar_stores;
    if (stores == modey_stores_seen) {
        modey_quiet++;
        modey_gap++;
    } else {
        /*
         * A quiet run just ENDED IN MORE STORES. Was it a pause INSIDE a
         * redraw? Three conditions, and each one is doing work:
         *
         *   - this guest flips, so it presents its own finished frames;
         *   - the stores the run sat on top of were made SINCE the last flip,
         *     so the page was mid-redraw underneath it (`modey_stores_seen` is
         *     still the pre-resume count at this point, which is the value that
         *     matters -- the new stores are the resumption, not the evidence);
         *   - it was short enough that we could plausibly have painted inside
         *     it. A run longer than the clamp is not evidence about redraws
         *     however it ended: it is a menu waiting for a key, and learning it
         *     is precisely the ~0.5 s latency this design replaced.
         *
         * `modey_gap` is deliberately NOT the same counter as `modey_quiet`:
         * `modey_quiet` is reset by a paint, and a run we painted inside is
         * exactly the run we most need to measure, or the rule never converges
         * and the same pause is repainted every guest frame for the level.
         */
        if (modey_gap != 0u && modey_gap <= X86_MODEY_QUIET_CLAMP
            && modey_is_flipping() && modey_stores_seen != modey_stores_flip
            && modey_gap > x86_modey_inframe_gap)
            x86_modey_inframe_gap = modey_gap;
        modey_gap = 0;
        modey_quiet = 0;
        modey_dirty = 1;    /* there is something NEW to show, once it settles */
    }
    modey_stores_seen = stores;

    modey_idle++;
    modey_stale++;
    modey_flip_age++;

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

    {
        /* Is this guest demonstrably PARTWAY THROUGH A REDRAW? It has flipped,
           so it presents its own frames, and it has stored since that flip, so
           what is in the page is newer than the frame it last called complete.
           A quiet run inside that is a gap in a redraw, not a finished picture
           -- see the block comment above, and the 62 fallbacks on hardware. */
        const int midredraw = (modey_is_flipping()
                               && stores != modey_stores_flip);
        uint32_t need;

#if X86_MODEY_QUIET_NEGCTL
        need = X86_MODEY_QUIET_FRAMES;      /* the rule that shipped first  */
#elif X86_MODEY_BACKOFF_NEGCTL
        need = midredraw ? modey_quiet_floor    /* the doubling that froze  */
                         : X86_MODEY_QUIET_FRAMES;  /* the intro and menus  */
#else
        /* LONGER than any in-frame gap this guest has shown, and no longer.
           The menus are not charged for the raycaster's worst pause: this is a
           measurement of the guest, not a number that only grows. */
        need = X86_MODEY_QUIET_FRAMES;
        if (midredraw && x86_modey_inframe_gap >= need)
            need = x86_modey_inframe_gap + 1u;
#endif
        if (modey_quiet >= need && modey_idle >= X86_MODEY_IDLE_FRAMES) {
            x86_modey_fallbacks++;
            if (midredraw) {
                /* Counted as what it is. The same witness the stale path uses,
                   so "the loop put a half-drawn page on the panel" has ONE
                   number however the loop got there. */
                uint32_t n = stores - modey_stores_flip;
                x86_modey_mid_draw++;
                if (n > x86_modey_mid_draw_max)
                    x86_modey_mid_draw_max = n;
                if (modey_quiet_floor < X86_MODEY_QUIET_CLAMP)
                    modey_quiet_floor *= 2u;    /* BACKOFF_NEGCTL only */
            }
            modey_idle = 0;
            modey_stale = 0;
            modey_dirty = 0;
            return 1;
        }
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
