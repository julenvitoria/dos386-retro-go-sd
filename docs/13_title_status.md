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
