# Status

Last verified 2026-08-13. Run instructions: [`TOOLING.md`](TOOLING.md).
Roadmap and phase definitions: [`PLAN.md`](PLAN.md).

## Verification state

| Tier | State |
|---|---|
| 1. gwemu bare-metal | 229/229 `test_opcodes.py`, plus `test_video`/`test_dos` |
| 2. gwemu + retro-go | `core_dtcm_ok=1`, bonus pool 153,600 B, 1,000,001 insns — matches tier 1 |
| 3. Hardware | 50.50 cyc/insn, 5.54 MIPS |

Cycles/insn: 57.50 pre-fold → 50.00 fold + absolute dispatch → 50.50 segmentation
→ 50.50 unchanged through 16-bit, the 66/67 axes, segment loads, far transfers
and REP: the width macros vanish at NSH=0 and none of it sits on the 32-bit hot
path. The fold and dispatch were measured together and are not separable.

## Phase 0/1 — baseline and CPU foundations

Complete except where listed below. The interpreter does 16/32-bit with the
`66`/`67` axes switching per instruction, segmentation on a descriptor pointer,
real-mode interrupts, string ops with `REP`, far transfers and segment loads.
Details are in the git history; `git log --oneline` is the index.

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

**Alley Cat draws its title screen** — `build/artifacts/alleycat.png`, captured
with `scripts/screenshot.py`. The guest runs continuously: `guest_halted=0`, no
unhandled vector, ~2 M HLE calls per 4 s, BDA ticks advancing at 18.2 Hz.
Five defects stood between it and this, all fixed with tier-1 cases first: the
bare-IRET stub sat inside the DOS arena; `moffs` ignored the segment base and
always fetched 4 bytes; `MOV r/m,imm` (C6/C7) had no memory form; `MOV Ew,Sreg`
to memory stored the decoder's leftovers instead of the selector; and nothing
drove the PIT tick.

- [ ] It stays ON the title screen. Scancodes reach the guest (`kbd_head ==
      kbd_tail`, `kbd_last=0xB9`) and its own INT 09h hook is installed
      correctly (`IVT[9] = 07A4:14B3`), so the next step is to find what the
      title loop is actually waiting on — trace it from the poll at CS 0x7A4.
- [ ] `sdcard.py`'s default ROM is now ALLEYCAT, so `test_screen.py` (which
      asserts the built-in demo pattern) needs an SD image with no loadable
      .dsk. Use `scripts/screenshot.py` to look at a real guest.
- [ ] Input into a BIOS keyboard buffer (state captured; INT 16h will consume it)
- [ ] DWT per-opcode-class profile; gate NOR backing on a code-vs-data measurement

## Phase 3 — Wolf3D

- [ ] COW/pool/eviction with dirty tracking and loud exhaustion; pool sizing
- [ ] Gate: measure DOS FAT/sector overhead → decide the INT 21h fast path
- [ ] Protected mode: descriptor validation, far transfers, IRET privilege change, paging walk
- [ ] Profile-directed tuning against a Wolf3D DWT profile

## TOPBENCH (the previous 16-bit correctness driver)

Superseded by Alley Cat, which now gets further. TOPBENCH still dies inside its
self-unpacker at ~1.34 M instructions, where `scripts/ref_trace.py` says a real
8086 needs 1,553,044 to reach INT 21h. It has not been re-measured since the
moffs, C6/C7 and 8C fixes, any of which could account for it -- re-run it before
resuming the trace diff, and respect TOOLING's alignment traps if you do.

## Open defects

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
