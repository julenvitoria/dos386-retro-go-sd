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

## Star Wars: Dark Forces — MEASURED 2026-08-15: it blocks at EXEC AL=03h, long before memory

The first measurement of this title with a working instrument
(`probe_report.py`, `X86_PROBE=1 X86_DOS_EXEC=1`, gwemu, 30 s;
`build/artifacts/probe_darkforces_exec.md`). It ran, made **67 recorded events
across 17 probes**, and stopped: `guest_exit_reason=4` (UNIMPLEMENTED),
`guest_halted=1`.

**DOS4GW.EXE never loads, and the memory wall is never reached.** What it asked
for and did not get:

| probe | × | what it is |
|---|---:|---|
| `INT 21h AH=4Bh AL=03h` | 1 | **LOAD OVERLAY.** `X86_DOS_EXEC=1` implements AL=00h only; AL=01h/03h are refused by design (`x86_dos.c`). **This is the blocker** |
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

## Alley Cat — playable

CGA mode 4, pixel-exact. Its skill menu wants K/H/T/A, which no button can send;
that needs the on-screen keyboard, not a mapping change.
