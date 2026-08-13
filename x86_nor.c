/*
 * Cold guest pages backed by memory-mapped NOR, with copy-on-write back to AXI.
 *
 * The problem this exists for: guest RAM is 640 KB of AXI SRAM and AXI is the
 * scarcest thing on this part (TOOLING.md: RAM_EMU headroom is measured in
 * single-digit kilobytes and running out is QUIET). Most of a DOS guest's
 * memory is never written after it settles -- the program image, its data
 * tables, the BIOS areas. Those pages do not need to be in SRAM at all.
 *
 * WHAT THIS FILE IS NOT. It does not talk to the flash. Programming NOR needs
 * the firmware's erase/program path, which is reachable only through the ABI,
 * and the ABI is src/main.c's business -- keeping it out of here is what lets
 * the whole mechanism be tested at tier 1 against a plain RAM buffer standing
 * in for "NOR", with no flash, no SD card and no firmware. It is also the seam
 * a real savestate system would attach to later: x86_nor_attach() takes bytes
 * that are already durable and says whether they may be trusted.
 *
 * ---- Why every NOR access traps, and why that is correct ------------------
 *
 * The fold check is `ADDS Rh, Rk, Rlin; BMI` -- bit 31 of the RESULT means
 * "not mapped". NOR is at 0x90000000, so bit 31 is SET and a NOR-backed page
 * traps on EVERY access, read or write. That is not a defect to engineer
 * around: the fast path is the hottest code in the project and the sentinel
 * encoding pays for itself on every one of the ~10 M guest instructions a
 * second that DO hit AXI. A cold page paying a slow path is the trade.
 *
 * The trap stubs in cpu_exec.S call in here. A READ is served straight out of
 * NOR -- XIP is normal memory to a load instruction. A WRITE cannot be, so it
 * takes a page from the COW pool, copies the NOR contents in, repoints the
 * fold entry and hands back the AXI address; the store that trapped then
 * completes against AXI and every later access to that page is on the fast
 * path again. That is minimal copy-on-write, and it is REQUIRED for
 * correctness rather than an optimisation.
 *
 * ---- Selection policy: there is none, deliberately ------------------------
 *
 * After the snapshot EVERY guest page points at NOR. Pages the guest writes
 * pull themselves back into AXI; pages it never writes stay in NOR forever at
 * no AXI cost. No sampler, no heuristic, no eviction ladder -- and as a side
 * effect the counters below MEASURE the thing that was previously guessed at:
 * what fraction of guest RAM is genuinely read-only once a title has settled.
 * Read-hot-but-never-written pages sit in NOR paying the slow path on every
 * read, which is a real cost and is exactly why it is counted rather than
 * assumed away.
 */
#include <stdint.h>
#include <string.h>

#include "cutils.h"
#include "x86_cpu.h"

#define PAGE_SIZE   4096u
#define LOW_1MB_PAGES 256u          /* granules in the real-mode 1 MB window */

/* ---- host-readable state and counters ---------------------------------- *
 *
 * All volatile and all global on purpose: probe.py reads them off a running
 * target, and they are the deliverable as much as the feature is. See
 * TOOLING.md for what each one answers.
 */
volatile uint32_t x86_nor_attached      = 0;  /* 1 = guest is running out of NOR */
volatile uint32_t x86_nor_base_addr     = 0;  /* XIP address of the snapshot    */
volatile uint32_t x86_nor_bytes         = 0;  /* bytes snapshotted              */
volatile uint32_t x86_nor_pages_total   = 0;  /* pages snapshotted              */
volatile uint32_t x86_nor_pages_nor     = 0;  /* still served out of NOR        */
volatile uint32_t x86_nor_pages_axi     = 0;  /* pulled back by COW             */
volatile uint32_t x86_nor_axi_free      = 0;  /* AXI bytes reclaimed, i.e. free */
volatile uint32_t x86_nor_reads         = 0;  /* slow-path READS                */
volatile uint32_t x86_nor_writes        = 0;  /* slow-path WRITES (COW hits)    */
volatile uint32_t x86_nor_cow_pulls     = 0;  /* pages copied NOR -> AXI        */
volatile uint32_t x86_nor_cow_failures  = 0;  /* COW pool empty: loud, not silent */
volatile uint32_t x86_nor_slow_cycles   = 0;  /* DWT cycles spent in here       */
volatile uint32_t x86_nor_unmapped      = 0;  /* traps that were a real fault   */
volatile uint32_t x86_nor_attach_err    = 0;  /* why the last attach was refused */

/* The COW pool. It IS the block that used to be guest RAM: once every page is
 * in NOR that 640 KB is free, and the only useful thing to do with it is hand
 * it back one page at a time as writes demand. It cannot be returned to
 * ram_malloc() -- that allocator has no free() -- so "AXI reclaimed" is
 * measured as what is still unclaimed here, which is the honest number. */
static uint8_t  *pool_base;
static uint32_t  pool_bytes;
static uint8_t  *pool_free_head;    /* singly linked through the pages themselves */

static uint8_t  *nor_base;
static uint32_t  nor_len;
static uint32_t  nor_first_page;    /* fold index the snapshot starts at */

#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004)

static void pool_reset(uint8_t *base, uint32_t bytes)
{
    pool_base = base;
    pool_bytes = bytes & ~(PAGE_SIZE - 1u);
    pool_free_head = NULL;
    /* Build the list back to front so the first allocation is the lowest page,
       which keeps a dump of the pool readable. */
    for (uint32_t off = pool_bytes; off >= PAGE_SIZE; off -= PAGE_SIZE) {
        uint8_t *p = base + off - PAGE_SIZE;
        *(uint8_t **)p = pool_free_head;
        pool_free_head = p;
    }
    x86_nor_axi_free = pool_bytes;
}

static uint8_t *pool_take(void)
{
    uint8_t *p = pool_free_head;
    if (!p)
        return NULL;
    pool_free_head = *(uint8_t **)p;
    x86_nor_axi_free -= PAGE_SIZE;
    return p;
}

/*
 * Repoint one guest page, and the real-mode 1 MB aliases that shadow it.
 *
 * x86_mem_wrap_1mb() aliases every index at or above 1 MB onto its counterpart
 * below, so a COW that only fixed the primary entry would leave sixteen stale
 * aliases still pointing into NOR -- and a guest that reached the same page
 * through a wrapped address would read the PRE-write bytes. The alias is only
 * touched when it still holds exactly the value the wrap would have given it,
 * so an index that was independently mapped to something else is never
 * disturbed.
 */
static void fold_repoint(uint32_t idx, uint8_t *host, const uint8_t *was)
{
    guest_page_table_ptr[idx] = FOLD_K(host, idx);
    if (idx >= LOW_1MB_PAGES)
        return;
    for (uint32_t i = idx + LOW_1MB_PAGES; i < FOLD_ENTRIES; i += LOW_1MB_PAGES) {
        if (guest_page_table_ptr[i] == FOLD_K(was, i))
            guest_page_table_ptr[i] = FOLD_K(host, i);
    }
}

/*
 * Take the guest's memory over to NOR.
 *
 * `nor` must already hold a byte-exact copy of [ram, ram+bytes) -- this
 * function VERIFIES that before it moves anything, and refuses the whole
 * operation if the readback disagrees. That ordering is the point: the fold
 * table is not touched until the bytes are known good, so a failed or
 * half-finished flash write leaves the guest running unchanged out of AXI.
 * There is no state in which some pages moved and others did not.
 *
 * Returns 0 on success; on failure nothing has changed and x86_nor_attach_err
 * says which check refused it.
 */
int x86_nor_attach(void *nor, uint32_t nor_bytes, void *ram, uint32_t ram_bytes,
                   uint32_t first_page)
{
    uint8_t *n = (uint8_t *)nor;
    uint8_t *r = (uint8_t *)ram;

    x86_nor_attach_err = 0;

    if (x86_nor_attached)              { x86_nor_attach_err = 1; return -1; }
    if (!n || !r)                      { x86_nor_attach_err = 2; return -1; }
    if (!ram_bytes || (ram_bytes & (PAGE_SIZE - 1u)))
                                       { x86_nor_attach_err = 3; return -1; }
    if (nor_bytes < ram_bytes)         { x86_nor_attach_err = 4; return -1; }
    /* The trap stubs recover the page index from the trapping ADDRESS, which
       only works if the snapshot is page-aligned. flash_alloc places files on
       an erase block, so this holds -- but it is load-bearing, so check it. */
    if ((uint32_t)n & (PAGE_SIZE - 1u)) { x86_nor_attach_err = 5; return -1; }
    if (first_page + (ram_bytes >> FOLD_SHIFT) > FOLD_ENTRIES)
                                       { x86_nor_attach_err = 6; return -1; }
    /* Readback. Under gwemu this is exact; on silicon it is the only thing
       that distinguishes a good program from an OCTOSPI that came back in the
       wrong mode. Either way it costs one pass and buys the whole invariant. */
    if (memcmp(n, r, ram_bytes) != 0)  { x86_nor_attach_err = 7; return -1; }

    uint32_t pages = ram_bytes / PAGE_SIZE;

    /* Only now. Every entry that currently points into the AXI block moves to
       the matching NOR page; anything else (VRAM, the open-bus filler, the
       BIOS id page) is left exactly as it was. */
    for (uint32_t i = 0; i < pages; i++) {
        uint8_t *axi = r + i * PAGE_SIZE;
        uint8_t *nor_page = n + i * PAGE_SIZE;
        uint32_t idx = first_page + i;
        if (guest_page_table_ptr[idx] != FOLD_K(axi, idx))
            continue;               /* not ours to move */
        fold_repoint(idx, nor_page, axi);
    }

    nor_base = n;
    nor_len  = ram_bytes;
    nor_first_page = first_page;
    pool_reset(r, ram_bytes);

    x86_nor_base_addr   = (uint32_t)n;
    x86_nor_bytes       = ram_bytes;
    x86_nor_pages_total = pages;
    x86_nor_pages_nor   = pages;
    x86_nor_pages_axi   = 0;
    x86_nor_attached    = 1;
    return 0;
}

/*
 * The trap stubs' two entry points.
 *
 * `bad` is the value the fold check produced, i.e. host_base + guest_linear.
 * For a NOR-backed page that IS the exact XIP address of the byte, and because
 * the snapshot is a linear image of guest RAM the page index falls straight
 * out of it -- no faulting address needs to be plumbed down from the site.
 * Anything outside the snapshot is a genuinely unmapped page and gets the old
 * behaviour: nor_ptr = 0, and the stub takes .mem_fault.
 *
 * Both are called with R0-R3/R12/LR already saved by the stub, so they are
 * ordinary AAPCS functions and may not touch R4-R11 -- which the compiler
 * guarantees.
 */
static inline uint32_t nor_offset(uint32_t bad)
{
    return (uint32_t)(bad - (uint32_t)nor_base);
}

void x86_nor_slow_r(uint32_t bad)
{
    uint32_t t0 = DWT_CYCCNT;
    uint32_t off = nor_offset(bad);

    if (!x86_nor_attached || off >= nor_len) {
        x86_nor_unmapped++;
        cpu_state_ptr->nor_ptr = 0;
        return;
    }
    x86_nor_reads++;
    /* Nothing to do: XIP is memory. The load that trapped re-issues against
       the same address and reads the snapshot. */
    cpu_state_ptr->nor_ptr = bad;
    x86_nor_slow_cycles += DWT_CYCCNT - t0;
}

void x86_nor_slow_w(uint32_t bad)
{
    uint32_t t0 = DWT_CYCCNT;
    uint32_t off = nor_offset(bad);

    if (!x86_nor_attached || off >= nor_len) {
        x86_nor_unmapped++;
        cpu_state_ptr->nor_ptr = 0;
        return;
    }
    x86_nor_writes++;

    uint32_t pg = off >> FOLD_SHIFT;
    uint32_t idx = nor_first_page + pg;
    uint8_t *nor_page = nor_base + (pg << FOLD_SHIFT);

    uint8_t *axi = pool_take();
    if (!axi) {
        /* Structurally impossible -- the pool is exactly as large as the
         * snapshot, so it cannot empty before every page is back in AXI. It is
         * checked anyway, and it is LOUD: a silent NULL here would be a store
         * through address 0, which is ITCM, i.e. the interpreter overwriting
         * its own code. That exact shape has cost this project days. */
        x86_nor_cow_failures++;
        cpu_state_ptr->nor_ptr = 0;
        return;
    }
    memcpy(axi, nor_page, PAGE_SIZE);
    fold_repoint(idx, axi, nor_page);

    x86_nor_cow_pulls++;
    x86_nor_pages_nor--;
    x86_nor_pages_axi++;

    cpu_state_ptr->nor_ptr = (uint32_t)axi + (off & (PAGE_SIZE - 1u));
    x86_nor_slow_cycles += DWT_CYCCNT - t0;
}
