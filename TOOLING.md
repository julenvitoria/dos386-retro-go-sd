# Tooling & Testing Hierarchy

Three tiers, each isolating a different class of failure. **Never skip a tier** —
new tooling pointed straight at silicon turns one unknown into two.

| Tier | What it proves | Boots |
|---|---|---|
| 1. gwemu, bare-metal | CPU core in isolation | `test_core.elf` |
| 2. gwemu + retro-go | Core inside the real firmware | retro-go + `dos.bin` |
| 3. Real hardware | Timing, pipeline, SD behaviour | same image as tier 2 |

gwemu is the basis throughout; plain QEMU adds nothing it does not already do.

Guest titles are **`.dsk` floppy images** (FAT12), not bare executables.

## Scripts (`scripts/`)

Run these directly. All take `--help`.

### `build_retrogo.py` — build the retro-go firmware

```bash
python3 scripts/build_retrogo.py                      # defaults
python3 scripts/build_retrogo.py --dry-run            # print the make command
python3 scripts/build_retrogo.py --clean              # required if flags change
python3 scripts/build_retrogo.py --extflash-offset-mb 4
python3 scripts/build_retrogo.py --bank 2             # productive dual-boot
```

Defaults to retro-go's release parameters with `INTFLASH_BANK=1` (boots
directly) and a 64 MB part. Give `--extflash-part-mb` and `--extflash-offset-mb`;
the size is derived. Builds `gwemu_release`, so emulator and device media match.

### `asm_metrics.py` — per-handler assembly metrics

```bash
python3 scripts/asm_metrics.py                        # compare to baseline
python3 scripts/asm_metrics.py --update               # re-baseline (commit it)
python3 scripts/asm_metrics.py --top 20
python3 scripts/asm_metrics.py --cycles --target hardware
```

### `probe.py` — see what a running target is doing

```bash
python3 scripts/probe.py --retrogo --wait-magic \
    --watch core_ready,core_dtcm_ok,benchmark_insns --deref cpu_state_ptr+8=EAX
python3 scripts/probe.py --target hardware --watch run_flag,benchmark_cycles
python3 scripts/probe.py --retrogo --set run_benchmark_flag=1 --watch benchmark_insns
```

Samples PC (labelled ITCM / firmware symbol / region), watched globals, and
pointer dereferences. Reach for this when a harness reports a value that makes
no sense — it is faster than lengthening a timeline, which is capped at 60s.

### `sdcard.py` — compose the SD card

```bash
python3 scripts/sdcard.py --image sdcard_retrogo.img   # gwemu image
python3 scripts/sdcard.py --mount /media/you/RETROGO   # card in a reader
python3 scripts/sdcard.py --probe                      # device, over the probe
```

Same content, three destinations. Lays down `sd_content` (fonts/lang/bios),
`/cores/<name>.bin`, `/roms/<dir>/<rom>.dsk` + sidecars, and a `/CONFIG` that
preselects the ROM and disables idle standby.

### `verify_placement.py` — check the target matches the build

```bash
python3 scripts/verify_placement.py                    # symbol regions only
python3 scripts/verify_placement.py --retrogo --wait-magic --section .core_itcm
```

Confirms symbols landed in the memory they were designed for, and byte-compares
a section against the ELF. Catches a hot loop linked into flash, or a core
segment that loaded stale or partially — which otherwise surfaces only as an
UNDEFINSTR HardFault far from the cause.

### `make_retrogo_config.py` — generate `/CONFIG`

```bash
python3 scripts/make_retrogo_config.py --out build/CONFIG --rom dos ALLEYCAT.dsk
```

Preselects a ROM (so no launcher navigation is needed) and disables idle
standby. Called automatically by `make retrogo_media`.

### Vendored

`make_sdcard_image.py` (partitioned SD image), `sync_from_firmware.sh`
(re-vendor `sdk/` from a firmware checkout, then update `SDK_VERSION`).

## Source layout (emulator side)

| File | Role |
|---|---|
| `cpu_exec*.S`, `opcode*.S` | the Thumb-2 interpreter and its dispatch tables |
| `x86_mem.c` | guest page pool, fold mapping, guest↔host copies |
| `x86_io.c` | port dispatch, PIT ch2 + speaker, CGA registers |
| `x86_video.c`, `cga_blit.S` | mode 4 blitter, mode 3 text renderer, palette |
| `x86_hle.c` | `0F 0B` stubs, IVT planting, service dispatch |
| `x86_bios.c` | INT 10h / 16h / 1Ah and the BIOS Data Area |
| `x86_dos.c` | INT 21h, PSP, MCB arena, MZ loader |
| `font8x8.h` | 8x8 font, public domain (Daniel Hepper), copied in |

## Harnesses (`tests/`)

```bash
python3 tests/test_opcodes.py            # 162 cases: EFLAGS, segmentation,
                                         # 16-bit, 66/67, interrupts, speaker
python3 tests/test_benchmark.py
python3 tests/test_game.py
python3 tests/test_video.py              # CGA mode 4 blitter, pixel-exact
python3 tests/test_screen.py             # what the LTDC actually scans out,
                                         # reconstructed to build/artifacts/screen.png
python3 tests/test_core_retrogo.py       # tier 2
```

All accept `--target gwemu|hardware`, `--gui`, `--audio`, `--icount`,
`--timeline`, `--physical-sdcard`, `--keep-temp`.

Each gwemu run copies the SD image and extflash (~66 MB) into `build/run/`,
which is deleted on exit. `--keep-temp` retains it for analysis. Nothing is
written to `/tmp` — it is RAM-backed on this machine.

## Tier 2 build order

```bash
python3 scripts/build_retrogo.py --clean   # firmware (separate tree)
make sdcard                                # core -> pack -> SD image
python3 tests/test_core_retrogo.py
```

`make sdcard` chains the core build, so the image can never be assembled around
a stale core.

**Where the line sits:** make compiles (`Makefile` for the bare-metal harness,
`Makefile.core` for the packed core) — it owns source→binary dependency graphs.
Python composes, deploys, orchestrates and talks to targets, because those need
validation, multiple destinations and target awareness.

Launch is driven by `/CONFIG` preselection plus
`timelines/start-preselected-game.tl` (presses A twice). Do not navigate menus.

## Modules (`scripts/common/`)

Importable support only — no CLIs.

| Module | Purpose |
|---|---|
| `target.py` | `Image` (what boots) + `GwemuTarget`/`HardwareTarget` (where). One launch path for every tier. Also `timeline_quit_time(path)`: a test that polls longer than the timeline's `quit` outlives gwemu, and the dropped GDB link looks like a target fault. Derive waits from it; never hardcode. |
| `elf.py` | `get_symbols`, `nearest_symbol` |
| `sdcard.py` | Three SD destinations (image / probe / mounted) plus `compose()`, which lays out what the firmware needs |
| `retrogo_build.py` | Firmware build + artifact staging |
| `retrogo_config.py` | `/CONFIG` blob generation |
| `core_abi.py` | Preflight: will this firmware accept this core? |
| `rglog.py` | Read retro-go's in-memory log ring |
| `faults.py` | Decode latched CPU faults |
| `asm_metrics.py` | Instruction counts, DWT capture |
| `probe.py` | Sample PC, globals and pointer derefs on a running target; `read_faults` decodes CFSR/HFSR/BFAR |
| `verify.py` | Symbol-region checks and ELF-vs-target section comparison |
| `builder.py`, `runner.py` | Tier-1 build convenience |

## Non-negotiables

- Build flags are **not** dependencies — change one, pass `--clean`.
- Green under gwemu ≠ hardware ready; gwemu does not model peripheral setup.
- Never use raw GDB subprocesses or `gnwmanager monitor`; use the backends.
- Never run concurrent builds or tests from one directory.
- All scripts are Python with `argparse`. No `.gdb` scripts.

Detail lives with the code: each module's docstring, plus
[`docs/04`](docs/04_retro_go_and_gwemu.md) (gwemu/retro-go traps) and
[`docs/06`](docs/06_gwemu_determinism.md) (determinism data).
