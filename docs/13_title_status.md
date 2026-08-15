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
instructions) cannot be the guard.

**Open lead, n=2 and NOT confirmed.** The last live PC before two of three
freezes was inside `ega_read_byte`, i.e. the `ega_shadow_rebuild()` path in
`x86_video_planar.c`. That function's own comment records that it "went
unnoticed at 8 KB and then wedged a device run at 64 KB", it rebuilds
`EGA_SHADOW_OFFSETS` = 16,384 bytes on every write that changes what a read
returns, and `probe.py --io-log` shows Keen's scroll loop writing Graphics
Controller index 5 (`OUT 0x3ce/0x3cf`) continuously -- the register whose bit 3
is read mode, which is exactly one of the rebuild triggers. In read mode 1 each
of the 16,384 bytes is a four-plane loop. PC sampling over SWD is halt-biased
and two samples are not a finding; the next step is a rebuild COUNTER and a
DWT bracket around the rebuild, not more sampling.

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

## Star Wars: Dark Forces — BLOCKED on the pagefile, not out of scope

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

## Alley Cat — playable

CGA mode 4, pixel-exact. Its skill menu wants K/H/T/A, which no button can send;
that needs the on-screen keyboard, not a mapping change.
