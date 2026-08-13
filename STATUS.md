# Status

Last verified 2026-08-13. How to run: [`TOOLING.md`](TOOLING.md). Roadmap: [`PLAN.md`](PLAN.md).

## Verification state

| Tier | State |
|---|---|
| 1. gwemu bare-metal | 247/247 `test_opcodes.py`, plus `test_video`/`test_text`/`test_dos`/`test_screen` |
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

## Open defects

- **RAM_EMU headroom is 2,528 bytes, and running out is SILENT.** The core ends
  at `__CORE_BSS_END__ = 0x24056620` in a region ending at `0x24100000` — 694,752
  B left against the 692,224 B the guest asks for (32 KB VRAM + 640 KB RAM + 4 KB
  filler). The firmware allocator does not refuse the overrun; it returns a
  pointer past the end, `core_ready` stays 1, and the GUEST takes
  `X86_EXIT_MEM_FAULT` a second in, which reads as a CPU or DOS bug. Reproduced
  from a clean tree with 2,624 B of dead `const` data, and again with 3 KB of
  BSS. Fix is core-side (put the guest pool on the 150 KB bonus pool, or make the
  overrun loud). See `TOOLING.md`.
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
- PC speaker: sub-frame beeps are never heard and the pitch reads low.
- gwemu's headless **screendump** produces nothing (Retro-Go's own menu comes
  back black too), but its **LTDC registers are modelled**, so `test_screen.py`
  rebuilds the scanned-out image. A black timeline screenshot is not a bug.
- No hardware probe this session, so `50.50 cyc/insn` predates the interrupt,
  speaker and video work.
