# TinyEMU x86 ARM Thumb v2 Project Plan

**Goal**: Rewrite the 32-bit x86 component of TinyEMU into ARM Thumb v2 assembly, heavily optimized for the STM32H7B0VB MCU running Retro-Go.

This project is broken down into three major architectural phases, each bound by extreme memory and performance constraints. Detailed technical implementation blueprints for each phase are maintained in the `docs/` directory.

## Phase 1: Memory Architecture & Paging
**Objective**: Emulate a DOS machine's RAM within the AXI SRAM available.

**Measured budget (2026-08-12)**: RAM_EMU is 724 KB (`__RAM_EMU_LENGTH__`), plus
~150 KB released as a *bonus pool* when the LCD runs in LUT8 — the framebuffer
pool is statically 300 KB for the largest mode, and LUT8 frees the upper half.
Total ~874 KB, but as **two disjoint regions**, and the bonus is uncached
(`._ram_uc`). The pool allocator must span both.
**Strategy**: 
- **Done**: checked address translation. `host = lin + k` from a flat DTCM
  table; unmapped pages hold `FOLD_INVALID` so one `ADDS`+`BMI` validates every
  access at zero cost. Entries are offsets, not bases.
- **Next**: COW on top of it — a second (write) map, a page pool, dirty tracking
  from day one, and loud exhaustion. NOR backing is measurement-gated: the prior
  project found ~100% of NOR-backable *data* granules get written anyway, so
  measure the code-vs-data split before building it.
- The fold table is pinned to 0-wait-state DTCM (16 KB for a 16 MB window).
- Eviction policy is **not** the lever: the prior project measured LRU capped at
  2.1x over FIFO, while the pool-size cliff is 350x. Spend effort on bytes of
  AXI, not on policy.
**Blueprint**: [`docs/01_memory_architecture.md`](docs/01_memory_architecture.md)

## Phase 2: CPU Pipeline Optimization
**Objective**: Port `x86_cpu.c` to Thumb v2 assembly with maximum Instructions Per Clock (IPC).
**Strategy**:
- The Cortex-M7 is a superscalar dual-issue processor. Assembly must be hand-interleaved to avoid Read-After-Write (RAW) data dependencies, ensuring both issue ports remain busy.
- The core execution loop is pinned to the 64KB ITCM for instantaneous instruction fetching.
- Critical x86 state variables (EIP, EFLAGS, Context Pointer) are permanently mapped to high ARM registers (`R8`-`R11`).
**Blueprint**: [`docs/02_cpu_pipeline_optimization.md`](docs/02_cpu_pipeline_optimization.md)

## Phase 3: Device Emulation & High-Level Emulation (HLE)
**Objective**: Implement minimal PC hardware and optimize disk I/O.
**Strategy**:
- **VGA**: 32KB of VRAM is permanently allocated. For EGA 256KB mode, the extra 224KB is dynamically borrowed from the guest RAM AXI pool.
- **Disk I/O**: Instead of emulating an IDE controller and double-caching sectors, we will High-Level Emulate (HLE) DOS FatFs. By hooking INT 21h, we bypass the guest OS block layer and route file requests directly to the host FatFs library.
**Blueprint**: [`docs/03_devices_and_hle.md`](docs/03_devices_and_hle.md)

## Testing & Validation Hierarchy
gwemu is the basis; validation occurs strictly in this order. See `TOOLING.md`.
1. **gwemu, bare-metal** (`test_core.elf`): Thumb v2 logic and ITCM/DTCM/AXI
   placement, isolated from Retro-Go.
2. **gwemu + Retro-Go**: the packed core inside the real firmware — ABI, ITCM
   segment, DTCM allocation, SD/video/input/audio.
3. **Real Hardware**: the only tier that sees the pipeline. Deterministic to
   ~1e-7, so it is the *precise* instrument, not merely the authoritative one.

## Execution Strategy

Steps 1 and 2 (build system, linker layout, CPU core foundation, DTCM/ITCM
placement, retro-go core packaging) are **complete and verified on hardware**.

## Backlog

Ordered within each queue. `STATUS.md` holds the evidence for each; this is the
list, not the history.

**Queue A — Dark Forces and the memory tier: CLOSED, measured 2026-08-14.**
Evidence in `STATUS.md`; the verdicts, so they are not re-opened by guess:
1. ~~Re-measure Dark Forces.~~ Done. It exercises the arena now (48h ×3,
   4Ah ×15, 49h ×0) and **the arena is correct** — the one refusal is real DOS
   semantics. DF dies in its FIRST frame slice at `0393:005E` executing its own
   "Not enough memory" string, exit reason 4, opcode `0x63`.
2. ~~Decide DPMI/VCPI/XMS.~~ **OUT OF SCOPE.** DF wants ~8 MB against the
   pager's 160 frames = 640 KB, and the read-mostly premise is dead: WOLF3D
   `_faults_r=0/_faults_w=104`, KEEN4 `0/134`. **Zero read-only pages in either
   working set.** Do not half-build DPMI.
3. Protected mode: **sized, not started** (~1,225 lines of tiny386 C, table in
   `STATUS.md`; V86 not needed since INT 21h is HLE'd). Gated on a title that
   needs it AND fits in 640 KB. There is none.
4. ~~Exercise the pager.~~ Done, WOLF3D and KEEN4: quiet, `_exhausted=0`, peak
   104/160 and 134/160 frames. **New defect found: eviction is not implemented
   at all** — eight documented `x86_pg_*` counters and `x86_livemap_holds()`
   have no callers and are GC'd out of the ELF.
5. **Keen 4 levels: REACHABLE. The "not fixable" verdict was WRONG and is
   retracted.** It quoted the prior port's budget (~24.9 KB of headroom, COW
   spare 0) and not ours. Measured here: the arena is 143,360 B, and KEEN4
   peaks at **134 of 160 pager frames**, leaving 26 frames = 106,496 B idle.
   `11-ega-planar.md`'s own table puts a correct picture at 48 KB/plane =
   196,608 B, so the deficit is 53,248 B = 13 pages against 26 spare. The
   mechanism is already designed in — `x86_pg_donated` ("frames given to the
   arena"), `x86_arena_add_region()`, and Phase 3's "borrowed from the guest
   RAM AXI pool". Being implemented alongside eviction, because donating frames
   without eviction turns a shortfall into a hard `X86_EXIT_MEM_FAULT`.

**Queue B — rendering, then the interpreter**
1. A **golden-screenshot gate** with programmatic comparison. Two "runs fine,
   renders garbage" regressions shipped in one session because nothing in the
   suite can see a picture.
2. Wolf3D's off-screen VRAM cache is too small: 8,192 offsets, discards above
   `0xE300`. Corrupts every cached picture. ~12 KB of arena is free.
3. Fold-trap the VGA aperture check so non-planar titles pay nothing for it.
4. Keen's watchdog reset and half-height intro.
5. A/B the DMA2D blit on hardware — merged, off, and unverified.
6. Retro-go overlay legibility over 256-colour modes.

**Not scheduled, but live:** the planar store path roughly halves the
interpreter for graphics titles (Wolf3D ~1.4 M ips in Mode Y against ~3.0 M in
text). Mode Y still paints mid-draw frames in gameplay
(`x86_modey_mid_draw`). Both are bigger than most of the above.
