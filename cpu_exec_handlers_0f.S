OP .op_0f_0F
    /* The 0F table comes from the mode record, not a fixed symbol, and lands
       in R3 so R2 keeps pointing at the record -- the two-byte opcodes that
       take a ModRM still need its decoder pointer. */
    LDR.W R3, [R2, #REC_TABLE_0F]
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD R10, R10, #1
#if X86_PROFILE
    /* Second dispatch of a two-byte opcode, so it CLOSES slot 0x0F (the escape
       fetch itself) and opens slot 256+n. R2 and R3 must survive -- the record
       and the 0F table -- so the macro gets R0/R4/R5/R12, all dead here. */
    PROF_DISPATCH R1, R12, R0, R4, R5, bias=256
#endif
    LDR.W PC, [R3, R1, LSL #2]
L(opcode_0f_dispatch_target):


/* 0F 00 -- group 6. SLDT (/0) and LLDT (/2) only.
 *
 * STR (/1) and LTR (/3) are NOT here and are not an oversight: PLAN.md records
 * that the TSS work is not needed because INT 21h is HLE'd, so there is no
 * task state to point a task register at. VERR (/4) and VERW (/5) wait for a
 * guest that asks for them. All four keep the .exit_loop they have today,
 * which names the opcode rather than inventing an answer.
 *
 * The operand is a 16-bit selector however the operand size is set. */
OP .op_0f_00
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    UBFX R3, R1, #3, #3
    CMP R3, #0
    BEQ.W L(OP_SLDT)
    CMP R3, #2
    BNE.W .exit_loop
    /* ---- LLDT ---- */
    BL L(LXXX_CPL_CHECK)
    CMP R1, #192                   @ mod==3: the selector is in a register
    BLO.W L(OP_LLDT_MEM)
    AND.W R5, R1, #7
    ADD.W R5, R8, R5, LSL #2
    LDRH.W R5, [R5, #8]
    B.W L(OP_LLDT_GO)
L(OP_LLDT_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN]
    BLX R12
    SEAM_CHECK R3, R7
    UBFX R3, R7, #12, #12
    LDR.W R3, [R9, R3, LSL #2]
    ADDS R3, R3, R7
    MEM_TRAP_R R3
    LDRH R5, [R3, #0]
L(OP_LLDT_GO):
    PUSH {R0-R3, R12, LR}
    MOV R0, R8
    UXTH R1, R5
    BL x86_pmode_lldt
    CBNZ R0, 1f
    POP {R0-R3, R12, LR}
    B.W .fetch_decode_loop
1:  POP {R0-R3, R12, LR}
    MOV.W R0, #X86_EXIT_GPF
    STR.W R0, [R8, #CPU_EXIT_REASON]
    LDR.W R0, [R8, #CPU_PMODE_ERR]
    STR.W R0, [R8, #CPU_EXIT_INFO]
    B.W .save_state_and_exit

/* SLDT: store the LDT selector. Unprivileged in both references. */
L(OP_SLDT):
    LDR.W R5, [R8, #CPU_LDT_SEL]
    CMP R1, #192
    BLO.W L(OP_SLDT_MEM)
    AND.W R3, R1, #7
    ADD.W R3, R8, R3, LSL #2
    STRH.W R5, [R3, #8]
    B.W .fetch_decode_loop
L(OP_SLDT_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN]
    BLX R12
    LDR.W R5, [R8, #CPU_LDT_SEL]
    MEM_GET_PTR R3, R7
    VGA_ST_H R5, R3, R12
    B.W .fetch_decode_loop

OP .op_0f_01
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    /* The ADDRESS-SIZE decoder from the mode record, not a hardwired 32-bit
       one. `BL L(DECODE_MODRM_32)` was unconditional here, so in a 16-bit
       guest -- which is every guest in this tree -- `LIDT [disp16]` (ModRM
       0x16) decoded as the 32-bit grammar's [ESI] and loaded the IDT from
       whatever ESI pointed at. It never showed because nothing walked the IDT
       afterwards. Same defect the docs describe for the five handlers that
       ignore 0x67; this is a sixth. */
    LDR.W R12, [R2, #REC_MODRM_FN]
    BLX R12
    UBFX R3, R1, #3, #3
    CMP R3, #2
    BEQ.N L(OP_LGDT)
    CMP R3, #3
    BEQ.N L(OP_LIDT)
    B.W .exit_loop

/* LGDT and LIDT read the SAME six-byte operand -- a 16-bit limit then a 32-bit
 * base -- and differ only in where they put it. They shared no code, and LGDT
 * PARSED NOTHING AT ALL: it checked CPL and branched to the fetch loop, so
 * every LGDT a guest issued was silently discarded and the GDT stayed at
 * base 0 limit 0. That is invisible until something walks the table, which is
 * exactly what this work adds, so it is fixed here rather than left.
 *
 * Operand shape and the 0x66 rule: references/tiny386/i386.c:3239-3256. The
 * 32-bit dword is ALWAYS loaded from memory; the 16-bit form differs only by
 * masking the base to 24 bits (i386.c:3246, `if (opsz16) base &= 0xffffff`).
 * The limit is 16 bits in both. DOSBox-X masks in the same place
 * (src/cpu/core_full/op.h:437,441).
 *
 * CPL 0 is required and the fault is #GP(0) -- tiny386 i386.c:3240,
 * DOSBox-X requires it of LLDT/LTR at src/cpu/cpu.cpp:2432,2456. */
L(OP_LGDT):
    BL L(LXXX_CPL_CHECK)
    BL L(LXXX_OPERAND)             @ R3 = limit, R4 = base
    STR.W R3, [R8, #CPU_GDT_LIMIT]
    STR.W R4, [R8, #CPU_GDT_BASE]
    B.W .fetch_decode_loop

L(OP_LIDT):
    BL L(LXXX_CPL_CHECK)
    BL L(LXXX_OPERAND)
    STRH.W R3, [R8, #CPU_IDTR_LIMIT]
    STR.W R4, [R8, #CPU_IDTR_BASE]
    B.W .fetch_decode_loop

/* CPL 0, but only once protected mode is on: in real mode there is no CPL and
   these are unprivileged. Returns, or branches away to the fault.
   R12 ONLY. This is called from LLDT *before* its ModRM is decoded, so R2 is
   still the live MODE RECORD and R1 the ModRM byte -- the register contract in
   CLAUDE.md makes clobbering R2 there a real bug, not a style point. */
L(LXXX_CPL_CHECK):
    LDR.W R12, [R8, #CPU_CR0]
    TST.W R12, #1
    IT EQ
    BXEQ LR                        @ real mode: no privilege to check
    LDR.W R12, [R8, #CPU_CPL]
    CMP R12, #0
    IT EQ
    BXEQ LR
    B.W .raise_gp_fault

/* The shared six-byte read. R7 = effective address from DECODE_MODRM.
   Returns limit in R3 and base in R4. Clobbers R2, R5. */
L(LXXX_OPERAND):
    SEAM_CHECK R2, R7
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LDRH R3, [R2, #0]              @ limit, 16 bits in both forms
    LDRH R4, [R2, #2]
    LDRH R5, [R2, #4]
    BFI R4, R5, #16, #16           @ base, 32 bits read in both forms
#if OPSZ == 2
    BIC.W R4, R4, #0xFF000000      @ ...but masked to 24 for the 16-bit form
#endif
    BX LR

OP .op_int3_CC
    MOV.W R4, #3
    B.N L(HANDLE_INTERRUPT)

OP .op_into_CE
    UBFX R3, R11, #11, #1
    CMP R3, #1
    BNE.W .fetch_decode_loop
    MOV.W R4, #4
    B.N L(HANDLE_INTERRUPT)

OP .op_int_imm8_CD
    CODE_PTR R4
    LDRB R4, [R4, #0]
    ADD.W R10, R10, #1
    B.N L(HANDLE_INTERRUPT)
L(raise_gp_fault):
    MOV.W R4, #13

/* Real-mode interrupt dispatch.
 *
 * The previous version read 8-byte protected-mode gate descriptors and pushed
 * 32-bit frames unconditionally, pushed a literal 0 where CS belongs, and
 * never installed the new CS -- so no vector could ever have worked in real
 * mode, which is the only mode that exists today.
 *
 * Real mode: the IVT entry is 4 bytes, offset first then selector; the frame
 * is FLAGS, CS, IP at the operand size; IF and TF are cleared on entry so the
 * handler is not immediately re-interrupted.
 *
 * Protected mode traps out. The gate path needs descriptor validation and a
 * possible stack switch, and quietly treating a gate as an IVT entry would
 * jump somewhere plausible and fail much later.
 *
 * R4 = vector number.
 */
#ifdef USE_16_BIT
    /* Reachable from the shared exit code in cpu_exec.S, which is outside both
       passes. Needs the Thumb marking or the linker cannot build the branch. */
    .global HANDLE_INTERRUPT_16
    .type HANDLE_INTERRUPT_16, %function
    .thumb_func
#endif
L(HANDLE_INTERRUPT):
    LDR.W R12, [R8, #CPU_CR0]
    TST.W R12, #1
    BNE.W .exit_loop

    PUSH_V R11, R3, R12            @ FLAGS
    LDRH.W R0, [R8, #SEG_CS_SEL]
    PUSH_V R0, R3, R12             @ CS
    LDR.W R0, [R8, #SEG_CS_BASE]
    SUB.W R0, R10, R0              @ IP of the NEXT instruction
    PUSH_V R0, R3, R12

    BIC.W R11, R11, #(1 << 9)      @ IF
    BIC.W R11, R11, #(1 << 8)      @ TF

    LDR.W R3, [R8, #CPU_IDTR_BASE]
    ADD.W R3, R3, R4, LSL #2       @ 4-byte IVT entries, not 8-byte gates
    MEM_GET_PTR_R R12, R3
    LDRH R0, [R12]                 @ new IP
    ADD.W R3, R3, #2
    MEM_GET_PTR_R R12, R3
    LDRH R5, [R12]                 @ new CS selector

    /* Which vector, and what it is about to install. x86_cs_log says an
       interrupt happened; only this says which one, which is the question the
       moment a guest hooks a vector and then walks off.
       Behind X86_CS_LOG with the CS ring, and off by default for the same
       reason -- note the four-instruction IVT RE-READ below, which exists only
       because this block needed R0 as scratch. Twelve instructions of logging
       cost sixteen. */
#if X86_CS_LOG
    MOVW R3, #:lower16:x86_int_vec_log_idx
    MOVT R3, #:upper16:x86_int_vec_log_idx
    LDR R12, [R3]
    ADD.W R0, R12, #1
    STR R0, [R3]
    AND.W R12, R12, #15
    MOVW R3, #:lower16:x86_int_vec_log
    MOVT R3, #:upper16:x86_int_vec_log
    ORR.W R0, R5, R4, LSL #16
    STR.W R0, [R3, R12, LSL #2]

    LDR.W R3, [R8, #CPU_IDTR_BASE]
    ADD.W R3, R3, R4, LSL #2
    MEM_GET_PTR_R R12, R3
    LDRH R0, [R12]                 @ new IP (reloaded: R0 was used above)
#endif

    PUSH {R0, LR}
    MOV.W R4, #1                   @ CS
    BL L(SET_SREG)
    POP {R0, LR}
    LDR.W R3, [R8, #SEG_CS_BASE]
    ADD.W R10, R0, R3
    B.W .fetch_decode_loop

OP .op_iret_CF
    /* Pops IP, CS, FLAGS -- the exact inverse of the frame above. The old
       version popped 32-bit values, skipped a word, and never installed CS. */
    MOVW R3, #:lower16:x86_iret_log
    MOVT R3, #:upper16:x86_iret_log
    LDR.W R12, [R8, #CPU_ESP]
    STR R12, [R3, #12]             @ SP before the pops
    POP_V R0, R3, R12              @ IP
    MOVW R3, #:lower16:x86_iret_log
    MOVT R3, #:upper16:x86_iret_log
    STR R0, [R3]
    POP_V R5, R3, R12              @ CS
    UXTH R5, R5
    MOVW R3, #:lower16:x86_iret_log
    MOVT R3, #:upper16:x86_iret_log
    STR R5, [R3, #4]
    POP_V R11, R3, R12             @ FLAGS
    MOVW R3, #:lower16:x86_iret_log
    MOVT R3, #:upper16:x86_iret_log
    STR R11, [R3, #8]
    PUSH {R0, LR}
    MOV.W R4, #1
    BL L(SET_SREG)
    POP {R0, LR}
    LDR.W R3, [R8, #SEG_CS_BASE]
    ADD.W R10, R0, R3
    B.W .fetch_decode_loop
/* The former L(handle_pit_or_irq) / L(exit_loop) lived here. Both were defined
   and never referenced -- the fetch loop has always branched to the versions in
   cpu_exec.S. They are deleted rather than wired up because the "real" one was
   also wrong: it stored to [R8, #2180], which is not a field at all (it is the
   last word of tlb_write_host), and it abused pit_reload as a pending flag.
   In-guest IRQ injection needs the real-mode IVT dispatch that does not exist
   yet; see PLAN.md Phase 2. Until then the timer exits to the host. */

/* Near Jcc. The displacement follows the OPERAND size -- rel16 under 16-bit,
   rel32 under 32-bit -- and it is signed. The handler was dual-emitted but read
   a hard-coded four bytes, so at 16 bits it consumed two bytes too many and
   jumped by a number assembled out of the following instruction. */
OP .op_0f_jcc_rel32
    UBFX R2, R1, #0, #4
    CODE_PTR R4
    LDS_V R4, R4
    EIP_ADV_V
    BL L(EVAL_COND)
    CMP R3, #1
    IT EQ
    ADDEQ R10, R4
    EIP_WRAP
    B.W .fetch_decode_loop

OP .op_0f_setcc
    UBFX R2, R1, #0, #4
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(OP_0F_SETCC_MEM)
    PUSH {R2}
    BL L(EVAL_COND)
    POP {R2}
    AND.W R4, R1, #7
    AND.W R5, R4, #3
    MOV.W R4, R4, LSR #2
    ADD.W R4, R4, R5, LSL #2
    ADD.W R4, R4, #8
    STRB.W R3, [R8, R4]
    B.W .fetch_decode_loop

L(OP_0F_SETCC_MEM):
    PUSH {R2}
    BL L(DECODE_MODRM_32)
    POP {R2}
    PUSH {R7}
    BL L(EVAL_COND)
    POP {R7}
    UBFX R12, R7, #12, #12
    LDR.W R12, [R9, R12, LSL #2]
    ADDS R12, R12, R7
    MEM_TRAP_W R12
    VGA_ST_B R3, R12, R4
    B.W .fetch_decode_loop

/* 0F 20 /r  MOV r32, CRn      0F 22 /r  MOV CRn, r32
 *
 * How a program tells a 386 from a 286: the 286 has no such instruction, so a
 * CPU benchmark runs this before anything else. TOPBENCH does, and without it
 * the interpreter stopped on the 0F escape.
 *
 * Both are ALWAYS 32-bit whatever the operand size, and the ModRM's mod field
 * is ignored -- the operand is a register however mod is encoded. Only CR0 is
 * modelled; the others read as zero, which is what a 386 with no paging and no
 * task switching would report anyway. Writing CR0 does not itself switch the
 * machine: a segment load with PE set is what traps, and that check already
 * exists in SET_SREG.
 */
OP_AGNOSTIC .op_0f_mov_r32_cr_20
    MOV.W R4, #0
    B.N L(OP_0F_CR)
OP_AGNOSTIC .op_0f_mov_cr_r32_22
    MOV.W R4, #1
L(OP_0F_CR):
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    AND.W R2, R1, #7               @ the r32
    MOV.W R2, R2, LSL #2
    ADD.W R2, R2, #8
    UBFX R3, R1, #3, #3            @ which control register
    CBNZ R4, 1f
    MOV.W R5, #0
    CMP R3, #0
    IT EQ
    LDREQ.W R5, [R8, #CPU_CR0]
    STR.W R5, [R8, R2]
    B.W .fetch_decode_loop
1:  LDR.W R5, [R8, R2]
    CMP R3, #0
    BNE.W .fetch_decode_loop       @ CR2/CR3/CR4: still not modelled
    /* ---- THE CR0.PE TRANSITION ----------------------------------------
     * Writing CR0 does NOT reload or invalidate the segment caches, and that
     * is not an omission -- it is what makes the standard entry sequence work.
     * Between the `MOV CR0` that sets PE and the far jump that follows it, CS
     * still holds its real-mode cache (base = sel<<4, D=0) and the guest is
     * executing 16-bit code at a real-mode base; the far JUMP is what installs
     * a real descriptor and FLUSHES THE PIPELINE. tiny386 does exactly this
     * and nothing more (references/tiny386/i386.c:2251-2255): flush the TLB if
     * PG/WP/PE moved, then store. The same holds leaving protected mode.
     *
     * What DOES have to happen on the transition is re-deriving the dispatch
     * table. The interpreter picks its mode record once, at entry, from CS.D
     * (cpu_exec.S), so a guest that leaves protected mode from a 32-bit code
     * segment would keep running the 32-bit tables against 16-bit real-mode
     * code. Done here, on the PE change only, because this instruction is cold
     * and every alternative site is not. */
    LDR.W R4, [R8, #CPU_CR0]
    STR.W R5, [R8, #CPU_CR0]
    EOR.W R4, R4, R5
    TST.W R4, #1                   @ did PE actually move?
    BEQ.W .fetch_decode_loop
    PUSH {R0-R3, R12, LR}
    MOV R0, R8
    BL x86_pmode_sync_mode
    POP {R0-R3, R12, LR}
    B.W .fetch_decode_loop

OP .op_0f_movzx_gv_eb
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(OP_0F_MOVZX_GV_EB_MEM)
    AND.W R4, R1, #7
    AND.W R5, R4, #3
    MOV.W R4, R4, LSR #2
    ADD.W R4, R4, R5, LSL #2
    ADD.W R4, R4, #8
    LDRB.W R4, [R8, R4]
    B.N L(OP_0F_MOVZX_GV_EB_WRITE)

L(OP_0F_MOVZX_GV_EB_MEM):
    BL L(DECODE_MODRM_32)
    UBFX R4, R7, #12, #12
    LDR.W R4, [R9, R4, LSL #2]
    ADDS R4, R4, R7
    MEM_TRAP_R R4
    LDRB R4, [R4, #0]

L(OP_0F_MOVZX_GV_EB_WRITE):
    UBFX R3, R1, #3, #3
    MOV.W R3, R3, LSL #2
    ADD.W R3, R3, #8
    STR.W R4, [R8, R3]
    B.W .fetch_decode_loop

OP .op_0f_movzx_gv_ew
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(OP_0F_MOVZX_GV_EW_MEM)
    AND.W R4, R1, #7
    MOV.W R4, R4, LSL #2
    ADD.W R4, R4, #8
    LDRH.W R4, [R8, R4]
    B.N L(OP_0F_MOVZX_GV_EW_WRITE)

L(OP_0F_MOVZX_GV_EW_MEM):
    BL L(DECODE_MODRM_32)
    SEAM_CHECK R4, R7
    UBFX R4, R7, #12, #12
    LDR.W R4, [R9, R4, LSL #2]
    ADDS R4, R4, R7
    MEM_TRAP_R R4
    LDRH R4, [R4, #0]

L(OP_0F_MOVZX_GV_EW_WRITE):
    UBFX R3, R1, #3, #3
    MOV.W R3, R3, LSL #2
    ADD.W R3, R3, #8
    STR.W R4, [R8, R3]
    B.W .fetch_decode_loop

OP .op_0f_movsx_gv_eb
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(OP_0F_MOVSX_GV_EB_MEM)
    AND.W R4, R1, #7
    AND.W R5, R4, #3
    MOV.W R4, R4, LSR #2
    ADD.W R4, R4, R5, LSL #2
    ADD.W R4, R4, #8
    LDRSB.W R4, [R8, R4]
    B.N L(OP_0F_MOVSX_GV_EB_WRITE)

L(OP_0F_MOVSX_GV_EB_MEM):
    BL L(DECODE_MODRM_32)
    UBFX R4, R7, #12, #12
    LDR.W R4, [R9, R4, LSL #2]
    ADDS R4, R4, R7
    MEM_TRAP_R R4
    LDRSB.W R4, [R4]

L(OP_0F_MOVSX_GV_EB_WRITE):
    UBFX R3, R1, #3, #3
    MOV.W R3, R3, LSL #2
    ADD.W R3, R3, #8
    STR.W R4, [R8, R3]
    B.W .fetch_decode_loop

OP .op_0f_movsx_gv_ew
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(OP_0F_MOVSX_GV_EW_MEM)
    AND.W R4, R1, #7
    MOV.W R4, R4, LSL #2
    ADD.W R4, R4, #8
    LDRSH.W R4, [R8, R4]
    B.N L(OP_0F_MOVSX_GV_EW_WRITE)

L(OP_0F_MOVSX_GV_EW_MEM):
    BL L(DECODE_MODRM_32)
    SEAM_CHECK R4, R7
    UBFX R4, R7, #12, #12
    LDR.W R4, [R9, R4, LSL #2]
    ADDS R4, R4, R7
    MEM_TRAP_R R4
    LDRSH.W R4, [R4]

L(OP_0F_MOVSX_GV_EW_WRITE):
    UBFX R3, R1, #3, #3
    MOV.W R3, R3, LSL #2
    ADD.W R3, R3, #8
    STR.W R4, [R8, R3]
    B.W .fetch_decode_loop
#ifndef USE_16_BIT

.align 2
.set .op_0f_0F, .op_0f_0F_32
.set .op_0f_00, .op_0f_00_32
.set .op_0f_01, .op_0f_01_32
.set .op_int3_CC, .op_int3_CC_32
.set .op_into_CE, .op_into_CE_32
.set .op_int_imm8_CD, .op_int_imm8_CD_32
.set .op_iret_CF, .op_iret_CF_32
.set .op_0f_jcc_rel32, .op_0f_jcc_rel32_32
.set .op_0f_setcc, .op_0f_setcc_32
.set .op_0f_movzx_gv_eb, .op_0f_movzx_gv_eb_32
.set .op_0f_movzx_gv_ew, .op_0f_movzx_gv_ew_32
.set .op_0f_movsx_gv_eb, .op_0f_movsx_gv_eb_32
.set .op_0f_movsx_gv_ew, .op_0f_movsx_gv_ew_32
.set .op_alu_ev_gv, .op_alu_ev_gv_32
.set .op_alu_gv_ev, .op_alu_gv_ev_32
.set .op_alu_eax_imm32, .op_alu_eax_imm32_32
.set .op_alu_ev_imm32_81, .op_alu_ev_imm32_81_32
.set .op_alu_ev_imm8_83, .op_alu_ev_imm8_83_32
.set .op_mov_ev_gv_89, .op_mov_ev_gv_89_32
.set .op_mov_gv_ev_8B, .op_mov_gv_ev_8B_32
.set .op_mov_ev_imm32_C7, .op_mov_ev_imm32_C7_32
.set .op_mov_r32_imm32, .op_mov_r32_imm32_32
.set .op_mov_eax_moffs32_A1, .op_mov_eax_moffs32_A1_32
.set .op_mov_moffs32_eax_A3, .op_mov_moffs32_eax_A3_32
.set .op_push_r32, .op_push_r32_32
.set .op_pop_r32, .op_pop_r32_32
.set .op_push_imm32_68, .op_push_imm32_68_32
.set .op_push_imm8_6A, .op_push_imm8_6A_32
.set .op_pusha_60, .op_pusha_60_32
.set .op_popa_61, .op_popa_61_32
.set .op_pushfd_9C, .op_pushfd_9C_32
.set .op_popfd_9D, .op_popfd_9D_32
.set .op_pop_ev_8F, .op_pop_ev_8F_32
.set .op_inc_r32, .op_inc_r32_32
.set .op_dec_r32, .op_dec_r32_32
.set .op_lea_8D, .op_lea_8D_32
.set .op_grp3_ev_F7, .op_grp3_ev_F7_32
.set .op_grp5_FF, .op_grp5_FF_32
.set .op_call_rel32_E8, .op_call_rel32_E8_32
.set .op_jmp_rel32_E9, .op_jmp_rel32_E9_32
.set .op_ret_C3, .op_ret_C3_32
.set .op_ret_imm16_C2, .op_ret_imm16_C2_32
.set .op_leave_C9, .op_leave_C9_32
.set .op_enter_C8, .op_enter_C8_32
.set .op_movsd_A5, .op_movsd_A5_32
.set .op_cmpsd_A7, .op_cmpsd_A7_32
.set .op_lodsd_AD, .op_lodsd_AD_32
.set .op_cwde_98, .op_cwde_98_32
.set .op_cdq_99, .op_cdq_99_32
.set .op_imul_imm32_69, .op_imul_imm32_69_32
.set .op_imul_imm8_6B, .op_imul_imm8_6B_32
.set .op_test_ev_gv_85, .op_test_ev_gv_85_32
.set .op_test_eax_imm32_A9, .op_test_eax_imm32_A9_32
.set .op_shift_ev_imm8_C1, .op_shift_ev_imm8_C1_32
.set .op_shift_ev_1_D1, .op_shift_ev_1_D1_32
.set .op_shift_ev_cl_D3, .op_shift_ev_cl_D3_32
    .global opcode_table_32
.set .op_les_C4, .op_les_C4_32
.set .op_lds_C5, .op_lds_C5_32
.set .op_push_es_06, .op_push_es_06_32
.set .op_push_cs_0E, .op_push_cs_0E_32
.set .op_push_ss_16, .op_push_ss_16_32
.set .op_push_ds_1E, .op_push_ds_1E_32
.set .op_pop_es_07, .op_pop_es_07_32
.set .op_pop_ss_17, .op_pop_ss_17_32
.set .op_pop_ds_1F, .op_pop_ds_1F_32
.set .op_jmp_far_EA, .op_jmp_far_EA_32
.set .op_call_far_9A, .op_call_far_9A_32
.set .op_retf_CB, .op_retf_CB_32
.set .op_retf_imm16_CA, .op_retf_imm16_CA_32
.set .op_xchg_eax_r32, .op_xchg_eax_r32_32
.set .op_xchg_ev_gv_87, .op_xchg_ev_gv_87_32
.set .op_stosd_AB, .op_stosd_AB_32
.set .op_scasd_AF, .op_scasd_AF_32
.set .op_in_eax_dx_ED, .op_in_eax_dx_ED_32
.set .op_in_eax_imm8_E5, .op_in_eax_imm8_E5_32
.set .op_out_dx_eax_EF, .op_out_dx_eax_EF_32
.set .op_out_imm8_eax_E7, .op_out_imm8_eax_E7_32
.set .op_insv_6D, .op_insv_6D_32
.set .op_outsv_6F, .op_outsv_6F_32
opcode_table_32:
#include "opcode_table.S"

.align 2
    .global opcode_table_0f_32
opcode_table_0f_32:
#include "opcode_0f_table.S"

#else

.align 2
.set .op_0f_0F, .op_0f_0F_16
.set .op_0f_00, .op_0f_00_16
.set .op_0f_01, .op_0f_01_16
.set .op_int3_CC, .op_int3_CC_16
.set .op_into_CE, .op_into_CE_16
.set .op_int_imm8_CD, .op_int_imm8_CD_16
.set .op_iret_CF, .op_iret_CF_16
.set .op_0f_jcc_rel32, .op_0f_jcc_rel32_16
.set .op_0f_setcc, .op_0f_setcc_16
.set .op_0f_movzx_gv_eb, .op_0f_movzx_gv_eb_16
.set .op_0f_movzx_gv_ew, .op_0f_movzx_gv_ew_16
.set .op_0f_movsx_gv_eb, .op_0f_movsx_gv_eb_16
.set .op_0f_movsx_gv_ew, .op_0f_movsx_gv_ew_16
.set .op_alu_ev_gv, .op_alu_ev_gv_16
.set .op_alu_gv_ev, .op_alu_gv_ev_16
.set .op_alu_eax_imm32, .op_alu_eax_imm32_16
.set .op_alu_ev_imm32_81, .op_alu_ev_imm32_81_16
.set .op_alu_ev_imm8_83, .op_alu_ev_imm8_83_16
.set .op_mov_ev_gv_89, .op_mov_ev_gv_89_16
.set .op_mov_gv_ev_8B, .op_mov_gv_ev_8B_16
.set .op_mov_ev_imm32_C7, .op_mov_ev_imm32_C7_16
.set .op_mov_r32_imm32, .op_mov_r32_imm32_16
.set .op_mov_eax_moffs32_A1, .op_mov_eax_moffs32_A1_16
.set .op_mov_moffs32_eax_A3, .op_mov_moffs32_eax_A3_16
.set .op_push_r32, .op_push_r32_16
.set .op_pop_r32, .op_pop_r32_16
.set .op_push_imm32_68, .op_push_imm32_68_16
.set .op_push_imm8_6A, .op_push_imm8_6A_16
.set .op_pusha_60, .op_pusha_60_16
.set .op_popa_61, .op_popa_61_16
.set .op_pushfd_9C, .op_pushfd_9C_16
.set .op_popfd_9D, .op_popfd_9D_16
.set .op_pop_ev_8F, .op_pop_ev_8F_16
.set .op_inc_r32, .op_inc_r32_16
.set .op_dec_r32, .op_dec_r32_16
.set .op_lea_8D, .op_lea_8D_16
.set .op_grp3_ev_F7, .op_grp3_ev_F7_16
.set .op_grp5_FF, .op_grp5_FF_16
.set .op_call_rel32_E8, .op_call_rel32_E8_16
.set .op_jmp_rel32_E9, .op_jmp_rel32_E9_16
.set .op_ret_C3, .op_ret_C3_16
.set .op_ret_imm16_C2, .op_ret_imm16_C2_16
.set .op_leave_C9, .op_leave_C9_16
.set .op_enter_C8, .op_enter_C8_16
.set .op_movsd_A5, .op_movsd_A5_16
.set .op_cmpsd_A7, .op_cmpsd_A7_16
.set .op_lodsd_AD, .op_lodsd_AD_16
.set .op_cwde_98, .op_cwde_98_16
.set .op_cdq_99, .op_cdq_99_16
.set .op_imul_imm32_69, .op_imul_imm32_69_16
.set .op_imul_imm8_6B, .op_imul_imm8_6B_16
.set .op_test_ev_gv_85, .op_test_ev_gv_85_16
.set .op_test_eax_imm32_A9, .op_test_eax_imm32_A9_16
.set .op_shift_ev_imm8_C1, .op_shift_ev_imm8_C1_16
.set .op_shift_ev_1_D1, .op_shift_ev_1_D1_16
.set .op_shift_ev_cl_D3, .op_shift_ev_cl_D3_16
    .global opcode_table_16
.set .op_les_C4, .op_les_C4_16
.set .op_lds_C5, .op_lds_C5_16
.set .op_push_es_06, .op_push_es_06_16
.set .op_push_cs_0E, .op_push_cs_0E_16
.set .op_push_ss_16, .op_push_ss_16_16
.set .op_push_ds_1E, .op_push_ds_1E_16
.set .op_pop_es_07, .op_pop_es_07_16
.set .op_pop_ss_17, .op_pop_ss_17_16
.set .op_pop_ds_1F, .op_pop_ds_1F_16
.set .op_jmp_far_EA, .op_jmp_far_EA_16
.set .op_call_far_9A, .op_call_far_9A_16
.set .op_retf_CB, .op_retf_CB_16
.set .op_retf_imm16_CA, .op_retf_imm16_CA_16
.set .op_xchg_eax_r32, .op_xchg_eax_r32_16
.set .op_xchg_ev_gv_87, .op_xchg_ev_gv_87_16
.set .op_stosd_AB, .op_stosd_AB_16
.set .op_scasd_AF, .op_scasd_AF_16
.set .op_in_eax_dx_ED, .op_in_eax_dx_ED_16
.set .op_in_eax_imm8_E5, .op_in_eax_imm8_E5_16
.set .op_out_dx_eax_EF, .op_out_dx_eax_EF_16
.set .op_out_imm8_eax_E7, .op_out_imm8_eax_E7_16
.set .op_insv_6D, .op_insv_6D_16
.set .op_outsv_6F, .op_outsv_6F_16
opcode_table_16:
#include "opcode_table.S"

.align 2
    .global opcode_table_0f_16
opcode_table_0f_16:
#include "opcode_0f_table.S"

#endif
