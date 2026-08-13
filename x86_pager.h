/*
 * Demand paging over the fold table.
 *
 * ---- What makes this cost nothing on the fast path -----------------------
 *
 * A non-resident page holds FOLD_INVALID (0x80000000) in its fold entry, so
 * the check the interpreter ALREADY does -- `ADDS Rh, Rk, Rlin; BMI` -- catches
 * it. There is no extra test, no permission bits and no second table. The
 * whole pager lives behind a branch the fast path was taking anyway, which is
 * why not one byte of assembly changes to add it.
 *
 * The trap hands the slow path `bad`, the value the check produced, which for
 * an unmapped page is `FOLD_INVALID + lin`. **The faulting linear address
 * falls straight out of it**: lin = bad - 0x80000000. That is the property
 * that makes demand paging reachable without plumbing a fault address down
 * from ~181 trap sites, and it is why 0x80000000-0x8FFFFFFF (a fold miss) and
 * 0x90000000+ (a NOR-backed page) are distinguishable by inspection.
 *
 * ---- Tiers -------------------------------------------------------------
 *
 * A page that is not resident is in exactly one of three places:
 *
 *   ZERO  never written. Costs nothing anywhere. Most of an 8 MB space.
 *   NOR   a clean page of the boot image, read in place over XIP (x86_nor.c).
 *         Zero AXI, zero SD.
 *   FILE  a page that was modified and then evicted, in the pagefile on SD.
 *
 * ---- Dirty tracking, without permission bits ---------------------------
 *
 * The fold has no write bit and cannot get one without costing the fast path,
 * so a write to a page that is already resident cannot be seen. The usual
 * answer -- treat every resident page as dirty -- is what produced the
 * previous project's 566 MB of pagefile traffic and 98.6% wasted writes.
 *
 * This does it the other way round: **decide at EVICTION by comparing against
 * the tier's authoritative copy.** A ZERO-tier page is scanned for a non-zero
 * byte; a NOR-tier page is compared against its snapshot. Either way the
 * answer is EXACT -- not a heuristic, not a conservative approximation -- and
 * a page that matches is DROPPED and reverts to its tier, costing no SD I/O at
 * all. A 4 KB compare is ~1 us against a pagefile write measured in
 * milliseconds, so the comparison is free at the scale it is traded against.
 *
 * FILE-tier pages have no in-memory authoritative copy to compare with, so
 * they are written back unconditionally and x86_pg_wb_file counts them. That
 * is the one conservative case and it is bounded: FILE tier only ever contains
 * pages that were genuinely modified at some point, so it does not contain the
 * read-only code and tables that made the previous project's figures so bad --
 * those are NOR tier and are never written.
 */
#ifndef X86_PAGER_H
#define X86_PAGER_H

#include <stdint.h>

#define X86_PG_SIZE   4096u
#define X86_PG_SHIFT  12u
/* The guest space the pager owns, in pages. 8 MB. Sized for the space rather
   than for the 640 KB a real-mode DOS guest can address, so the structures do
   not have to be rebuilt when something can reach above 1 MB. */
#define X86_PG_PAGES  2048u

/* Backing-store callbacks. The pager does not know what an SD card is: the ABI
   lives in src/main.c and keeping it there is what lets the whole mechanism be
   driven at tier 1 against a plain RAM buffer standing in for the pagefile,
   with no card, no FatFs and no firmware. Both return 0 on success.
   `slot` is the page's index in the pagefile; the mapping is direct, so slot
   IS the guest page number and no allocator is needed. */
typedef int (*x86_pg_read_fn)(uint32_t slot, void *dst);
typedef int (*x86_pg_write_fn)(uint32_t slot, const void *src);

/* Per-page state: flags, hotness and frame index for every page of the space.
   8 KB, and it belongs in DTCM -- it is read on every fault. Supplied by the
   caller because a core gets its DTCM from mem_ctl() at runtime, and because
   8 KB of static .bss overflows RAM_EMU in the tier-1 harness. */
#define X86_PG_TABLE_BYTES (4u * X86_PG_PAGES)
int  x86_pager_set_tables(void *block, uint32_t bytes);

/* Hand the pager the AXI it may hand out, in whole pages. May be called more
   than once; extents need not be contiguous with each other. */
int  x86_pager_add_frames(void *base, uint32_t bytes);
/* Drop every frame and every page's state. */
void x86_pager_reset(void);
void x86_pager_set_backing(x86_pg_read_fn rd, x86_pg_write_fn wr);
/* Tell the pager which guest pages it owns. Pages outside this window are left
   exactly as they are -- VRAM, the mode 13h aperture, the open-bus filler and
   the BIOS id page are fixed mappings and must never be paged. */
void x86_pager_own(uint32_t first_page, uint32_t count);

/*
 * Service a fold miss. `lin` is the faulting guest linear address; `write` is
 * 1 for a store. Returns the host address of that byte, or 0 if the page is
 * genuinely not ours (which is a real X86_EXIT_MEM_FAULT, not a page fault).
 *
 * Called from the interpreter's trap stubs by way of x86_nor_slow_r/_w, and
 * from x86_guest_ptr() on the host side.
 */
uint32_t x86_pager_fault(uint32_t lin, int write);

/* Where the NOR snapshot is, so a fill can read from it. Mirrored from
   x86_nor.c rather than reached into: the two files own different halves of
   the same page's life and a shared global would hide which. */
void x86_pager_set_nor(const void *base, uint32_t bytes, uint32_t first_page);

/* Hotness. Fed by faults (free) and by guest-EIP samples taken at each
   interpreter exit. Decayed by halving, which is one pass over 2 KB. */
void x86_pager_note_eip(uint32_t lin);
void x86_pager_decay(void);

/* ---- host-readable state ------------------------------------------------ */
extern volatile uint32_t x86_pg_frames_total;   /* AXI pages the pager holds  */
extern volatile uint32_t x86_pg_frames_free;
extern volatile uint32_t x86_pg_resident;       /* guest pages resident       */
extern volatile uint32_t x86_pg_faults_r;       /* read faults serviced       */
extern volatile uint32_t x86_pg_faults_w;
extern volatile uint32_t x86_pg_faults_host;    /* faults from x86_guest_ptr  */
extern volatile uint32_t x86_pg_fill_zero;      /* filled from the ZERO tier  */
extern volatile uint32_t x86_pg_fill_nor;       /* filled from NOR            */
extern volatile uint32_t x86_pg_fill_file;      /* read back from the pagefile */
extern volatile uint32_t x86_pg_evictions;
extern volatile uint32_t x86_pg_drop_clean;     /* evicted with NO SD write   */
extern volatile uint32_t x86_pg_wb_zero;        /* dirty, but became all-zero */
extern volatile uint32_t x86_pg_wb_file;        /* written to the pagefile    */
extern volatile uint32_t x86_pg_wb_bytes;
extern volatile uint32_t x86_pg_wb_opens;       /* fopen/fseek/fwrite/fclose  */
extern volatile uint32_t x86_pg_wb_us_worst;    /* worst single writeback     */
extern volatile uint32_t x86_pg_io_errors;
extern volatile uint32_t x86_pg_exhausted;      /* no victim could be found   */
extern volatile uint32_t x86_pg_unmapped;       /* faults outside our window  */
extern volatile uint32_t x86_pg_donated;        /* frames given to the arena  */

#endif /* X86_PAGER_H */
