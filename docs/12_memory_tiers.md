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

### 1a. The pagefile — `src/main.c`, and it is wired

`pgf_read()` / `pgf_write()` in `src/main.c`, handed to `x86_pager_set_backing()`
from `guest_reset()`. `slot` IS the guest page number, so a slot is `slot*4096`
into one file and there is no allocator. The file is `x86page.swp` next to the
`.dsk` — the one directory this core has demonstrated it can write — and it is
truncated at every guest reset.

**Open-per-call**, exactly like `sd_sector_write()`: there is no ranged SD write
in the ABI, and `x86_pg_wb_opens` counts these because the counter's name *is*
this design. `pgf_ready` / `pgf_result` / `pgf_writes` / `pgf_reads` /
`pgf_errors` are the host-visible witnesses.

**MEASURED, WOLF3D gameplay under gwemu: `x86_pg_wb_us_worst` = 24,593 µs.**
One writeback is ~25 ms — one and a half frames, and `docs/09` explains why
overrunning a frame costs a whole frame rather than the overrun. It is paid 39
times at level load and then never again in a 42 s run, so it reads as a
one-off hitch and not a per-frame cost; a title with a working set that does not
settle would be a different story, and a write-behind queue is the answer if one
appears. gwemu timing is not to be trusted (`docs/04`) — carry the number to
silicon, do not bank it.

## 2. Eviction — built, and the header is now a description

*(Historical note: this section used to read "EVICTION IS NOT IMPLEMENTED".
It is implemented; `tests/test_paging.py` drives all of it at tier 1 against a
RAM buffer standing in for the pagefile.)*

The design, for reference: `x86_pager.h` documents eviction-time dirty
detection by comparison against the tier's authoritative copy, and declares `x86_pg_evictions`,
`_drop_clean`, `_wb_zero`, `_wb_file`, `_wb_bytes`, `_wb_opens`, `_wb_us_worst`
and `_donated`.

All of them are live. MEASURED, WOLF3D gameplay under gwemu with 34 frames
donated: `_evictions`=39, `_drop_clean`=0, `_wb_file`=39, `_wb_zero`=0,
`_io_errors`=0, `_exhausted`=0 — and the invariant
`drop_clean + wb_file + io_errors == evictions` holds. `x86_livemap_holds()`
and the pin bitmap are both consulted by the victim picker.

The design, verbatim, because it is the reason the figures look like that:
**decide at EVICTION by comparing against the tier's authoritative copy** — scan a ZERO page
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

**Those two rows are TITLE SCREENS, and they undersell the pressure.** Measured
since, on WOLF3D driven into GAMEPLAY (`timelines/wolf3d-ingame.tl`): resident
reaches **160/160** — the pool is full. The 104 figure is the credits screen and
quoting it as the working set is how "56 idle frames" was arrived at. Donation
still fits, because 126 frames is enough for the gameplay set; but size any
future donation against a title that is PLAYING, not one that is booting.

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
| `X86_ARENA_ID_MODEYP` | Mode Y's REAL display pages 1 and 2, 64 KB each |

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
of work — and why donation stayed gated on a backing store existing until the
pagefile above landed.

### 5a. What donation is actually spent on: Mode Y's real pages

Wolf3D triple-buffers across display starts `0x0000`, `0x4100`, `0x8200`
(stride 16,640, learned). With one page's storage all three fold onto the same
bytes and the raycaster draws the next page over the one being shown. `vga_cell()`
in `x86_video_planar.c` is the one place that says where a plane byte lives; it
now takes page 1 and 2 from `X86_ARENA_ID_MODEYP` when they exist and falls back
to the alias when they do not, counting every fallback in
`x86_vga_modey_alias_hits`. `x86_vga_modey_pages` is 1 (aliased), 2 or 3.

MEASURED, WOLF3D gameplay under gwemu: `_donated`=34 frames / 139,264 B,
`_donate_refused`=0, `x86_vga_modey_pages`=**3**, `alias_hits`=0. Guest RAM
falls from 160 to 126 frames, which costs 39 one-off page-outs at level load
and nothing after that — the gameplay working set fits in 126.

**A single captured frame cannot see any of this, in either direction.** In
gameplay the panel is painted AT THE GUEST'S FLIP, synchronously, from a page it
has just finished; a before/after gwemu screenshot of the same moment is a clean
Wolf3D frame both ways. The property is asserted structurally instead, at tier 1,
by `tests/test_video.py`'s Mode Y real-page arm — with the alias, a write to
page 1 overwrites page 0, and that negative control is seen to fail in the same
run.

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
