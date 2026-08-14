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
- **In progress**: pager eviction, and donating spare guest frames to the arena
  — [`docs/12`](docs/12_memory_tiers.md).
- **Measurement-gated**: NOR backing. Re-measure the code-vs-data split first;
  it is currently 100% write, which kills the premise
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

### Queue A — Dark Forces and the memory tier: CLOSED

Verdicts only; evidence in [`docs/12`](docs/12_memory_tiers.md) and
[`docs/13`](docs/13_title_status.md).

1. Dark Forces exercises the MCB arena and **the arena is correct** — its one
   refusal is real DOS semantics. DF dies in its first frame slice.
2. **DPMI/VCPI/XMS — NOT out of scope. That verdict is RETRACTED.** The
   measurement (zero read-only pages, 100% of residency via writes) kills
   **NOR** backing, which needs a read-mostly set to XIP from flash. It says
   nothing about a **pagefile**, whose whole purpose is pages that were
   written — that is the FILE tier. `x86_pager.h` sizes the pager at
   `X86_PG_PAGES = 2048` = **8 MB**, "sized for the space rather than for the
   640 KB a real-mode DOS guest can address". Backing 8 MB is the design, not
   a stretch. Prerequisite chain: pagefile → XMS/EMS → DPMI.
3. Protected mode **sized, not started** (~1,225 lines; V86 not needed). The
   old gate — "a title that needs it *and* fits in 640 KB; there is none" —
   was circular: the 640 KB ceiling only exists because there is no pagefile.
   Gated on item 2 instead.
4. Pager exercised. **Eviction is not implemented** — now in progress.
5. **Keen 4's levels are REACHABLE** — the "not fixable" verdict is retracted.

### Queue B — the planar store path first, then rendering

0. **The planar store path halves the interpreter for graphics titles** (~1.4 M
   ips in Mode Y against ~3.0 M in text). Four changes, each measured:
   precompute a mode index at `OUT` time; fast-path the dominant single-plane
   write-mode-0 case; keep the hot cases out of C; batch `REP STOS`/`MOVS`.
   Baselines to beat and the counters to read are in
   [`docs/09`](docs/09_frame_pacing_and_clocks.md) §7.
1. A **golden-screenshot gate** with programmatic comparison. Two "runs fine,
   renders garbage" regressions shipped in one session because nothing in the
   suite can see a picture. A golden captured from a broken build blesses the
   bug, so each must be eye-verified once and that verification recorded.
2. Wolf3D's off-screen VRAM cache is too small — discards above `0xE300`.
3. Fold-trap the VGA aperture check so non-planar titles pay nothing for it.
4. Keen's watchdog reset and half-height intro.
5. A/B the DMA2D blit on hardware — merged, off, unverified.
6. Retro-go overlay legibility over 256-colour modes.

### Queued — `x86_vga_planar_store`: exhaust C, then consider assembly

**Gated on measurement, in this order. Do not start at step 3.**

1. **Re-profile it.** The case for converting rested on pre-optimisation data:
   PC samples used to land in `x86_vga_planar_store`, and after the write-class
   work they moved back into ITCM and `cls_general` went to 0. Establish what
   share of frame time the planar path costs *now*, on hardware
   (`profile_opcodes.py --retrogo --target hardware --settle 20 --window 25`),
   with Wolf3D in Mode Y.
2. **Exhaust the C.** More classes, cheaper `vga_cell()` (cache the page base
   and recompute only when the offset leaves it), avoid re-deriving what the
   class already implies. Measure each.
3. **Only then assembly, and only for the ABI.** The win is not out-guessing
   the compiler — GCC's Thumb-2 is good — it is escaping AAPCS. `R8`-`R11` are
   reserved for CPU state and are callee-saved, so every call from the
   interpreter forces a prologue/epilogue: ~8 instructions of stub plus the
   callee's own, ~20-25 cycles of pure overhead against a fast-path body of
   ~10-15. Hand assembly reads `R8` directly and returns without touching the
   stack.

**Cost to weigh against it:** once converted, the C is dead and every future
change is hand-written. This function was modified ~6 times in one session,
including the change that bought +55-73%. Freezing it early would have made
that harder. **If step 1 says the path is a single-digit share of frame time,
stop — the answer is no.**

Keep the C as the oracle if it is converted: diff assembly against C on the
same inputs, the way fast paths are already graded against the general path.

### Not scheduled, but live

- Mode Y still paints mid-draw frames (`x86_modey_mid_draw`).
- **VBE 320x240** (`DOS_MODE_VBE240` in the prior port) is the only mode that
  fills the panel and reduces the blit to one `memcpy` — not ported. Note it is
  a *custom* mode number: little period software will ask for it.
- An adapter-advertisement Options entry (CGA / EGA+VGA / VGA-only), so a title
  that cannot render what we claim becomes a switch, not a black screen.
