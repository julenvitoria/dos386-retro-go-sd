OP .op_0f_0F
    /* The 0F table comes from the mode record, not a fixed symbol, and lands
       in R3 so R2 keeps pointing at the record -- the two-byte opcodes that
       take a ModRM still need its decoder pointer. */
    LDR.W R3, [R2, #REC_TABLE_0F]
    UBFX R1, R10, #12, #12
    LDR.W R1, [R9, R1, LSL #2]
    ADDS R1, R1, R10
    BMI .mem_fault
    LDRB R1, [R1, #0]
    ADD R10, R10, #1
    LDR.W PC, [R3, R1, LSL #2]
L(opcode_0f_dispatch_target):


OP .op_0f_01
    UBFX R1, R10, #12, #12
    LDR.W R1, [R9, R1, LSL #2]
    ADDS R1, R1, R10
    BMI .mem_fault
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    BL L(DECODE_MODRM_32)
    UBFX R3, R1, #3, #3
    CMP R3, #2
    BEQ.N L(OP_LGDT)
    CMP R3, #3
    BEQ.N L(OP_LIDT)
    B.W .exit_loop

L(OP_LGDT):
    LDR.W R2, [R8, #CPU_CR0]
    TST.W R2, #1
    BEQ.W .fetch_decode_loop
    LDRH.W R2, [R8, #SEG_CS_SEL]
    AND.W R2, R2, #3
    CMP R2, #0
    BNE.W .raise_gp_fault
    B.W .fetch_decode_loop

L(OP_LIDT):
    LDR.W R2, [R8, #CPU_CR0]
    TST.W R2, #1
    BEQ.N L(OP_LIDT_OK)
    LDRH.W R2, [R8, #SEG_CS_SEL]
    AND.W R2, R2, #3
    CMP R2, #0
    BNE.W .raise_gp_fault
L(OP_LIDT_OK):
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    BMI .mem_fault
    LDRH R3, [R2, #0]
    STRH.W R3, [R8, #44]
    ADD.W R2, R2, #2
    LDRH R4, [R2, #0]
    ADD.W R2, R2, #2
    LDRH R5, [R2, #0]
    BFI R4, R5, #16, #16
    STR.W R4, [R8, #40]
    B.W .fetch_decode_loop

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
    UBFX R4, R10, #12, #12
    LDR.W R4, [R9, R4, LSL #2]
    ADDS R4, R4, R10
    BMI .mem_fault
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
    MEM_GET_PTR R12, R3
    LDRH R0, [R12]                 @ new IP
    ADD.W R3, R3, #2
    MEM_GET_PTR R12, R3
    LDRH R5, [R12]                 @ new CS selector

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

OP .op_0f_jcc_rel32
    UBFX R2, R1, #0, #4
    UBFX R4, R10, #12, #12
    LDR.W R4, [R9, R4, LSL #2]
    ADDS R4, R4, R10
    BMI .mem_fault
    LDR R4, [R4, #0]
    ADD.W R10, R10, #4
    BL L(EVAL_COND)
    CMP R3, #1
    IT EQ
    ADDEQ R10, R4
    B.W .fetch_decode_loop

OP .op_0f_setcc
    UBFX R2, R1, #0, #4
    UBFX R1, R10, #12, #12
    LDR.W R1, [R9, R1, LSL #2]
    ADDS R1, R1, R10
    BMI .mem_fault
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    AND.W R4, R1, #192
    CMP R4, #192
    BNE.N L(OP_0F_SETCC_MEM)
    PUSH {R2}
    BL L(EVAL_COND)
    POP {R2}
    AND.W R4, R1, #7
    AND.W R5, R4, #3
    MOV.W R5, R5, LSL #2
    MOV.W R4, R4, LSR #2
    ADD R4, R5
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
    BMI .mem_fault
    STRB.W R3, [R12]
    B.W .fetch_decode_loop

OP .op_0f_movzx_gv_eb
    UBFX R1, R10, #12, #12
    LDR.W R1, [R9, R1, LSL #2]
    ADDS R1, R1, R10
    BMI .mem_fault
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    AND.W R2, R1, #192
    CMP R2, #192
    BNE.N L(OP_0F_MOVZX_GV_EB_MEM)
    AND.W R4, R1, #7
    AND.W R5, R4, #3
    MOV.W R5, R5, LSL #2
    MOV.W R4, R4, LSR #2
    ADD R4, R5
    ADD.W R4, R4, #8
    LDRB.W R4, [R8, R4]
    B.N L(OP_0F_MOVZX_GV_EB_WRITE)

L(OP_0F_MOVZX_GV_EB_MEM):
    BL L(DECODE_MODRM_32)
    UBFX R4, R7, #12, #12
    LDR.W R4, [R9, R4, LSL #2]
    ADDS R4, R4, R7
    BMI .mem_fault
    LDRB R4, [R4, #0]

L(OP_0F_MOVZX_GV_EB_WRITE):
    UBFX R3, R1, #3, #3
    MOV.W R3, R3, LSL #2
    ADD.W R3, R3, #8
    STR.W R4, [R8, R3]
    B.W .fetch_decode_loop

OP .op_0f_movzx_gv_ew
    UBFX R1, R10, #12, #12
    LDR.W R1, [R9, R1, LSL #2]
    ADDS R1, R1, R10
    BMI .mem_fault
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    AND.W R2, R1, #192
    CMP R2, #192
    BNE.N L(OP_0F_MOVZX_GV_EW_MEM)
    AND.W R4, R1, #7
    MOV.W R4, R4, LSL #2
    ADD.W R4, R4, #8
    LDRH.W R4, [R8, R4]
    B.N L(OP_0F_MOVZX_GV_EW_WRITE)

L(OP_0F_MOVZX_GV_EW_MEM):
    BL L(DECODE_MODRM_32)
    UBFX R4, R7, #12, #12
    LDR.W R4, [R9, R4, LSL #2]
    ADDS R4, R4, R7
    BMI .mem_fault
    LDRH R4, [R4, #0]

L(OP_0F_MOVZX_GV_EW_WRITE):
    UBFX R3, R1, #3, #3
    MOV.W R3, R3, LSL #2
    ADD.W R3, R3, #8
    STR.W R4, [R8, R3]
    B.W .fetch_decode_loop

OP .op_0f_movsx_gv_eb
    UBFX R1, R10, #12, #12
    LDR.W R1, [R9, R1, LSL #2]
    ADDS R1, R1, R10
    BMI .mem_fault
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    AND.W R2, R1, #192
    CMP R2, #192
    BNE.N L(OP_0F_MOVSX_GV_EB_MEM)
    AND.W R4, R1, #7
    AND.W R5, R4, #3
    MOV.W R5, R5, LSL #2
    MOV.W R4, R4, LSR #2
    ADD R4, R5
    ADD.W R4, R4, #8
    LDRSB.W R4, [R8, R4]
    B.N L(OP_0F_MOVSX_GV_EB_WRITE)

L(OP_0F_MOVSX_GV_EB_MEM):
    BL L(DECODE_MODRM_32)
    UBFX R4, R7, #12, #12
    LDR.W R4, [R9, R4, LSL #2]
    ADDS R4, R4, R7
    BMI .mem_fault
    LDRSB.W R4, [R4]

L(OP_0F_MOVSX_GV_EB_WRITE):
    UBFX R3, R1, #3, #3
    MOV.W R3, R3, LSL #2
    ADD.W R3, R3, #8
    STR.W R4, [R8, R3]
    B.W .fetch_decode_loop

OP .op_0f_movsx_gv_ew
    UBFX R1, R10, #12, #12
    LDR.W R1, [R9, R1, LSL #2]
    ADDS R1, R1, R10
    BMI .mem_fault
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    AND.W R2, R1, #192
    CMP R2, #192
    BNE.N L(OP_0F_MOVSX_GV_EW_MEM)
    AND.W R4, R1, #7
    MOV.W R4, R4, LSL #2
    ADD.W R4, R4, #8
    LDRSH.W R4, [R8, R4]
    B.N L(OP_0F_MOVSX_GV_EW_WRITE)

L(OP_0F_MOVSX_GV_EW_MEM):
    BL L(DECODE_MODRM_32)
    UBFX R4, R7, #12, #12
    LDR.W R4, [R9, R4, LSL #2]
    ADDS R4, R4, R7
    BMI .mem_fault
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
.set .op_stosd_AB, .op_stosd_AB_32
.set .op_scasd_AF, .op_scasd_AF_32
.set .op_in_eax_dx_ED, .op_in_eax_dx_ED_32
.set .op_in_eax_imm8_E5, .op_in_eax_imm8_E5_32
.set .op_out_dx_eax_EF, .op_out_dx_eax_EF_32
.set .op_out_imm8_eax_E7, .op_out_imm8_eax_E7_32
opcode_table_32:
#include "opcode_table.S"

.align 2
    .global opcode_table_0f_32
opcode_table_0f_32:
#include "opcode_0f_table.S"

#else

.align 2
.set .op_0f_0F, .op_0f_0F_16
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
.set .op_stosd_AB, .op_stosd_AB_16
.set .op_scasd_AF, .op_scasd_AF_16
.set .op_in_eax_dx_ED, .op_in_eax_dx_ED_16
.set .op_in_eax_imm8_E5, .op_in_eax_imm8_E5_16
.set .op_out_dx_eax_EF, .op_out_dx_eax_EF_16
.set .op_out_imm8_eax_E7, .op_out_imm8_eax_E7_16
opcode_table_16:
#include "opcode_table.S"

.align 2
    .global opcode_table_0f_16
opcode_table_0f_16:
#include "opcode_0f_table.S"

#endif
