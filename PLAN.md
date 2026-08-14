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

**Queue A — Dark Forces and the memory tier**
1. Re-measure Dark Forces. The MCB arena is fixed but DF issues zero INT 21h
   calls since the launch-path revert, so the arena is unexercised by it.
2. **Decide DPMI/VCPI/XMS in or out of scope by MEASURING first** — the
   code-vs-data write split for a DOS/4GW image. DF wants ~8 MB against ~840 KB
   of AXI. Do not half-build it; the answer may be "out of scope".
3. Protected mode: descriptor walk, far transfers, IRET privilege change,
   paging. Sized against `references/tiny386`. `CR0` has never been non-zero.
4. Exercise the pager. ZERO/NOR/FILE tiers are built and **no guest has ever
   run against them**.
5. Keen 4 levels are **not fixable** — 256 KB card against ~145 KB of arena.
   Recorded so it is not re-proposed.

**Queue B — rendering, then the interpreter**
0. ~~The planar store path halves the interpreter.~~ **MOSTLY DONE**: the write
   class is decided at `OUT` time; the two dominant idioms have fast paths.
   gwemu WOLF3D t+24 s: ips 5.35 M -> 8.3-9.2 M, `cls_general` -> 0, picture
   bit-identical. Left: B0.3 (collides with the `vga_cell()` single-definition
   invariant) and B0.4 (batched planar `REP`). Confirm on hardware: `STATUS.md`.
1. ~~A **golden-screenshot gate**.~~ **DONE**: `scripts/golden_screens.py` +
   `tests/test_goldens.py`. WOLF3D (Mode Y) and ALLEYCAT (CGA) bit-exact;
   KEEN4 (EGA 0Dh) recorded `known_bad`.
2. ~~Wolf3D's off-screen cache too small.~~ **DONE**: fitted from the arena,
   8,192 -> 15,616 offsets, the whole `0xC300..0xFFFF`; `lost_cache` 16,896
   -> 0. Not confirmed by picture — gwemu never showed the corruption.
3. Fold-trap the VGA aperture check (~80 sites x 4 instructions) so non-planar
   titles pay nothing. **Now also blocks item 4**: for 0Dh the 64 KB aperture
   claim is only a CPU READ SHADOW, and trapping reads would free it.
4. Keen's watchdog reset and the clipped intro. **DIAGNOSED, prediction WRONG**
   — the intro does not widen the Offset. Measured `crtc_offset=0x20` (pitch
   64), `crtc_start=15360`, card 19,456: `(19456-15360)/64` = exactly the 64
   rows that render. 200 rows need 28,160 offsets = 112,640 B against 77,824
   fitted; the aperture holds the other 65,536 (see item 3).
5. A/B the DMA2D blit on hardware — merged, off, and unverified.
6. Retro-go overlay legibility over 256-colour modes.

**Not scheduled, but live:** Mode Y still paints mid-draw frames in gameplay
(`x86_modey_mid_draw`).
