# Status

Last verified 2026-08-13. Run instructions: [`TOOLING.md`](TOOLING.md).
Roadmap and phase definitions: [`PLAN.md`](PLAN.md).

## Verification state

| Tier | State |
|---|---|
| 1. gwemu bare-metal | 209/209 `test_opcodes.py`, plus `test_video`/`test_dos`/`test_screen` |
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
- [ ] Assertions in `test_game.py` / `test_benchmark.py`
- [ ] Realign `docs/01`–`03`, `05`; `docs/04`, `docs/06` current

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
- [ ] DWT per-opcode-class profile over a real Alley Cat frame
- [ ] Gate: measure NOR code-vs-data clean fraction before any NOR backing

## Phase 3 — Wolf3D

- [ ] COW/pool/eviction with dirty tracking and loud exhaustion; pool sizing
- [ ] Gate: measure DOS FAT/sector overhead → decide the INT 21h fast path
- [ ] Protected mode: descriptor validation, far transfers, IRET privilege change, paging walk
- [ ] Profile-directed tuning against a Wolf3D DWT profile

## TOPBENCH (the current 16-bit correctness driver)

The .dsk streams, AUTOEXEC.BAT names the program, the MZ loader places it and
its Borland relocator runs. Four bug classes found and fixed against it so far
(see `git log`): the v-width shift group was pass-agnostic; `INC`/`DEC r/m8` and
`INC`/`DEC Ev` set no flags; thirteen stack handlers ignored `SS.base` and used a
4-byte slot; the shift operand FETCH and `XCHG` were 32-bit in 16-bit mode.

It now runs ~1.34 M instructions and dies inside the self-unpacker.
`scripts/ref_trace.py` says a real 8086 takes 1,553,044 to reach INT 21h, so
roughly one bug remains between here and the first DOS call. The failure is that
our machine leaves the relocation copy loop sooner than the reference does; the
copy itself, its direction, large counts, granule crossing and the 16-bit index
wrap are all covered by passing tier-1 cases, so the cause is elsewhere. No
screenshot yet: `hle_calls_seen` is still 0, meaning nothing has been printed.

Known-broken and untested, from a read of the handler table -- any of them could
be next: `TEST` 0x84/0xA8/0xA9 set no flags at all, and 0x98/0x99 are
dual-emitted with fixed bodies so 16-bit `CBW`/`CWD` execute `CWDE`/`CDQ`.

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
