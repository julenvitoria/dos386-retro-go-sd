# TinyEMU x86 (STM32H7B0VB / Retro-Go Port)

This project rewrites the TinyEMU x86 (32-bit) core into highly optimized ARM Thumb v2 assembly for the STM32H7B0VB MCU running the Retro-Go framework.

## Project Workflow & Navigation
This project relies on strict documentation organization to manage extreme architectural complexity. 
**ALWAYS consult these files to orient yourself:**

1. **`STATUS.md`**: Your immediate dashboard. Check this for a quick overview of current progress and our immediate sprint focus.
2. **`PLAN.md`**: The master roadmap. This is your cheatsheet to understand how the current task fits into the big picture, and points to the technical blueprints in `docs/`.
3. **`TOOLING.md`**: Details the scripts, the support modules, and the strict 3-tier testing hierarchy (gwemu bare-metal -> gwemu + Retro-Go -> Hardware).
4. **`docs/`**: The meat of the project. Contains the comprehensive, micro-managed technical designs for each subsystem.

*Note: `CLAUDE.md`, `STATUS.md`, and `PLAN.md` must strictly remain under 100 lines so they are never skipped over.*

## Core Conceptual Constraints (Read Carefully)
Working on this project requires a fundamental mindset shift:
- **Silicon**: We must play to the STM32H7B0VB's strengths and utilize its capabilities as efficiently as possible.
- **Memory**: We must emulate 8MB of guest RAM using only ~840KB of AXI SRAM via a NOR Flash XIP + COW paging strategy. 
- **Pipeline**: Cortex-M7 is superscalar dual-issue. Assembly must be written to avoid data dependencies and pipeline stalls.
- **Zero-Wait-State**: Hot execution loops MUST be pinned to ITCM (64KB). Critical state and page tables MUST be pinned to DTCM (128KB).
- **FatFs HLE**: Do not emulate raw block devices. We bypass the guest OS block layer and translate DOS file I/O directly to FatFs calls.
- **Scripting & Target Interaction**: All scripts MUST be written in Python. **NO `.gdb` scripts.** Any script communicating with a target must use `gnwmanager`'s interchangeable backends (`GDBBackend` for `gwemu`, `OpenOCDBackend` for real hardware). This enables powerful, reusable Python scripts that can multiplex tasks (e.g., polling the retro-go log buffer while simultaneously reading/writing memory or inspecting PC values) seamlessly across both emulation and silicon.

## Reserved Register Contract
*The R12 incident — 136 handler lines using the dispatch table base as scratch —
happened because this contract lived only in a comment. It lives here now.*

| Reg | Holds |
|---|---|
| `R2` | **Active MODE RECORD** from dispatch until the ModRM call. Not reserved after that. It carries the dispatch table, the 0F table at `[R2,#-8]` and the address-size ModRM decoder at `[R2,#-4]`; `66`/`67` are one `EOR` on it. A handler must not clobber R2 before `BLX` on the decoder — use R3 for the mod test. |
| `R6` | **Active segment descriptor POINTER** (not a base value). `R8` is the "no override" sentinel; reset at `.fetch_decode_loop`, set by an override prefix, which re-enters at `.fetch_next_byte_no_reset` so it survives its own instruction and no further. |
| `R8` | CPU context (DTCM) |
| `R9` | Fold table (DTCM) |
| `R10` | Linear EIP |
| `R11` | Flag state |

Scratch is `R0`–`R5` and `R12`. `R0` is the opcode and `R1` the ModRM byte
through a handler; `R7` is the effective address out of `DECODE_MODRM`.
Handlers are emitted twice, once per operand size, and differ only through the
width macros (`LD_V`/`ST_V`/`LDREG_V`/`STREG_V`/`LDS_V`/`EIP_ADV_V`/`SP_*`/
`NSH`/`OPSZ`). Write against those, never a bare `LDR`/`STR`, or the handler is
silently 32-bit in both passes -- which is exactly what the dead
`MEM_READ_SZ`/`STACK_INC` scheme left behind. A handler that must be per-pass
is declared with `OP`, not `OP_AGNOSTIC`, and needs a `.set` alias in both
blocks of `cpu_exec_handlers_0f.S` or the shared tables will not resolve.
**`DECODE_MODRM` clobbers `R2`–`R5` and returns the linear address in `R7`** —
anything live across that call must be recomputed after it or spilled. Reading a
source register *before* the call to fill the walk's shadow is a real bug that
`MOV Ev,Gv` shipped with; `R1` survives, so recompute from it instead.

## Project Concepts
*If a conceptual misunderstanding happens repeatedly during a session, or if a third-party tool has extremely strict caveats, it MUST be briefly referenced here and thoroughly documented in `docs/` to prevent future errors.*
- **Never write scratch to `/tmp`, and always clean up.** `/tmp` here is RAM-backed; a gwemu run copies the SD image and extflash (~66 MB each) and leaking a few dozen fills it. Run scratch goes under `build/run/` in the project, is deleted in a `finally` (plus an `atexit` guard), and is kept only with an explicit `--keep-temp`. Any new code that creates temporary files follows the same pattern.
- **Never use the scratchpad for work the project will repeat.** Target inspection, measurement and build steps always recur — they belong in `scripts/` (user-facing CLIs) and `scripts/common/` (importable modules). Extend what exists rather than adding a near-duplicate; redundant scripts are how four overlapping `*_runner` modules happened. **Any change to that codebase requires updating [`TOOLING.md`](TOOLING.md)** — keep it to what each script does and how to run it, with rationale in the module docstring.
- **Timelines are capped at 60s TOTAL, and 30s is usually plenty.** A gwemu input timeline exists to open a preselected ROM, not to wait for something to happen. If a run needs a longer window to show a result, that is a bug to find with `scripts/probe.py`, not a wait to sit through — sampling PC and watched globals locates it in minutes, a long timeline just burns them.
- **Never skip test tiers.** New tooling's first execution must NOT be against silicon — a failure then has two possible causes (your tool, or the target) and you cannot separate them. Run under gwemu first, always. `HardwareTarget` also flashes internal flash, overwriting what is there. See [`TOOLING.md`](TOOLING.md).
- **Green under gwemu ≠ hardware ready.** `gwemu` does not model peripheral *initialisation*: extflash at `0x90000000` is plain readable memory there, but on silicon it needs OCTOSPI memory-mapped setup. This is broader than `docs/04`'s timing and SD-busy caveats — anything depending on a peripheral being set up is unproven under gwemu. Bare-metal firmware must have real fault handlers, or every hardware failure looks like a hang.
- **Retro-Go & gwemu Gotchas**: Do not trust `gwemu` for timing, beware of stale SD card images during build, and never run concurrent builds in a shared folder. See [`docs/04_retro_go_and_gwemu.md`](docs/04_retro_go_and_gwemu.md) for the exact rules to avoid wasting hours.
