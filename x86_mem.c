/*
 * Guest physical memory: a page pool and the fold-table mapping over it.
 *
 * This is deliberately NOT a pager. There is no fault path, no eviction, no
 * copy-on-write and no backing store -- those belong to the Wolf3D/Win3.11
 * work, where the guest wants more memory than the machine has. A DOS game in
 * conventional memory does not: 640 KB of RAM plus a 16 KB CGA window is
 * 656 KB against a measured ~724 KB AXI pool, so every guest page can simply
 * be resident. What is "dynamic" here is only that the pages come out of a
 * pool at runtime rather than from a static array, which is what lets the
 * video window be sized by the mode the guest selects.
 *
 * Keeping it this small is the point. The prior project measured eviction
 * policy as worth at most 2.1x while the pool-size cliff was 350x, so there is
 * nothing to be gained by building machinery before something actually needs
 * to overcommit.
 */
#include <stdint.h>
#include <string.h>

#include "cutils.h"
#include "x86_cpu.h"

#define PAGE_SIZE 4096u

/* Host-readable: any mapping we refused. Non-zero means some region is
   deliberately absent, which is a far better thing to read than a hardfault. */
volatile uint32_t x86_map_failures = 0;

static uint8_t *pool_base;
static uint32_t pool_bytes;
static uint32_t pool_used;

void x86_mem_pool_init(void *base, uint32_t bytes)
{
    pool_base = (uint8_t *)base;
    pool_bytes = bytes & ~(PAGE_SIZE - 1u);
    pool_used = 0;
}

uint32_t x86_mem_pool_free(void)
{
    return pool_bytes - pool_used;
}

static uint8_t *pool_alloc_page(void)
{
    if (pool_used + PAGE_SIZE > pool_bytes)
        return NULL;                 /* caller reports it; never silent */
    uint8_t *p = pool_base + pool_used;
    pool_used += PAGE_SIZE;
    return p;
}

/*
 * Map [guest, guest+bytes) to freshly allocated, zeroed pages.
 *
 * Returns 0 on success, or the number of pages it could not place. Exhaustion
 * is reported rather than swallowed: a pool that quietly stops mapping behaves
 * exactly like one that works until the guest touches the gap, and that is the
 * single most expensive class of bug this project has hit.
 */
int x86_map_range(uint32_t guest, uint32_t bytes)
{
    uint32_t first = guest >> FOLD_SHIFT;
    uint32_t count = (bytes + PAGE_SIZE - 1u) / PAGE_SIZE;
    int failed = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = first + i;
        if (idx >= FOLD_ENTRIES) {
            failed++;
            continue;
        }
        uint8_t *page = pool_alloc_page();
        if (!page) {
            failed++;
            x86_map_failures++;
            continue;
        }
        memset(page, 0, PAGE_SIZE);
        guest_page_table_ptr[idx] = FOLD_K(page, idx);
    }
    return failed;
}

/* Point a guest range at memory we already have -- the LCD-facing framebuffer,
   or a ROM image -- without taking pool pages for it. */
int x86_map_fixed(uint32_t guest, void *host, uint32_t bytes)
{
    uint32_t first = guest >> FOLD_SHIFT;
    uint32_t count = (bytes + PAGE_SIZE - 1u) / PAGE_SIZE;

    /* REFUSE A NULL BACKING. A fold entry built from NULL is worse than no
     * entry at all: FOLD_K(NULL, idx) yields a small positive host address, so
     * the sentinel check (ADDS/BMI) PASSES and the handler dereferences it --
     * a HARD FAULT inside the interpreter instead of a clean X86_EXIT_MEM_FAULT
     * the emulator can report. That is how a failed VRAM/filler allocation
     * surfaced as a hardfault at GRP5_MEM_16's `ldrh r3,[r2]`, with nothing
     * anywhere saying an allocation had failed.
     * Leaving the range unmapped keeps FOLD_INVALID, so the guest takes a
     * clean fault and x86_map_failures says why. */
    if (!host) {
        x86_map_failures++;
        return -1;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = first + i;
        if (idx >= FOLD_ENTRIES)
            return -1;
        guest_page_table_ptr[idx] = FOLD_K((uint8_t *)host + i * PAGE_SIZE, idx);
    }
    return 0;
}

/*
 * Guest linear -> host pointer, for host-side code servicing an HLE call.
 *
 * Goes through the same fold table the interpreter uses, so an unmapped page
 * is reported rather than silently dereferenced, and a request that would
 * straddle out of its granule is refused: the fold answers for one 4 KB page,
 * and a service copying a filename or a disk buffer can easily cross one. The
 * caller must split or use x86_guest_copy.
 */
void *x86_guest_ptr(uint32_t lin, uint32_t bytes)
{
    if (bytes == 0)
        return NULL;
    uint32_t idx = lin >> FOLD_SHIFT;
    if (idx >= FOLD_ENTRIES)
        return NULL;
    uint32_t k = guest_page_table_ptr[idx];
    if (k == FOLD_INVALID)
        return NULL;
    if (((lin + bytes - 1u) >> FOLD_SHIFT) != idx)
        return NULL;                 /* would cross a granule */

    /* A NOR-backed page must be pulled back into AXI BEFORE the pointer
     * escapes, because the caller may write through it.
     *
     * This resolves the fold itself and so never goes near the interpreter's
     * trap stubs. Every HLE service that writes into guest memory arrives here
     * -- the BDA tick, INT 21h filling a DTA, a disk read landing in a guest
     * buffer -- and without this those stores would go straight at the XIP
     * window. Under gwemu that appears to work, because its OCTOSPI model
     * backs the region with ordinary memory; on silicon the store is dropped
     * and the guest diverges with nothing anywhere saying why. It was found by
     * reading x86_nor_pages_axi = 1 after 25 s of a real title, which is far
     * fewer written pages than any DOS guest can plausibly have.
     *
     * It COWs on read as well as on write, deliberately: this is the same
     * conservative rule the unqualified MEM_GET_PTR follows, and for the same
     * reason -- pessimistic is a cost, wrong is a corrupted guest. The host
     * side is a few thousand calls per second, not millions. */
    uint32_t host = lin + k;
    if (x86_nor_attached && (host & 0x80000000u)) {
        uint32_t axi = x86_nor_cow(host);
        if (!axi)
            return NULL;
        return (void *)axi;
    }
    return (void *)host;
}

/* Copy across granules, one page at a time. Returns bytes actually moved, so
   a short result means the guest range ran into unmapped memory. */
uint32_t x86_guest_copy_out(void *dst, uint32_t lin, uint32_t bytes)
{
    uint8_t *d = (uint8_t *)dst;
    uint32_t done = 0;
    while (done < bytes) {
        uint32_t off = (lin + done) & (PAGE_SIZE - 1u);
        uint32_t run = PAGE_SIZE - off;
        if (run > bytes - done)
            run = bytes - done;
        void *p = x86_guest_ptr(lin + done, run);
        if (!p)
            break;
        memcpy(d + done, p, run);
        done += run;
    }
    return done;
}

uint32_t x86_guest_copy_in(uint32_t lin, const void *src, uint32_t bytes)
{
    const uint8_t *s = (const uint8_t *)src;
    uint32_t done = 0;
    while (done < bytes) {
        uint32_t off = (lin + done) & (PAGE_SIZE - 1u);
        uint32_t run = PAGE_SIZE - off;
        if (run > bytes - done)
            run = bytes - done;
        void *p = x86_guest_ptr(lin + done, run);
        if (!p)
            break;
        memcpy(p, s + done, run);
        done += run;
    }
    return done;
}

/*
 * Real-mode address wrap, and the A20 gate that decides whether it happens.
 *
 * WHAT THIS USED TO DO, AND WHY IT WAS WRONG. It aliased every fold entry from
 * 1 MB to 16 MB onto its counterpart in the low 1 MB. That is an 8086's 20-bit
 * address space, not this machine's, and it has a consequence far bigger than
 * an inaccuracy: with all 3,840 entries above 1 MB held as aliases, there is
 * no memory above 1 MB at all, and the 8 MB guest space this project is being
 * built toward cannot exist. It was harmless only because nothing had ever
 * mapped anything up there.
 *
 * What a real AT wraps is ONE 64 KB window. Segment:offset reaches at most
 * 0xFFFF:0xFFFF = 0x10FFEF, so only 0x100000-0x110000 is reachable past the
 * megabyte, and masking A20 forces address bit 20 low so that window reads the
 * bottom of memory. Everything from 0x110000 up is ordinary extended memory
 * and was never part of the wrap.
 *
 * So: sixteen pages, conditional on the gate. A block-copy loop walking
 * segments off the top still comes back round, which is the property programs
 * depend on and the reason this exists.
 */
volatile uint32_t x86_a20_changes = 0;
volatile uint32_t x86_a20_state = 0;

void x86_mem_wrap_1mb(void)
{
    for (uint32_t i = 0; i < X86_HMA_PAGES; i++) {
        uint32_t dst = X86_HMA_FIRST_PAGE + i;
        if (dst >= FOLD_ENTRIES)
            break;
        if (x86_io.a20_open) {
            /* The gate is open: the HMA is its own memory. Nothing here owns
             * it yet -- there is no XMS driver and no pager stage behind this
             * one -- so it is left UNMAPPED rather than aliased. A guest that
             * opens A20 and then reads the HMA takes a clean fault that
             * x86_map_failures/the trap counters can name, instead of silently
             * reading the interrupt vector table and believing it. */
            guest_page_table_ptr[dst] = FOLD_INVALID;
            continue;
        }
        uint32_t k = guest_page_table_ptr[i];
        if (k == FOLD_INVALID) {
            guest_page_table_ptr[dst] = FOLD_INVALID;
            continue;
        }
        /* k is host - guest_index<<12, so re-bias it for this index. */
        guest_page_table_ptr[dst] = k - ((dst - i) << FOLD_SHIFT);
    }
    x86_a20_state = x86_io.a20_open ? 1u : 0u;
}

void x86_a20_set(int open)
{
    uint8_t want = open ? 1u : 0u;
    if (x86_io.a20_open == want)
        return;                       /* a probe is not a transition */
    x86_io.a20_open = want;
    x86_a20_changes++;
    x86_mem_wrap_1mb();
}

void x86_unmap_all(void)
{
    for (uint32_t i = 0; i < FOLD_ENTRIES; i++)
        guest_page_table_ptr[i] = FOLD_INVALID;
}
