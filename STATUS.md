# Status

Last verified 2026-08-12. Run instructions: [`TOOLING.md`](TOOLING.md).
Roadmap and phase definitions: [`PLAN.md`](PLAN.md).

## Verification state

| Tier | State |
|---|---|
| 1. gwemu bare-metal | 162/162 `tests/test_opcodes.py`, plus `test_video.py` |
| 2. gwemu + retro-go | `core_dtcm_ok=1`, bonus pool 153,600 B, 1,000,001 insns — matches tier 1 |
| 3. Hardware | 50.50 cyc/insn, 5.54 MIPS |

`tests/asm_baseline.json`: 577 symbols, 8252 insns, 29904 bytes.
Dispatch-table symbols are data; `objdump` counts them as instructions.

Cycles/insn: 57.50 pre-fold → 50.00 fold + absolute dispatch → 50.50 segmentation
→ 50.50 unchanged through 16-bit, the 66/67 axes, segment loads, far transfers
and REP: the width macros vanish at NSH=0 and none of it sits on the 32-bit hot
path. The fold and dispatch were measured together and are not separable.

## Phase 0 — repo baseline

- [x] ITCM/DTCM placement; generated `x86_offsets.inc`; flag layer; PIT reload
      and distinct `X86_EXIT_*`; dead code and `.text.discard` removed
- [x] `scripts/` CLIs + `scripts/common/` modules; `asm_metrics.py` baseline
- [x] Hardware tier reachable; retro-go core with ITCM segment, DTCM via
      `mem_ctl`, and the 153,600-byte LUT8 bonus pool
- [ ] Assertions in `test_game.py` / `test_benchmark.py`; `_Static_assert` for `hw_accel.S` PIC offsets
- [ ] Realign `docs/01`–`03`, `05`. [`docs/03`](docs/03_devices_and_hle.md) §3 still describes the rejected INT 21h bypass. `docs/04`, `docs/06` current.

## Phase 1 — CPU foundations

- [x] Checked fold: `host = lin + k`, `FOLD_INVALID` + `ADDS`/`BMI`, 171 inline walks replaced
- [x] Absolute-pointer dispatch (`LDR.W PC, [R2, R0, LSL #2]`): −4 insns per
      dispatched instruction, ±64 KB anchor constraint gone
- [x] `TBH` table audit — 11 sites, all anchors correct
- [x] R6 reserved as the active segment descriptor pointer (net −48 insns)
- [x] Segmentation on the ModRM path: DS default, SS for `[(E)BP]`/ESP-EBP SIB,
      all six override prefixes at 2 insns each. +0.5 cyc/insn on silicon
- [x] `MOV` 88/89/8A/8B memory forms (previously `.exit_loop`)
- [x] 16-bit operand size, via width macros and the `LSL #NSH` ALU-core collapse
      (shifts vanish at NSH=0, so 32-bit is unchanged)
- [x] addr16 ModRM decoder — a different grammar, not a narrower one
- [x] Dynamic `66`/`67` as independent axes, per instruction, in either default
      mode: four DTCM mode records, one `EOR` per prefix, no per-instruction
      cost. +1 insn per memory operand for the indirect decoder
- [x] Segment bases outside the ModRM path
  - [x] stack ops — all stack access goes through SS.base with SP wrap
  - [x] string ops — DS:(E)SI → ES:(E)DI, override honoured, index wrap
  - [x] `PUSH`/`POP`/`CALL`/`RET`
  - [ ] `moffs` `A0`–`A3` (offset should follow ADDRESS size)
- [x] Segment register loads — `MOV Sreg,Ew` / `MOV Ew,Sreg`, `PUSH`/`POP` Sreg,
      `LES`/`LDS`. Real mode installs base = sel<<4, limit 0xFFFF; CR0.PE traps
      out rather than quietly installing a real-mode base
- [x] Far `JMP`/`CALL` (`EA`/`9A`) and `RETF`
- [x] `REP`/`REPNE` by rewind — one element per pass through the fetch loop, so
      a long `REP MOVS` cannot stall the PIT
- [ ] `XLAT`, BCD (`27`/`2F`/`37`/`3F`), `WAIT`, `INS`/`OUTS`, `BOUND`/`ARPL`
- [ ] Limit checks off the R6 descriptor, incl. real-mode 64 KB wrap
- [ ] Lazy PF/AF (currently eager, ~9 of ~16 insns per ALU op)
- [ ] Flag coverage: 8-bit `NEG`, `MUL`/`DIV`, rotates, 8-bit shift cores, shift `OF` for count==1, count==0 for every shift form
- [ ] Size-aware `MEM_R*`/`MEM_W*` macros + CI grep banning raw `[R9,` outside them
- [ ] Software TLB above the 16 MB direct window

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
- [ ] Hardware keyboard: port 0x60, IRQ 1, INT 09h feeding the BIOS ring
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
