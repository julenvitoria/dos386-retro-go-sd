# Status

Last verified 2026-08-12. Run instructions: [`TOOLING.md`](TOOLING.md).
Roadmap and phase definitions: [`PLAN.md`](PLAN.md).

## Verification state

| Tier | State |
|---|---|
| 1. gwemu bare-metal | 188/188 `test_opcodes.py`, plus `test_video`/`test_dos`/`test_screen` |
| 2. gwemu + retro-go | `core_dtcm_ok=1`, bonus pool 153,600 B, 1,000,001 insns — matches tier 1 |
| 3. Hardware | 50.50 cyc/insn, 5.54 MIPS |

`tests/asm_baseline.json`: 596 symbols, 8751 insns, 31840 bytes.
Dispatch-table symbols are data; `objdump` counts them as instructions.

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

- [x] Real-mode interrupt dispatch: 4-byte IVT, FLAGS/CS/IP frame, IF/TF
      cleared, CS installed; `IRET` inverts it. `CR0.PE` traps out
- [x] `0F 0B imm8` hypercall, gated on `hle_enabled`, real `#UD` when off;
      sets `exit_reason=X86_EXIT_HLE_INT`, `exit_info=selector`
- [x] HLE dispatch: `0F 0B` stubs planted per vector, host-side service table
- [x] BIOS: INT 10h / 16h / 1Ah over a real BIOS Data Area
- [x] CGA text mode 3 (8x8 font, 16-colour attributes, 40 visible columns)
- [x] DOS: real MCB arena (alloc/free/resize/coalesce, largest-block reporting),
      PSP, MZ loader with relocations, INT 21h core + file handles on FatFs
- [x] Hardware keyboard: 8042-style queue on ports 0x60/0x64, IRQ 1 delivered
      in-guest at the instruction boundary (gated by IF, no per-instruction
      cost), INT 09h filling the BIOS ring for INT 16h. Port-level on purpose:
      identical in protected mode, and a game with its own INT 09h needs none
      of the BIOS layer
- [ ] Stream the `.dsk` from SD through the firmware's FatFs
- [ ] Load ALLEYCAT.EXE through `x86_dos_exec` instead of the built-in demo
- [x] PC speaker: PIT ch2 + port 0x61 driven by real guest OUT; square-wave
      renderer with phase carried across buffers
- [x] Port I/O on a C dispatch
- [x] Guest page pool + fold mapping (`x86_mem.c`). Not a pager: no faults, no
      eviction, no COW — every guest page stays resident
- [x] CGA mode 4 → LUT8 blitter in Thumb-2 at 0.59 instructions/pixel,
      pixel-exact against an independent Python expansion (`test_video.py`)
- [x] CGA registers: 0x3D8 mode, 0x3D9 background/palette/intensity, 0x3DA
      retrace status (both bits must move or a polling guest spins forever)
- [x] Core emulator loop: 640 KB RAM + 32 KB VRAM from AXI, frame-paced CPU
      slices, CGA blit to the LCD, speaker to the audio buffer, gamepad polled.
      A built-in real-mode guest drives all three, and `test_screen.py`
      confirms the image the display controller is scanning
- [ ] Input into a BIOS keyboard buffer (state captured; INT 16h will consume it)
- [ ] Real 18.2 Hz PIT tick driving INT 08h
- [ ] DWT per-opcode-class profile over a real Alley Cat frame
- [ ] Gate: measure NOR code-vs-data clean fraction before any NOR backing

## Phase 3 — Wolf3D

- [ ] COW/pool/eviction with dirty tracking and loud exhaustion; pool sizing
- [ ] Gate: measure DOS FAT/sector overhead → decide the INT 21h fast path
- [ ] Protected mode: descriptor validation, far transfers, IRET privilege change, paging walk
- [ ] Profile-directed tuning against a Wolf3D DWT profile

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
