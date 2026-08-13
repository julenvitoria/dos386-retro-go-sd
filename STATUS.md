# Status

Last verified 2026-08-13. How to run: [`TOOLING.md`](TOOLING.md). Roadmap: [`PLAN.md`](PLAN.md).

## Verification state

| Tier | State |
|---|---|
<<<<<<< HEAD
| 1. gwemu bare-metal | 268/268 `test_opcodes.py`, plus `test_video`/`test_text`/`test_dos`/`test_keyboard`/`test_screen`; `test_audio` runs natively, no target |
=======
| 1. gwemu bare-metal | 268/268 `test_opcodes.py`, plus `test_video`/`test_text`/`test_dos`/`test_keyboard`/`test_osk`/`test_screen` |
>>>>>>> worktree-agent-af6ed0df80835aae3
| 2. gwemu + retro-go | `core_dtcm_ok=1`, bonus pool 153,600 B, 1,000,001 insns — matches tier 1 |
| 3. Hardware | 50.50 cyc/insn, 5.54 MIPS |

Cycles/insn: 57.50 pre-fold → 50.00 fold + absolute dispatch → 50.50 with
segmentation, unchanged through the 66/67 axes, segment loads and REP.

## Phase 0/1 — baseline and CPU foundations

Complete except below.

- [ ] `XLAT` (no segment base), `WAIT`, `INS`/`OUTS`, `BOUND`/`ARPL`
- [ ] Five handlers call `DECODE_MODRM` by symbol, so they ignore a `0x67` prefix
- [ ] Limit checks off the R6 descriptor; lazy PF/AF; software TLB above 16 MB
- [ ] Size-aware `MEM_R*`/`MEM_W*` macros + CI grep banning raw `[R9,`
- [ ] Assertions in `test_game.py`/`test_benchmark.py`; realign `docs/01`–`03`, `05`


## Phase 2 — Alley Cat

17 opcode slots still reach `.exit_loop`: x87 (`D8`-`DF`), `BOUND`/`ARPL`,
`INS`/`OUTS`, `XLAT`, `WAIT`, `INT1`.

Done: real-mode interrupts and `IRET`; the `0F 0B` hypercall and its HLE service
table; BIOS INT 10h/16h/1Ah over a real BDA; CGA text mode 3 and a mode 4 LUT8
blitter, both pixel-exact; CGA registers with a moving retrace bit; a 8042
keyboard with IRQ 1; PC speaker on PIT ch2; port I/O; the guest page pool and
fold mapping; the frame-paced loop; and the DOS layer.

- [ ] DWT per-opcode profile: tool done (`profile_opcodes.py`, `X86_PROFILE=1`),
      clean under gwemu, silicon pending; gate NOR backing on code-vs-data

## On-screen keyboard — shipped; mouse UI — shell only

`x86_osk.c`, wired from `poll_input()`/`core_repaint()` in `src/main.c`. The UI
is a port of the author's previous DOS port; the injection under it is ours —
XT make/break codes through `x86_kbd_scancode()` plus IRQ 1. **GAME cycles
OFF -> keyboard -> mouse -> OFF**, so `pad_scancode[6]` is deliberately 0 and
GAME no longer sends Return (the physical START button does).

Measured under gwemu: `tests/test_osk.py` 12/12 (including a shifted cell
reaching the BDA ring as `!`), tier 2's `check_game_opens_osk` (A puts 0x1E on
port 0x60 inside the guest), no fault with retro-go's PAUSE menu over it, and
screenshots over both a text-mode and a graphics-mode screen. Cost 3,331 B
text; RAM_EMU headroom after it 4,960 B.

**Mouse mode is a UI shell with nothing listening: there is no INT 33h driver.**
What one would take, and why half-building it would be worse than none, is in
[`docs/input/05-osk-port.md`](docs/input/05-osk-port.md) with the rest of the
detail.

## String ops: the batched fast path, and what it measured

The silicon profile of TOPBENCH said string ops **plus their prefixes were ~59%
of all cycles**, and `F3` alone was **17% purely as re-dispatch overhead** — 7 M
dispatches doing no guest work. That is the price of the rewind design: one
element per pass through fetch/decode/dispatch, twice (prefix + string opcode).

`.str_exit` now re-enters the string handler **directly** for the next element
(`rep_handler`, resolved off the live dispatch table by the prefix) instead of
rewinding EIP. Semantics are untouched — the handler still re-walks the fold per
element, so granule straddles, DF=1, 16/32-bit index wrap and the ZF early exit
all behave exactly as on the slow path, which is why CMPS/SCAS batch as safely
as MOVS/STOS/LODS. The batch is bounded by `rep_batch` (256, reloaded by the
prefix) and by `pit_counter`, which IS the slice budget: a batched element
decrements it exactly as a loop trip would, so `guest_kips` pacing and
`guest_insn_debt` stay honest, and a raised IRQ — which the host signals by
dropping `pit_counter` to 1 — still lands on the same element boundary.

Measured under gwemu, TOPBENCH, `--settle 0 --window 8` (the whole guest life,
so both runs execute the identical instruction stream; control opcodes agree to
a few %). Only the COUNTS mean anything under gwemu:

| | F3 | F2 | A4 | A5 | A6 | AE | AB | AD | string+prefix | all dispatches |
|---|---|---|---|---|---|---|---|---|---|---|
| before | 343,238 | 40,980 | 177,391 | 116,043 | 41,276 | 41,300 | 25,080 | 37,108 | 847,492 | 2,534,356 |
| after | 23,075 | 269 | 55,852 | 1,860 | 802 | 803 | 2,768 | 17,751 | 130,235 | 1,968,543 |
| | **14.9x** | 152x | 3.2x | 62x | 51x | 51x | 9.1x | 2.1x | **6.5x** | **−22%** |

`AC` LODSB is unchanged (23.0 k → 23.9 k): TOPBENCH runs it unprefixed, so
there is nothing to batch — the fast path is not silently skipping it.
`asm_metrics`: `.str_exit` 17 → 37 (+20), `REP_ONE_16/32` +4 each; no string
handler changed. Silicon numbers not yet taken.
## The PC speaker, and the four things that made it sound tinny

The device report was "the beep is lower quality than the old DOS port". The
generator from that port (`dos_audio_wave.h`) is **vendored** here as
`x86_audio_wave.h` — `<stdint.h>` only, which is what makes it host-testable —
and `tests/host_audio.c` measures both implementations side by side. All four
defects are measurements, not opinions:

1. **Pitch.** The old path computed an integer `1193182 / divisor` and then a
   16-bit step from it. Two floors compound and the error grows as the tone
   falls: **middle C came out 4.7 cents flat, C3 12.9 and C2 13.5**. The
   32-bit accumulator takes the divisor directly — no integer frequency exists
   in the signal path — and the worst case over the same divisors is
   **0.23 cents**.
2. **No Nyquist guard.** At 16 kHz every divisor below 149 was above Nyquist
   and folded down as a loud, tuneless alias; divisor 10 (119 kHz) came out as
   a **7,318 Hz** tone. Those are silence now.
3. **The half-buffer was four frames long.** `GW_AUDIO_GET_LENGTH` only reports
   what `START` last set, so asking first and starting with the answer left the
   firmware default of 1077 samples — 67 ms at 16 kHz, refilled every ~17 ms.
   The writer lapped the DMA reader three times over and a correct tone reached
   the SAI as a stitch of restarts. Now 48 kHz with an 800-sample half-buffer:
   one panel period exactly, inside the 1077 ceiling.
4. **48 kHz, and the volume knob.** A speaker square is all edge; at 16 kHz
   every edge quantised to one of ~48 samples per period. Amplitude follows
   `common_emu_sound_get_volume()` (0..255, `>> 11`) instead of a fixed 6000.

Kept: the timestamped-event renderer and the 8253 latch fixes below. The ring
carries the **divisor** now, not an integer Hz, and the anti-click phase reset
fires on the rising edge the events already locate.

Measured under gwemu, Alley Cat: 26 tone changes, **0 dropped**, 59 fps, 3.0 M
insn/s. `x86_spk_log` shows the real sequence — `43h=B6` (ch2, lo/hi, mode 3),
`42h=FB`, `42h=11`, `61h=03` — i.e. divisor **0x11FB = 4603 = 259.2 Hz**.
gwemu cannot judge audio quality; the listening test is the user's.

## Phase 3 — Wolf3D

- [ ] COW/pool/eviction with dirty tracking, loud exhaustion; pool sizing
- [ ] Gate: measure DOS FAT/sector overhead → decide the INT 21h fast path
- [ ] Protected mode: descriptors, far transfers, IRET privilege change, paging
- [ ] Profile-directed tuning against a Wolf3D DWT profile
## TOPBENCH — running, scoring, screenshotted

The 4 px cell is an **Everex Micro Enhancer 5x8** face squeezed to 4, not a
condensation of the 8x8 one, with 0xB0-0xDF hand-drawn on a fixed grid (double
verticals alias to single; double horizontals do not). Three captures of the
same menu are in `build/artifacts/`: `font_a_current.png` (the old
OR-condensation), `font_b_everex.png` (shipping) and
`font_c_antialiased.png`. Text renders through a row-expansion blit now — one
32-bit store per glyph row instead of four byte writes.

`TOPBENCH.EXE -l` scores **236** (`build/artifacts/topbench.png`); without `-l`
it draws its full-screen menu (`topbench_ui.png`). Both with **no pan** — the
4 px cell puts all 80 columns on the panel. Six defects were in the way: a
128-glyph ASCII font; 4 of 16 CLUT entries reaching the panel; INT 21h AH=3Ch
and a read-only floppy (`Runtime error 001`); and INT 10h AH=12h/1Ah unanswered,
which left TOPBENCH's detection unit reading through a pointer it never set.

`trace_diff.py` agrees with a reference 8086 (Unicorn) through startup and the
unpacker — 1,532,786 instructions to the first INT 21h. Sixteen defect classes
closed, one commit each; one shape accounts for four — reading a register
operand BEFORE `DECODE_MODRM`, which uses R4/R5 as scratch. `refdos.py` still
has no port I/O, so the oracle stops at the first CGA-retrace timing loop.

## TOPBENCH's INI, and the INT 21h calls that were not there

The device report was "the menu runs but it cannot read its files". **Measured**,
with a new file-only trace ring (`x86_dos_file_log`, because `x86_hle_log` is
100% 0x1601 keyboard polls between one file call and the next): TOPBENCH does
`3Ch`/`40h`/`3Eh` on its own `topbench.log`, then **`43h AL=00` on
`database.ini`, which the default arm answered CF=1, AX=0x0001 "invalid
function"**. It never issued the `3Dh` that would have opened the file. The open
and read paths were never reached, let alone broken -- an unimplemented call in
the MIDDLE of the sequence is indistinguishable, to the guest, from a missing
file.

Implemented: `43h` get/set attributes (`f_stat`; SET is accepted and changes
nothing, FatFs is `FF_USE_CHMOD=0`), `4Eh`/`4Fh` FindFirst/FindNext with our own
DOS 8.3 wildcard matcher over `f_opendir`/`f_readdir` (`FF_USE_FIND=0`) and a
serial stamped into the DTA, `41h` delete, `56h` rename, `19h`/`0Eh`/`47h`. Two
more defects the tier-1 test caught on its own: **a drive-letter prefix was
fatal** -- FatFs is `FF_STR_VOLUME_ID=0`, so `"A:TEST.INI"` returns
FR_INVALID_DRIVE and every program that names its own drive got "file not
found"; and `2Fh` get-DTA returned the bottom NIBBLE of the linear address with
no segment, so a program that saved and restored its DTA pointed into the IVT.

Tier 1: `tests/test_dos.py` now drives all of it from GUEST code against a
64 KB FAT12 volume the test builds (`scripts/common/fat12.py`), so the expected
bytes are the test's own -- open, a short read at EOF (partial count, CF CLEAR),
the zero read after it, seek from start and from end, close, the not-found path
(CF set, AX=02), attributes, FindFirst/FindNext through the DTA, and both
`A:TEST.INI` and lower-case `test.ini`. It fails on the pre-fix core on exactly
three of those and passes after.

Tier 2, TOPBENCH under gwemu: the file log now reads `43h` OK, `3Dh` -> handle
5 on `database.ini`, thirteen `3Fh` reads of 0x2400 bytes each (the file is
124,054 B) and `3Eh`; `x86_dos_last_error` is 0. The screen says it too --
before, rows 0-2 were empty backdrop; after, they are database records read out
of the INI:

```
   0 |....Celeron.Tualatin.clone....
   1 |....Athlon.4400+.@.2.2GHz.clone
   2 |....Toshiba.Portege.7140CT....
```

## The .dsk sector cache, and what the stutter actually was

A device report said TOPBENCH's database listing loads but STUTTERS as entries
appear (124 KB of `database.ini` in 13 reads of 0x2400). The suggested fix was
"a sector buffer in AHB". **Measured first, and the measurement changed the
design.**

What one guest read costs, end to end, measured under gwemu (tier 2) and read
out of the firmware source:

1. `x86_disk_run_hist` is **115 in slot 0 and zero everywhere else** — every
   `disk_read()` is for exactly ONE sector. FatFs never asks for a run, because
   INT 21h AH=3Fh serves the guest through a 512-byte bounce buffer, so
   `f_read` is handed one sector at a time and its multi-sector direct path is
   never taken. `FF_FS_TINY 1` is not the cause; the bounce buffer is.
2. Each of those becomes `rg_storage_copy_file_range_to_ram`, which is stdio
   and **re-opens the .dsk every call**: `fopen`→`f_open` (a directory walk),
   `fseek`→`f_lseek` which with `FF_USE_FASTSEEK 0` **walks the cluster chain
   from byte zero reading FAT sectors**, one `f_read`, `fclose`.

So the cost is **per-CALL, not per-byte** — and that is what picks the
mechanism. A plain LRU cache would fix the FAT/directory re-reads and do
nothing for a 124 KB stream; plain read-ahead would fix the stream and thrash
on the metadata. What shipped is both: **4 windows x 16 KB, LRU-replaced**, out
of the LCD bonus pool. The window is the read-ahead; the set is the cache.

Measured, same guest, same timeline, gwemu tier 2 (counts are exact there):

| | disk_read calls | backend calls | sectors fetched | hits/misses |
|---|---|---|---|---|
| before | 115 | 115 | 115 | — |
| after | 115 | **7** | 224 | 108 / 7 |

**16.4x fewer SD round trips, 93.9% hit rate**, `x86_disk_errors=0`,
`x86_disk_cache_ok=1`. 59 fps, `frame_hist` all in bucket 0, `frame_overruns=0`.
`tests/test_disk_cache.py` streams 248 single-sector reads into **9** backend
calls (28x) and 3,000 random reads into 2,901 (vs 3,000 uncached).

**gwemu cannot show the stutter and this does not claim to have fixed it.**
`docs/04` is explicit that SD busy state is not modelled, so the SD is
effectively free there; the call/sector/hit COUNTS above are exact and the
TIMING win is unproven until someone reads `x86_disk_us_total`,
`x86_disk_us_worst` and `frame_hist` off silicon.

Two things the test found that reasoning had not:

- **The cache could be WORSE than no cache.** A multi-sector request that
  straddles a window boundary with neither half resident costs two fills where
  a direct read costs one call — the random-access arm measured 3,024 backend
  calls against 3,000 uncached. That case now goes straight through, so "never
  more backend calls than the uncached path" is an invariant, not a hope.
- **RAM_EMU was oversubscribed by 920 bytes** and the only symptom was
  `core_ready=0`, `frames_rendered=0` — `guest_ram` silently failed to allocate
  and the emulator never started. Baseline headroom was **480 bytes**, not the
  4,016 this file used to quote. Dropping LFN and exFAT from OUR FatFs (it
  mounts FAT12 floppies; the firmware's separate instance keeps LFN) reclaimed
  **8,296 bytes**, so headroom is now **7,680 B with the cache in**.

Not built, and the honest reason: **asynchronous completion across frames** is
the deeper fix and it is **not reachable from the core**. The ABI's storage
entries are all synchronous and blocking, the SPI1 driver uses the polled
`HAL_SPI_TransmitReceive`, and no DMA stream is wired to SPI1. See
`docs/03` and the report in this commit's message for the exact ABI additions
it would take.

## Guest timing — three clocks, measured

The loop used to derive everything from one constant (`PIT_SLICE_INSNS`), and
after the merge it carried **two** BIOS-tick drivers at once: one per rendered
frame (182/600, exact only at 60 fps — at the device's measured 10 fps it fired
at ~3 Hz) and one per instruction slice (`TICKS_PER_SLICE = 10`, ~100 Hz and
moving with our throughput). That is Alley Cat's "pitch right, timing horrible".

Now separated, none of them a hardcoded 60:

1. **Display** — the panel's own swap signal; the period is *learned*
   (`panel_ms_min`), so an overclock needs no constant changed.
2. **Guest wall clock** — the firmware's ms counter. 18.2065 Hz exactly, carry
   in fixed point, catch-up capped at 4 ticks with the excess counted in
   `guest_ticks_dropped`.
3. **Guest CPU rate** — `guest_kips`, paced against wall time. `0` is
   unthrottled; anything else runs at that speed *and no faster*, with the
   shortfall counted in `guest_insn_deficit`.

Measured under gwemu (`probe.py`, tier 2): tick 18.24 Hz **identical** at 60 fps
and at a forced 19 fps, and `0040:006C` tracks it exactly. Paced at 1200 kips
(286-12): 1,200,000 insn/s, 60 fps, deficit 0. Unthrottled: ~10 M insn/s at
60 fps. Asked for 100,000 kips: still 60 fps, deficit climbing ~90 M/s — loud,
not silent. TOPBENCH scores 46 at 1200 kips (was 236 when a "CGA frame" was
200,000 instructions, i.e. a machine 6x faster than we are); no divide-by-zero.

Also fixed here: HLT no longer fabricates an INT 08h on demand (a guest that
HLTs in a wait loop got a timer tick every time it asked); the PC speaker is
rendered from timestamped *events* rather than point-sampled once a frame, so
a beep shorter than one frame is heard; the 8253 lo-only/hi-only writes keep
the other byte; a counter-latch command is no longer stored as an access mode;
and `AUTOEXEC.BAT`'s command tail is no longer dropped (`TOPBENCH.EXE -l`).

`guest_kips` is a single global, and its default is now **KIPS_MAX** —
unthrottled, "as fast as the silicon delivers". The deficit has no meaning with
no target, so it is held at zero in that mode and `guest_ips_achieved` is the
number to read. Per-title selection via `roms/dos/*.cfg` is **not** wired: those
files belong to another project's core and nothing here parses them.

## The CPU is a menu entry, and fps/busy are no longer zero

**CPU Speed** is a Retro-Go game-options entry now (Max / 8088 4.77 / 286 8 /
286 12 / 386SX 16 / 386DX 33), passed where we used to pass `NULL`. The key it
persists under is **"Palette"**, not a name of our own: the firmware's
`odroid_settings_app_int32_get/set` match against a fixed whitelist and ignore
anything else *silently*, and the slot is per-app so the name is ours alone.
`cpu_profile_persisted` reads the value straight back so the round trip is
visible without a reboot.

Measured under gwemu, `timelines/set-cpu-option.tl`: menu opens, our callback
fires (`cpu_profile_events`), RIGHT sets `guest_kips` 0 → 330 and the achieved
rate goes 12.0 M → **330,000 insn/s exactly**, CFSR/HFSR/BFAR clean. Save & Quit
back to the launcher and relaunch: the core comes up at **330** with no menu
touched — persistence across a restart, measured, not argued.

`common_emu_frame_loop()` is now called once per iteration, which is the only
thing that feeds `odroid_system_tick()` — never calling it is why the overlay
read `0.0 (0.0) fps` and `busy: 0.0%`. **Pacing is not shared**: the panel's
swap signal (clock 1) still owns it and the frame loop DEFERS — its draw/skip
verdict is ignored and it is fed our measured mean period so its integrator
agrees with the pacer that is really running. Feeding it `panel_ms_min` instead
(the *shortest* iteration) made it call 768 of 1032 drawn frames "skipped".
`cpumon_sleep()` in the swap wait is what makes `busy_ms` accumulate.

Measured: firmware `counters` = 1024 frames, **0** skipped, 8198 ms busy over
17.4 s → **58.7 fps, 47.0% busy**, cross-checked against an independent
`frames_rendered` rate of 59.0 fps. The 18.2 Hz tick survives both ends: 18.18 Hz
at 59 fps, and 18.2 Hz at ~10 fps (forced with `--set max_slice=4000000`) once
the ticks the catch-up cap deliberately dropped are counted.

The 4 KB open-bus filler for 0xA0000-0xFFFFF now comes from the **bonus pool**,
not RAM_EMU. It was the last allocation in a region with a few hundred bytes
spare, so any growth in the core silently unmapped upper memory;
`upper_filler_ok` says whether it is backed at all.

## The BIOS keyboard path, measured end to end

A device report said keys reached the guest but INT 16h never returned them,
with TOPBENCH's menu unresponsive. **Measured under gwemu, it does not
reproduce, and the reported evidence does not support the diagnosis.**

Tier 1, `tests/test_keyboard.py` -- guest code executing INT 16h, not host C
calls: head/tail initialise to 0x1E; the push writes AT the offset the tail
points to and wraps at 0x3E; AH=01 returns 0x1C0D without dequeueing and puts
ZF on the IRET FRAME with the right sense in both directions; AH=00 dequeues
the same word; the pop wraps. Five mutations (head/tail zeroed, ZF inverted,
tail treated as an index, no wrap, destructive peek) each make it fail.

Tier 2, TOPBENCH's own menu under gwemu: `x86_kb_pushes == x86_kb_pops ==
x86_kb_peek_hits` on every key -- queued, seen and taken -- while
`x86_bios_int16_calls` climbs ~23,000/s, which is simply what TOPBENCH's menu
loop does and is NOT evidence of a stall. `x86_hle_log` says the poll is
**AH=01**, not AH=00. And the picture moves: two DOWN presses walk the
selection from Introduction to Database Operations (the help line at row 24
changes with it) and GAME dismisses the Introduction dialog
(`timelines/press-menu-keys.tl`, `timelines/press-game-menu.tl`).

What the device evidence actually shows is that INT 09h consumed every
scancode. It says nothing about what INT 16h did, because the device had no
counter for it -- `x86_kb_pops` is that counter now, and one probe run on
silicon settles it. The remaining silicon-only candidate is that the BDA page
is not writable there: every `bda_w*` does `if (p) ... ;` and drops the write
in silence, which produces exactly the reported symptom and is invisible under
gwemu, where the whole low megabyte is plain RAM.

## Cold guest RAM in NOR — shipped read-only-first, and what it measured

**Cold RAM->NOR** is a Retro-Go game-options entry next to CPU Speed. Pressing
A freezes the guest, snapshots all 640 KB of guest RAM into memory-mapped NOR,
points every page at it and frees the AXI; pages the guest writes pull
themselves back into AXI by copy-on-write. No selection heuristic, no sampler,
no pagefile, no eviction — demand-driven, which is what makes the fraction of
genuinely read-only memory a MEASUREMENT rather than a guess.

Two facts about the firmware decided the shape, both read out of its source:

1. **There is exactly one flash-write door**,
   `odroid_overlay_cache_file_in_flash_relocate()` — the ROM cache. It takes a
   PATH. No buffer-to-flash call, no `OSPI_Erase`/`OSPI_Program`, no way to
   query flash size or invalidate the cache. So the snapshot goes in through
   the RELOCATE CALLBACK, which gets each 16 KB chunk in RAM before it is
   programmed and may modify it: the SD file only has to be the right SIZE, and
   guest memory is never dumped to the card.
2. **A cache HIT programs nothing and never calls the callback**, while still
   returning a valid-looking address into an older snapshot. Three independent
   guards: a fresh filename per snapshot, the callback's coverage is counted
   (`nor_snap_cb_bytes` must be 655,360), and `x86_nor_attach()` reads the
   flash back and byte-compares it against guest RAM before it moves a single
   fold entry. A half-applied remap is impossible by ordering, not by care.

**gwemu models OCTOSPI erase and page-program, and its XIP window shares the
backing store** (`hw/misc/gnw_h7b0_ospi.c`, `hw/arm/gnw_h7b0_soc.c:491`). So
the FULL chain is exercised at tier 2, not just the read side — this is a
stronger gwemu result than `CLAUDE.md`'s "extflash is plain memory there"
suggests, and that bullet is now too pessimistic about this one peripheral.

Measured, Alley Cat, tier 2, `timelines/nor-snapshot.tl` (counts exact):

| | before snapshot | after |
|---|---|---|
| guest RAM in AXI | 655,360 B | **4,096 B** |
| pages NOR / AXI | 0 / 160 | **159 / 1** |
| guest rate | 2.98 M insn/s | **0.36–0.52 M insn/s** |
| fps | 60 | 60 |

**99.4% of guest RAM was reclaimed and the guest kept running** —
`x86_nor_attach_err=0`, `x86_nor_cow_failures=0`, `x86_nor_unmapped=0`,
`x86_disk_errors=0`. The snapshot itself took 1,614 ms end to end.

And the price, which is the other half of the answer: **~6–8x slower**, with
~1.0 M slow-path reads per second. Every INSTRUCTION FETCH from a NOR-backed
code page traps, and demand-driven means code pages never leave NOR. That is
the accepted trade stated plainly rather than the sentinel being weakened — but
it is why v2 wants code pages kept in AXI, which the DWT per-opcode profile was
always going to gate.

Caveats that are not negotiable: **the 99.4% is an IDLE title.** Alley Cat sat
at its skill menu polling INT 16h, so `x86_nor_writes` was 0 and the one COW
came from the HOST (the BDA tick). A playing guest will write far more. And
**erase/program latency, XIP read latency versus AXI, and the real chip's erase
granularity are silicon-only** — gwemu's flash is free.

One defect the counters found on their own: `x86_nor_pages_axi = 1` after 25 s
was implausibly small, and the reason was that **every host-side write bypassed
the trap**. `x86_guest_ptr()` resolves the fold itself, so the BDA tick, INT
21h filling a DTA and a disk read landing in a guest buffer were all storing
straight at the XIP window. gwemu makes that look like it works, because its
model backs the region with ordinary memory; on silicon the store is dropped
and the guest diverges silently. `x86_guest_ptr()` now COWs first.

Cost: ITCM 36,416 → 48,208 B of 65,536 (~360 trap stubs — handlers are emitted
twice). **RAM_EMU headroom 7,680 → 4,576 B** (`__CORE_BSS_END__` 0x24057078 →
0x24057c98). That is the number to watch before anything else lands.

## Open defects

- **A segment override in front of `REP` survives only the first element.**
  `rep_resume` is the address of the F2/F3 prefix, not of the whole prefix run,
  so `26 F3 A4` (`ES: REP MOVSB`) rewinds to the `F3` and every element after
  the first reads from DS. Pre-existing and independent of the batching, which
  keeps the override for the elements it runs in-handler (R6 is not reset there)
  and so is if anything closer to the hardware. The fix is to record the START
  of the prefix run, not the repeat prefix.
- **TOPBENCH halts ~3 s in on `INSB` (`6C`)** — `exit_reason=4`,
  `exit_info=0x6C`, EIP 21695, identical before and after the string work.
  `INS`/`OUTS` are on the Phase 0/1 list above. It is why a TOPBENCH profile
  window has to start at `--settle 0`: past ~3 s there is no guest running and
  the table comes back empty.

- **RAM_EMU headroom is 7,680 bytes (measured 2026-08-13, after the FatFs
  reclaim), and running out is QUIET.** It was **480** before that reclaim, and
  adding the sector cache took it to **-920**, at which point `guest_ram`
  returned NULL, `core_ready` stayed 0 and the emulator simply never started --
  no fault, no message. Measure it (`arm-none-eabi-nm build/core/dos_core.elf |
  grep __CORE_BSS_END__`) before and after any change to the core; the core's
  TEXT is in this region too, so CODE growth counts, not just data. The core ends at `__CORE_BSS_END__ = 0x24057078` in a region ending
  at `0x24100000` — 692,144 B against the 688,128 B the guest asks for (32 KB
  VRAM + 640 KB RAM). `ram_malloc()` bound-checks and returns NULL rather than
  overrunning, so nothing is corrupted, but a caller that tolerates NULL loses a
  feature in silence — which is what happened to the 4 KB open-bus filler, now
  moved to the bonus pool and reported by `upper_filler_ok`. Half the fix
  `TOOLING.md` asked for; the guest POOL itself is still RAM_EMU-only.
- Anti-aliased text is measured VIABLE on palette — TOPBENCH's menu uses exactly
  8 distinct (fg,bg) pairs and 32 CLUT entries buy exactly 8 four-entry blocks
  (`screenshot.py --pairs`) — and blocked only by the above. It is a build-time
  option (`make sdcard X86_TEXT_AA=1`), fits with 40 bytes to spare, and only by
  dropping the bilevel font table to pay for its own.
- The text CLUT published 16 entries, so retro-go's overlay (which ORs a fixed
  +32) found zeros and dimmed the screen to black. Fixed: 32 in every mode.
- `moffs` (`A0`–`A3`) takes its offset at the operand size, not the ADDRESS size.
- `MOV SS,x` does not inhibit interrupts, so one can land on a half-switched stack.
- Protected-mode segment loads trap out (`CR0.PE` set) — no descriptor walk yet.
- `tlb_*` arrays: 2 KB of DTCM declared, unreferenced.
- Text framing, FIXED: an 8x8 cell fits only 40 of 80 columns, so a UI centred
  on the 80 (TOPBENCH's menu is columns 25-52) was sliced at column 40, and no
  40-column window could contain a 28-column box plus its context. 4 px now;
  80 x 4 = 320 exactly. `x86_video_pan` survives as a no-op.
- **GAME does reach the guest as Return — measured.** `input_pad_seen` bit 5,
  0x1C/0x9C queued, IRQ 1 raised, and the guest read `0x1C` off port 0x60
  (`x86_kbd_read_log`). The chain is not the defect. What is: the *test* pressed
  gwemu's `start`, which is `B_START`/`ODROID_INPUT_X` and not the GAME button
  at all; and Alley Cat's skill-level menu wants K/H/T/A, which **no button can
  send** — so Return there is correctly delivered and correctly ignored. The
  eight-button pad cannot reach a letter menu; that needs the on-screen
  keyboard, not a mapping change.
- `tests/test_core_retrogo.py` fails its blitter/CLUT assertions whenever a real
  `.dsk` is on the card: they expect `demo_guest`'s pattern, and the demo only
  runs when no disk mounts. Pre-existing; the assertions need to be
  guest-aware.
- gwemu's headless **screendump** produces nothing (Retro-Go's own menu comes
  back black too), but its **LTDC registers are modelled**, so `test_screen.py`
  rebuilds the scanned-out image. A black timeline screenshot is not a bug.
- No hardware probe this session, so `50.50 cyc/insn` predates the interrupt,
  speaker and video work.
