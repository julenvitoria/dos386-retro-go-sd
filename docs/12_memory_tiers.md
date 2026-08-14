# The memory tiers: pager, arena, and what is NOT built

Companion to [`01_memory_architecture.md`](01_memory_architecture.md), which
covers the fold table itself. This file is about what sits on top of it, and
about the gap between what `x86_pager.h` documents and what the tree does.

## 1. The pager

`x86_pager.h` / `x86_pager.c`. 4 KB pages, `X86_PG_PAGES` = 2,048 (8 MB of guest
space), per-page state in 8 KB of DTCM supplied by the caller.

A non-resident page is in exactly one of three places:

| tier | meaning | cost |
|---|---|---|
| ZERO | never written | nothing anywhere; most of an 8 MB space |
| NOR | a clean page of the boot image, read in place over XIP | zero AXI, zero SD |
| FILE | modified and then evicted, in the pagefile on SD | an SD read to fault back |

The fast path is free because a non-resident page holds `FOLD_INVALID`
(`0x80000000`) and the interpreter's existing `ADDS`/`BMI` catches it — no extra
test, no permission bits, no second table. `0x80000000`-`0x8FFFFFFF` (a fold
miss) and `0x90000000`+ (NOR-backed) are distinguishable by inspection, and the
faulting address falls out of the trap value as `bad - 0x80000000`.

Backing store is abstracted behind `x86_pg_read_fn`/`x86_pg_write_fn`, set by
`src/main.c`. That separation is deliberate: it is what lets the whole mechanism
be driven at tier 1 against a plain RAM buffer with no card, no FatFs and no
firmware.

## 2. EVICTION IS NOT IMPLEMENTED — read this before trusting the header

`x86_pager.h:117-162` documents eviction-time dirty detection by comparison
against the tier's authoritative copy, and declares `x86_pg_evictions`,
`_drop_clean`, `_wb_zero`, `_wb_file`, `_wb_bytes`, `_wb_opens`, `_wb_us_worst`
and `_donated`.

**Nothing in the tree increments any of them.** `x86_pager.c:287-293` is the
whole of victim selection: if the free list is empty it returns `NO_FRAME` and
the guest takes `X86_EXIT_MEM_FAULT`. `--gc-sections` drops all eight symbols
from `dos_core.elf`, which is how it was found — `probe.py` could not resolve
them. `x86_livemap_holds()` (`x86_arena.c`) is likewise uncalled: the eviction
veto ring is armed and never consulted, because nothing evicts.

*(Being implemented. Until it lands, treat the header as a specification, not a
description.)*

The design it specifies, when built, is worth preserving verbatim: **decide at
EVICTION by comparing against the tier's authoritative copy** — scan a ZERO page
for a non-zero byte, compare a NOR page against its snapshot — so the answer is
EXACT rather than conservative, and a page that matches is DROPPED with no SD
I/O. The alternative (treat every resident page as dirty) is what produced the
previous project's 566 MB of pagefile traffic and 98.6% wasted writes. A 4 KB
compare is ~1 µs against a pagefile write measured in milliseconds.

FILE tier is the one conservative case — no in-memory authority to compare
against — and it is bounded, because FILE only ever holds pages that were
genuinely modified.

## 3. Measured pressure, and the read-mostly premise

gwemu tier 2, whole runs:

| title | resident | `_faults_r` | `_faults_w` | `_fill_zero` / `_nor` / `_file` |
|---|---:|---:|---:|---|
| WOLF3D (210 M insns) | 104/160 | **0** | 104 | 104 / 0 / 0 |
| KEEN4 | 134/160 | **0** | 134 | 134 / 0 / 0 |

`_frames_total=160` (the full 640 KB, no frame lost to alignment),
`_exhausted=0`, `_unmapped=0`, `_io_errors=0`.

Two conclusions:

1. **No title has come close to pressure.** The pool never empties, so the
   failure arm is dead code in practice — and the NOR and FILE tiers have never
   been touched by a real guest (`x86_nor_attached=0`).
2. **The read-mostly premise that NOR-XIP rests on is FALSE for every guest we
   can run.** Zero pages in either working set are read-only; 100% became
   resident through a write. That is the same shape the prior project measured
   ("~100% of NOR-backable data granules get written anyway"), now confirmed
   here. Gate any NOR work on re-measuring this, not on the design's promise.

## 4. The arena

`x86_arena.c` / `x86_arena.h`. Display storage carved from the LCD bonus pool,
with fixed claim ids rather than an allocator, so a leaked claim is impossible
and a probe can name what holds it.

| id | use |
|---|---|
| `X86_ARENA_ID_CGA` | 0xB8000, 32 KB |
| `X86_ARENA_ID_VGA13` | 0xA0000, 64 KB linear |
| `X86_ARENA_ID_EGA` | 0xA0000, 4 planes |
| `X86_ARENA_ID_SPARE` | Mode Y's off-screen cache |

**Modes are mutually exclusive, so peak demand is the LARGEST mode, not the
sum.** CGA VRAM and the 13h aperture used to be permanent allocations — 96 KB
spent to serve one mode at a time, which is why EGA 0Dh was unreachable. They
are claims now, and a planar mode can lend the CGA claim out.

Measured: `x86_arena_bytes = 143,360`. The layout is computed at **runtime** from
the pool's actual base, because the bonus pool arrives 2048-aligned and not
4096-aligned, and a compile-time map loses a whole page to that.

**Fragmentation is real**: claims sit in init order, so releasing one leaves
*two* holes and the aperture must be re-claimed to compact. Check
`x86_arena_largest`, not just `x86_arena_free`.

Every arena page is **pinned** — it is display storage, the blitter reads it
every frame, and evicting one is visible instantly. The pin bitmap is indexed by
guest page number so the victim picker can reject one in a single test.

`x86_guest_ptr()` hands out raw host pointers that **no caller releases**, so a
reference count cannot work; the livemap ring is a small set of
recently-handed-out pages that eviction must refuse. The prior project measured
**5 such evictions producing 26,329 wrong bytes** with its `cow_lost == 0`
invariant green throughout.

## 5. Donating guest frames to the arena — the route that was overlooked

`PLAN.md` Phase 3 states the intent: *"For EGA 256KB mode, the extra 224KB is
dynamically borrowed from the guest RAM AXI pool."* The mechanism is
`x86_pg_donated` ("frames given to the arena") plus `x86_arena_add_region()`.

This is what makes Keen 4's levels reachable, and the arithmetic is in
[`13_title_status.md`](13_title_status.md).

**Donation without eviction turns a later shortfall into a hard
`X86_EXIT_MEM_FAULT` instead of a page-out**, which is why the two are one piece
of work.

## 6. Where things live

`src/main.c`'s `bonus_layout()`, computed at runtime:

| block | size |
|---|---:|
| 13h palette (absorbs the base's misalignment) | 2 KB |
| open-bus filler, aliased over 0xA0000-0xFFFFF | 4 KB |
| BIOS identification page at 0xFF000 | 4 KB |
| **the arena** — everything else | 143,360 B |

The `.dsk` sector cache is **not** here: it is `GW_MEM_AHB`, because it is a DMA
staging buffer and D2 SRAM is the only SRAM DMA1/DMA2 can reach, and because
`GW_MEM_AHB` is the only ABI pool with a real `free()`.

**RAM_EMU exhaustion is SILENT.** `ram_malloc()` bound-checks and returns NULL,
so nothing is corrupted — but a caller that tolerates NULL loses a feature with
no message, which is what happened to the open-bus filler. Measure
`__CORE_BSS_END__` before and after any core change; the core's TEXT is in this
region too, so code growth counts.
