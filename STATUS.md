# Status

Last verified 2026-08-13. How to run: [`TOOLING.md`](TOOLING.md). Roadmap: [`PLAN.md`](PLAN.md).

## Verification state

| Tier | State |
|---|---|
| 1. gwemu bare-metal | 248/248 `test_opcodes.py`, plus `test_video`/`test_dos`/`test_screen` |
| 2. gwemu + retro-go | `core_dtcm_ok=1`, bonus pool 153,600 B, 1,000,001 insns — matches tier 1 |
| 3. Hardware | 50.50 cyc/insn, 5.54 MIPS |

Cycles/insn: 57.50 pre-fold → 50.00 fold + absolute dispatch → 50.50 with
segmentation, and unchanged through 16-bit, the 66/67 axes, segment loads, far
transfers and REP — the width macros vanish at NSH=0.

## Phase 0/1 — baseline and CPU foundations

Complete except where listed below. The interpreter does 16/32-bit with the
`66`/`67` axes per instruction, segmentation on a descriptor pointer, real-mode
interrupts, string ops with `REP`, far transfers and segment loads.

- [ ] `XLAT` (no segment base), `WAIT`, `INS`/`OUTS`, `BOUND`/`ARPL`
- [ ] Five handlers call `DECODE_MODRM` by symbol, so they ignore a `0x67` prefix
- [ ] Limit checks off the R6 descriptor; lazy PF/AF; software TLB above 16 MB
- [ ] Size-aware `MEM_R*`/`MEM_W*` macros + CI grep banning raw `[R9,`
- [ ] Assertions in `test_game.py`/`test_benchmark.py`; realign `docs/01`–`03`, `05`

## Phase 2 — Alley Cat

17 opcode slots still reach `.exit_loop`: x87 (`D8`-`DF`), `BOUND`/`ARPL`,
`INS`/`OUTS`, `XLAT`, `WAIT`, `INT1`.

Done: real-mode interrupt dispatch and `IRET`; the `0F 0B` hypercall and its
HLE service table with stubs planted per vector; BIOS INT 10h/16h/1Ah over a
real BDA; CGA text mode 3 and a mode 4 LUT8 blitter at 0.59 insns/pixel, both
checked pixel-exact; CGA registers including a moving retrace bit; a port-level
8042 keyboard with IRQ 1 delivered at the instruction boundary; PC speaker on
PIT ch2; port I/O on a C dispatch; the guest page pool and fold mapping; the
frame-paced emulator loop; and the DOS layer -- MCB arena, PSP, MZ loader with
relocations, INT 21h on FatFs -- fed by the guest `.dsk` streamed from SD.

- [ ] DWT per-opcode-class profile; gate NOR backing on a code-vs-data measurement

## Phase 3 — Wolf3D

- [ ] COW/pool/eviction with dirty tracking, loud exhaustion; pool sizing
- [ ] Gate: measure DOS FAT/sector overhead → decide the INT 21h fast path
- [ ] Protected mode: descriptors, far transfers, IRET privilege change, paging
- [ ] Profile-directed tuning against a Wolf3D DWT profile

## TOPBENCH — running, scoring, screenshotted

`TOPBENCH.EXE -l` boots from its `.dsk`, unpacks, benchmarks and prints
**"This system's TOPBENCH Score: 236"**, iterating continuously as `-l` asks.
Capture: `build/artifacts/topbench.png`, via

```bash
python3 scripts/screenshot.py --timeline timelines/run-to-completion.tl \
    --wait 48 --pan 16 --text --out build/artifacts/topbench.png
```

`scripts/trace_diff.py` agrees with a reference 8086 (Unicorn) for the whole of
startup and the self-unpacker — 1,532,786 instructions to the first INT 21h,
no divergence at any stride.

Getting here closed sixteen defect classes, one commit each in `git log`.
**CPU**: the v-width shift group pass-agnostic; `INC`/`DEC` flags and an AF
built from `~a`; thirteen stack handlers ignoring `SS.base`; the shift operand
fetch and `XCHG` at 32 bits; `TEST` flags; 16-bit `CBW`/`CWD` running
`CWDE`/`CDQ`; near `Jcc` on rel32 and `LOOP`/`JCXZ` on ECX; `LEA` returning a
linear address; `MOV moffs` with a 32-bit offset and no DS base; eight handlers
with no memory form; `MOV Ew,Sreg` storing decoder scratch; `MOV CRn`; the BCD
group. **Platform**: `maxalloc` ignored (Borland sized its heap from it and died
with `Runtime error 203`); the IRET stub planted inside the DOS arena; a BIOS
tick that never advanced; `HLT` as a stop rather than an idle, with INT 08h
never delivered; INT 21h AH=06h/44h and INT 10h AH=08h/11h unanswered.

One shape accounts for four of them: reading a register operand BEFORE calling
`DECODE_MODRM`, which uses R4 and R5 as scratch.

Still open on this path: `refdos.py` has no port I/O, so the oracle stops at the
first thing TOPBENCH times against the CGA retrace. Extending it there is what
would let the comparison run to the score.

## Open defects

- `moffs` (`A0`–`A3`) takes its offset at the operand size; it should follow the
  ADDRESS size.
- `MOV SS,x` does not inhibit interrupts for the following instruction, so an
  interrupt can land on a half-switched stack.
- Protected-mode segment loads trap out (`CR0.PE` set) — no descriptor walk yet.
- 8-bit `ADC`/`SBB` ignored the carry-in entirely (a normalized `ADCS` injects it
  24 bits below the operand's LSB) and 8-bit `AF` read the wrong bit. Both fixed
  by `X86_ADC_NARROW`; the 16-bit width would have inherited them.
- `tlb_*` arrays: 2 KB of DTCM declared, unreferenced.
- Tier 2 harness had two faults, now fixed: it polled for 150 s against a 35 s
  timeline (so every run ended in a GDB traceback), and it never armed
  `run_benchmark_flag`, so it read a figure the core had stopped producing.
- gwemu's headless **screendump** produces nothing — Retro-Go's own menu comes
  back black too, so those PNGs prove nothing either way. Its **LTDC registers
  are modelled**, though, so `tests/test_screen.py` reconstructs the scanned-out
  image from L1CFBAR + the firmware's CLUT and checks it. Do not read a black
  timeline screenshot as a video bug.
- The hardware probe was not detected during this session, so `50.50 cyc/insn`
  is the last measured figure and predates the interrupt, speaker and video work.
