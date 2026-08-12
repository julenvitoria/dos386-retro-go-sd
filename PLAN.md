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

**`STATUS.md` carries the live task order.** In short: absolute-pointer dispatch
(−4 instructions per dispatched instruction), audit the `TBH` tables, lazy
PF/AF, finish flag coverage — then Phase 2's segmentation, prefixes and real
16-bit support, which is what Alley Cat actually needs.
