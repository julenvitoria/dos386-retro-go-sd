# Plan

**Goal**: rewrite TinyEMU's 32-bit x86 core as ARM Thumb-2 assembly for the
STM32H7B0VB running Retro-Go.

State: [`STATUS.md`](STATUS.md). Running: [`TOOLING.md`](TOOLING.md). Design:
[`docs/`](docs/). **Roadmap and backlog — a list, not a narrative.**

## Phase 1 — memory architecture

Emulate a DOS machine's RAM inside the AXI available: RAM_EMU 724 KB plus
~150 KB released as a bonus pool when the LCD runs LUT8, as **two disjoint
regions**.

- **Done**: checked translation. `host = lin + k` from a flat DTCM table;
  unmapped pages hold `FOLD_INVALID` so one `ADDS`+`BMI` validates every access
  at zero cost. Entries are offsets, not bases.
- **Done**: the granule seam — [`docs/10`](docs/10_granule_seam.md).
- **Done**: eviction, the SD pagefile, and donating spare guest frames to the
  arena — [`docs/12`](docs/12_memory_tiers.md). This is the groundwork Queue C
  depends on; the 8 MB space `X86_PG_PAGES` declares is now actually backed.
- **Dead, measured**: NOR backing. 100% of residency is via writes, and XIP
  from flash needs a read-mostly set. It says nothing about the pagefile, whose
  FILE tier exists precisely for written pages
  ([`docs/12`](docs/12_memory_tiers.md) §3).
- Eviction *policy* is not the lever: LRU was measured capped at 2.1x over FIFO
  while the pool-size cliff is 350x. Spend effort on bytes, not policy.

Blueprint: [`docs/01`](docs/01_memory_architecture.md).

## Phase 2 — interpreter

Thumb-2 with maximum IPC; hot loop in ITCM; `R8`-`R11` reserved for CPU state
(the contract is in `CLAUDE.md` and is binding).

Measured on hardware: **+12.9%** work across the decode round and threading the
opcode's fold to every fetch of its instruction. `LEA` is the decode floor to
watch — it touches no memory, so its cost is ModRM decode alone.

Blueprint: [`docs/02`](docs/02_cpu_pipeline_optimization.md).

## Phase 3 — devices and HLE

Minimal PC hardware; DOS file I/O translated straight to FatFs rather than
emulating a block device. Video: mode 13h, Mode Y and EGA 0Dh render pixel-exact
([`docs/08`](docs/08_vga_mode13h.md)); timing model in
[`docs/11`](docs/11_pit_and_interrupt_controller.md).

Blueprint: [`docs/03`](docs/03_devices_and_hle.md).

## Validation hierarchy

Strictly in this order — see `TOOLING.md`, and `CLAUDE.md` on never skipping a
tier.

1. **gwemu bare-metal** — Thumb-2 logic, ITCM/DTCM/AXI placement, no Retro-Go.
2. **gwemu + Retro-Go** — ABI, DTCM allocation, SD, video, input, audio.
3. **Hardware** — the only tier that sees the pipeline, and the only one that
   sees peripheral initialisation or real memory ordering.

## Backlog

### Queue A — memory tier: the groundwork is DONE

1. ~~MCB arena~~ — correct; DF's one refusal is real DOS semantics.
2. ~~Pager eviction~~ — built: second-chance clock, comparison-based dirty
   detection, both vetoes wired, six negative controls seen to fail.
3. ~~Pagefile~~ — built. `x86page.swp` beside the `.dsk`, `slot` == guest page.
   Measured: 39 writes, 5 reads, 0 errors, `_exhausted=0`.
   **Cost: ~25 ms per writeback** — one and a half frames, paid at level load.
   A write-behind queue is the fix if a title needs one.
4. ~~Frame donation~~ — built and live: 34 frames / 139,264 B to Wolf3D.
5. **Keen 4's levels** — now fundable by the same mechanism. Blocked on B3
   freeing the 0Dh read shadow; see Queue B.

### Queue B — rendering

0. ~~Planar store path~~ — write class decided at `OUT` time. **+55-73%** on
   Wolf3D in Mode Y.
1. ~~Golden-screenshot gate~~ — `scripts/golden_screens.py`. **Caveat proven in
   practice: ALLEYCAT's golden was captured while the title was stuck on its
   first frame, so it blessed a broken state. Attestation records what a golden
   SHOWS, not whether the emulator was healthy. Re-bless against an
   owner-verified run.**
2. ~~Mode Y off-screen cache~~ — 8,192 → 15,616 offsets; `lost_cache` → 0.
3. **Fold-trap the VGA aperture check** — in progress. Removes 4-5 instructions
   from every memory access in every title, and may free the 65,536-byte 0Dh
   read shadow, which is what Keen's intro needs.
4. **Keen's half-height intro** — diagnosed: `crtc_start=15360`, pitch 64,
   19,456 offsets fitted → exactly 64 rows. Needs 28,160 offsets. Blocked on 3.
5. **DMA2D A/B** — in progress; chained 13h only, timing unprovable below tier 3.
6. **Overlay legibility over 256-colour modes** — in progress.
7. **Mode Y real pages** — done, 3 pages, `alias_hits=0`. Awaiting the owner's
   verdict on the doubled-walls artefact.
8. Alley Cat's other cats flicker in and out. New, unmeasured.

### Queue C — Dark Forces, in dependency order

The groundwork Queue A just landed is what makes this a road rather than a wall.
**Nothing here starts before the step above it is measured.**

1. ~~**XMS**~~ — built. A BLOCK-COPY interface, so its memory is never folded,
   never AXI-resident and costs the interpreter nothing; it lives in
   `x86page.swp` above every pager slot. Advertises 4 MB, **0 with no pagefile**.
   MEASURED: WOLF3D uses it unprompted (278 moves, 430,636 B, 0 errors) at a 4%
   frame cost — 31% until the file handle was held open.
   [`docs/12`](docs/12_memory_tiers.md) §6.
2. **EMS** (`EMMXXXX0`, INT 67h, a page frame in the UMB region). Independent of
   XMS and cheaper; several titles probe for it and we currently answer absent.
3. **DPMI** (INT 2Fh AX=1687h, INT 31h) — needs 1 and the descriptor walk from
   4. This is what DOS/4GW actually asks for; VCPI is the fallback it tries
   first and may not be needed.
4. **Protected mode** — ~1,225 lines sized against `references/tiny386`:
   descriptor walk, far transfers and gates, `IRET` across privilege, paging.
   **V86 is not needed** — INT 21h is HLE'd, which removes the TSS work too.
5. **Dark Forces** — re-measure against 1-4. It dies in its first frame slice
   today with "Not enough memory"; that message is the whole dependency list.

**Measure at each step, not at the end.** The last time this chain was judged
it was called out of scope on a 640 KB denominator that only existed because
no pagefile was wired.

### Queued — SylverB's FatFs DMA + SPI perf work

`references/FatFs-DMA` (branch `DMA`, `6d6f6fb`). **The changes are in
`user_diskio_spi.c` / `user_diskio_softspi.c` — the SD driver, which lives in
the FIRMWARE, not FatFs core.** Our `lib/FatFs` already matches that tree bar
`ffconf.h` and one function we added, so this is firmware work we consume.

- `fd1ff62` — bulk `HAL_SPI_TransmitReceive` for 512-byte blocks, **CMD18
  multi-block reads** with a capability probe, ACMD23 pre-count, CMD17
  fallback. **Needs no DMA**, and replaces per-sector round trips — plausibly
  the larger win for our access pattern.
- `6d6f6fb` — DMA for SD reads, +125 lines.

Why it matters: the pagefile costs **~25 ms per writeback**, XMS measured an
`fopen`/`fclose` per move costing **31%** of Wolf3D's frame rate, and
`docs/12` parks the `.dsk` cache in D2 AHB SRAM *specifically* because that is
the only SRAM DMA1/DMA2 can reach — "against the day SPI1's polled transfer
becomes a DMA transfer". That day is this branch.

**BLOCKED on upstream.** SylverB has a retro-go-sd side still to come — the
driver file is in his FatFs repo but the plumbing is not: DMA stream/handle
setup, the transfer buffer in **D2/AHB SRAM** (DMA1/DMA2 cannot reach DTCM),
and cache maintenance around the transfer. `docs/12` already parks the `.dsk`
cache in AHB for exactly this. Do not start half of it; wait for his part.

**Both commits are READS.** The pagefile's cost is a write, so measure before
assuming it helps writeback; it should help disk-heavy titles and XMS-backed
asset loading either way.

### Not scheduled, but live

- **VBE 320x240** (`DOS_MODE_VBE240` in the prior port) — the only mode that
  fills the panel and reduces the blit to one `memcpy`. Custom mode number, so
  little period software asks for it.
- An **adapter-advertisement Options entry** (CGA / EGA+VGA / VGA-only), so a
  title that cannot render what we claim is a user-visible switch, not a black
  screen.
- **`x86_vga_planar_store` in assembly** — gated on re-profiling it first. The
  case rested on pre-optimisation data; the write-class work moved PC samples
  out of it. The win is escaping AAPCS (`R8`-`R11` are reserved *and*
  callee-saved, so every call from the interpreter forces a prologue), ~20-25
  cycles against a ~10-15 cycle body. **If the path is a single-digit share of
  frame time, the answer is no.** Once converted the C is dead and every change
  is hand-written — this function changed ~6 times in one session.
