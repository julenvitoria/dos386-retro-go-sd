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

void x86_unmap_all(void)
{
    for (uint32_t i = 0; i < FOLD_ENTRIES; i++)
        guest_page_table_ptr[i] = FOLD_INVALID;
}
