/*
 * Demand paging over the fold table. The design, the tiers and the
 * compare-at-eviction dirty rule are in x86_pager.h; this file is the
 * mechanism.
 *
 * Nothing here runs on the fast path. Every entry point is reached from a
 * trap the interpreter was already taking for an unmapped page, or from the
 * host side, so the cost of the whole subsystem to a resident access is zero
 * instructions -- which is the property that has to be preserved and is why
 * not one byte of assembly changes.
 */
#include <stdint.h>
#include <string.h>

#include "cutils.h"
#include "x86_pager.h"
#include "x86_arena.h"
#include "x86_cpu.h"

volatile uint32_t x86_pg_frames_total = 0;
volatile uint32_t x86_pg_frames_free  = 0;
volatile uint32_t x86_pg_resident     = 0;
volatile uint32_t x86_pg_faults_r     = 0;
volatile uint32_t x86_pg_faults_w     = 0;
volatile uint32_t x86_pg_faults_host  = 0;
volatile uint32_t x86_pg_fill_zero    = 0;
volatile uint32_t x86_pg_fill_nor     = 0;
volatile uint32_t x86_pg_fill_file    = 0;
volatile uint32_t x86_pg_evictions    = 0;
volatile uint32_t x86_pg_drop_clean   = 0;
volatile uint32_t x86_pg_wb_zero      = 0;
volatile uint32_t x86_pg_wb_file      = 0;
volatile uint32_t x86_pg_wb_bytes     = 0;
volatile uint32_t x86_pg_wb_opens     = 0;
volatile uint32_t x86_pg_wb_us_worst  = 0;
volatile uint32_t x86_pg_io_errors    = 0;
volatile uint32_t x86_pg_exhausted    = 0;
volatile uint32_t x86_pg_unmapped     = 0;
volatile uint32_t x86_pg_donated      = 0;

/* ---- per-page state ------------------------------------------------------
 *
 * Three arrays indexed by guest page number, 8 KB of DTCM for the whole 8 MB
 * space. Deliberately NOT packed into the fold entry: the fold is read on
 * every guest memory access and the one-add translation is what it buys, so
 * anything the fast path does not need stays out of it.
 */
#define PG_RESIDENT   0x01u
#define PG_TIER_MASK  0x06u
#define PG_TIER_ZERO  0x00u
#define PG_TIER_NOR   0x02u
#define PG_TIER_FILE  0x04u

/* The block is supplied by the caller, and it belongs in DTCM. It is read on
 * every fault and written on every hotness bump, and DTCM is the 0-wait-state
 * memory this project pins its hot state to. It is not declared here because a
 * retro-go core cannot place static .bss in DTCM -- that pool is handed out at
 * runtime through mem_ctl() -- and 8 KB of ordinary .bss overflowed RAM_EMU in
 * the tier-1 harness by 176 bytes, which is exactly the silent-overrun shape
 * this project keeps getting bitten by. */
static uint8_t  *pg_flags;
static uint8_t  *pg_hot;
static uint16_t *pg_frame;

int x86_pager_set_tables(void *block, uint32_t bytes)
{
    if (!block || bytes < X86_PG_TABLE_BYTES)
        return -1;
    uint8_t *b = (uint8_t *)block;
    pg_flags = b;
    pg_hot   = b + X86_PG_PAGES;
    pg_frame = (uint16_t *)(b + 2u * X86_PG_PAGES);
    x86_pager_reset();
    return 0;
}

#define NO_FRAME 0xFFFFu

/* ---- frames --------------------------------------------------------------
 *
 * A frame is one 4 KB page of AXI. They come from up to four extents, because
 * the AXI this part can offer is not one block: RAM_EMU past the core's BSS
 * and whatever the LCD bonus pool has left are disjoint.
 */
#define MAX_FRAMES 256u
#define MAX_EXTENTS 4u

typedef struct { uint8_t *base; uint32_t first, count; } extent_t;
static extent_t extents[MAX_EXTENTS];
static uint32_t extent_count;

static uint16_t frame_page[MAX_FRAMES];     /* which guest page holds it */
static uint16_t free_list[MAX_FRAMES];
static uint32_t frame_count;
static uint32_t free_count;

/* The NOR snapshot, mirrored from x86_nor.c so a fill can read it. */
static const uint8_t *nor_base;
static uint32_t nor_pages;
static uint32_t nor_first;

static x86_pg_read_fn  backing_read;
static x86_pg_write_fn backing_write;

static uint32_t own_first, own_count;

#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004)

static uint8_t *frame_addr(uint32_t f)
{
    for (uint32_t e = 0; e < extent_count; e++) {
        if (f >= extents[e].first && f < extents[e].first + extents[e].count)
            return extents[e].base + (f - extents[e].first) * X86_PG_STRIDE;
    }
    return NULL;
}

void x86_pager_reset(void)
{
    if (pg_flags) {
        memset(pg_flags, 0, X86_PG_PAGES);
        memset(pg_hot, 0, X86_PG_PAGES);
        for (uint32_t i = 0; i < X86_PG_PAGES; i++)
            pg_frame[i] = NO_FRAME;
    }
    memset(extents, 0, sizeof(extents));
    extent_count = frame_count = free_count = 0;
    own_first = own_count = 0;
    x86_pg_frames_total = x86_pg_frames_free = x86_pg_resident = 0;
}

int x86_pager_add_frames(void *base, uint32_t bytes)
{
    if (!base || extent_count >= MAX_EXTENTS)
        return -1;
    uint32_t start = ((uint32_t)(uintptr_t)base + X86_PG_SIZE - 1u)
                     & ~(X86_PG_SIZE - 1u);
    uint32_t end = (uint32_t)(uintptr_t)base + bytes;
    if (end <= start)
        return -1;
    /* STRIDE, not SIZE. Each frame owns X86_PG_SLACK bytes past its top so a
       straddling guest access has somewhere to land that belongs to nobody --
       see the seam block in x86_pager.h. A pool sized in whole pages therefore
       yields one fewer frame than it used to, which is why every caller was
       moved to X86_PG_POOL_BYTES() rather than left to lose a frame silently. */
    uint32_t n = (end - start) / X86_PG_STRIDE;
    if (frame_count + n > MAX_FRAMES)
        n = MAX_FRAMES - frame_count;
    if (!n)
        return -1;

    extents[extent_count].base  = (uint8_t *)(uintptr_t)start;
    extents[extent_count].first = frame_count;
    extents[extent_count].count = n;
    extent_count++;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t f = frame_count + i;
        frame_page[f] = NO_FRAME;
        free_list[free_count++] = (uint16_t)f;
    }
    frame_count += n;

    x86_pg_frames_total = frame_count;
    x86_pg_frames_free  = free_count;
    return 0;
}

void x86_pager_set_backing(x86_pg_read_fn rd, x86_pg_write_fn wr)
{
    backing_read = rd;
    backing_write = wr;
}

void x86_pager_own(uint32_t first_page, uint32_t count)
{
    own_first = first_page;
    own_count = count;
}

void x86_pager_set_nor(const void *base, uint32_t bytes, uint32_t first_page)
{
    nor_base  = (const uint8_t *)base;
    nor_pages = bytes >> X86_PG_SHIFT;
    nor_first = first_page;
    /* Every page the snapshot covers is NOR tier from here on. Its fold entry
       is x86_nor.c's business; what this records is where a fill comes from
       once the page is pulled into AXI. */
    if (!pg_flags)
        return;
    for (uint32_t i = 0; i < nor_pages; i++) {
        uint32_t p = first_page + i;
        if (p < X86_PG_PAGES && !(pg_flags[p] & PG_RESIDENT))
            pg_flags[p] = (uint8_t)((pg_flags[p] & ~PG_TIER_MASK) | PG_TIER_NOR);
    }
}

static int owns(uint32_t page)
{
    return pg_flags && own_count && page >= own_first
           && page < own_first + own_count && page < X86_PG_PAGES;
}

/*
 * Install a page's fold entry, and the HMA alias if the A20 gate is masked.
 *
 * The alias is why this is a function and not a store. x86_mem_wrap_1mb()
 * points the sixteen pages above 1 MB at the bottom of memory while A20 is
 * masked, so a page that faults in without fixing its alias leaves a guest
 * reaching the same memory through a wrapped address looking at FOLD_INVALID
 * -- an endless refault on a page that is already resident.
 */
static void fold_install(uint32_t page, uint8_t *host)
{
    guest_page_table_ptr[page] = FOLD_K(host, page);
    if (page < X86_HMA_PAGES && !x86_io.a20_open) {
        uint32_t a = X86_HMA_FIRST_PAGE + page;
        if (a < FOLD_ENTRIES)
            guest_page_table_ptr[a] = FOLD_K(host, a);
    }
}

static void fold_clear(uint32_t page)
{
    guest_page_table_ptr[page] = FOLD_INVALID;
    if (page < X86_HMA_PAGES && !x86_io.a20_open) {
        uint32_t a = X86_HMA_FIRST_PAGE + page;
        if (a < FOLD_ENTRIES)
            guest_page_table_ptr[a] = FOLD_INVALID;
    }
}

static void hot_bump(uint32_t page)
{
    if (pg_hot[page] < 255u)
        pg_hot[page]++;
}

void x86_pager_note_eip(uint32_t lin)
{
    uint32_t p = lin >> X86_PG_SHIFT;
    if (pg_hot && p < X86_PG_PAGES)
        hot_bump(p);
}

void x86_pager_decay(void)
{
    if (!pg_hot)
        return;
    for (uint32_t i = 0; i < X86_PG_PAGES; i++)
        pg_hot[i] >>= 1;
}

/* ---- filling a fresh frame ---------------------------------------------- */

static int fill_frame(uint32_t page, uint8_t *dst)
{
    switch (pg_flags[page] & PG_TIER_MASK) {
    case PG_TIER_NOR:
        if (nor_base && page >= nor_first && page < nor_first + nor_pages) {
            memcpy(dst, nor_base + (page - nor_first) * X86_PG_SIZE,
                   X86_PG_SIZE);
            x86_pg_fill_nor++;
            return 0;
        }
        /* The snapshot went away under us. Zero is the only safe answer, and
           it is counted as a zero fill rather than silently as a NOR one. */
        break;
    case PG_TIER_FILE:
        if (backing_read && backing_read(page, dst) == 0) {
            x86_pg_fill_file++;
            return 0;
        }
        /* A pagefile read that fails is DATA LOSS, not a slow path. It must be
           loud: the guest gets zeroes and the counter says the guest's memory
           is no longer what it wrote. */
        x86_pg_io_errors++;
        break;
    default:
        break;
    }
    memset(dst, 0, X86_PG_SIZE);
    x86_pg_fill_zero++;
    return 0;
}

static uint32_t take_free_frame(void)
{
    if (!free_count)
        return NO_FRAME;
    uint32_t f = free_list[--free_count];
    x86_pg_frames_free = free_count;
    return f;
}

uint8_t *x86_pager_slack(uint32_t page)
{
    if (!pg_flags || page >= X86_PG_PAGES || !(pg_flags[page] & PG_RESIDENT))
        return NULL;
    uint8_t *host = frame_addr(pg_frame[page]);
    return host ? host + X86_PG_SIZE : NULL;
}

uint32_t x86_pager_fault(uint32_t lin, int write)
{
    uint32_t page = lin >> X86_PG_SHIFT;

    if (!owns(page)) {
        /* Not a page fault at all -- a genuinely unmapped address. The caller
           takes X86_EXIT_MEM_FAULT, which is the honest answer and the one
           that names the guest's own bug rather than ours. */
        x86_pg_unmapped++;
        return 0;
    }

    if (write)
        x86_pg_faults_w++;
    else
        x86_pg_faults_r++;
    hot_bump(page);

    if (pg_flags[page] & PG_RESIDENT) {
        /* Already resident: the fold entry disagreed with our own state, which
           means something repointed it behind us. Reinstall rather than
           allocate a second frame for the same page. */
        uint8_t *host = frame_addr(pg_frame[page]);
        if (host) {
            fold_install(page, host);
            return (uint32_t)(uintptr_t)host + (lin & (X86_PG_SIZE - 1u));
        }
    }

    uint32_t f = take_free_frame();
    if (f == NO_FRAME) {
        /* LOUD. A pool that silently eats stores is indistinguishable from one
           that works, and this project has hit silent allocation failure five
           times. The guest takes a clean memory fault and the counter says
           exactly why. */
        x86_pg_exhausted++;
        return 0;
    }

    uint8_t *host = frame_addr(f);
    if (!host) {
        x86_pg_exhausted++;
        return 0;
    }
    fill_frame(page, host);
    /* Prime the slack from the neighbour AS IT STANDS NOW. A page becoming
       resident takes a fresh snapshot; storing and restoring an old one is the
       stale-neighbour bug the prior port traced
       (references/8086tiny/8086tiny.c:10182-10190). A straddling access still
       goes through x86_seam_sync(), which pulls live bytes -- this only means
       the eight bytes are never uninitialised. */
    {
        uint32_t nk = (page + 1u < FOLD_ENTRIES)
                      ? guest_page_table_ptr[page + 1u] : FOLD_INVALID;
        if (nk != FOLD_INVALID)
            memcpy(host + X86_PG_SIZE,
                   (const void *)(uintptr_t)(((page + 1u) << X86_PG_SHIFT) + nk),
                   X86_PG_SLACK);
        else
            memset(host + X86_PG_SIZE, 0, X86_PG_SLACK);
    }

    frame_page[f] = (uint16_t)page;
    pg_frame[page] = (uint16_t)f;
    pg_flags[page] |= PG_RESIDENT;
    x86_pg_resident++;
    fold_install(page, host);

    return (uint32_t)(uintptr_t)host + (lin & (X86_PG_SIZE - 1u));
}
