# Per-title state, and the verdicts that should not be re-derived

What each title does today, what stops it, and — where the answer is "never" —
the arithmetic, so nobody spends a day rediscovering it.

Measure with `scripts/probe_report.py --title <name>`, which leads with what we
did **not** answer. Judge pictures with `scripts/screenshot.py`; judge a text
screen's *shading* with `--attr`, never `--text`.

## The non-regression baseline

Three titles, three renderers. These pictures and pixel counts are the
non-regression test for anything touching the launch path, the DOS layer or
video:

| title | renderer | non-zero px | what it shows |
|---|---|---:|---|
| WOLF3D | Mode Y | 57,874 / 76,800 | title screen: logo, two guards, id credit |
| ALLEYCAT | CGA mode 4 | 64,000 | joystick prompt |
| KEEN4 | EGA 0Dh | 60,463 | MAIN MENU console |

**An instruction counter is not a non-regression test.** A DOS-layer change once
executed 64.7 M instructions "and still running" while printing rubbish, and was
merged on that basis.

## Wolfenstein 3-D — runs, with two live rendering defects

Reaches gameplay. Mode Y arms, the page stride is **learned as 16,640** from the
guest's own display-start writes (the id Galaxy engine's `SCREENSIZE`), and the
signon, title, Options and difficulty screens render correctly.

Live defects, both measured on hardware:

1. **Cached pictures are noise.** The episode-select screen has perfect text and
   six thumbnails of garbage. Wolf3D caches pictures in off-screen VRAM and
   blits them with latch copies; text goes straight to the visible page, which
   is why they differ.

   | counter | value |
   |---|---:|
   | `x86_vga_modey_cache_offs` | 8,192 (claim succeeded, `_lost_nocache=0`) |
   | `x86_vga_modey_lost_cache` | **16,896** stores discarded above `0xE300` |
   | `x86_vga_latch_miss` | **4,928** latch loads reading an absent cell |

   The cache covers `0xC300`-`0xE2FF`; Wolf3D's writes reach `0xEBFF`, needing
   ~10,496 offsets. ~12 KB of arena is free.

2. **Mid-draw frames.** `x86_modey_mid_draw` climbs into the thousands during
   gameplay — the panel showing a half-drawn page. Four presentation bugs have
   been fixed already; see [`08_vga_mode13h.md`](08_vga_mode13h.md) §4.

**Performance:** ~1.4 M ips in Mode Y against ~3.0 M in text, PC samples in
`x86_vga_planar_store`. The planar store path, not the interpreter, is the limit.

## Commander Keen 4 — banner and menus render; levels need more card

Mode 0Dh is **adopted**, not refused, and the arena claim succeeds. The startup
banner detects correctly — `--attr` at 2 s shows the bright runs are **values**:
`√CGA`, `√EGA/VGA`, `√Keyboard`, `√PC Speaker`, and the memory box reads
`√Standard 390 Kbytes` where the EXE ships `xxxxx` placeholders.

Keen brightens what it detected; the characters are identical either way, so
**`--text` is structurally blind to the result** and reading it cost a session
once.

Past the banner it drew black, and the geometry says why: Keen programs CRTC
Offset `0x20` (64 B/row, a 512-pixel virtual row), so a 200-row window spans
**12,800 plane offsets against 8,192 fitted**. 60.2% of its stores were
discarded and the plane dump was all zeros — an honest black frame, not a blit
defect. Growing the card from the arena moved this; the remaining gap is below.

### The WEDGE: measured on silicon, and it is NOT the pagefile

Reproducible on HARDWARE every run. Both `guest_insns_total` **and**
`frames_rendered` freeze together, WWDG fires (`boot_magic=0xbad00005`), the
device reboots into KEEN4 and does it again. The freeze point VARIES between
runs -- 14.9 M insns/347 frames, 19.9 M/352, 31.4 M/640 -- so the reported
"34.7 M over 944 frames" is one sample of a spread, not a landmark. **Under
gwemu the same core reaches the MAIN MENU and runs indefinitely** (153 M insns,
2,081 frames, screenshots in `build/artifacts/keen4*/`), so nothing below tier 3
can see it.

**The pagefile-thrash hypothesis is DISPROVEN, on silicon.** The tidy story was:
0Dh donates 60 of 160 frames, Keen's 134-frame working set overflows the
remaining 100, and a storm of 4 KB pagefile writes at up to 433 ms each stalls
the frame loop past the watchdog. It is wrong. With `X86_PG_DONATE_RESERVE`
raised to 134 the donation is refused outright and the pagefile is never
touched -- `x86_pg_donated=0`, `x86_pg_frames_total=160`, `x86_pg_evictions=0`,
`x86_pg_wb_file=0`, `x86_pg_wb_us_worst=0`, measured on the device -- **and it
still wedges.**

> `x86_pg_exhausted == 0` is consistent with thrash AND with a completely
> unrelated hang. Only the eviction and writeback counters separate them, and
> they say: no I/O anywhere near the freeze.

That both counters freeze together says the FRAME LOOP stopped, not the guest --
so this is host-side, and `MAX_SLICE_CEIL` (which bounds a slice in
instructions) cannot be the guard. **This paragraph was right and the section
below now says why.**

**THE `ega_shadow_rebuild()` LEAD IS DEAD AS A CAUSE.** It was measured with a
counter and a DWT bracket (`X86_EGA_SHPROF=1`, `x86_video_planar.c`), on
silicon, over five KEEN4 runs, and the numbers refuse it:

| | measured, hardware |
|---|---:|
| one rebuild | **846,000 cycles = 3.0 ms** at 280 MHz |
| rebuilds in a whole pre-wedge run | **10** (~0.15/s), not "continuous" |
| burst coincident with the wedge | 76-86, ~230 ms of cycles |
| the wedging stall | **seconds** -- the WWDG window is ~472 ms |

So the rebuild is extravagant and it is a MARKER of the wedging code path (the
burst appears in 3 of 3 wedging runs and in neither non-wedging run), but it is
a passenger. Proven by fixing it: the precise read-state guard below cuts
steady-state rebuilds 10 -> 2 and the burst is UNCHANGED at 76-79, because
those are genuine read-state changes -- **and KEEN4 wedges exactly as before,
twice in a 60 s run, at 31.5 M/638 and 22.5 M/468.**

### ANSWERED, AND FIXED: one FRAME outlives a watchdog window that covers the whole frame

**There is no runaway loop, in the guest or in the host.** The
"one SLICE never returns / the guest is still executing" reading (from the
rebuild counter appearing to advance across the freeze) is RETRACTED. It was
built on a counter sampled either side of the reset the wedge itself causes.
The direct measurement refuses it: `cpu_state.pit_counter` is stored **once per
guest instruction** and is readable over the probe while the target runs, and
across the freeze it is **FROZEN** -- 2066, unchanged over four samples spanning
1.5 s, with `pit_reload` 5304. The guest is not executing at all.

`probe.py --deref cpu_state_ptr+2184=PITCNT` is the whole instrument. It costs
no build, no RAM_EMU and no flag; reach for it before adding a counter.

**What is actually happening, measured on silicon:**

1. A frame's GUEST stage takes longer than the ~472 ms WWDG window. The
   watchdog was refreshed **exactly once per iteration, at the bottom of the
   body**, so the window covered a WHOLE frame and any single overlong stage
   reset the device.
2. The reset is `BSOD_WATCHDOG` = **5**, and `0xbad00005` is
   `BOOT_MAGIC_BSOD | 5` -- not a code to decode further.
3. After it, retro-go's `BSOD()` sits in `while (buttons_get() == 0 || ...)`
   waiting for a keypress, then `HAL_NVIC_SystemReset()`. **Every PC sample
   taken during "the wedge" is that spin loop** (`buttons_get`,
   `HAL_GPIO_ReadPin`, `HAL_Delay`), which is why PC sampling kept naming
   firmware input code. The freeze is not a hang; the device is already dead
   and waiting to be told to reboot.
4. Why the stage is slow is NOT a mystery and NOT a new bug: the instruction
   budget is not a time budget. ~50,000 instructions is 18 ms of ordinary code
   and most of a second of the aperture-plus-`ega_shadow_rebuild()` mix at
   846,000 cycles a rebuild. So `ega_shadow_rebuild` is, after all, the
   PASSENGER the measurement said it was -- it makes the frame expensive, and
   the missing refresh is what turns an expensive frame into a reset.

**The fix is host-side and is a general guard, not a Keen workaround**
(`src/main.c`): `FRAME_PHASE(id)` publishes which stage the frame loop is in
and **refreshes the watchdog at every stage boundary**, so the window bounds a
STAGE rather than a whole iteration. A slow stage is now a slow frame.

Graded on silicon, both directions, on the same timeline:

| build | result |
|---|---|
| per-stage refresh ON, wall-clock escape OFF | **51.6 s, 56.0 M insns, 1,095 frames, no reset** |
| per-stage refresh OFF (`X86_PHASE_WDOG=0`), escape ON | **WEDGES TWICE** -- 29.5 M/601 and 20.2 M/419, `x86_frame_phase` = 4 (`X86_PHASE_GUEST`) at both |
| shipping (both) | 51.6 s, 55.7 M insns, 1,128 frames, ~3.0 M insn/s, `frame_ms_worst` 67 |

`X86_PHASE_WDOG=0` is the negative control and it is SEEN TO FAIL. The stage
identifier in `x86_frame_phase` at the freeze is what named the guest stage;
nothing derived it.

**Two further bounds went in with it, and neither is the fix** -- say so if
either is ever quoted as one. `GUEST_SLICE_WALL_MS` (120 ms) escapes the
sub-slice loop, `guest_slice_overruns` counts it, and it fired 9 times in one
run; `GUEST_ASM_MS_MAX` (60 ms) halves the next budget when ONE
uninterruptible `x86_cpu_interp_asm()` call overruns, published as
`guest_asm_ms_worst` / `guest_asm_overruns`. The escape ALONE does not fix the
wedge (row 2 above has it on), because it can only act between calls and the
overlong stage is a single call. `guest_asm_ms_worst` reads **14 ms** on a
healthy run, so the ceiling has headroom over normal play.

**Still open, and separate: the artefacting.** The owner reaches the main menu
and the level-selection screen, where the picture corrupts before the crash.
That is the EGA card shortfall below (`x86_vga_planar_lost`), not this.

### The read-shadow guard, which is a real fix and is NOT this fix

`x86_vga_planar_gc_write()` rebuilt the 16 KB shadow whenever one of GC 2/4/5/7
changed VALUE. "The register changed" is not "what a read returns changed":
under read mode 0 Colour Compare and Don't Care are never consulted, under read
mode 1 Read Map Select is not, and only bit 3 of GC 5 is read state at all --
bits 0-1 are the WRITE mode, which is what a latch-copy loop toggles. So
`ega_read_sig()` reduces the four registers to what a read actually depends on
and the rebuild fires only when THAT moves. Graded by six read-path probes in
`tests/test_video.py`'s 0Dh arm (Read Map Select, read mode 1, Colour Compare,
Colour Don't Care, and the return to read mode 0) and by two negative controls
in `scripts/run_planar_controls.py`, both seen to fail.

### The EGA card, and what the intro costs

| | plane offsets | four planes |
|---|---:|---:|
| card fitted NOW (arena alone, reserve 134) | **35,840** | 143,360 B |
| card when the donation fired (reserve 100) | 61,440 | 245,760 B |
| 200 rows at `crtc_start=15360`, pitch 64 | 28,160 | 112,640 B |

`x86_vga_planar_lost` still reads **102,195** at 35,840 offsets, so the panning
intro drives `crtc_start` well past the 15,360 the static figure was taken at:
covering a pan that reaches start S costs `S + 12,800` offsets.

**The donation cannot close that gap for this title, structurally.** An arena
claim may not span regions, so the card is the best SINGLE run -- the arena's
143,360 B or the donated run, never their sum. Beating the arena therefore takes
~35 frames, and 160 minus Keen's 134-frame peak leaves 26. Above a reserve that
respects the working set the mechanism is inert here; below one, it buys card at
the price of SD writes in the frame loop. Growing the card for Keen means
growing the ARENA, not borrowing frames.

### The "levels are not fixable" verdict is RETRACTED

It reasoned from `x86_arena_free = 0` to *"no allocation, policy or eviction
change closes the gap"*, and that step does not follow: **the arena is not the
only AXI on this device.**

| | |
|---|---:|
| correct picture (the reference's own table) | 48 KB/plane = **196,608 B** |
| arena | 143,360 B |
| deficit | **53,248 B = 13 pages** |
| KEEN4 peak pager use | 134 of 160 frames |
| spare frames | **26 = 106,496 B** |

The route is `x86_pg_donated` + `x86_arena_add_region()` — see
[`12_memory_tiers.md`](12_memory_tiers.md) §5.

Two errors produced the wrong verdict, and both are the same shape:

- It quoted the **prior port's** budget (~24.9 KB of headroom, COW spare 0) as
  if it were a property of this hardware.
- It quoted **64 KB/plane**, which is where the reference's *discards reach
  zero*, not where the picture becomes correct — their own table says 48 KB
  renders correctly. That made the gap look 2x worse than it is.

> **An inherited number from another project is not a property of this
> hardware.** Re-measure before concluding anything is impossible. This cost a
> wrong "impossible" once and a wrong "38% idle" the same day.

## Star Wars: Dark Forces — MEASURED 2026-08-15 (THIRD PASS): AH=52h/29h are IN, and the extender now runs its MEMORY HUNT

`INT 21h AH=52h` (list of lists) and `AH=29h` (parse filename into FCB) are now
implemented in `x86_dos.c`. The second pass below named them as the suspects and
said the way to grade them was whether the far jump moved. **It did not move —
it disappeared.**

Same command, same flags, same 30 s window
(`probe_report.py --title darkforces --core-flag X86_DOS_EXEC=1 --skip-cost`),
plus a direct `probe.py` sample of `cpu_state_ptr` for the exit state.

**The control was run, and it matters.** The instrumented build reads
`guest_ram_pages=158`, `guest_ram_pages_short=2` — and so does the SAME build
with `x86_dos.c`, `x86_cpu.h` and `x86_hle.c` reverted to master
(`git checkout master -- …`, rebuilt clean, re-measured). `guest_ram_free_at_claim`
is **651,264 on both**. So the two lost frames are the instrument's, already
present on master, and **this change costs zero pager frames**. The baseline run
reproduced the second pass exactly: `guest_exit_reason=4`, 269,100 insns. Every
difference below is therefore attributable to the two new calls and to nothing
else.

**Both calls are answered.** `x86_dos_sysvars_calls=2`, `x86_dos_parse_calls=2`,
both `CF=0`; `AH=52h` returns `ES:BX = 0050:0022`.

**THE FAR JUMP INTO UNWRITTEN MEMORY IS GONE.** It used to die at linear
0x3E0C3, ~0x2E800 bytes past the image, executing `0xFF` filler. It now dies at
`CS:EIP = 023E:0C24` = linear **0x02FC4**, which is **inside the DOS4GW image**
(seg 0x81, 61,604 B, spanning 0x810..0xF6F4). It is executing the extender's own
code instead of blank memory.

**And it has reached a phase it had never reached.** The second pass recorded
that it asked "NOTHING about DPMI, VCPI or XMS". It now asks for all of the
first two, in order:

| probe | × | we answered |
|---|---:|---|
| `INT 2Fh AX=1687h` | 1 | **DPMI** — AX left non-zero, i.e. "not present" |
| `INT 67h AH=DEh` | 2 | **VCPI** — AH=0x84, "function not supported" |
| `INT 67h AH=43h` / `AH=45h` | 1 each | EMS allocate / free |
| `INT 15h AH=BFh AL=DEh` | 2 | unanswered (INT 15h is not even installed) |
| `INT 15h AH=C0h` | 1 | unanswered — get system configuration |
| `INT 21h AH=38h` | 1 | unanswered — get country info |

**THE NEXT BLOCKER, and it is mechanically explained, not guessed.** Told no by
DPMI, VCPI and INT 15h, the extender goes looking for extended memory itself:

* it opens A20 through the fast gate — the port log shows `OUT 0x92, 0x02` and
  `IN 0x92`, and `x86_a20_state=1` with `x86_a20_changes=1`, exactly one real
  transition;
* it loads **`ES=0xFFFF`** (register file at the exit: `ES=0xFFFF`, `DX=0xFFFF`,
  `AX=0x0002` — the value it just wrote to port 0x92);
* and it reads over the 1 MB line. `ES=0xFFFF` reaches linear 0x100000 and up.

There is nothing there. `x86_mem_wrap_1mb()` aliases 0x100000-0x110000 onto the
low 64 KB **only while A20 is MASKED** (`x86_cpu.h`, `X86_HMA_FIRST_PAGE`), and
the guest has just unmasked it; the pager owns `x86_pager_own(0, GUEST_RAM_SIZE
/ 4096)` = **640 KB** (`src/main.c:100,1885`). So the fold entry is invalid and
the access takes `X86_EXIT_MEM_FAULT`. **`guest_exit_reason=5`.**

**IT IS NOT THE WOLF3D MEMORY WALL, which is the other thing reason 5 means.**
`x86_pg_exhausted=0`, `x86_pg_evictions=0`, `x86_dos_load_shorts=0`,
`x86_map_failures=0`. Nothing was refused for want of a frame; the address was
never ownable in the first place.

**`CR0=0`. STILL REAL MODE.** The descriptor walk built in `docs/14` remains
completely unexercised by this title — the claim was tested here rather than
assumed, and it still holds. `x86_xms_detects=0`: XMS is never asked either, so
answering the HMA is upstream of every memory-manager question, not downstream
of one.

### The second pass, which named AH=52h/29h (superseded by the above)

## Star Wars: Dark Forces — MEASURED 2026-08-15 (SECOND PASS): AL=03h is IN, DOS4GW loads whole, and it dies past it

`INT 21h AH=4Bh AL=03h` (LOAD OVERLAY) is now implemented in `x86_dos.c`, and
the measurement below — which named it as the blocker — has been re-run against
it (`probe_report.py --title darkforces --core-flag X86_DOS_EXEC=1`, gwemu,
30 s).

**DOS4GW.EXE now loads, completely.** The overlay is taken (`4Bh/03 -> ok
AX=0x0000`, path log `DF/dos4gw.exe`), `x86_dos_overlays=1`, at load segment
**0x81** with relocation factor **0x81** — Dark Forces passes the two words
equal, which is the ordinary idiom. **The image is whole**:
`x86_dos_load_bytes=61604`, `x86_dos_load_short=0`, `x86_dos_load_shorts=0`.
The guest now executes **269,100** instructions where it previously executed
**0**.

**IT IS NOT THE MEMORY WALL.** `x86_pg_exhausted=0`, `x86_pg_evictions=0`,
`x86_pg_frames_total=158` (2 short only because `X86_PROBE=1` *and*
`X86_DOS_EXEC=1` are both in this build), `x86_seam_overflow=0`. Wolf3D's
`guest_exit_reason=5` (MEM_FAULT) does **not** appear, and an overlay load is
not the same shape after all: it writes into the caller's OWN block, which the
caller has already allocated and touched, so there are no ~30 fresh pages to
find. **This is a Queue B problem, not a Queue A one.**

**Where it now dies.** `guest_exit_reason=4` (UNIMPLEMENTED), `exit_info=0xFF`,
`CS:EIP = 0x3674:0x7983` = linear **0x3E0C3**, `CR0=0` (still REAL MODE). The
32 bytes there are **all `0xFF`** — memory that was never written. The DOS4GW
image spans linear 0x810..0xF8B4, so the guest far-jumped roughly **0x2E800
bytes past the end of the program it just loaded**. It is executing blank
memory; the "unimplemented opcode" is the blank itself, not a missing handler.

**The next blocker, and the prime suspect.** Two DOS calls are still refused,
and both happen in the handful of calls immediately BEFORE that jump:

| probe | × | what it is |
|---|---:|---|
| `INT 21h AH=52h` | 1 | **get the DOS list-of-lists.** A DOS extender uses it to find the DOS data segment and the head of the MCB chain. Refused, it returns CF=1 AX=0001 with ES:BX untouched — and a segment computed from that garbage is exactly the shape of a far jump to 0x3674 |
| `INT 21h AH=29h` | 2 | parse filename into an FCB. Refused immediately before the overlay load |

Neither was implemented in this pass. `AH=52h` is the one to build next, and
the way to grade it is this same re-measurement: if the far jump moves or
disappears, it was the cause.

**It still asks NOTHING about DPMI, VCPI or XMS** — no `INT 2Fh AX=1687h`, no
`AX=DE00h` — and touches exactly one port (`OUT 0x0000`, unmodelled). So the
extender has not yet reached its own memory-manager detection, and the
prerequisite chain further down this file is still not what gates the title.

### The first pass, which named AL=03h (superseded by the above)

The first measurement of this title with a working instrument
(`probe_report.py`, `X86_PROBE=1 X86_DOS_EXEC=1`, gwemu, 30 s;
`build/artifacts/probe_darkforces_exec.md`). It ran, made **67 recorded events
across 17 probes**, and stopped: `guest_exit_reason=4` (UNIMPLEMENTED),
`guest_halted=1`.

**DOS4GW.EXE never loads, and the memory wall is never reached.** What it asked
for and did not get:

| probe | × | what it is |
|---|---:|---|
| `INT 21h AH=4Bh AL=03h` | 1 | **LOAD OVERLAY.** At the time, `X86_DOS_EXEC=1` implemented AL=00h only. **This was the blocker, and it is now implemented — see above** |
| `INT 21h AH=52h` | 1 | get the DOS list-of-lists — a DOS-internals probe |
| `INT 21h AH=29h` | 2 | parse filename into an FCB |

Everything else it touched was answered: 30 × `AH=40h` (write — an error
message), 15 × `AH=4Ah`, 3 × `AH=48h`, the open/read/close set, and 2 ×
`AH=4Ch` (terminate).

**It asks NOTHING about DPMI, VCPI or XMS.** No `INT 2Fh AX=1687h`, no
`AX=DE00h`, no port access at all. So the prerequisite chain below is not
what gates this title today, and **`X86_DOS_EXEC=1` does not change its
outcome**: the next thing to build is EXEC AL=03h (overlay load), and only then
is there a memory question to measure. It did NOT hit Wolf3D's
`guest_exit_reason=5` wall — it never got far enough to be given the chance.

## Star Wars: Dark Forces — BLOCKED on the pagefile, not out of scope (SUPERSEDED by the measurement above)

> **RETRACTED, 2026-08-14: the "out of scope" verdict below is wrong.** It reads
> the zero-read-only-pages measurement as condemning *all* backing stores. It
> condemns **NOR** — XIP from flash needs a read-mostly set — and says nothing
> about a **pagefile**, whose entire purpose is pages that were written (the
> FILE tier). `x86_pager.h` sizes the pager at `X86_PG_PAGES = 2048` = **8 MB**,
> "sized for the space rather than for the 640 KB a real-mode DOS guest can
> address". The 640 KB figure below is not a ceiling, it is the pool that exists
> *because no pagefile is wired yet*. Prerequisite chain:
> **pagefile → XMS/EMS → DPMI → protected mode → Dark Forces.**
> Point 1 below (12.5x) is arithmetic against the wrong denominator; points 2
> and 3 stand as written.

## Star Wars: Dark Forces — the original, WRONG verdict (superseded above)

Halts in its **first frame slice** at `CS:EIP 0393:005E`, executing DOS/4GW's own
message strings (`"…$Not enough memory"`), `exit_reason=4`, opcode `ARPL`.

**The MCB arena is exercised and correct** — 3 × `AH=48h`, 15 × `AH=4Ah`, 0 ×
`AH=49h`, and the one refusal is real DOS semantics: block `0x0071` cannot grow
past an allocated block sitting above it. Real DOS refuses this too.

Why DPMI/VCPI/XMS stays unbuilt:

1. DF wants **~8 MB** flat against the pager's 160 frames = **640 KB**. 12.5x,
   and not a policy problem.
2. The read-mostly premise NOR-XIP rests on is false for every guest we can run
   (see [`12_memory_tiers.md`](12_memory_tiers.md) §3). A protected-mode linear
   heap will not be better than a real-mode game's, and it is 12.5x larger.
3. The dependency chain is XMS + VCPI + DPMI servers, `CR0.PE`, the descriptor
   walk, far transfers and gates, `IRET` across privileges, paging with a real
   TLB — **and then the memory still is not there.**

**Do not half-build DPMI.**

## Epic Pinball — BLOCKED on one call: `INT 21h AH=4Bh` EXEC

Measured 2026-08-15, gwemu tier 2, `EPIC_PINBALL.dsk` (11.7 MB), stock master,
`X86_PROBE=0` (rings only — the accumulator's RAM_EMU cost is not needed to
answer this and its pager trap is not worth taking).

**It boots, reaches DOS, loads and runs `PINBALL.EXE`, and stops at the launcher.**
The literal screen, mode 3, attribute `0x07` in all 2,000 cells (checked with
`--attr`; nothing is highlighted, nothing is a false "detected"):

```
Please run SETUP!
Press any Key.
```

Then `guest_halted=1`, `guest_exit_reason=4` (UNIMPLEMENTED), frozen at
`guest_insns_total=8,421,787` across three samples 8 s apart.

### The cause, and it is ours

`x86_hle_unhandled_vector=33` (0x21), `x86_hle_unhandled_func=75` (**0x4B**).
`probe.py --dos-log` gives the whole conversation — four file calls, and they
are the textbook prelude to an EXEC:

| # | call | result |
|---|---|---|
| 0 | `47h` getcwd | ok |
| 1 | `3Dh` open `PINBALL.EXE` (itself) | ok, handle 5 |
| 2 | `42h/02` lseek to end | ok, `AX=0x9255` = 37,461 = the exact file size |
| 3 | `3Eh` close | ok |
| 4 | `4Ah` resize own block to `0x926` paras (37,472 B) | ok — **shrink-to-self** |

Then `AH=4Bh`, which falls off the end of the `INT 21h` switch in `x86_dos.c`
and returns `CF=1, AX=0x01` ("invalid function"). The launcher reads that as
"cannot start the child", prints the SETUP message and waits for a key
(`x86_io_log`: one port access in the whole run, `IN 0x60` = 0x39, space, from
the timeline). After the key it runs off into an unimplemented opcode and halts.

**Negative control, seen to succeed where we fail:** the SAME `.dsk` under
DOSBox-X (`ref_dos.py --engine dosbox --launch PINBALL.EXE`) reaches mode 13h
and paints the "EPIC PIN…" dot-matrix intro
(`build/artifacts/ref_epic_db.png`). The image is configured and correct; the
message is not the disk's fault. (QEMU on the same image also reaches mode 13h
but paints nothing — a reference limit, not evidence either way.)

**This is the same blocker as Dark Forces' "Not enough memory"** — a second,
independent title dying on the one missing DOS call. `x86_dos_exec()` (the MZ
loader) already exists and is what loads the initial program; what is missing is
the `AH=4Bh` entry: the ASCIZ path at `DS:DX`, the EXEC parameter block at
`ES:BX` (child cmdline and two FCBs), a child PSP whose parent field points at
the caller, and save/restore of the parent's `CS:IP`/`SS:SP` so the child's
`AH=4Ch` returns to it rather than exiting the guest. `AL=03` (load overlay) is
a separate, smaller case.

### Everything else it asked for, we answered

| counter | value | reading |
|---|---|---|
| `x86_video_mode_byte` | 3 | text only |
| `x86_video_mode_log` | tag 6 request 3 (old 4), tag 1 adopted 3 / 80 col, tag 3 renderer followed, is_text=1 | **no tag 5** — no mode was refused, and no graphics mode was ever asked for |
| `x86_xms_detects` / `_calls` | 0 / 0 | never got that far |
| `x86_ems_calls` | 0 | ditto |
| DPMI `2Fh/1687h`, VCPI `DE00h` | never issued | — |
| Sound Blaster ports 0x220 / 0x388 | **never touched** | the only port access in the run is `IN 0x60`. `SOUNDCRD.INF` on the disk reads `REAL16 MAS ISBSERIES MDRV004R.MUS` — a Miles AIL/SB-series driver is configured, so SB probing is what to expect *after* EXEC lands, but this title has produced no evidence about it yet |
| `x86_pg_frames_total` | 160 | healthy; the guest ran |
| `x86_pg_evictions` / `_fill_file` / `_exhausted` | 0 / 0 / 0 | no thrash, nothing near the working set |
| `x86_arena_largest` | 110,592 | untouched, text mode only |

Memory is **not** this title's problem and there is no evidence it ever will be:
it never allocated past its own image. `PINBALL.EXE` is a 37 KB launcher; the
13 tables are separate `EP1..EP13.EXE` of ~220–300 KB each, each with a `.DAT`,
which is precisely why it needs EXEC — and why, once EXEC lands, the next
measurement to take is a table's working set, not the launcher's.

**Next blocker, one line: `INT 21h AH=4Bh`.** Nothing beyond it has been
measured, so no further claim about this title is supported.

## Protected mode — SIZED, not started

`CR0` has never been non-zero on this tree. Sized against
`references/tiny386/i386.c` (C line counts; ours is higher, being Thumb-2):

| piece | tiny386 | ~lines |
|---|---|---:|
| `read_desc` + `set_seg` + `clear_segs` | `1048-1135` | 88 |
| paging + software TLB | `527-724` | 198 |
| far `CALL`/`JMP` + gates, `task_switch` | `4214-4509` | 296 |
| interrupts with privilege change | `4510-4848` | 339 |
| `IRET`/far `RET` across privileges | `4849-5041` | 193 |
| `LAR`/`LSL`/`VERR`/`VERW`, `check_ioperm` | `3335-3360`, `3536-3620` | 111 |

**~1,225 lines**, plus the CPL and limit checks tiny386 macro-expands into
`cpu_exec1` itself — which for us means touching the R6 descriptor contract in
every handler, and that is not a line count.

`x86_cpu.h` already reserves 2 KB of `tlb_*` arrays that **nothing references**;
that is the intended backing.

**V86 is NOT needed.** It exists to run real-mode DOS under a supervisor, and
our INT 21h never reaches guest code. tiny386 carries 18 `VM`-flag sites we
would not have.

**Do not start without a title that needs it AND fits in 640 KB.** DF does not
fit, and nothing else in `roms/dos` sets `CR0` at all.

## TOPBENCH — the interpreter benchmark

Runs in **text mode**, which makes it a clean interpreter benchmark that
reflects none of the graphics work. Its six sub-timings read `65535` until port
`0x20` answered — see
[`11_pit_and_interrupt_controller.md`](11_pit_and_interrupt_controller.md) §3.

## Mortal Kombat — BLOCKED on the HMA, after four walls in the first 200 instructions

First measured 2026-08-15, gwemu tier 2, `timelines/start-preselected-game.tl`.
The `.dsk` mounts, the program LOADS (`dsk_startup_ok=1`, `dsk_exec_rc=0`), the
BIOS sets text mode 3 — and then the title dies inside its own C-runtime
startup, before it has drawn one pixel or asked for anything a device could
answer. **Every counter that names a subsystem read healthy the whole time**:
`x86_pg_frames_total=160`, `_exhausted=0`, `_evictions=0`, `x86_arena_largest=110,592`,
`x86_xms_detects=0`. The video mode log holds three events for the entire run —
the BIOS's own boot set-mode and the renderer following it — and no tag 5, so
there is nothing refused and nothing to render. This title has never asked for
video, memory, XMS or sound. It never got that far.

`guest_insns_total=0` is the shape to recognise: the guest halted inside its
FIRST frame slice, and the slice budget is only banked for a live guest, so a
title that dies this early reports zero rather than a rate.

### The four walls, in the order the guest hit them

Each was found the same way — `cpu_state_ptr->exit_reason`, `+2200` for the
offending opcode (`CPU_EXIT_INFO`), and `probe.py --guest` to disassemble
around CS:EIP. Three were ours and are FIXED here; the fourth is not an opcode.

| # | at | opcode | what the guest was doing |
|---|---|---|---|
| 1 | `023E:68CE` | `0F 01 /4` **SMSW** | reading CR0.EM before probing for an x87 |
| 2 | `023E:68D1` | `DB E3` **FNINIT** | the x87 detection itself |
| 3 | `023E:6624` | `0F BA /7` **BTC** | toggling EFLAGS.ID and .AC to identify the CPU |
| 3b| `023E:664D` | `0F A2` **CPUID** | ...having found ID writable |
| 4 | `023E:0C24` | *not an opcode* | `REPE CMPSW` against `FFFF:0030` — the A20 wrap test |

Walls 1-3b are one continuous routine: `PUSH -1 / SMSW AX / TEST AL,4 /
JNZ / FNINIT / FNSTSW [BP-2] / CMP [BP-2],CL`, then `PUSHF / BTC EDX,21 /
BTC EDX,18 / POPF / PUSHF / CPUID`. It is the standard Microsoft/Borland
startup preamble, **not** anything specific to this title, which is why the
fixes are worth more than one game: any DOS title built with those runtimes
runs it.

- **SMSW** was `.exit_loop` because `0F 01`'s ModRM walk was unconditional and
  SMSW is the only member of the group with a register form.
- **D8-DF** now behave as a machine with NO COPROCESSOR FITTED: the escape is
  ignored and only its operand is stepped over, which is what makes the
  detection above conclude "no x87" instead of killing the guest. `x86_x87_escapes`
  counts them, and **it is the number to read before trusting a picture from a
  new title** — a detection costs a handful (this title: **2**), a title
  actually computing in floating point runs millions and is silently wrong.
- **`0F BA`** (BT/BTS/BTR/BTC, imm8 form only) and **`0F A2`** (CPUID leaves 0
  and 1, a 486SX with no feature bits) are implemented. The register-source bit
  ops `0F A3/AB/B3/BB` address a bit STRING and are deliberately still loud.

Fourteen cases in `tests/test_opcodes.py` (274-287) cover all of it; all
fourteen were SEEN TO FAIL with the table entries reverted.

### The single next blocker: the HMA has no backing when A20 is open

The guest opens A20 through the **fast gate at port 0x92** (`x86_a20_changes=1`,
`x86_a20_state=1`; the io log's only two entries are that IN and that OUT) and
immediately runs the classic wrap test — `ES=0xFFFF`, `REPE CMPSW` at
`FFFF:0030` against `0000:0020`, i.e. linear **0x100020**. That page is not
mapped: `x86_mem_wrap_1mb()` writes `FOLD_INVALID` across the sixteen HMA pages
whenever the gate is open, on purpose ("nothing here owns it yet"), and the
pager's `owns()` does not cover them either. Result: `x86_pg_unmapped=1`,
`X86_EXIT_MEM_FAULT`, dead. `x86_seam_overflow=1` is the same event seen from
the seam — a straddle whose low granule has no frame cannot be served.

**Size of the fix: 16 pages = 64 KB, against 144 free frames.** It is not a
memory problem and not an XMS problem — `x86_xms_detects` is still 0, the guest
has not talked to a driver at all. The pager already has an `aux_first/aux_count`
region beside its own; giving the HMA sixteen ordinary demand-paged frames is
the shape of it. Whether the wrap test then PASSES is the second question and
the more interesting one: the guest is checking that the gate really opened.



CGA mode 4, pixel-exact. Its skill menu wants K/H/T/A, which no button can send;
that needs the on-screen keyboard, not a mapping change.
