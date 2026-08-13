# Status

Last verified 2026-08-13. How to run: [`TOOLING.md`](TOOLING.md). Roadmap: [`PLAN.md`](PLAN.md).

## Verification state

| Tier | State |
|---|---|
| 1. gwemu bare-metal | 247/247 `test_opcodes.py`, plus `test_video`/`test_text`/`test_dos`/`test_screen` |
| 2. gwemu + retro-go | `core_dtcm_ok=1`, bonus pool 153,600 B, 1,000,001 insns — matches tier 1 |
| 3. Hardware | 50.50 cyc/insn, 5.54 MIPS |

Cycles/insn: 57.50 pre-fold → 50.00 fold + absolute dispatch → 50.50 with
segmentation, unchanged through 16-bit, the 66/67 axes, segment loads, far
transfers and REP — the width macros vanish at NSH=0.

## Phase 0/1 — baseline and CPU foundations

Complete except below: 16/32-bit with the `66`/`67` axes per instruction,
segmentation on a descriptor pointer, real-mode interrupts, `REP` string ops,
far transfers and segment loads.

- [ ] `XLAT` (no segment base), `WAIT`, `INS`/`OUTS`, `BOUND`/`ARPL`
- [ ] Five handlers call `DECODE_MODRM` by symbol, so they ignore a `0x67` prefix
- [ ] Limit checks off the R6 descriptor; lazy PF/AF; software TLB above 16 MB
- [ ] Size-aware `MEM_R*`/`MEM_W*` macros + CI grep banning raw `[R9,`
- [ ] Assertions in `test_game.py`/`test_benchmark.py`; realign `docs/01`–`03`, `05`

## Phase 2 — Alley Cat

17 opcode slots still reach `.exit_loop`: x87 (`D8`-`DF`), `BOUND`/`ARPL`,
`INS`/`OUTS`, `XLAT`, `WAIT`, `INT1`.

Done: real-mode interrupts and `IRET`; the `0F 0B` hypercall and its HLE
service table; BIOS INT 10h/16h/1Ah over a real BDA; CGA text mode 3 and a
mode 4 LUT8 blitter at 0.59 insns/pixel, both pixel-exact; CGA registers with a
moving retrace bit; a port-level 8042 keyboard with IRQ 1 at the instruction
boundary; PC speaker on PIT ch2; port I/O on a C dispatch; the guest page pool
and fold mapping; the frame-paced loop; and the DOS layer -- MCB arena, PSP, MZ
loader, INT 21h on FatFs -- fed by the `.dsk` streamed from SD.

- [ ] DWT per-opcode-class profile; gate NOR backing on a code-vs-data measurement

## Phase 3 — Wolf3D

- [ ] COW/pool/eviction with dirty tracking, loud exhaustion; pool sizing
- [ ] Gate: measure DOS FAT/sector overhead → decide the INT 21h fast path
- [ ] Protected mode: descriptors, far transfers, IRET privilege change, paging
- [ ] Profile-directed tuning against a Wolf3D DWT profile

## TOPBENCH — running, scoring, screenshotted

`TOPBENCH.EXE -l` boots from its `.dsk`, unpacks, benchmarks and prints
**"This system's TOPBENCH Score: 236"** — `build/artifacts/topbench.png`
(`screenshot.py --timeline timelines/run-to-completion.tl --wait 48 --pan 16`).

Run WITHOUT `-l` it draws its full-screen menu correctly --
`build/artifacts/topbench_ui.png` (`--pan 22`). Four defects were in the way:
a 128-glyph ASCII font, so CP437 box drawing came out as letters; 4 of 16 CLUT
entries reaching the panel; INT 21h AH=3Ch unanswered, which Borland reported
as `Runtime error 001` before drawing anything; and a read-only floppy, so
answering it needed write-back. `tests/test_text.py` covers the renderer now.

`scripts/trace_diff.py` agrees with a reference 8086 (Unicorn) for the whole of
startup and the unpacker — 1,532,786 instructions to the first INT 21h.

Getting here closed sixteen defect classes, one commit each in `git log` --
the shift group, `INC`/`DEC`/`TEST` flags, `SS.base` in thirteen stack
handlers, 32-bit `XCHG`, `CBW`/`CWD`, rel32 `Jcc`, `LEA`, `MOV moffs`, eight
missing memory forms, `MOV Ew,Sreg`, `MOV CRn`, BCD; and on the platform side
`maxalloc`, the misplaced IRET stub, a dead BIOS tick, `HLT` as a stop, and
INT 21h AH=06h/44h + INT 10h AH=08h/11h. One shape accounts for four: reading
a register operand BEFORE `DECODE_MODRM`, which uses R4/R5 as scratch.

Still open: `refdos.py` has no port I/O, so the oracle stops at the first thing
TOPBENCH times against the CGA retrace.

## Open defects

- `moffs` (`A0`–`A3`) takes its offset at the operand size, not the ADDRESS size.
- `MOV SS,x` does not inhibit interrupts for the next instruction, so one can
  land on a half-switched stack.
- Protected-mode segment loads trap out (`CR0.PE` set) — no descriptor walk yet.
- 8-bit `ADC`/`SBB` ignored the carry-in and 8-bit `AF` read the wrong bit; both
  fixed by `X86_ADC_NARROW`, which the 16-bit width would have inherited.
- `tlb_*` arrays: 2 KB of DTCM declared, unreferenced.
- Tier 2 harness had two faults, now fixed: it polled 150 s against a 35 s
  timeline, and never armed `run_benchmark_flag`.
- Only 40 of 80 text columns fit a 320 px panel at 8x8; the window follows the
  cursor and `x86_video_pan` slides it, but there is **no pan on a gamepad
  button**, which is what a human needs to read a full-width UI.
- PC speaker: sub-frame beeps are never heard and the pitch reads low. Filed
  separately; untouched by the video work.
- gwemu's headless **screendump** produces nothing — Retro-Go's own menu comes
  back black too. Its **LTDC registers are modelled**, so `test_screen.py`
  reconstructs the scanned-out image from L1CFBAR + the firmware's CLUT. Do not
  read a black timeline screenshot as a video bug.
- No hardware probe this session, so `50.50 cyc/insn` predates the interrupt,
  speaker and video work.
