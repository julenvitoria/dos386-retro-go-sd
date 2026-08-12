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
    return (void *)(lin + k);
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
 * Real-mode address wrap.
 *
 * With A20 masked, an 8086 address space is 20 bits: segment:offset can reach
 * 0x10FFEF, and everything at or above 1 MB wraps to the bottom. Programs rely
 * on it -- a block-copy loop walking segments upward runs off the top and
 * expects to come back round, and faulting there instead stops it dead.
 *
 * Every fold entry above 1 MB is aliased onto its counterpart in the low 1 MB.
 * The table is index-based, so this is a plain copy of k values; no memory is
 * spent and the wrap costs nothing at access time.
 */
void x86_mem_wrap_1mb(void)
{
    const uint32_t low = 0x100000u >> FOLD_SHIFT;   /* 256 granules in 1 MB */
    for (uint32_t i = low; i < FOLD_ENTRIES; i++) {
        uint32_t src = i & (low - 1u);
        uint32_t k = guest_page_table_ptr[src];
        if (k == FOLD_INVALID) {
            guest_page_table_ptr[i] = FOLD_INVALID;
            continue;
        }
        /* k is host - guest_index<<12, so re-bias it for this index. */
        guest_page_table_ptr[i] = k - ((i - src) << FOLD_SHIFT);
    }
}

void x86_unmap_all(void)
{
    for (uint32_t i = 0; i < FOLD_ENTRIES; i++)
        guest_page_table_ptr[i] = FOLD_INVALID;
}
