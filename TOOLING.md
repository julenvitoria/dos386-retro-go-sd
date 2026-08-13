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

`--watch` reads globals. `--deref SYM+OFF` follows a pointer into a struct.
`--dump SYM:N[xG]` reads a symbol's OWN storage as N words grouped G per line,
which is what an array needs — `--deref` would treat the first word as a
pointer. A literal `0xADDR` works in place of a symbol, for memory with no name
(guest RAM, a stack frame, a peripheral register). Dumped words that land
inside a known function are annotated with it, so a saved LR reads as
`HANDLE_INTERRUPT_16+0xd9` rather than a bare address. Dumps happen AFTER
sampling: read at t=0 and you see the state before the target has done
anything, which is how a full log reads as empty.

```bash
python3 scripts/probe.py --retrogo --wait-magic \
    --watch core_ready,core_dtcm_ok,benchmark_insns --deref cpu_state_ptr+8=EAX
python3 scripts/probe.py --dump x86_cs_log:16x4=CSLOG  # array contents
python3 scripts/probe.py --dump 0x2408bf48:16x4=STACK  # or a raw address
python3 scripts/probe.py --target hardware --watch run_flag,benchmark_cycles
python3 scripts/probe.py --retrogo --set run_benchmark_flag=1 --watch benchmark_insns
```

Samples PC (labelled ITCM / firmware symbol / region), watched globals, and
pointer dereferences. Reach for this when a harness reports a value that makes
no sense — it is faster than lengthening a timeline, which is capped at 60s.

### `watch_memory.py` — catch whatever is writing to an address

```bash
# who is scribbling on the relocated stub at guest linear 0x2E920?
python3 scripts/watch_memory.py --retrogo --wait-magic \
    --addr '*guest_ram+0x2E920' --length 4
python3 scripts/watch_memory.py --retrogo --wait-magic --addr core_ready --hits 3 \
    --peek '*guest_ram+0x810:224'
```

`probe.py` samples; this catches. A corrupting write is one instruction among
millions and polling never sees it, so this sets a real data watchpoint and
reports the ARM PC at the stop resolved against the ELF — inside the
interpreter that PC is the handler, which names the guest instruction class.
`--addr` takes a raw `0x…`, a symbol, or `*SYM+OFF` to dereference a pointer
(guest RAM needs the last form). `--peek` hexdumps on every hit, which turns a
watchpoint on a completion flag into a clean snapshot of what a load produced.

Caveat: a stop can repeat on the same access without advancing. Treat repeated
identical hits as one event, not as several.

### `trace_diff.py` — bisect our trace against a reference, automatically

```bash
python3 scripts/trace_diff.py --dsk references/.../TOPBENCH.dsk --args=-l --bisect
python3 scripts/trace_diff.py --dsk ... --args=-l --shift 0 --skip 43648
python3 scripts/trace_diff.py --dsk ... --args=-l --shift 0 --skip 1032768 --reg CX
```

Rebuilds the core with the sampler in `cpu_exec.S` switched on (parameters in
`x86_trace.inc`, rewritten by this script and restored on exit), runs it under
gwemu, runs the identical image under Unicorn, and reports the first sample
where the two disagree. `--bisect` walks 16384 → 256 → 1 by itself.

It exists as a script because every part of it is a trap, and each one cost
real time here:

- A failed build followed by `&&`-chained steps leaves a **stale image** on the
  SD card and the probe reports the previous build's trace as the new one.
- The sampler must **saturate, not wrap**. A ring spans only its own length and
  silently shows the end of the run wherever you point it.
- A long `REP` makes thousands of consecutive EIP samples identical, so an
  index-by-index diff reports a match where there is only coincidence. Confirm
  with `--reg CX`.
- `--reg FL` samples R11, not `cpu_state.eflags`: EFLAGS is cached in a register
  and only written back on exit. AF is excluded from the comparison because x86
  leaves it undefined after shifts, rotates, logic and MUL/DIV.
- Unicorn fires its code hook once per `REP` element **and once more** with CX
  already zero. `refx86` drops that phantom firing; without it every index after
  a REP is off by one and it reads as "the target left the loop early".
- Anything we execute that the reference cannot know about — HLE stub
  instructions after an interrupt — shifts our index against theirs.
  `x86_cs_log_idx` says whether any interrupt was taken at all.

### `ref_trace.py` — run the same guest under a reference x86

```bash
python3 scripts/ref_trace.py --dsk references/.../TOPBENCH.dsk --args=-l --summary
python3 scripts/ref_trace.py --dsk ... --args=-l --every 16384 --count 64
python3 scripts/ref_trace.py --dsk ... --args=-l --skip 43520 --every 1 --count 64
```

When a guest walks off into memory it does not own, the question is always
"which instruction first behaved differently from a real 8086". This runs the
identical image under Unicorn (`pip install unicorn`) and prints a trace in the
same shape, so the two can be diffed. The image is built by
`scripts/common/dosimage.py`, which mirrors `x86_dos_exec` — same PSP, same
load segment, same relocations, same entry registers; starting anywhere else
would make the comparison meaningless. It stops at the first software
interrupt, which for a self-unpacking program is the finish line.

**Aligning the two traces takes care.** Our interpreter re-enters its fetch loop
once per `REP` element, and so does Unicorn's hook, but instructions executed in
the HLE stub page are ours alone and desynchronise the counters. A long `REP`
also makes thousands of consecutive samples identical, so a naive index-by-index
diff reports a match where there is only coincidence — confirm a suspected
divergence by comparing a register (CX, DX) over the same window, not just EIP.

### `screenshot.py` — capture what the panel is being sent

```bash
python3 scripts/screenshot.py --wait 20 --out build/artifacts/topbench.png
python3 scripts/screenshot.py --timeline timelines/run-to-completion.tl \
    --wait 48 --pan 16 --text --out build/artifacts/topbench.png
python3 scripts/screenshot.py --target hardware --wait 20
```

`--text` dumps the 80x25 character buffer alongside the image; judging 8x8
glyphs by eye is how a `Runtime error 203` went unread through two captures.
`--pan N` pins the text window's leftmost column: the panel shows 40 of the 80
columns and normally follows the cursor, so anything further right -- a score at
the end of a wide line -- is invisible until you pin it.

gwemu's own headless screendump comes back black, so it proves nothing. The
LTDC registers ARE modelled: this reads Layer 1's framebuffer address, pitch
and line count, reads that memory and the firmware's CLUT, and rebuilds the
frame. It also prints a coarse text map of the result, so a black or garbage
capture is obvious without opening the file. `tests/test_screen.py` does this
for one expected image; this is the general form.

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
python3 tests/test_dos.py                # MCB arena, walked from the host
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
`timelines/run-to-completion.tl` is the same launch with a 58 s window, for a
guest doing real work whose result only appears at the end -- the one case where
the wait IS the result rather than a symptom to chase with `probe.py`.

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
| `probe.py` | Sample PC, globals, pointer derefs and array dumps on a running target; `read_faults` decodes CFSR/HFSR/BFAR |
| `watchpoint.py` | Z2/Z3 data watchpoints over the GDB remote protocol, and the unsolicited stop reply that follows a resume |
| `dosimage.py` | FAT12 reader + MZ loader that mirrors `x86_dos_exec`, so a reference emulator starts from OUR entry state |
| `refx86.py` | Unicorn run of that image, sampled like the target's own sampler |
| `refdos.py` | A minimal DOS/BIOS for the reference, so it can run PAST the first INT 21h and say what the program should do. Deliberately not modelled on our x86_dos.c -- mirroring it would make the two agree on the same mistake. It has no port I/O, so it cannot follow a program that times against the CGA retrace |
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
