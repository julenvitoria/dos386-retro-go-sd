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

#### The three clocks, and how to watch them

The emulator loop has three independent clocks (see the block comment at the
top of `src/main.c`). All of them are host-readable, and all of them are
host-**writable** where that is a knob, so the trade is measured on a running
target instead of rebuilt for:

```bash
# what the guest is actually getting
python3 scripts/probe.py --retrogo --wait-magic \
    --watch guest_kips,guest_slice_now,guest_ips_achieved,guest_fps_achieved,\
guest_insn_deficit,guest_ticks,guest_uptime_ms,guest_ticks_dropped,\
frame_ms_last,guest_exec_ms,panel_ms_min

python3 scripts/probe.py --retrogo --wait-magic --set guest_kips=0       # max
python3 scripts/probe.py --retrogo --wait-magic --set guest_kips=330     # 8088
```

| Symbol | Meaning |
|---|---|
| `guest_kips` | emulated CPU speed in thousands of insns/s. **0 = unthrottled, and the default.** `KIPS_*` in `src/main.c` has the profile rungs; the PAUSE/SET menu's "CPU Speed" entry sets it too |
| `cpu_profile_persisted` / `cpu_profile_events` | what the settings store kept when the menu entry was changed, and how many dialog callbacks reached us. The menu path has no other witness — a malformed options array never calls us at all |
| `frame_10us_avg` | mean iteration period, 8x oversampled, in 10 us units. What the firmware's `common_emu_frame_loop()` observer is fed |
| `upper_filler_ok` | is 0xA0000-0xFFFFF backed? It comes from the bonus pool now, but a 0 here explains an option-ROM scan faulting |
| `guest_insn_deficit` | instructions the emulated CPU owed and never ran. The answer to "I asked for a 386 — am I getting one?". Meaningful only in paced mode |
| `guest_ips_achieved` / `guest_fps_achieved` | measured over a 1 s window. The number that matters in unthrottled mode |
| `guest_ticks` / `guest_ticks_dropped` | 18.2065 Hz BIOS ticks emitted, and ones refused by the catch-up cap |
| `panel_ms_min` | the display period, **learned** from the shortest iteration ever completed. Nothing assumes 60 fps |
| `x86_kbd_read_log` / `x86_spk_log` | the last 32 bytes off port 0x60, and the last 32 writes that can move the speaker tone. `--dump x86_kbd_read_log:32x8` |
| `x86_kb_pushes` / `x86_kb_pops` / `x86_kb_peek_hits` | the BDA ring's producer and its two consumers. "The guest polls INT 16h forever" has two opposite causes -- nothing was queued, or something dequeued it first -- and only these three separate them. `pushes == pops == peek_hits` means every key was queued, seen and taken; `pushes > 0` with `pops == 0` is the reported failure |
| `x86_dos_file_log` / `x86_dos_path_log` | The last 32 **file-family** INT 21h calls as word pairs -- `(func << 8) \| AL`, then `(CF << 16) \| AX` -- and the last four paths opened. `x86_hle_log` cannot answer a file question: a guest polling the keyboard fills all 32 of its slots between one open and the next, which is exactly what it did while TOPBENCH's real failure (`43h` -> CF=1, AX=1) sat invisible. `--dump x86_dos_file_log:64x4 --dump x86_dos_path_log:32x4` |
| `x86_disk_reads` / `x86_disk_backend_reads` | guest-side `disk_read()` calls and how many of them reached the SD card. The RATIO is the sector cache's whole point, it is exact under gwemu, and it is the number to quote — TOPBENCH boots on 115 / 7 |
| `x86_disk_cache_ok` / `x86_disk_cache_bytes` | is the cache BACKED? A 0 here is a silent-allocation failure and the only symptom is the stutter the cache exists to remove |
| `x86_disk_cache_hits` / `_misses` / `_fills` / `_evictions` / `_bypass` | sectors served from a resident window, sectors that forced a fetch, window fetches, and requests that went straight through. Rising `_evictions` with a poor hit rate means the working set no longer fits four windows |
| `x86_disk_us_total` / `x86_disk_us_worst` / `x86_disk_errors` | time in the backend, and the worst single call. **Meaningless under gwemu** (`docs/04`: SD busy state is not modelled) and the reason the timing claim needs silicon |
| `frame_hist` / `frame_ms_worst` / `frame_overruns` | the frame-time TAIL, which is what "stutter" means. Buckets are multiples of the learned panel period, so `frame_hist[0]` is on-time and anything else is a missed deadline. A mean or a minimum cannot show a hitch; only this can. Write `frame_stats_reset=1` to open a window around a specific event. `--dump frame_hist:8x4` |
| `x86_hle_log` | the last 32 services as `(vector << 8) \| AH`, bit 16 set if nobody answered. This is what says whether a guest is polling INT 16h AH=00 or AH=01 -- a distinction the two paths do not share a symptom for. `--dump x86_hle_log:32x8` |

Guest time must not move with render rate. The measurement that proves it is
`guest_ticks / guest_uptime_ms` at two different frame rates — force a low one
with a large `--set guest_kips=…` or by loading a heavy guest.

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

`--text` dumps the 80x25 character buffer alongside the image, decoded as
**CP437**; judging small glyphs by eye is how a `Runtime error 203` went unread
through two captures, and printing only 0x20-0x7E hid every box-drawing and
shading code behind a `.`, which made a correctly drawn full-screen UI read as
four rows of stray letters on an empty screen.
`--text-aa 0|1` picks the renderer before capturing, so the SAME screen can be
grabbed both ways from one run rather than from two builds that also differ in
where the guest got to. `--pairs` counts the DISTINCT (fg,bg) attribute pairs a
text screen uses — the number that decides whether anti-aliasing fits, since
each pair costs a contiguous block of four CLUT entries.

`--pan N` pins the text window's leftmost column. It is now a no-op: the text
cell is 4 px wide, so all 80 columns land on the 320 px panel and there is
nothing off-screen to pan to. It is kept for a mode wider than the panel.

gwemu's own headless screendump comes back black, so it proves nothing. The
LTDC registers ARE modelled: this reads Layer 1's framebuffer address, pitch
and line count, reads that memory and the firmware's CLUT, and rebuilds the
frame. It also prints a coarse text map of the result, so a black or garbage
capture is obvious without opening the file. `tests/test_screen.py` does this
for one expected image; this is the general form.

### `profile_opcodes.py` — which x86 opcodes burn the cycles

```bash
make clean && make sdcard X86_PROFILE=1                   # instrumented core
python3 scripts/profile_opcodes.py --retrogo --settle 20 --window 25
python3 scripts/profile_opcodes.py --retrogo --target hardware \
    --settle 20 --window 25 --json build/artifacts/profile_topbench.json
python3 scripts/profile_opcodes.py --retrogo --sort mean   # expensive handlers
python3 scripts/profile_opcodes.py --retrogo --sort count  # hot but cheap ones
python3 scripts/profile_opcodes.py --bench --window 60     # tier 1 self-check
```

Reads DWT->CYCCNT at every dispatch and charges the delta to the PREVIOUS
opcode, into a 513-slot DTCM table (256 one-byte + 256 `0F` + one HOST slot for
time outside the interpreter). Prints count, total cycles, mean, overhead-
corrected mean and % of all cycles; `--json`/`--csv` make a profile diffable
across builds the way `tests/asm_baseline.json` already is for static counts.
Handler names come from the LIVE dispatch tables, resolved with `probe.py`'s
symbol machinery, so they cannot drift from what ran.

`--settle N` discards the first N seconds and then resets the table, so the
window is a steady state and not DOS startup. `--bench` (bare-metal only) runs
the synthetic benchmark, whose answer is known exactly — that is how the
plumbing is checked, not by trusting the cycles.

**The instrumentation is behind `X86_PROFILE=1` and OFF by default.** It sits in
the fetch/dispatch tail, the hottest and most register-starved code in the
project, and it costs ~65 cycles per dispatch on top of a ~50-cycle instruction.
Flags are not dependencies: build clean or you are profiling the previous core.

Three things the tool says in its own output, because none of them is optional:

- **A gwemu profile is not a profile.** gwemu does not model the pipeline
  (`docs/06`), so only the COUNTS mean anything there. Run it under gwemu first
  anyway — the point of that pass is to prove an instrumented dispatch tail does
  not fault, before it goes anywhere near silicon.
- **Attribution is approximate.** A delta measured between two dispatches on a
  superscalar M7 carries part of the previous instruction and of the cache/AXI
  state this one inherited. Rank hogs with it; do not quote a per-opcode cycle
  count as truth.
- **Measurement overhead is calibrated on the target** by `x86_profile_calibrate`
  (an empty loop against the same macro), stored in the table, and printed as a
  separate corrected column. Nothing on the target subtracts it, and there is no
  fudge factor. It is a lower bound — the calibration loop predicts perfectly
  and stays in cache.

A slot is closed by the NEXT dispatch, so the last instruction before each exit
from the interpreter is uncounted (one per interp call). Profiling also slows
the emulator, so anything timing-dependent in the guest is perturbed; that is
noted, not corrected.

### `make_font.py` — regenerate `font8x8.h`, `font4x8.h`, `font4x8aa.h`

```bash
python3 scripts/make_font.py                  # rewrite all three headers
python3 scripts/make_font.py --check          # fail if any is out of date
python3 scripts/make_font.py --show 0xC9      # one glyph, all three tables
```

Assembles a full 256-glyph **CP437** table from Daniel Hepper's public-domain
`font8x8` blocks, which are indexed by Unicode, plus 42 glyphs (card suits,
arrows, the maths operators at 0xEC-0xFE) drawn in the generator. Run it only
when the font changes; the results are committed.

It then emits the two **4-pixel-wide tables the renderer draws from**. 80
columns of 8 px is 640 and the panel is 320, so an 8x8 cell could only ever
show 40 of the 80 columns — and a DOS UI is centred on the 80, so the window
sliced it in half wherever it sat. At 4 px the whole 80x25 screen is exactly
320x200.

`font4x8.h` (1 bit per pixel) is the shipping face. Its letters are
**Bm437_EverexME_5x8** from VileR's Int10h Oldschool PC Font Pack, squeezed 5→4.
That replaced an OR-condensation of the 8x8 face, which looked poor for a
reason no tuning fixes: an 8 px face encodes stem placement and counters chosen
for eight columns, and halving the width destroys all of them at once. Everex
Micro Enhancer is a real character generator that showed 80 columns in a
five-pixel cell, so it was designed narrow. Measured over the 256 glyphs, only
77 put ink in the fifth column and 48 of those are line art that is hand-drawn
anyway, so the squeeze is exact almost everywhere; where it is not, columns 3
and 4 are OR-ed, touching only a right edge.

**0xB0–0xDF is hand-drawn in the generator, not converted** (`LINEART`,
`SHADES`, `SOLIDS`). This is what decides whether a narrow font is usable: a
horizontal rule must reach both cell edges or every frame comes out dashed, and
the vertical rule must sit in the same column in every glyph or corners miss. So
the grid is fixed — vertical at column 1, single horizontal at row 4, double
horizontals at rows 3 and 5. **Double VERTICAL rules deliberately alias to
single**: two strokes with a one-pixel gap in a four-pixel cell is mush and makes
every corner ambiguous. Double horizontals survive, so a `╔═╗` frame still reads
differently from a `┌─┐` one. `tests/test_text.py` asserts both properties over
the whole range, not just samples.

`font4x8aa.h` (2 bits per pixel) is the anti-aliased table: the authentic 8x8
glyph box-filtered 2:1 keeping the intermediate level, so a stem falling between
two output pixels becomes two half-lit pixels instead of vanishing. Line art is
promoted from the hand-drawn bilevel art at full level rather than filtered —
filtering a one-pixel rule would draw every frame at half brightness. See
"Anti-aliased text" below for why it is not the default.

DOS text is CP437, so the renderer indexes the font with the whole character
byte. The tree used to carry Hepper's 128 ASCII glyphs only, which forced a
`& 0x7F` in `text_render` and turned every box-drawing rule and shading block of
a full-screen UI into an unrelated letter.

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
| `x86_profile.c/.h` | per-opcode DWT cycle profiler storage + calibration (`X86_PROFILE=1` only) |
| `x86_disk.c` | the block device under our FatFs, plus the .dsk sector cache: 4 x 16 KB LRU windows from the bonus pool, which are the read-ahead AND the cache |
| `x86_mem.c` | guest page pool, fold mapping, guest↔host copies |
| `x86_io.c` | port dispatch, PIT ch2 + speaker, CGA registers |
| `x86_video.c`, `cga_blit.S` | mode 4 blitter, mode 3 text renderer (4x8 cells, all 80 columns), palette |
| `x86_hle.c` | `0F 0B` stubs, IVT planting, service dispatch |
| `x86_bios.c` | INT 10h / 16h / 1Ah and the BIOS Data Area |
| `x86_dos.c` | INT 21h, PSP, MCB arena, MZ loader |
| `font8x8.h` | 8x8 CP437 font, public domain (Daniel Hepper); the BIOS font handed to the guest |
| `font4x8.h` | 4x8 1-bit CP437, generated — Everex letters + hand-drawn line art; what `text_render` draws |
| `font4x8aa.h` | 4x8 2-bit CP437, generated — the anti-aliased table, linked only when `X86_TEXT_AA=1` |

## Harnesses (`tests/`)

```bash
python3 tests/test_opcodes.py            # 162 cases: EFLAGS, segmentation,
                                         # 16-bit, 66/67, interrupts, speaker
python3 tests/test_benchmark.py
python3 tests/test_game.py
python3 tests/test_dos.py                # MCB arena, walked from the host, plus
                                         # FILE I/O from guest code against a
                                         # FAT12 volume the test builds itself:
                                         # 3Dh/3Fh/42h/3Eh/43h/4Eh/4Fh, the
                                         # not-found path, a drive-letter
                                         # prefix and a lower-case name
python3 tests/test_keyboard.py           # BDA ring -> INT 16h, consumed by
                                         # GUEST code: AH=01 peek + its ZF on
                                         # the IRET frame, AH=00 dequeue, the
                                         # empty case and a wrap at the end of
                                         # the ring
python3 tests/test_video.py              # CGA mode 4 blitter, pixel-exact
python3 tests/test_text.py               # CGA text mode 3, pixel-exact:
                                         # CP437 at 4x8 and 8x8, attributes,
                                         # all 80 columns, scroll, line-art
                                         # continuity, the 32-entry CLUT, and
                                         # BOTH renderers with their per-frame
                                         # palette allocation
python3 tests/test_screen.py             # what the LTDC actually scans out,
                                         # reconstructed to build/artifacts/screen.png
                                         # (checks whichever mode the core is
                                         # in -- text if a guest is running)
python3 tests/test_disk_cache.py         # .dsk sector cache + read-ahead:
                                         # window alignment, LRU residency
                                         # under a stream, a short window at
                                         # the end of the image, invalidation
                                         # on write, a failed fetch, and 3,000
                                         # random reads byte-compared against
                                         # the UNCACHED path
python3 tests/test_core_retrogo.py       # tier 2
```

`test_disk_cache.py` is the one harness that does NOT drive a target. It
compiles the shipping `x86_disk.c` for the HOST (`-DX86_DISK_HOST_TEST`, which
only swaps the DWT read for a variable) against a small shim that supplies a
backend reader, and drives `disk_read`/`disk_write` through ctypes. The cache
is arithmetic over a callback — alignment, replacement, boundaries — and those
bugs are found in milliseconds off-target and in ten minutes on one. It caught
two the design had not: a straddling multi-sector read cost MORE backend calls
than no cache at all, and an undersized buffer must be refused outright rather
than half-used.

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

**Button names are not what they look like.** gwemu's `press start` drives
`B_START` — a button the stock Game & Watch does not have, which retro-go maps
to `ODROID_INPUT_X`, a bit the core does not read. The physical **GAME** button
is `ODROID_INPUT_START`, so `press game` is what tests the core's Return
mapping. A timeline that says `press start` exercises nothing, which is how
"START sends Return" stayed believed-working while the device disagreed.
`timelines/press-game.tl` presses TIME (N, to answer Alley Cat's joystick
prompt) and then GAME, and `tests/test_core_retrogo.py` asserts the byte
0x1C reaches the guest through `x86_kbd_read_log`.
`timelines/set-cpu-option.tl` walks the PAUSE/SET menu to the core's own
"CPU Speed" entry, changes it, then Save & Quits and relaunches — so one run
covers "the menu opens", "the change takes effect" and "it survives a restart".
It navigates by pressing UP once in the settings menu: the dialog's selection
wraps and our entry is appended last, so this keeps working if the firmware
grows an option.
`timelines/press-menu-keys.tl` and `timelines/press-game-menu.tl` drive a
guest's own MENU -- DOWN (scancode 0x50, an extended key with no ASCII) and
then GAME (Return) -- with a screenshot between each press. A keyboard chain is
only proven when the guest's PICTURE changes: counters can all look right while
the key goes to a program that ignores it. Capture with
`screenshot.py --timeline ... --wait N --text` at two different `N`.
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
| `opcode_profile.py` | Host side of the per-opcode DWT profile: locate/reset/read the DTCM table, resolve slots to handler symbols off the live dispatch tables, render table/JSON/CSV. Layout and the on-target half live in `x86_profile.h` / `cpu_exec.S` |
| `probe.py` | Sample PC, globals, pointer derefs and array dumps on a running target; `read_faults` decodes CFSR/HFSR/BFAR |
| `watchpoint.py` | Z2/Z3 data watchpoints over the GDB remote protocol, and the unsolicited stop reply that follows a resume |
| `fat12.py` | Builds a 64 KB FAT12 volume in memory with named files and known contents. A test that asserts on a shipped `.dsk` is asserting on someone else's data; this lets `test_dos.py` write the bytes it then expects to read back |
| `dosimage.py` | FAT12 reader + MZ loader that mirrors `x86_dos_exec`, so a reference emulator starts from OUR entry state |
| `refx86.py` | Unicorn run of that image, sampled like the target's own sampler |
| `refdos.py` | A minimal DOS/BIOS for the reference, so it can run PAST the first INT 21h and say what the program should do. Deliberately not modelled on our x86_dos.c -- mirroring it would make the two agree on the same mistake. It has no port I/O, so it cannot follow a program that times against the CGA retrace |
| `textmode.py` | Host-side model of the CGA text renderer: CP437 font load (4x8 by default, the table the renderer uses), the 80x25 -> palette-index render at 4x8 cells, AH=06 scrolling, and a readable CP437 dump. Shared by `test_text.py` (seeded pattern) and `test_screen.py` (the live guest) |
| `verify.py` | Symbol-region checks and ELF-vs-target section comparison |
| `builder.py`, `runner.py` | Tier-1 build convenience |

## Non-negotiables

- Build flags are **not** dependencies — change one, pass `--clean`. **This
  includes `lib/FatFs/ffconf.h`**: editing it and rebuilding incrementally links
  objects compiled against two different configs, and `FF_FS_EXFAT` alone
  changes the width of `LBA_t`. That produced a hang and a mounted-but-empty
  directory, both of which read as "the change broke FatFs" and neither of
  which was real. `rm -f lib/FatFs/*.o build/core/*.o` first.
- **Our FatFs is not the firmware's.** Ours mounts the FAT12 inside a guest
  `.dsk` and is built with LFN and exFAT OFF (a floppy has neither, and it buys
  8 KB of RAM_EMU). The firmware's separate instance reads the `.dsk` off the
  card and keeps LFN. Code that walks a `FILINFO` must use `DOS_SHORT_NAME()`,
  because `altname` does not exist without LFN.
- **Measure `__CORE_BSS_END__` after any change to the core.** The core's TEXT
  lives in RAM_EMU too, so code growth counts. Overshooting is SILENT: the
  guest RAM allocation returns NULL, `core_ready` stays 0 and the emulator
  never starts.
- Green under gwemu ≠ hardware ready; gwemu does not model peripheral setup.
- Never use raw GDB subprocesses or `gnwmanager monitor`; use the backends.
- Never run concurrent builds or tests from one directory.
- All scripts are Python with `argparse`. No `.gdb` scripts.

Detail lives with the code: each module's docstring, plus
[`docs/04`](docs/04_retro_go_and_gwemu.md) (gwemu/retro-go traps) and
[`docs/06`](docs/06_gwemu_determinism.md) (determinism data).

## Anti-aliased text, and the CLUT budget it lives in

`lcd_set_clut(clut, count)` clamps `count` to 32 and then writes a 40%-darkened
twin of entry `i` at `count + i`. Retro-go's overlay reaches those twins by
OR-ing `LCD_DARKEN_BIT = 0x20` — a FIXED +32 — into every framebuffer byte, so
the two only agree when `count` is exactly 32. Indices 64-67 are the overlay's
own theme (`LCD_OVERLAY_CLUT_BASE`), and the firmware cache is 68 entries.

**So the budget is 32 entries, not 64.** 32-63 are not free; they are the twins.
The core published 16, which put the twins at 16-31 while the overlay read
32-47 — zeros — so opening retro-go's menu dimmed the game to solid black. It
publishes 32 now, in every mode.

Anti-aliasing spends that budget as eight four-entry blocks, allocated **per
frame** to the (fg,bg) pairs the 2,000 cells actually use, ranked by cell count;
pairs that miss out degrade to bilevel in the nearest published colour.
Measured with `screenshot.py --pairs`, TOPBENCH's menu uses exactly 8 — a fit
with nothing to spare — and its boot console uses 1.

```bash
make sdcard X86_TEXT_AA=1      # capture-only core: AA on, bilevel table dropped
```

It is **off by default**, and the reason is size, not quality:

> RAM_EMU spans `0x2404B000`-`0x24100000` (741,376 B). The core ends at
> `__CORE_BSS_END__` (measure it: `arm-none-eabi-nm build/core/dos_core.elf`),
> and the guest asks for 32 KB VRAM + 640 KB RAM = 688,128 B from that region.
> **Measured 2026-08-13: BSS end `0x24057078`, headroom 4,016 bytes.** It was
> 424 bytes before the 4 KB open-bus filler was moved to the bonus pool; the
> "2,528 bytes" this document used to quote was two commits of growth out of
> date, which is why the number is now a measurement instruction and not a
> constant.

Each narrow table is 2 KB, so an AA core must drop the bilevel one to pay for
it (`X86_TEXT_BILEVEL_FONT=0`, set automatically) and degrades by thresholding
the 2-bit glyph instead. The bare-metal harness has no such limit and links
both, which is how `test_text.py` checks each renderer against its own font.

**The trap this hid behind:** an over-allocation is quiet. `ram_malloc()` does
bound-check and returns NULL (`Core/Src/gw_malloc.c`) — so nothing is
overwritten — but a NULL that a caller tolerates is a feature silently missing,
not an error. That is exactly what happened to the 4 KB open-bus filler: it was
the LAST allocation, `if (upper_filler)` skipped the mapping, and the symptom
was an option-ROM scan faulting in the guest. It comes from the bonus pool now,
and `upper_filler_ok` reports it. If a change to the core makes a previously
working guest fault early, check `__CORE_BSS_END__` before anything else.
