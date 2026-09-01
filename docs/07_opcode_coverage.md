# 07 — x86 Opcode Coverage Audit

**The per-opcode tables in this document are GENERATED. Do not hand-edit them.**

    python3 scripts/gen_opcode_coverage.py --write     # regenerate
    python3 scripts/gen_opcode_coverage.py --check     # CI: is this stale?

Everything between the `BEGIN GENERATED` / `END GENERATED` markers is derived
from `opcode_table.S`, `opcode_0f_table.S` and the handler bodies in
`cpu_exec_handlers.S` / `cpu_exec_handlers_0f.S` by parsing them. The sections
after the markers are hand-written analysis that a parser cannot produce.

## Why this document is generated now

It was hand-written, and it rotted twice.

Commit `7f4c12a` fixed the `0F 8x` near-`Jcc` rel16 defect and the `E0`–`E3`
`LOOP`/`JCXZ` address-size gap. **249 commits later this document still listed
both as DEFECT.** `60` PUSHA, `61` POPA, `68` PUSH Iv and `84`/`85` TEST were
likewise repaired while the table still called them broken, and `27`/`2F`/`37`/
`3F` (BCD), `D4`/`D5` (AAM/AAD) and `6C`–`6F` (INS/OUTS) were implemented while
the table still called them `.exit_loop`. A sprint was planned against those
rows and an agent was mis-tasked as a result.

A stale coverage document is worse than no coverage document: it is confidently
wrong, and it is read by people deciding what to build. Hence the generator.

Target class is **386 real mode**. 486/Pentium+ instructions are out of scope
rather than gaps.

## What the generator will NOT tell you

It reports mechanical properties of the source text — which dispatch slot
reaches a handler, whether that handler is emitted once or per operand size,
which width primitives and flag macros are reachable from its body. It follows
one handler's delegation into shared cores (`L(ALU_DISPATCH_8)`,
`L(SHIFT_TBH_32)`, …) so an ALU handler is not falsely reported as writing no
flags.

**It never claims a handler is CORRECT.** Whether a flag computation matches
the 386, whether a segment base is applied in the right place, whether an
addressing mode is decoded at the right width — none of that is derivable by
grep, and all of it is marked UNKNOWN. A `REVIEW:` marker means "a human should
read this handler", not "this handler is broken"; an empty warnings cell means
"no marker fired", not "verified correct". `tests/test_opcodes.py` and the
hand-written sections below are where semantic verdicts live.


## How the two operand-size passes work (and how to read the "dual/AGN" column)

`cpu_exec.S` includes `cpu_exec_handlers.S` **twice** — once with `OPSZ 4 / NSH 0` and
once with `OPSZ 2 / NSH 16`. A handler declared `OP` is emitted in both passes with a
`_32` / `_16` suffix and each dispatch table points at its own copy. A handler declared
`OP_AGNOSTIC` is emitted **once** and shared by both tables.

Therefore: **any width-sensitive instruction declared `OP_AGNOSTIC` is 32-bit in both
modes.** Every table row below carries the handler symbol and `(dual)` or `(AGN)` so this
is checkable at a glance. Cross-checking the `.set` alias list in
`cpu_exec_handlers_0f.S` confirms every `OP`-declared handler has both a `_32` and a
`_16` alias — there are no dangling aliases and no handler used by a table that is
undefined.

The width-correct primitives are `LD_V/ST_V/LDS_V`, `LDREG_V/STREG_V`, `EIP_ADV_V`,
`EIP_WRAP`, `PUSH_V/POP_V`, `SP_DEC/SP_INC/SP_WRAP`, `OPSZ`, `NSH`, `VNORM/VDENORM/
VMASK/VSEXT/VTEST`. A dual-emitted handler that still uses a bare `LDR`/`STR`/`#4` is
flagged below.

`PUSH_V`/`POP_V` are the only stack primitives that apply `SS.base` and wrap SP at the
stack width. Thirteen handlers still open-code the stack with `LDR R4,[R8,#24]` /
`SUB #4` and are listed as defects.

### Defects by category (implemented but wrong — these fail *silently*)

| Category | Count of affected opcode slots | Examples |
|---|---|---|
| Missing flags entirely | 13 | `84`, `A8`, `A9`, `F6 /0`, `FE /0 /1`, `FF /0 /1`, `69`, `6B`, `D4`, `D5` |
| Silently 32-bit in 16-bit mode (`OP_AGNOSTIC` on a width-sensitive body, or bare `LDR`/`STR`/`#4`) | 21 | `87`, `91`–`97`, `98`, `99`, `8D`, `A1`, `CA`, `C1`, `D1`, `D3`, `0F 80`–`0F 8F` |
| Stack accessed as a raw linear address (no `SS.base`, hard-coded ±4) | 13 | `60`, `61`, `68`, `6A`, `8F`, `9C`, `9D`, `C2`, `C8`, `C9`, `CA`, `FF /2`, `FF /6` |
| Memory ModRM form missing (register form only) | 6 | `85`, `86`, `87`, `8F`, `C6`, `C7` |
| Non-stack memory operand used without a segment base | 5 | `A0`, `A1`, `A2`, `A3`, `D7` |
| Address-size axis ignored (uses full `ECX`/32-bit moffs) | 6 | `E0`, `E1`, `E2`, `E3`, `A0`, `A2` |
| CS.base mishandled on a control transfer | 4 | `C2`, `CA`, `FF /2`, `FF /4` |
| Implemented as a silent no-op | 1 | `84` (TEST). `0F 01 /2` LGDT is no longer a no-op. |

Slots can appear in more than one row. Counting distinct one-byte slots, **43 of the 235
implemented opcodes have at least one defect** and **7 more are partial** (register form
only). That leaves roughly **185 one-byte opcodes believed correct**.

<!-- BEGIN GENERATED -- scripts/gen_opcode_coverage.py -->

### Generated hard numbers

| Metric | Count |
|---|---|
| One-byte slots with a handler | **255 / 256** |
| One-byte slots dispatching to `.exit_loop` | **1 / 256** |
| Two-byte (0F) slots with a handler | **64 / 256** |
| Two-byte slots dispatching to `.exit_loop` | **192 / 256** |
| Implemented slots carrying at least one mechanical warning | **190** |

Every row below is DERIVED. `status` is `reached` or `.exit_loop`, taken from
the dispatch table. `mode` is `OP` (dual-emitted) or `OP_AGNOSTIC`, taken from the
handler declaration. `warnings` are greps over the handler body, listed in the
legend below. **Semantic correctness of a flag or an addressing computation is
not derivable and is reported as UNKNOWN -- an empty `warnings` cell means no
mechanical warning fired, NOT that the handler is correct.**

Warning legend:

| warning | what was grepped |
|---|---|
| `AGN+raw32` | declared `OP_AGNOSTIC` yet the body mixes width primitives with a bare `LDR.W`/`STR.W` or a hard-coded `#4` |
| `AGN, bare 32-bit body` | declared `OP_AGNOSTIC` and uses no width primitive at all -- 32-bit in both passes unless the instruction is byte-sized |
| `dual but no width primitive` | declared `OP` but the body has no `LD_V`/`ST_V`/`LDREG_V`/...  |
| `open-coded stack` | touches ESP as `[R8, #24]` and never calls `PUSH_V`/`POP_V` |
| `ModRM by symbol (ignores 0x67)` | calls `L(DECODE_MODRM_16/32)` directly instead of through `[R2, #REC_MODRM_FN]` |
| `no X86_FLAGS_* macro` | the body invokes no flag macro; benign for MOV/PUSH/JMP, a real gap for ALU |


### One-byte opcodes -- all 256 (generated)

| Op | Status | Handler | Mode | Flag macros | Warnings |
|---|---|---|---|---|---|
| 00 | reached | `.op_alu_eb_gb` | AGN | - | - |
| 01 | reached | `.op_alu_ev_gv` | dual | - | - |
| 02 | reached | `.op_alu_gb_eb` | AGN | - | - |
| 03 | reached | `.op_alu_gv_ev` | dual | - | - |
| 04 | reached | `.op_alu_al_imm8` | AGN | - | - |
| 05 | reached | `.op_alu_eax_imm32` | dual | - | - |
| 06 | reached | `.op_push_es_06` | dual | - | no flag macro reachable |
| 07 | reached | `.op_pop_es_07` | dual | - | no flag macro reachable |
| 08 | reached | `.op_alu_eb_gb` | AGN | - | - |
| 09 | reached | `.op_alu_ev_gv` | dual | - | - |
| 0A | reached | `.op_alu_gb_eb` | AGN | - | - |
| 0B | reached | `.op_alu_gv_ev` | dual | - | - |
| 0C | reached | `.op_alu_al_imm8` | AGN | - | - |
| 0D | reached | `.op_alu_eax_imm32` | dual | - | - |
| 0E | reached | `.op_push_cs_0E` | dual | - | no flag macro reachable |
| 0F | reached | `.op_0f_0F` | dual | - | no flag macro reachable |
| 10 | reached | `.op_alu_eb_gb` | AGN | - | - |
| 11 | reached | `.op_alu_ev_gv` | dual | - | - |
| 12 | reached | `.op_alu_gb_eb` | AGN | - | - |
| 13 | reached | `.op_alu_gv_ev` | dual | - | - |
| 14 | reached | `.op_alu_al_imm8` | AGN | - | - |
| 15 | reached | `.op_alu_eax_imm32` | dual | - | - |
| 16 | reached | `.op_push_ss_16` | dual | - | no flag macro reachable |
| 17 | reached | `.op_pop_ss_17` | dual | - | no flag macro reachable |
| 18 | reached | `.op_alu_eb_gb` | AGN | - | - |
| 19 | reached | `.op_alu_ev_gv` | dual | - | - |
| 1A | reached | `.op_alu_gb_eb` | AGN | - | - |
| 1B | reached | `.op_alu_gv_ev` | dual | - | - |
| 1C | reached | `.op_alu_al_imm8` | AGN | - | - |
| 1D | reached | `.op_alu_eax_imm32` | dual | - | - |
| 1E | reached | `.op_push_ds_1E` | dual | - | no flag macro reachable |
| 1F | reached | `.op_pop_ds_1F` | dual | - | no flag macro reachable |
| 20 | reached | `.op_alu_eb_gb` | AGN | - | - |
| 21 | reached | `.op_alu_ev_gv` | dual | - | - |
| 22 | reached | `.op_alu_gb_eb` | AGN | - | - |
| 23 | reached | `.op_alu_gv_ev` | dual | - | - |
| 24 | reached | `.op_alu_al_imm8` | AGN | - | - |
| 25 | reached | `.op_alu_eax_imm32` | dual | - | - |
| 26 | reached | `.op_seg_es_26` | AGN | - | no flag macro reachable |
| 27 | reached | `.op_daa_27` | AGN | - | no flag macro reachable |
| 28 | reached | `.op_alu_eb_gb` | AGN | - | - |
| 29 | reached | `.op_alu_ev_gv` | dual | - | - |
| 2A | reached | `.op_alu_gb_eb` | AGN | - | - |
| 2B | reached | `.op_alu_gv_ev` | dual | - | - |
| 2C | reached | `.op_alu_al_imm8` | AGN | - | - |
| 2D | reached | `.op_alu_eax_imm32` | dual | - | - |
| 2E | reached | `.op_seg_cs_2E` | AGN | - | no flag macro reachable |
| 2F | reached | `.op_das_2F` | AGN | - | no flag macro reachable |
| 30 | reached | `.op_alu_eb_gb` | AGN | - | - |
| 31 | reached | `.op_alu_ev_gv` | dual | - | - |
| 32 | reached | `.op_alu_gb_eb` | AGN | - | - |
| 33 | reached | `.op_alu_gv_ev` | dual | - | - |
| 34 | reached | `.op_alu_al_imm8` | AGN | - | - |
| 35 | reached | `.op_alu_eax_imm32` | dual | - | - |
| 36 | reached | `.op_seg_ss_36` | AGN | - | no flag macro reachable |
| 37 | reached | `.op_aaa_37` | AGN | - | no flag macro reachable |
| 38 | reached | `.op_alu_eb_gb` | AGN | - | - |
| 39 | reached | `.op_alu_ev_gv` | dual | - | - |
| 3A | reached | `.op_alu_gb_eb` | AGN | - | - |
| 3B | reached | `.op_alu_gv_ev` | dual | - | - |
| 3C | reached | `.op_alu_al_imm8` | AGN | - | - |
| 3D | reached | `.op_alu_eax_imm32` | dual | - | - |
| 3E | reached | `.op_seg_ds_3E` | AGN | - | no flag macro reachable |
| 3F | reached | `.op_aas_3F` | AGN | - | no flag macro reachable |
| 40 | reached | `.op_inc_r32` | dual | INCDEC | - |
| 41 | reached | `.op_inc_r32` | dual | INCDEC | - |
| 42 | reached | `.op_inc_r32` | dual | INCDEC | - |
| 43 | reached | `.op_inc_r32` | dual | INCDEC | - |
| 44 | reached | `.op_inc_r32` | dual | INCDEC | - |
| 45 | reached | `.op_inc_r32` | dual | INCDEC | - |
| 46 | reached | `.op_inc_r32` | dual | INCDEC | - |
| 47 | reached | `.op_inc_r32` | dual | INCDEC | - |
| 48 | reached | `.op_dec_r32` | dual | INCDEC | - |
| 49 | reached | `.op_dec_r32` | dual | INCDEC | - |
| 4A | reached | `.op_dec_r32` | dual | INCDEC | - |
| 4B | reached | `.op_dec_r32` | dual | INCDEC | - |
| 4C | reached | `.op_dec_r32` | dual | INCDEC | - |
| 4D | reached | `.op_dec_r32` | dual | INCDEC | - |
| 4E | reached | `.op_dec_r32` | dual | INCDEC | - |
| 4F | reached | `.op_dec_r32` | dual | INCDEC | - |
| 50 | reached | `.op_push_r32` | dual | - | no flag macro reachable |
| 51 | reached | `.op_push_r32` | dual | - | no flag macro reachable |
| 52 | reached | `.op_push_r32` | dual | - | no flag macro reachable |
| 53 | reached | `.op_push_r32` | dual | - | no flag macro reachable |
| 54 | reached | `.op_push_r32` | dual | - | no flag macro reachable |
| 55 | reached | `.op_push_r32` | dual | - | no flag macro reachable |
| 56 | reached | `.op_push_r32` | dual | - | no flag macro reachable |
| 57 | reached | `.op_push_r32` | dual | - | no flag macro reachable |
| 58 | reached | `.op_pop_r32` | dual | - | no flag macro reachable |
| 59 | reached | `.op_pop_r32` | dual | - | no flag macro reachable |
| 5A | reached | `.op_pop_r32` | dual | - | no flag macro reachable |
| 5B | reached | `.op_pop_r32` | dual | - | no flag macro reachable |
| 5C | reached | `.op_pop_r32` | dual | - | no flag macro reachable |
| 5D | reached | `.op_pop_r32` | dual | - | no flag macro reachable |
| 5E | reached | `.op_pop_r32` | dual | - | no flag macro reachable |
| 5F | reached | `.op_pop_r32` | dual | - | no flag macro reachable |
| 60 | reached | `.op_pusha_60` | dual | - | no flag macro reachable |
| 61 | reached | `.op_popa_61` | dual | - | no flag macro reachable |
| 62 | reached | `.op_bound_62` | dual | - | - |
| 63 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 64 | reached | `.op_seg_fs_64` | AGN | - | no flag macro reachable |
| 65 | reached | `.op_seg_gs_65` | AGN | - | no flag macro reachable |
| 66 | reached | `.op_pfx_66` | AGN | - | no flag macro reachable |
| 67 | reached | `.op_pfx_67` | AGN | - | REVIEW: AGN body mixes width primitives with 26 bare word op(s), no flag macro reachable |
| 68 | reached | `.op_push_imm32_68` | dual | - | no flag macro reachable |
| 69 | reached | `.op_imul_imm32_69` | dual | - | - |
| 6A | reached | `.op_push_imm8_6A` | dual | - | no flag macro reachable |
| 6B | reached | `.op_imul_imm8_6B` | dual | - | no flag macro reachable |
| 6C | reached | `.op_insb_6C` | AGN | - | no flag macro reachable |
| 6D | reached | `.op_insv_6D` | dual | - | no flag macro reachable |
| 6E | reached | `.op_outsb_6E` | AGN | - | no flag macro reachable |
| 6F | reached | `.op_outsv_6F` | dual | - | no flag macro reachable |
| 70 | reached | `.op_jcc_rel8_7X` | AGN | - | REVIEW: AGN body mixes width primitives with 37 bare word op(s) |
| 71 | reached | `.op_jcc_rel8_7X` | AGN | - | REVIEW: AGN body mixes width primitives with 37 bare word op(s) |
| 72 | reached | `.op_jcc_rel8_7X` | AGN | - | REVIEW: AGN body mixes width primitives with 37 bare word op(s) |
| 73 | reached | `.op_jcc_rel8_7X` | AGN | - | REVIEW: AGN body mixes width primitives with 37 bare word op(s) |
| 74 | reached | `.op_jcc_rel8_7X` | AGN | - | REVIEW: AGN body mixes width primitives with 37 bare word op(s) |
| 75 | reached | `.op_jcc_rel8_7X` | AGN | - | REVIEW: AGN body mixes width primitives with 37 bare word op(s) |
| 76 | reached | `.op_jcc_rel8_7X` | AGN | - | REVIEW: AGN body mixes width primitives with 37 bare word op(s) |
| 77 | reached | `.op_jcc_rel8_7X` | AGN | - | REVIEW: AGN body mixes width primitives with 37 bare word op(s) |
| 78 | reached | `.op_jcc_rel8_7X` | AGN | - | REVIEW: AGN body mixes width primitives with 37 bare word op(s) |
| 79 | reached | `.op_jcc_rel8_7X` | AGN | - | REVIEW: AGN body mixes width primitives with 37 bare word op(s) |
| 7A | reached | `.op_jcc_rel8_7X` | AGN | - | REVIEW: AGN body mixes width primitives with 37 bare word op(s) |
| 7B | reached | `.op_jcc_rel8_7X` | AGN | - | REVIEW: AGN body mixes width primitives with 37 bare word op(s) |
| 7C | reached | `.op_jcc_rel8_7X` | AGN | - | REVIEW: AGN body mixes width primitives with 37 bare word op(s) |
| 7D | reached | `.op_jcc_rel8_7X` | AGN | - | REVIEW: AGN body mixes width primitives with 37 bare word op(s) |
| 7E | reached | `.op_jcc_rel8_7X` | AGN | - | REVIEW: AGN body mixes width primitives with 37 bare word op(s) |
| 7F | reached | `.op_jcc_rel8_7X` | AGN | - | REVIEW: AGN body mixes width primitives with 37 bare word op(s) |
| 80 | reached | `.op_alu_eb_imm8_80` | AGN | - | - |
| 81 | reached | `.op_alu_ev_imm32_81` | dual | - | - |
| 82 | reached | `.op_alu_eb_imm8_80` | AGN | - | - |
| 83 | reached | `.op_alu_ev_imm8_83` | dual | - | - |
| 84 | reached | `.op_test_eb_gb_84` | AGN | LOGIC | - |
| 85 | reached | `.op_test_ev_gv_85` | dual | LOGIC | - |
| 86 | reached | `.op_xchg_eb_gb_86` | AGN | - | REVIEW: AGN body mixes width primitives with 1 bare word op(s), no flag macro reachable |
| 87 | reached | `.op_xchg_ev_gv_87` | dual | - | no flag macro reachable |
| 88 | reached | `.op_mov_eb_gb_88` | AGN | - | REVIEW: AGN body mixes width primitives with 1 bare word op(s), no flag macro reachable |
| 89 | reached | `.op_mov_ev_gv_89` | dual | ARITH, LOGIC | - |
| 8A | reached | `.op_mov_gb_eb_8A` | AGN | ARITH, LOGIC | REVIEW: AGN body mixes width primitives with 10 bare word op(s) |
| 8B | reached | `.op_mov_gv_ev_8B` | dual | ARITH, LOGIC | - |
| 8C | reached | `.op_mov_Ew_seg_8C` | AGN | - | REVIEW: AGN body mixes width primitives with 13 bare word op(s), no flag macro reachable |
| 8D | reached | `.op_lea_8D` | dual | - | no flag macro reachable |
| 8E | reached | `.op_mov_seg_Ew_8E` | AGN | - | REVIEW: AGN body mixes width primitives with 13 bare word op(s), no flag macro reachable |
| 8F | reached | `.op_pop_ev_8F` | dual | ARITH, LOGIC | - |
| 90 | reached | `.op_nop` | AGN | - | no flag macro reachable |
| 91 | reached | `.op_xchg_eax_r32` | dual | - | no flag macro reachable |
| 92 | reached | `.op_xchg_eax_r32` | dual | - | no flag macro reachable |
| 93 | reached | `.op_xchg_eax_r32` | dual | - | no flag macro reachable |
| 94 | reached | `.op_xchg_eax_r32` | dual | - | no flag macro reachable |
| 95 | reached | `.op_xchg_eax_r32` | dual | - | no flag macro reachable |
| 96 | reached | `.op_xchg_eax_r32` | dual | - | no flag macro reachable |
| 97 | reached | `.op_xchg_eax_r32` | dual | - | no flag macro reachable |
| 98 | reached | `.op_cwde_98` | dual | - | no flag macro reachable |
| 99 | reached | `.op_cdq_99` | dual | - | no flag macro reachable |
| 9A | reached | `.op_call_far_9A` | dual | - | no flag macro reachable |
| 9B | reached | `.op_wait_9B` | AGN | - | no flag macro reachable |
| 9C | reached | `.op_pushfd_9C` | dual | - | no flag macro reachable |
| 9D | reached | `.op_popfd_9D` | dual | - | no flag macro reachable |
| 9E | reached | `.op_sahf_9E` | AGN | - | no flag macro reachable |
| 9F | reached | `.op_lahf_9F` | AGN | - | no flag macro reachable |
| A0 | reached | `.op_mov_al_moffs8_A0` | AGN | - | no flag macro reachable |
| A1 | reached | `.op_mov_eax_moffs32_A1` | dual | - | no flag macro reachable |
| A2 | reached | `.op_mov_moffs8_al_A2` | AGN | - | no flag macro reachable |
| A3 | reached | `.op_mov_moffs32_eax_A3` | dual | - | no flag macro reachable |
| A4 | reached | `.op_movsb_A4` | AGN | - | no flag macro reachable |
| A5 | reached | `.op_movsd_A5` | dual | - | no flag macro reachable |
| A6 | reached | `.op_cmpsb_A6` | AGN | ARITH | - |
| A7 | reached | `.op_cmpsd_A7` | dual | ARITH | - |
| A8 | reached | `.op_test_al_imm8_A8` | AGN | LOGIC | - |
| A9 | reached | `.op_test_eax_imm32_A9` | dual | LOGIC | - |
| AA | reached | `.op_stosb_AA` | AGN | - | no flag macro reachable |
| AB | reached | `.op_stosd_AB` | dual | - | no flag macro reachable |
| AC | reached | `.op_lodsb_AC` | AGN | - | no flag macro reachable |
| AD | reached | `.op_lodsd_AD` | dual | - | no flag macro reachable |
| AE | reached | `.op_scasb_AE` | AGN | ARITH | - |
| AF | reached | `.op_scasd_AF` | dual | ARITH | - |
| B0 | reached | `.op_mov_r8_imm8` | AGN | - | no flag macro reachable |
| B1 | reached | `.op_mov_r8_imm8` | AGN | - | no flag macro reachable |
| B2 | reached | `.op_mov_r8_imm8` | AGN | - | no flag macro reachable |
| B3 | reached | `.op_mov_r8_imm8` | AGN | - | no flag macro reachable |
| B4 | reached | `.op_mov_r8_imm8` | AGN | - | no flag macro reachable |
| B5 | reached | `.op_mov_r8_imm8` | AGN | - | no flag macro reachable |
| B6 | reached | `.op_mov_r8_imm8` | AGN | - | no flag macro reachable |
| B7 | reached | `.op_mov_r8_imm8` | AGN | - | no flag macro reachable |
| B8 | reached | `.op_mov_r32_imm32` | dual | - | no flag macro reachable |
| B9 | reached | `.op_mov_r32_imm32` | dual | - | no flag macro reachable |
| BA | reached | `.op_mov_r32_imm32` | dual | - | no flag macro reachable |
| BB | reached | `.op_mov_r32_imm32` | dual | - | no flag macro reachable |
| BC | reached | `.op_mov_r32_imm32` | dual | - | no flag macro reachable |
| BD | reached | `.op_mov_r32_imm32` | dual | - | no flag macro reachable |
| BE | reached | `.op_mov_r32_imm32` | dual | - | no flag macro reachable |
| BF | reached | `.op_mov_r32_imm32` | dual | - | no flag macro reachable |
| C0 | reached | `.op_shift_eb_imm8_C0` | AGN | - | no flag macro reachable |
| C1 | reached | `.op_shift_ev_imm8_C1` | dual | - | no flag macro reachable |
| C2 | reached | `.op_ret_imm16_C2` | dual | - | no flag macro reachable |
| C3 | reached | `.op_ret_C3` | dual | - | no flag macro reachable |
| C4 | reached | `.op_les_C4` | dual | - | no flag macro reachable |
| C5 | reached | `.op_lds_C5` | dual | - | no flag macro reachable |
| C6 | reached | `.op_mov_eb_imm8_C6` | AGN | - | no flag macro reachable |
| C7 | reached | `.op_mov_ev_imm32_C7` | dual | - | no flag macro reachable |
| C8 | reached | `.op_enter_C8` | dual | - | no flag macro reachable |
| C9 | reached | `.op_leave_C9` | dual | - | no flag macro reachable |
| CA | reached | `.op_retf_imm16_CA` | dual | - | no flag macro reachable |
| CB | reached | `.op_retf_CB` | dual | - | no flag macro reachable |
| CC | reached | `.op_int3_CC` | dual | - | - |
| CD | reached | `.op_int_imm8_CD` | dual | - | - |
| CE | reached | `.op_into_CE` | dual | - | - |
| CF | reached | `.op_iret_CF` | dual | - | - |
| D0 | reached | `.op_shift_eb_1_D0` | AGN | - | no flag macro reachable |
| D1 | reached | `.op_shift_ev_1_D1` | dual | - | no flag macro reachable |
| D2 | reached | `.op_shift_eb_cl_D2` | AGN | - | - |
| D3 | reached | `.op_shift_ev_cl_D3` | dual | - | - |
| D4 | reached | `.op_aam_D4` | AGN | - | no flag macro reachable |
| D5 | reached | `.op_aad_D5` | AGN | - | no flag macro reachable |
| D6 | reached | `.op_salc_D6` | AGN | - | no flag macro reachable |
| D7 | reached | `.op_xlat_D7` | AGN | - | REVIEW: no width primitive, 2 bare word op(s), no flag macro reachable |
| D8 | reached | `.op_esc_x87` | AGN | - | REVIEW: no width primitive, 2 bare word op(s), no flag macro reachable |
| D9 | reached | `.op_esc_x87` | AGN | - | REVIEW: no width primitive, 2 bare word op(s), no flag macro reachable |
| DA | reached | `.op_esc_x87` | AGN | - | REVIEW: no width primitive, 2 bare word op(s), no flag macro reachable |
| DB | reached | `.op_esc_x87` | AGN | - | REVIEW: no width primitive, 2 bare word op(s), no flag macro reachable |
| DC | reached | `.op_esc_x87` | AGN | - | REVIEW: no width primitive, 2 bare word op(s), no flag macro reachable |
| DD | reached | `.op_esc_x87` | AGN | - | REVIEW: no width primitive, 2 bare word op(s), no flag macro reachable |
| DE | reached | `.op_esc_x87` | AGN | - | REVIEW: no width primitive, 2 bare word op(s), no flag macro reachable |
| DF | reached | `.op_esc_x87` | AGN | - | REVIEW: no width primitive, 2 bare word op(s), no flag macro reachable |
| E0 | reached | `.op_loopnz_E0` | AGN | - | REVIEW: no width primitive, 4 bare word op(s), no flag macro reachable |
| E1 | reached | `.op_loopz_E1` | AGN | - | REVIEW: no width primitive, 4 bare word op(s), no flag macro reachable |
| E2 | reached | `.op_loop_E2` | AGN | - | REVIEW: no width primitive, 4 bare word op(s), no flag macro reachable |
| E3 | reached | `.op_jcxz_E3` | AGN | - | REVIEW: no width primitive, 4 bare word op(s), no flag macro reachable |
| E4 | reached | `.op_in_al_imm8_E4` | AGN | - | no flag macro reachable |
| E5 | reached | `.op_in_eax_imm8_E5` | dual | - | no flag macro reachable |
| E6 | reached | `.op_out_imm8_al_E6` | AGN | - | no flag macro reachable |
| E7 | reached | `.op_out_imm8_eax_E7` | dual | - | no flag macro reachable |
| E8 | reached | `.op_call_rel32_E8` | dual | - | no flag macro reachable |
| E9 | reached | `.op_jmp_rel32_E9` | dual | - | no flag macro reachable |
| EA | reached | `.op_jmp_far_EA` | dual | - | no flag macro reachable |
| EB | reached | `.op_jmp_rel8_EB` | AGN | - | no flag macro reachable |
| EC | reached | `.op_in_al_dx_EC` | AGN | - | no flag macro reachable |
| ED | reached | `.op_in_eax_dx_ED` | dual | - | no flag macro reachable |
| EE | reached | `.op_out_dx_al_EE` | AGN | ARITH, LOGIC | - |
| EF | reached | `.op_out_dx_eax_EF` | dual | ARITH, LOGIC | - |
| F0 | reached | `.op_lock_F0` | AGN | - | no flag macro reachable |
| F1 | reached | `.op_int1_F1` | dual | - | - |
| F2 | reached | `.op_repne_F2` | AGN | - | REVIEW: no width primitive, 1 bare word op(s), no flag macro reachable |
| F3 | reached | `.op_rep_F3` | AGN | - | REVIEW: no width primitive, 9 bare word op(s), no flag macro reachable |
| F4 | reached | `.op_hlt_F4` | AGN | - | REVIEW: no width primitive, 5 bare word op(s), no flag macro reachable |
| F5 | reached | `.op_cmc_F5` | AGN | - | no flag macro reachable |
| F6 | reached | `.op_grp3_eb_F6` | AGN | ARITH, LOGIC | REVIEW: AGN body mixes width primitives with 66 bare word op(s) |
| F7 | reached | `.op_grp3_ev_F7` | dual | ARITH, LOGIC | - |
| F8 | reached | `.op_clc_F8` | AGN | - | no flag macro reachable |
| F9 | reached | `.op_stc_F9` | AGN | - | no flag macro reachable |
| FA | reached | `.op_cli_FA` | AGN | - | REVIEW: AGN body mixes width primitives with 2 bare word op(s), no flag macro reachable |
| FB | reached | `.op_sti_FB` | AGN | - | REVIEW: AGN body mixes width primitives with 1 bare word op(s), no flag macro reachable |
| FC | reached | `.op_cld_FC` | AGN | - | no flag macro reachable |
| FD | reached | `.op_std_FD` | AGN | ARITH, INCDEC, LOGIC | REVIEW: AGN body mixes width primitives with 15 bare word op(s) |
| FE | reached | `.op_grp4_FE` | AGN | INCDEC | REVIEW: AGN body mixes width primitives with 3 bare word op(s) |
| FF | reached | `.op_grp5_FF` | dual | INCDEC | - |

### Two-byte (0F) opcodes -- all 256 (generated)

| Op | Status | Handler | Mode | Flag macros | Warnings |
|---|---|---|---|---|---|
| 0F 00 | reached | `.op_0f_00` | dual | - | no flag macro reachable |
| 0F 01 | reached | `.op_0f_01` | dual | - | - |
| 0F 02 | reached | `.op_0f_lar_02` | dual | - | no flag macro reachable |
| 0F 03 | reached | `.op_0f_lsl_03` | dual | - | no flag macro reachable |
| 0F 04 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 05 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 06 | reached | `.op_0f_clts_06` | AGN | - | REVIEW: no width primitive, 4 bare word op(s), no flag macro reachable |
| 0F 07 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 08 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 09 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 0A | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 0B | reached | `.op_hypercall_0F0B` | AGN | - | REVIEW: AGN body mixes width primitives with 60 bare word op(s) |
| 0F 0C | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 0D | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 0E | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 0F | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 10 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 11 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 12 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 13 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 14 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 15 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 16 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 17 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 18 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 19 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 1A | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 1B | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 1C | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 1D | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 1E | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 1F | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 20 | reached | `.op_0f_mov_r32_cr_20` | AGN | - | REVIEW: AGN body mixes width primitives with 7 bare word op(s), no flag macro reachable |
| 0F 21 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 22 | reached | `.op_0f_mov_cr_r32_22` | AGN | - | REVIEW: no width primitive, 2 bare word op(s), no flag macro reachable |
| 0F 23 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 24 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 25 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 26 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 27 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 28 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 29 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 2A | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 2B | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 2C | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 2D | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 2E | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 2F | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 30 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 31 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 32 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 33 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 34 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 35 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 36 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 37 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 38 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 39 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 3A | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 3B | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 3C | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 3D | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 3E | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 3F | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 40 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 41 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 42 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 43 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 44 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 45 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 46 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 47 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 48 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 49 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 4A | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 4B | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 4C | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 4D | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 4E | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 4F | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 50 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 51 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 52 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 53 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 54 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 55 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 56 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 57 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 58 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 59 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 5A | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 5B | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 5C | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 5D | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 5E | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 5F | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 60 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 61 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 62 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 63 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 64 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 65 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 66 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 67 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 68 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 69 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 6A | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 6B | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 6C | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 6D | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 6E | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 6F | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 70 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 71 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 72 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 73 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 74 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 75 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 76 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 77 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 78 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 79 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 7A | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 7B | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 7C | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 7D | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 7E | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 7F | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F 80 | reached | `.op_0f_jcc_rel32` | dual | - | - |
| 0F 81 | reached | `.op_0f_jcc_rel32` | dual | - | - |
| 0F 82 | reached | `.op_0f_jcc_rel32` | dual | - | - |
| 0F 83 | reached | `.op_0f_jcc_rel32` | dual | - | - |
| 0F 84 | reached | `.op_0f_jcc_rel32` | dual | - | - |
| 0F 85 | reached | `.op_0f_jcc_rel32` | dual | - | - |
| 0F 86 | reached | `.op_0f_jcc_rel32` | dual | - | - |
| 0F 87 | reached | `.op_0f_jcc_rel32` | dual | - | - |
| 0F 88 | reached | `.op_0f_jcc_rel32` | dual | - | - |
| 0F 89 | reached | `.op_0f_jcc_rel32` | dual | - | - |
| 0F 8A | reached | `.op_0f_jcc_rel32` | dual | - | - |
| 0F 8B | reached | `.op_0f_jcc_rel32` | dual | - | - |
| 0F 8C | reached | `.op_0f_jcc_rel32` | dual | - | - |
| 0F 8D | reached | `.op_0f_jcc_rel32` | dual | - | - |
| 0F 8E | reached | `.op_0f_jcc_rel32` | dual | - | - |
| 0F 8F | reached | `.op_0f_jcc_rel32` | dual | - | - |
| 0F 90 | reached | `.op_0f_setcc` | dual | INCDEC | - |
| 0F 91 | reached | `.op_0f_setcc` | dual | INCDEC | - |
| 0F 92 | reached | `.op_0f_setcc` | dual | INCDEC | - |
| 0F 93 | reached | `.op_0f_setcc` | dual | INCDEC | - |
| 0F 94 | reached | `.op_0f_setcc` | dual | INCDEC | - |
| 0F 95 | reached | `.op_0f_setcc` | dual | INCDEC | - |
| 0F 96 | reached | `.op_0f_setcc` | dual | INCDEC | - |
| 0F 97 | reached | `.op_0f_setcc` | dual | INCDEC | - |
| 0F 98 | reached | `.op_0f_setcc` | dual | INCDEC | - |
| 0F 99 | reached | `.op_0f_setcc` | dual | INCDEC | - |
| 0F 9A | reached | `.op_0f_setcc` | dual | INCDEC | - |
| 0F 9B | reached | `.op_0f_setcc` | dual | INCDEC | - |
| 0F 9C | reached | `.op_0f_setcc` | dual | INCDEC | - |
| 0F 9D | reached | `.op_0f_setcc` | dual | INCDEC | - |
| 0F 9E | reached | `.op_0f_setcc` | dual | INCDEC | - |
| 0F 9F | reached | `.op_0f_setcc` | dual | INCDEC | - |
| 0F A0 | reached | `.op_0f_push_fs_A0` | dual | - | no flag macro reachable |
| 0F A1 | reached | `.op_0f_pop_fs_A1` | dual | - | no flag macro reachable |
| 0F A2 | reached | `.op_0f_cpuid_A2` | AGN | - | REVIEW: no width primitive, 9 bare word op(s), no flag macro reachable |
| 0F A3 | reached | `.op_0f_bt_A3` | dual | - | no flag macro reachable |
| 0F A4 | reached | `.op_0f_shld_imm_A4` | dual | - | no flag macro reachable |
| 0F A5 | reached | `.op_0f_shld_cl_A5` | dual | - | no flag macro reachable |
| 0F A6 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F A7 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F A8 | reached | `.op_0f_push_gs_A8` | dual | - | no flag macro reachable |
| 0F A9 | reached | `.op_0f_pop_gs_A9` | dual | - | no flag macro reachable |
| 0F AA | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F AB | reached | `.op_0f_bts_AB` | dual | - | no flag macro reachable |
| 0F AC | reached | `.op_0f_shrd_imm_AC` | dual | - | no flag macro reachable |
| 0F AD | reached | `.op_0f_shrd_cl_AD` | dual | - | no flag macro reachable |
| 0F AE | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F AF | reached | `.op_0f_imul_AF` | dual | - | no flag macro reachable |
| 0F B0 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F B1 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F B2 | reached | `.op_0f_lss_B2` | dual | - | no flag macro reachable |
| 0F B3 | reached | `.op_0f_btr_B3` | dual | - | no flag macro reachable |
| 0F B4 | reached | `.op_0f_lfs_B4` | dual | - | no flag macro reachable |
| 0F B5 | reached | `.op_0f_lgs_B5` | dual | - | no flag macro reachable |
| 0F B6 | reached | `.op_0f_movzx_gv_eb` | dual | INCDEC | - |
| 0F B7 | reached | `.op_0f_movzx_gv_ew` | dual | INCDEC | - |
| 0F B8 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F B9 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F BA | reached | `.op_0f_ba` | dual | - | no flag macro reachable |
| 0F BB | reached | `.op_0f_btc_BB` | dual | - | no flag macro reachable |
| 0F BC | reached | `.op_0f_bsf_BC` | dual | - | no flag macro reachable |
| 0F BD | reached | `.op_0f_bsr_BD` | dual | - | no flag macro reachable |
| 0F BE | reached | `.op_0f_movsx_gv_eb` | dual | INCDEC | - |
| 0F BF | reached | `.op_0f_movsx_gv_ew` | dual | INCDEC | - |
| 0F C0 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F C1 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F C2 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F C3 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F C4 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F C5 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F C6 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F C7 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F C8 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F C9 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F CA | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F CB | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F CC | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F CD | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F CE | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F CF | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F D0 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F D1 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F D2 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F D3 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F D4 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F D5 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F D6 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F D7 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F D8 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F D9 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F DA | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F DB | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F DC | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F DD | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F DE | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F DF | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F E0 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F E1 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F E2 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F E3 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F E4 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F E5 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F E6 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F E7 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F E8 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F E9 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F EA | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F EB | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F EC | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F ED | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F EE | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F EF | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F F0 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F F1 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F F2 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F F3 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F F4 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F F5 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F F6 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F F7 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F F8 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F F9 | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F FA | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F FB | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F FC | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F FD | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F FE | **`.exit_loop`** | `.exit_loop` | - | - | - |
| 0F FF | **`.exit_loop`** | `.exit_loop` | - | - | - |

### Group `/reg` reachability (generated)

`Grp1` and `Grp2` dispatch through an 8-entry `TBH` indexed by a 3-bit field, so
all eight `/reg` values reach code by construction and no chain appears here.
`Grp3`/`Grp4`/`Grp5` use a `CMP`/`BEQ` chain: any `/reg` the chain does not name falls
to `.exit_loop`. The chains below are extracted from the source.

| Dispatch | /reg reached | /reg falling to `.exit_loop` |
|---|---|---|
| `GRP3_F6_DISPATCH` | /0 -> `GRP3_F6_TEST`, /2 -> `GRP3_F6_NOT`, /3 -> `GRP3_F6_NEG`, /4 -> `GRP3_F6_MUL`, /5 -> `GRP3_F6_IMUL`, /6 -> `GRP3_F6_DIV`, /7 -> `GRP3_F6_IDIV` | /1 |
| `GRP3_F7_DISPATCH` | /0 -> `GRP3_F7_TEST`, /2 -> `GRP3_F7_NOT`, /3 -> `GRP3_F7_NEG`, /4 -> `GRP3_F7_MUL`, /5 -> `GRP3_F7_IMUL`, /6 -> `GRP3_F7_DIV`, /7 -> `GRP3_F7_IDIV` | /1 |
| `GRP4_DISPATCH` | /0 -> `GRP4_INC`, /1 -> `GRP4_DEC` | /2, /3, /4, /5, /6, /7 |
| `GRP5_DISPATCH` | /0 -> `GRP5_INC`, /1 -> `GRP5_DEC`, /2 -> `GRP5_CALL`, /3 -> `GRP5_CALLF`, /4 -> `GRP5_JMP`, /5 -> `GRP5_JMPF`, /6 -> `GRP5_PUSH` | /7 |

<!-- END GENERATED -->


---

# Hand-written analysis (NOT generated)

Everything below is human analysis that a parser cannot produce. It is kept
because it is genuinely useful — and it is **dated**, because it was written
against an older tree.

> **WARNING — the prioritised defect list below has NOT been re-derived against
> the current tree.** Several entries in it are known to be FIXED: `0F 8x`
> near-`Jcc`, `E0`–`E3` address size, `60`/`61` PUSHA/POPA, `68` PUSH Iv,
> `84`/`85` TEST, the BCD group, and INS/OUTS. Treat every individual claim as
> a HYPOTHESIS to check against the generated table above and against the
> handler body, not as a statement about today's code. It is retained for its
> reasoning and its taxonomy, not for its status column.


## Group opcodes — every `/reg` individually (hand-written, DATED)

Reachability of each `/reg` is GENERATED above. This section is the semantic
commentary the generator cannot produce — what each member does, and the
structural lessons. Its status claims are dated; check them against the
generated reachability table before acting on one.

Dispatch mechanism matters here. `Grp1` and `Grp2` use a `TBH` jump table with exactly
eight entries indexed by a 3-bit field, so **no `/reg` value can fall through** — all
eight always reach code. `Grp3`/`Grp4`/`Grp5` use a `CMP`/`BEQ` chain and anything the
chain does not name falls to `.exit_loop`.

### Grp1 — `0x80` Eb,Ib / `0x81` Ev,Iv / `0x82` (alias of 80) / `0x83` Ev,Ib

Dispatch: `L(ALU_DISPATCH_*)` → `ALU_TABLE_M`, 8-entry `TBH`.

| /reg | Op | 80 | 81 | 82 | 83 | Flags | Forms |
|---|---|---|---|---|---|---|---|
| /0 | ADD | yes | yes | yes | yes | full (CF OF SF ZF AF PF) | reg + mem |
| /1 | OR  | yes | yes | yes | yes | CF=0 OF=0, SF ZF PF; AF undefined, left alone | reg + mem |
| /2 | ADC | yes | yes | yes | yes | full; narrow path uses `X86_ADC_NARROW` so CF-in is honoured at 8/16 bits | reg + mem |
| /3 | SBB | yes | yes | yes | yes | full, same narrow path | reg + mem |
| /4 | AND | yes | yes | yes | yes | CF=0 OF=0, SF ZF PF | reg + mem |
| /5 | SUB | yes | yes | yes | yes | full | reg + mem |
| /6 | XOR | yes | yes | yes | yes | CF=0 OF=0, SF ZF PF | reg + mem |
| /7 | CMP | yes | yes | yes | yes | full, result discarded | reg + mem |

32 of 32 sub-opcode slots implemented. `0x81`/`0x83` are dual-emitted and width-correct;
`0x80`/`0x82` are byte-sized and correctly `OP_AGNOSTIC`.

### Grp2 — `0xC0`/`0xC1` E,Ib · `0xD0`/`0xD1` E,1 · `0xD2`/`0xD3` E,CL

Dispatch: `L(SHIFT_TBH_8)` / `L(SHIFT_TBH_32)`, 8-entry `TBH`.

| /reg | Op | C0 | C1 | D0 | D1 | D2 | D3 | Flags |
|---|---|---|---|---|---|---|---|---|
| /0 | ROL | yes | yes | yes | yes | yes | yes | CF + OF only; SF/ZF/AF/PF correctly untouched |
| /1 | ROR | yes | yes | yes | yes | yes | yes | CF + OF only |
| /2 | RCL | yes | yes | yes | yes | yes | yes | CF + OF; true 9-/17-/33-bit rotate with the correct modulus |
| /3 | RCR | yes | yes | yes | yes | yes | yes | CF + OF |
| /4 | SHL | yes | yes | yes | yes | yes | yes | CF, OF = MSB^CF, SF ZF PF; AF undefined |
| /5 | SHR | yes | yes | yes | yes | yes | yes | CF, OF = original MSB, SF ZF PF |
| /6 | SAL (= SHL) | yes | yes | yes | yes | yes | yes | correctly aliased to the SHL body |
| /7 | SAR | yes | yes | yes | yes | yes | yes | CF, OF cleared, SF ZF PF |

48 of 48 sub-opcode slots implemented, reg + mem on every one. The count is masked to 5
bits *before* the zero test, so a masked-to-zero count leaves all flags alone — correct.
**But the three v-width entry points (`C1`, `D1`, `D3`) fetch the operand with a bare
32-bit `LDR`** — see the defects section.

### Grp3 — `0xF6` Eb / `0xF7` Ev

Dispatch: `L(GRP3_F6_DISPATCH)` / `L(GRP3_F7_DISPATCH)`, `CMP` chain.

| /reg | Op | F6 (byte) | F7 (v) |
|---|---|---|---|
| /0 | TEST E,I | **BROKEN** — fetches the immediate, then branches to the fetch loop. No AND, no flags. | implemented, `ANDS` + `X86_FLAGS_LOGIC`, immediate at operand size |
| /1 | TEST (undocumented alias of /0) | **missing** — not in the `CMP` chain, falls to `.exit_loop` | **missing** |
| /2 | NOT | implemented; correctly writes no flags | implemented, `ST_V` |
| /3 | NEG | implemented, full flags, CF = (operand != 0) | implemented, `VNORM`-normalised, full flags |
| /4 | MUL | implemented; AX = AL*r/m8; CF/OF = "did not fit in AL"; SF/ZF/AF/PF undefined and left alone (correct) | implemented; 16-bit path writes DX:AX, 32-bit path uses `UMULL` for EDX:EAX; CF/OF correct |
| /5 | IMUL | implemented, CF/OF = "does not fit in AL" | implemented, `SMULL` / 16-bit path, CF/OF correct |
| /6 | DIV | implemented; #DE via the IVT on divide-by-zero *and* on quotient overflow | implemented; **32-bit form raises #DE whenever EDX != 0** — a true 64/32 divide is not modelled |
| /7 | IDIV | implemented, #DE on zero and on signed-quotient overflow | implemented; 16-bit form assembles DX:AX correctly; 32-bit form ignores EDX entirely (see defects) |

F6: 7 of 8 reach code, 1 of those 7 is a no-op. F7: 7 of 8 reach code and are correct.

### Grp4 — `0xFE` Eb

| /reg | Op | Status |
|---|---|---|
| /0 | INC Eb | implemented, reg + mem — **writes NO FLAGS** (OF SF ZF AF PF required, CF preserved) |
| /1 | DEC Eb | implemented, reg + mem — **writes NO FLAGS** |
| /2../7 | (invalid) | correctly `.exit_loop` |

### Grp5 — `0xFF` Ev

| /reg | Op | Status |
|---|---|---|
| /0 | INC Ev | implemented, reg + mem, `LD_V`/`ST_V` — **writes NO FLAGS** |
| /1 | DEC Ev | implemented, reg + mem — **writes NO FLAGS** |
| /2 | CALL near Ev | implemented, but pushes `R10` (the **linear** EIP, CS.base included) and then sets `R10` to the raw target offset **without** adding CS.base; stack is open-coded on raw ESP with a hard-coded −4. Self-consistent only when CS.base = SS.base = 0. |
| /3 | CALL far m16:16 | implemented (`L(GRP5_FAR_ENTRY)`): reads offset then selector from memory, pushes return CS:IP with `PUSH_V`, installs CS via `SET_SREG`. Offset always read with `LDRH`, so `m16:32` is not supported. |
| /4 | JMP near Ev | implemented, but sets `R10` to the raw offset with no CS.base — same defect as /2. |
| /5 | JMP far m16:16 | implemented, same path as /3. |
| /6 | PUSH Ev | implemented, but open-coded stack on raw ESP with a hard-coded −4. |
| /7 | (invalid) | correctly `.exit_loop` |

### `0F 00` — Grp6

| /reg | Op | Status |
|---|---|---|
| /0 | SLDT Ew | implemented |
| /1 | STR Ew | implemented — the task register is loaded and stored; NO TASK SWITCH |
| /2 | LLDT Ew | implemented |
| /3 | LTR Ew | implemented — validates the descriptor as an AVAILABLE TSS and marks it busy |
| /4 | VERR Ew | implemented — ZF only, never faults |
| /5 | VERW Ew | implemented — ZF only, never faults |
| /6, /7 | (invalid) | correctly `.exit_loop` |

ALL SIX members are reachable. `LTR` is here because `scripts/opcode_demand.py`
counted two static sites in DOS4GW.EXE and `STR` five, and a TSS at DPL 0 is
wanted for SS0:ESP0 and the I/O permission bitmap — it is **not** evidence of a
ring change, and no task switch is built. `VERR`/`VERW` share the machinery of
`0F 02`/`0F 03` and are refused in real mode, where they are not instructions.
See [`docs/14`](14_protected_mode.md).

### `0F 01` — Grp7

Handler `.op_0f_01`. **A GROUP IS NOT AN OPCODE, and the exit says so**: the
`/0`, `/1` and `/7` arms carry the ModRM byte into `exit_info`'s high half, so a
refusal reads `0xMM0F01` and names the member. It reported a bare `0x0F` for
years — see the retraction in [`docs/13`](13_title_status.md).

**The two members with a REGISTER form (`/4` SMSW, `/6` LMSW) are decided BEFORE
the ModRM walk.** LGDT/LIDT are memory-only, so the walk used to be
unconditional and a `mod=11` encoding fell straight through the group into
`.exit_loop`. That single structural detail cost two titles their startup
(`SMSW AX` for Mortal Kombat's C runtime) and two more their entry into
protected mode (`LMSW AX`).

| /reg | Op | Status |
|---|---|---|
| /0 | SGDT Ms | **missing** — `.exit_loop`, reporting `0xMM0F01` |
| /1 | SIDT Ms | **missing** — same |
| /2 | LGDT Ms | implemented — shares `L(LXXX_OPERAND)` with LIDT. (The "STUB that loads nothing" recorded here previously is fixed.) |
| /3 | LIDT Ms | implemented: 16-bit limit and 32-bit base through the ModRM effective address. |
| /4 | SMSW Ew | implemented, register **and** memory form. Unprivileged. |
| /5 | (invalid) | correctly unreachable |
| /6 | LMSW Ew | **implemented** — `cr0 = (cr0 & ((~0xf)\|1)) \| (src & 0xf)`, i.e. PE/MP/EM/TS only and **PE is set-only** (tiny386 `i386.c:3292`, DOSBox-X `cpu.cpp:2788`, which agree). CPL 0 in protected mode only. Shares MOV CR0's PE-transition tail, so `x86_pmode_sync_mode()` runs on the edge. **This is the instruction that puts a DOS extender into protected mode** — it uses the 286 sequence, not `MOV CR0`, so nothing else here substitutes for it. `tests/test_opcodes.py` 276-278. |
| /7 | INVLPG | **missing** (486 anyway) |

Also: `.op_0f_01` reaches the decoder through `[R2, #REC_MODRM_FN]`, so `0x67` is
honoured. (An earlier note here said it called `L(DECODE_MODRM_32)` by symbol
and ignored the prefix; that is fixed.)

### `0F BA` — Grp8

| /reg | Op | Status |
|---|---|---|
| /0../3 | (invalid) | n/a |
| /4 | BT Ev,Ib | **unimplemented** (table slot is `.exit_loop`) |
| /5 | BTS Ev,Ib | **unimplemented** |
| /6 | BTR Ev,Ib | **unimplemented** |
| /7 | BTC Ev,Ib | **unimplemented** |

---

## Prefixes

| Prefix | Status | How |
|---|---|---|
| `26` ES: | implemented | `ADD R6, R8, #SEG_ES_SEL` then re-enter at `.fetch_next_byte_no_reset`. `R6` is reset to the `R8` sentinel at each instruction boundary, so the override lives exactly one instruction. The sentinel (rather than "R6 = DS") is what lets the ModRM decoder distinguish an absent prefix from an explicit `3E` on a `[BP]` form. |
| `2E` CS: | implemented | as above |
| `36` SS: | implemented | as above |
| `3E` DS: | implemented | as above |
| `64` FS: | implemented | as above |
| `65` GS: | implemented | as above |
| `66` operand size | implemented | `EOR R2, R2, #OPSIZE_XOR` — one instruction, switches the whole dispatch table (and the 0F table, at `[R2,#-8]`) to the other pass. Applying it twice cancels. |
| `67` address size | implemented | `EOR R2, R2, #ADDRSIZE_XOR`, independent axis. Honoured by every handler that reaches the decoder via `[R2, #REC_MODRM_FN]`; **ignored** by the five handlers that call `L(DECODE_MODRM_32)` by symbol (`0F 01`, `0F 9x` memory form, `0F B6/B7/BE/BF` memory form). |
| `F0` LOCK | implemented as a no-op | Correct for a uniprocessor, but it branches to `.fetch_decode_loop`, which resets the segment-override register and decrements the PIT again. `ES: LOCK ...` loses the `ES:`. |
| `F2` REPNE | implemented | Arms `CPU_REP_ACTIVE`/`CPU_REP_ZF_SENSE`/`CPU_REP_RESUME`; one element per pass through the main loop, then EIP is rewound to the prefix. Keeps the PIT serviceable inside a long REP. Counter is CX or ECX **by address size**. Ignored on non-string opcodes, as on hardware. |
| `F3` REP/REPE | implemented | Same machinery, ZF sense 1; sense 2 ("no ZF test") selected for `A4/A5` MOVS and `AA..AD` STOS/LODS. |

Segment override coverage on the *destination* of string ops is correctly refused
(`STR_DST` always uses ES), and correctly honoured on the source (`STR_SRC`).

---

## x87 and other whole-block gaps

| Block | Slots | Status |
|---|---|---|
| `D8`–`DF` x87 escapes | 8 | **Entirely unimplemented.** No FPU state exists in the CPU struct; all eight escape opcodes exit to the host as UNIMPLEMENTED. Any program that probes for a coprocessor or emits floating point will stop here. `9B` WAIT/FWAIT is also unimplemented, so even the "no FPU present" idiom (`FNINIT` / `FNSTSW`) cannot run. |
| BCD adjusts `27 2F 37 3F` (DAA/DAS/AAA/AAS) | 4 | Unimplemented. `D4`/`D5` (AAM/AAD) *are* implemented but set no flags. |
| String port I/O `6C`–`6F` | 4 | Unimplemented. |
| `62` BOUND, `63` ARPL | 2 | Unimplemented. |
| `9B` WAIT | 1 | Unimplemented. |
| `F1` ICEBP, `F4` HLT | 2 | Unimplemented in the table. A `.op_hlt_F4` handler exists in the source but is referenced by neither dispatch table — dead code. |
| 386 bit instructions `0F A3/AB/B3/BB`, `0F BA`, `0F BC/BD` | 7 | Unimplemented: BT/BTS/BTR/BTC/BSF/BSR. |
| 386 double shifts `0F A4/A5/AC/AD` | 4 | Unimplemented: SHLD/SHRD. |
| 386 segment loads `0F B2/B4/B5` | 3 | Unimplemented: LSS/LFS/LGS. |
| 386 `0F AF` IMUL Gv,Ev | 0 | Implemented. |
| 386 `0F A0/A1/A8/A9` PUSH/POP FS,GS | 4 | Unimplemented. |
| 386 control/debug regs `0F 20`–`0F 27` | 8 | Unimplemented — consistent with a real-mode-only core. |
| 286 protected mode `0F 00`, `0F 02`, `0F 03`, `0F 06` | 0 | Implemented: all of Grp6, `LAR`, `LSL`, `CLTS`. `LAR`/`LSL`/`VERR`/`VERW` exit as unimplemented in REAL mode, where they are not instructions. |
| 486/Pentium+ (`0F 31` RDTSC, `0F A2` CPUID, `0F C8`–`CF` BSWAP, CMPXCHG, XADD, MMX/SSE) | 158 | **Out of scope** for a 386-class target. |

## Defects, prioritised

Ordered by likely impact on real DOS software. These matter more than the unimplemented
opcodes because an unimplemented opcode stops the interpreter loudly with
`X86_EXIT_UNIMPLEMENTED` and the offending byte in `exit_info`, whereas everything below
produces a wrong answer and keeps running.

**HOW TO READ `exit_info`** — it is not always one byte, and it used to lie:

| value | means |
|---|---|
| `0x00`–`0xFF` | a one-byte opcode |
| `0x0Fnn` | the TWO-byte opcode `0F nn`. The 0F escape keeps the second byte in R1 and `.exit_loop` reports R0, so R0 is now loaded with `0x0F00\|nn` at the second dispatch. **Before that, every unimplemented two-byte opcode reported a bare `0x0F`**, which made three unrelated titles look like they shared one blocker for several passes ([`docs/13`](13_title_status.md)). |
| `0xMM0F01` | group 7 member: `MM` is the ModRM byte, so `/reg` names which. |

Read it with `scripts/probe_report.py`, which now derefs `cpu_state_ptr` and
prints `cpu_exit_info` alongside `cpu_cs_sel`/`cpu_eip`/`cpu_cr0` — the reported
`CS` used to come from somewhere else entirely.

### P0 — silent wrong answers on ubiquitous instructions

1. **`0x84` TEST Eb,Gb is a no-op.** The register form fetches the ModRM and branches
   straight to the fetch loop: no `AND`, no flags. The memory form exits. `TEST AL,AL /
   JZ` is one of the most common two-instruction sequences in existence; here it
   branches on whatever flags were left over.
2. **`0xA8` / `0xA9` TEST AL,Ib / TEST eAX,Iv compute the AND and discard it without
   writing flags.** Same class of failure as 1.
3. **`0xF6 /0` TEST Eb,Ib fetches the immediate and does nothing else.** Third member of
   the same family. (`0xF7 /0` is correct — the fix pattern is right there.)
4. **`0xC6` / `0xC7` MOV E,imm have no memory form.** `MOV WORD PTR [BX],1234h` exits as
   unimplemented. This is loud rather than silent, but it is so common that it belongs at
   this priority.
5. **`0xFE /0 /1` and `0xFF /0 /1` INC/DEC memory-or-register write no flags.** `INC` /
   `JNZ` and `DEC` / `JNZ` loop idioms are everywhere. (The register-direct forms `40`–`4F`
   *do* set flags correctly, so the bug only shows on the ModRM forms.)

### P1 — wrong whenever segments are not flat, i.e. always in real mode

6. **Thirteen handlers use raw `ESP` as a linear address** with a hard-coded ±4:
   `60` PUSHA, `61` POPA, `68`/`6A` PUSH imm, `8F` POP Ev, `9C` PUSHF, `9D` POPF,
   `C2` RET imm, `C8` ENTER, `C9` LEAVE, `CA` RETF imm, `FF /2` CALL near, `FF /6` PUSH Ev.
   They ignore `SS.base` *and* the operand size. In 16-bit mode `PUSHA` strides 4 bytes
   while storing 2. `PUSH_V`/`POP_V` already do this correctly and are used by
   `50`–`5F`, `06`/`07`, `E8`, `C3`, `CB`, `9A`, `EA`, `FF /3`, `FF /5`.
7. **`0xCA` RETF imm16 pops CS and throws it away.** It loads the selector into `R3` and
   never calls `SET_SREG`, so a far return with a stack-adjust keeps executing in the
   caller's segment. It is also `OP_AGNOSTIC` (always a 32-bit far return) and installs
   the new IP without `CS.base`. `0xCB` RETF does all three correctly — the contrast is
   the strongest evidence this is an oversight, not a design choice.
8. **`0xC2` RET imm16** installs the popped offset as EIP without adding `CS.base`, on
   top of the raw-ESP problem.
9. **`0xFF /2` CALL near / `0xFF /4` JMP near** push the *linear* EIP and set EIP to a
   *segment-relative* offset. The two errors cancel only when `CS.base == 0`.
10. **`0xD7` XLAT** uses `EBX + AL` as a linear address: no `DS` base, no override
    support, and no 16-bit masking of BX.
11. **`0xA0`–`0xA3` MOV moffs** use the moffs as a linear address with no `DS` base and
    no override. `A0`/`A2` additionally read a fixed 32-bit moffs regardless of address
    size; `A1`/`A3` read it at *operand* size when it must follow *address* size.

### P2 — silently 32-bit in 16-bit mode

12. **`0xC1` / `0xD1` / `0xD3` (v-width shifts)** are dual-emitted, but both the register
    and the memory operand fetch use a bare `LDR`. In the 16-bit pass this reads 4 bytes
    for a 16-bit memory operand (which can also fault at a page boundary) and the whole
    32-bit register. `SHR`, `SAR`, `ROR` and `RCR` then mix the upper half into the
    result and into CF/OF/SF/ZF/PF. The store-back is `ST_V`, so the damage is confined
    to the value and the flags rather than to neighbouring memory.
13. **`0x91`–`0x97` XCHG eAX,r** and **`0x87` XCHG Ev,Gv** are `OP_AGNOSTIC` with bare
    `LDR.W`/`STR.W`: `XCHG AX,BX` also swaps the high halves of EAX/EBX.
14. **`0x98` CBW/CWDE and `0x99` CWD/CDQ are dual-emitted but have fixed 32-bit bodies.**
    In the 16-bit pass, `98` must be CBW (AL→AX) and executes CWDE (AX→EAX); `99` must be
    CWD (sign of AX into DX) and executes CDQ, overwriting all of EDX.
15. **`0x0F 80`–`0x0F 8F` near Jcc** read the displacement with a hard-coded `LDR` and
    `ADD R10,#4`. Under 16-bit operand size these take a **rel16**, so the handler
    consumes two bytes too many and jumps by a displacement assembled from the following
    instruction's bytes. Any 386-aware 16-bit code (very common in DOS extenders and
    later-era games) that uses a near conditional jump will derail.
16. **`0x8D` LEA** writes the computed address with a bare 32-bit `STR` rather than
    `STREG_V`.
17. **`0x0F B6/B7/BE/BF` MOVZX/MOVSX** write the destination with a full 32-bit `STR.W`,
    clobbering the high half of the destination register in 16-bit mode.
18. **`0xA1` MOV eAX,Ov** writes EAX with a full 32-bit store.
19. **`0x61` POPA** writes every restored register with a full 32-bit store.

### P3 — missing memory forms and address-size gaps

20. **Register-form-only handlers**: `85` TEST Ev,Gv, `86`/`87` XCHG, `8F` POP Ev,
    `C6`/`C7` MOV E,imm. The MOV family (`88/89/8A/8B`) had exactly this bug and has been
    fixed; these five were not swept in the same pass.
21. **`E0`–`E3` LOOP/LOOPZ/LOOPNZ/JCXZ use the full 32-bit ECX** regardless of address
    size. In 16-bit code they must use CX. They happen to work whenever the upper half of
    ECX is already zero, which is most of the time — a classic latent failure.
22. **Five handlers call `L(DECODE_MODRM_32)` by symbol** instead of through
    `[R2, #REC_MODRM_FN]`, so they follow the *pass's* address size and ignore a `0x67`
    prefix: `0F 01`, `0F 9x` SETcc (memory form), `0F B6/B7/BE/BF` (memory form).

### P4 — missing flags on less-common instructions

23. **`0x69` / `0x6B` IMUL Gv,Ev,I** set no CF/OF. Software tests these to detect
    overflow.
24. **`0xD4` AAM / `0xD5` AAD** set no SF/ZF/PF (all three are architecturally defined
    for these instructions). AAM with an imm8 of 0 exits to the host instead of raising
    `#DE`.
25. **`0xF7 /6` DIV, 32-bit form**, raises `#DE` whenever `EDX != 0` rather than
    performing a 64/32 divide. `0xF7 /7` IDIV, 32-bit form, ignores EDX entirely and
    divides only EAX — a genuine wrong answer rather than a refusal.

### P5 — architectural stubs and leftovers

26. **`0F 01 /2` LGDT validates and then does nothing.** It succeeds silently without
    loading GDTR. A guest that sets up a GDT and then tries to enter protected mode gets
    no error here and fails later somewhere unrelated. Failing loudly would be better
    than a silent success.
27. **`0x9D` POPF loads EFLAGS with no mask**, so a guest can set reserved bits, IOPL and
    VM directly.
28. **`0xCF` IRET still contains debug instrumentation**, writing four words to
    `x86_iret_log` on every single execution.
29. **`0xFB` STI does not implement the one-instruction interrupt shadow**, and
    **`0x17` POP SS does not inhibit interrupts** for the following instruction. The
    classic `MOV SS,ax / MOV SP,imm` sequence can therefore take an interrupt on a
    half-updated stack. The PIT check only fires at counter expiry, which makes this rare
    and non-deterministic — the worst combination to debug.
30. **`0xF4` HLT's handler exists but is unreachable**; the table entry is `.exit_loop`.
    Net behaviour is nearly the same, minus the CR0/CPL `#GP` check.
31. **`0xF0` LOCK clears a preceding segment override** and double-decrements the PIT.

## What a real DOS game or utility actually needs

Ordered by what will bite first when a real binary is pointed at this core.

**Will stop the interpreter immediately (loud):**

- **`C6`/`C7` MOV mem,imm.** Almost nothing gets past initialisation without storing a
  constant to memory. This is the single highest-value fix in the report.
- **x87 (`D8`–`DF`) and `9B` WAIT.** Any program built by a compiler with floating point
  linked in, and anything that probes for a coprocessor. There is no partial credit here
  — an FPU is a substantial subsystem. Deciding early whether the target set is
  integer-only matters more than any individual opcode.
- **`0F BC/BD` BSF/BSR.** Standard compiler output. (`0F AF` IMUL Gv,Ev is now implemented.)
- **`0F B2/B4/B5` LSS/LFS/LGS.** `LSS` in particular is how 32-bit-aware code switches
  stacks atomically.
- **`27/2F/37/3F` DAA/DAS/AAA/AAS.** Old-school BCD arithmetic; rarer in games, common in
  business utilities and in hand-written assembly from the era.
- **`6C`–`6F` INS/OUTS.** Used by disk and video BIOS-style code.

**Will produce wrong output while appearing to work (quiet, and therefore worse):**

- **`84`/`A8`/`A9`/`F6 /0` TEST with no flags** and **`FE`/`FF` INC/DEC with no flags**.
  These four defects alone will make almost any real program take wrong branches. They
  are cheap to fix — `X86_FLAGS_LOGIC` and `X86_FLAGS_INCDEC` already exist and are used
  correctly a few hundred lines away.
- **The raw-ESP stack handlers.** Real-mode DOS programs essentially never run with
  `SS.base == 0`; a `.COM` or `.EXE` gets a segmented stack. `PUSHA`/`POPA`/`PUSHF`/
  `POPF`/`ENTER`/`LEAVE`/`RET imm`/`RETF imm` are all in this set, so the failure will
  show up as stack corruption on the first interrupt-heavy or frame-using code path,
  which is very hard to trace back to its cause.
- **`CA` RETF imm16 not reloading CS.** Any far call into an overlay or a resident driver
  that returns with a stack adjust silently continues in the wrong segment.
- **`E0`–`E3` LOOP using ECX.** Latent: correct until something leaves garbage in the
  upper half of ECX, then a loop runs an astronomically wrong number of times.
- **`C1`/`D1`/`D3` v-shifts reading 32 bits.** The source comment already documents
  exactly this failure mode for a previous variant of the bug (`SHL SI,CL` leaving
  overflow in the high half of ESI, which a 16-bit relocator then uses as an address) —
  the operand *fetch* was not converted when the rest of the handler was.
- **`0F 8x` near Jcc consuming 4 bytes in 16-bit mode.** Not universal, but when it
  happens the instruction stream desynchronises and the failure looks like memory
  corruption rather than a decode bug.

**Probably safe to defer:**

- The `0F 00` / `0F 01` protected-mode group, `0F 20`–`0F 27` control registers, `62`
  BOUND, `63` ARPL. A real-mode-only core does not need them, and the segment layer traps
  loudly on `CR0.PE = 1` rather than pretending. The one exception is the **LGDT stub**,
  which should be made to fail loudly instead of succeeding silently.
- All 486/Pentium+ opcodes.

## Things this audit could not determine by reading the code

- Whether the `[R8,#40]` / `[R8,#44]` literals in the LIDT path actually correspond to
  `idtr.base` / `idtr.limit`. `x86_offsets.inc` is generated at build time and was not
  read (no builds were run, per the read-only constraint). The other handlers use
  symbolic offsets, so these two literals are at minimum a style inconsistency worth
  checking.
- Whether the `SHIFT`/`ALU` `TBH` tables assemble to the offsets intended. They are built
  from `.short (target - (dispatch + 4)) / 2` expressions; correctness was assumed from
  the expressions, not verified against a disassembly.
- Runtime behaviour of any of it. This is a static reading of the source only — no build
  and no test run was performed.
