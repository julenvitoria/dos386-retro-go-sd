# Status

Last verified 2026-08-12. Run instructions: [`TOOLING.md`](TOOLING.md).
Roadmap and phase definitions: [`PLAN.md`](PLAN.md).

## Verification state

| Tier | State |
|---|---|
| 1. gwemu bare-metal | 159/159 `tests/test_opcodes.py` |
| 2. gwemu + retro-go | `core_dtcm_ok=1`, bonus pool 153,600 B, 1,000,001 insns — matches tier 1 |
| 3. Hardware | 50.50 cyc/insn, 5.54 MIPS |

`tests/asm_baseline.json`: 580 symbols, 7880 insns, 28412 bytes.
Dispatch-table symbols are data; `objdump` counts them as instructions.

Cycles/insn: 57.50 pre-fold → 50.00 fold + absolute dispatch → 50.50 segmentation
→ 50.50 unchanged through 16-bit, the 66/67 axes, segment loads, far transfers
and REP: the width macros vanish at NSH=0 and none of it sits on the 32-bit hot
path. The fold and dispatch were measured together and are not separable.

## Phase 0 — repo baseline

- [x] `.itcram_hot` → ITCMRAM, CPU state → DTCM
- [x] `x86_offsets.inc` generated from the C struct; CS 48/60 disagreement gone
- [x] Flag layer: 8/32-bit ALU, both `TEST` forms, `INC`/`DEC`, 32-bit shifts, `NEG`
- [x] PIT reload and distinct `X86_EXIT_*` reasons
- [x] Dead code removed; `.text.discard` eliminated
- [x] `scripts/` CLIs + `scripts/common/` modules; `asm_metrics.py` baseline
- [x] Hardware tier reachable (`OpenOCDBackend`, fault handlers)
- [x] Retro-go core: ITCM segment, DTCM via `mem_ctl`, 153,600-byte LUT8 bonus pool
- [ ] Assertions in `test_game.py` / `test_benchmark.py`; `_Static_assert` for `hw_accel.S` PIC offsets
- [ ] Realign `docs/01`–`03`, `05`. [`docs/03`](docs/03_devices_and_hle.md) §3 still describes the rejected INT 21h bypass. `docs/04`, `docs/06` current.

## Phase 1 — CPU foundations

- [x] Checked fold: `host = lin + k`, `FOLD_INVALID` + `ADDS`/`BMI`, 171 inline walks replaced
- [x] Absolute-pointer dispatch: `LDR.W PC, [R2, R0, LSL #2]`, entries `.word handler + 1`
  - −4 insns per dispatched instruction (fetch loop 12 → 8), −4 per 0F dispatch
  - ±64 KB anchor constraint removed; cold handlers may live in AXI
  - +2 KB ITCM for table width. Move to DTCM deferred; ITCM at ≈24 KB of 64 KB
- [x] `TBH` table audit — 11 sites, all anchors correct
- [x] R6 reserved as active segment descriptor pointer (net −48 insns to free)
- [x] Segmentation, ModRM path
  - default DS; SS for `[(E)BP+disp]` and SIB based on ESP/EBP
  - all six override prefixes (`26 2E 36 3E 64 65`): 2 insns, no store, no branch
  - +1 insn at loop top, +0.5 cyc/insn on silicon
- [x] `MOV` 88/89/8A/8B memory forms (previously `.exit_loop`)
- [x] 16-bit operand size (the `_16` pass had been a duplicate of the 32-bit
      handlers; `MEM_READ_SZ`/`STACK_INC` were referenced nowhere). Width macros
      `LD_V`/`ST_V`/`LDREG_V`/`STREG_V`/`LDS_V`/`EIP_ADV_V`/`SP_*`; ALU-core
      collapse via `LSL #NSH` so host NZCV are the x86 flags for the width, and
      the shifts vanish at NSH=0; `CALL` pushes IP not linear EIP; `rel16`
      sign-extends; IP wraps at 16 bits
- [x] addr16 ModRM decoder — a different grammar, not a narrower one
- [x] Dynamic `66`/`67` as independent axes, per instruction, in either default
      mode. Four 2 KB DTCM mode records, each carrying its dispatch table, 0F
      table and address-size decoder; R2 already holds the record at prefix
      entry, so each prefix is one `EOR` plus re-entry — no per-instruction
      cost. +1 instruction per memory operand for the indirect decoder.
- [x] Segment bases outside the ModRM path
  - [x] stack ops — all stack access goes through SS.base with SP wrap
  - [x] string ops — DS:(E)SI → ES:(E)DI, override honoured, index wrap
  - [x] `PUSH`/`POP`/`CALL`/`RET`
  - [ ] `moffs` `A0`–`A3` (offset should follow ADDRESS size)
- [x] Segment register loads — `MOV Sreg,Ew` / `MOV Ew,Sreg`, `PUSH`/`POP` Sreg,
      `LES`/`LDS`. Real mode installs base = sel<<4, limit 0xFFFF; CR0.PE traps
      out rather than quietly installing a real-mode base
- [x] Far `JMP`/`CALL` (`EA`/`9A`) and `RETF` — `RETF` had been popping CS and
      discarding it, so a far return kept running in the caller's segment
- [x] `REP`/`REPNE` — implemented by rewind, one element per pass through the
      fetch loop, so a long `REP MOVS` cannot stall the PIT. The continuation
      test sits on the string ops' shared exit and costs nothing elsewhere
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
      cleared, CS installed; `IRET` inverts it. The old path read 8-byte gates,
      pushed 32-bit frames and a literal 0 for CS, and never installed it, so
      no vector could ever have worked. `CR0.PE` traps out
- [x] `0F 0B imm8` hypercall, gated on `hle_enabled`, real `#UD` when off;
      sets `exit_reason=X86_EXIT_HLE_INT`, `exit_info=selector`
- [ ] FreeDOS bring-up; INT 13h → FatFs; INT 10h/16h/1Ah
- [ ] Video (CGA/EGA → LCD), input, PIT timing, PC speaker
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
