# Status

Last verified 2026-08-13. Run instructions: [`TOOLING.md`](TOOLING.md).
Roadmap and phase definitions: [`PLAN.md`](PLAN.md).

## Verification state

| Tier | State |
|---|---|
| 1. gwemu bare-metal | 236/236 `test_opcodes.py`, plus `test_video`/`test_dos`/`test_screen` |
| 2. gwemu + retro-go | `core_dtcm_ok=1`, bonus pool 153,600 B, 1,000,001 insns — matches tier 1 |
| 3. Hardware | 50.50 cyc/insn, 5.54 MIPS |

Cycles/insn: 57.50 pre-fold → 50.00 fold + absolute dispatch → 50.50 with
segmentation, and unchanged through 16-bit, the 66/67 axes, segment loads, far
transfers and REP — the width macros vanish at NSH=0.

## Phase 0/1 — baseline and CPU foundations

Complete except where listed below. The interpreter does 16/32-bit with the
`66`/`67` axes per instruction, segmentation on a descriptor pointer, real-mode
interrupts, string ops with `REP`, far transfers and segment loads.

- [ ] `XLAT`, BCD (`27`/`2F`/`37`/`3F`), `WAIT`, `INS`/`OUTS`, `BOUND`/`ARPL`
- [ ] Limit checks off the R6 descriptor, incl. real-mode 64 KB wrap
- [ ] Lazy PF/AF (currently eager, ~9 of ~16 insns per ALU op)
- [ ] Size-aware `MEM_R*`/`MEM_W*` macros + CI grep banning raw `[R9,`
- [ ] Software TLB above the 16 MB direct window
- [ ] Assertions in `test_game.py`/`test_benchmark.py`; realign `docs/01`–`03`, `05`

## Phase 2 — Alley Cat

21 opcode slots still reach `.exit_loop`: x87 (`D8`-`DF`), BCD
(`27`/`2F`/`37`/`3F`), `BOUND`/`ARPL`, `INS`/`OUTS`, `XLAT`, `WAIT`, `INT1`.

Done: real-mode interrupt dispatch and `IRET`; the `0F 0B` hypercall and its
HLE service table with stubs planted per vector; BIOS INT 10h/16h/1Ah over a
real BDA; CGA text mode 3 and a mode 4 LUT8 blitter at 0.59 insns/pixel, both
checked pixel-exact; CGA registers including a moving retrace bit; a port-level
8042 keyboard with IRQ 1 delivered at the instruction boundary; PC speaker on
PIT ch2; port I/O on a C dispatch; the guest page pool and fold mapping; the
frame-paced emulator loop; and the DOS layer -- MCB arena, PSP, MZ loader with
relocations, INT 21h on FatFs -- fed by the guest `.dsk` streamed from SD.

- [ ] Input into a BIOS keyboard buffer (state captured; INT 16h will consume it)
- [ ] Real 18.2 Hz PIT tick driving INT 08h
- [ ] DWT per-opcode-class profile; gate NOR backing on a code-vs-data measurement

## Phase 3 — Wolf3D

- [ ] COW/pool/eviction with dirty tracking and loud exhaustion; pool sizing
- [ ] Gate: measure DOS FAT/sector overhead → decide the INT 21h fast path
- [ ] Protected mode: descriptor validation, far transfers, IRET privilege change, paging walk
- [ ] Profile-directed tuning against a Wolf3D DWT profile

## TOPBENCH (the current 16-bit correctness driver)

**Verified**: `scripts/trace_diff.py` now agrees with a reference 8086
(Unicorn) for the whole of TOPBENCH's startup and self-unpacker -- 1,532,786
instructions, to the first INT 21h, with no divergence at any stride. The CPU
core is clean over everything the reference can reach.

Getting there closed ten defect classes, one commit each in `git log`: the
v-width shift group pass-agnostic; `INC`/`DEC` setting no flags (r/m8, Ev) and
computing AF from `~a` (register); thirteen stack handlers ignoring `SS.base`;
the shift operand fetch and `XCHG` running 32-bit; `TEST` setting no flags and
16-bit `CBW`/`CWD` running `CWDE`/`CDQ`; near `Jcc` taking a rel32 and
`LOOP`/`JCXZ` counting ECX; **`LEA` returning the linear address** instead of
the offset; **`MOV moffs` reading a 32-bit offset with no DS base**, which
indexed past the fold table into a HardFault; and six handlers with no memory
form. The last four were found by the trace diff, not by reading code.

The guest now unpacks fully, reaches its real entry at CS=0x2822, and makes 29
BIOS/DOS calls, all of them answered. It then takes an `FF /3` far indirect
transfer at linear 0x29973 through a pointer holding 0x9E40:0x1C03 and executes
open-bus filler. No screenshot yet: nothing has been printed.

The reference cannot follow it past that point -- `refdos.py` has no port I/O,
and TOPBENCH times itself against the CGA retrace, so under the reference it
divides by a zero count at 26EA:0117. Extending the shim with port I/O is what
would make the oracle reach the end.

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
