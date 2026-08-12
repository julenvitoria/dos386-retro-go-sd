    .syntax unified
    .cpu cortex-m7
    .thumb

    .section .itcram_hot, "ax", %progbits
    .global x86_cpu_interp_asm
    .type x86_cpu_interp_asm, %function

    /* 
     * Register Mapping:
     * R8 : CPU Context in DTCM (cpu_state_dtcm)
     * R9 : Page Table in DTCM (guest_page_table)
     * R10: Current x86 Instruction Pointer (EIP)
     * R11: Current x86 EFLAGS cache
     *
     * R12 is NOT reserved. It is the ARM intra-procedure scratch register and
     * handlers use it freely for address translation, so the active opcode
     * dispatch table lives in DTCM at TABLE_BASE_OFFSET and is reloaded on
     * every fetch (0-wait-state, dual-issues with the page-table walk).
     */

    /* Field offsets are generated from the C struct -- never hand-written.
       CS lived at segs[1] (offset 60) in x86_cpu.h while five assembly sites
       read offset 48, which is ES. */
#include "x86_offsets.inc"

    .equ EIP_OFFSET, CPU_EIP
    .equ EFLAGS_OFFSET, CPU_EFLAGS
    .equ REGS_OFFSET, CPU_REGS
    .equ REG_ESP_OFFSET, CPU_ESP
    .equ TABLE_BASE_OFFSET, CPU_TABLE_BASE

    /* Mode-record geometry. Must match mode_records[] in the C side. */
    .equ MODE_REC_STRIDE, 2048
    .equ OPSIZE_XOR,      2048          @ 66: toggle operand size
    .equ ADDRSIZE_XOR,    4096          @ 67: toggle address size
    .equ REC_MODRM_FN,    -4
    .equ REC_TABLE_0F,    -8
    .equ MODE_REC_TABLE_OFF, 1024

    /* Translate a guest linear address to a host pointer, CHECKED.
     *
     * host = lin + k, where k comes from the fold table. A mapped page yields
     * a RAM pointer (bit 31 clear); an unmapped page's FOLD_INVALID drives the
     * result negative, so ADDS/BMI catches it. Same four instructions as the
     * old UBFX/LDR/BFC/BFI, but that version trusted the entry: a zero entry
     * produced `0 | offset`, and address 0 is ITCM -- so a guest write to an
     * unmapped page silently overwrote the interpreter's own instructions. */
    .macro MEM_GET_PTR dest_ptr, vaddr
        UBFX \dest_ptr, \vaddr, #12, #12
        LDR \dest_ptr, [R9, \dest_ptr, LSL #2]
        ADDS \dest_ptr, \dest_ptr, \vaddr
        BMI .mem_fault
    .endm

    .macro MEM_READ8 dest, vaddr, scratch
        MEM_GET_PTR \scratch, \vaddr
        LDRB \dest, [\scratch]
    .endm

    .macro MEM_WRITE8 src, vaddr, scratch
        MEM_GET_PTR \scratch, \vaddr
        STRB \src, [\scratch]
    .endm

    .macro MEM_READ16 dest, vaddr, scratch
        MEM_GET_PTR \scratch, \vaddr
        LDRH \dest, [\scratch]
    .endm

    .macro MEM_WRITE16 src, vaddr, scratch
        MEM_GET_PTR \scratch, \vaddr
        STRH \src, [\scratch]
    .endm

    .macro MEM_READ32 dest, vaddr, scratch
        MEM_GET_PTR \scratch, \vaddr
        LDR \dest, [\scratch]
    .endm

    .macro MEM_WRITE32 src, vaddr, scratch
        MEM_GET_PTR \scratch, \vaddr
        STR \src, [\scratch]
    .endm

    // Helper macro for checking IOPL
    .macro CHECK_IOPL scratch1, scratch2
        LDR \scratch1, [R8, #CPU_EFLAGS]  // EFLAGS
        TST \scratch1, #0x00020000        // VM flag
        BNE .raise_gp_fault               // VM86 mode always checks IOPL
        LDR \scratch1, [R8, #CPU_CR0]     // CR0
        TST \scratch1, #1
        BEQ 1f                            // If PE=0, no IOPL check
        LDRH \scratch1, [R8, #SEG_CS_SEL]    // CS.sel
        AND \scratch1, \scratch1, #3
        UBFX \scratch2, R11, #12, #2      // IOPL from EFLAGS
        CMP \scratch1, \scratch2
        BHI .raise_gp_fault
1:
    .endm

x86_cpu_interp_asm:
    // PROLOGUE: Save ARM host state
    PUSH {R4-R11, LR}

    // Load state pointers into the high registers, INDIRECTLY.
    //
    // These used to be MOVW/MOVT of absolute symbols, which only works when the
    // state is statically placed. As a retro-go core it cannot be: DTCM is not
    // a core load target, it is a firmware bump pool reached through
    // mem_ctl(GW_MEM_DTC, ...) at runtime ("AHB/DTCM are firmware dynamic
    // pools, not load targets" -- gnw_core_meta.h). Going through pointer
    // globals lets the identical assembly serve both the bare-metal harness
    // (pointers -> static arrays) and the core (pointers -> dtc_malloc'd).
    // Cost is two loads per interp call, not per instruction.
    MOVW R8, #:lower16:cpu_state_ptr
    MOVT R8, #:upper16:cpu_state_ptr
    LDR R8, [R8]
    MOVW R9, #:lower16:guest_page_table_ptr
    MOVT R9, #:upper16:guest_page_table_ptr
    LDR R9, [R9]


    // Load cached x86 state
    LDR R10, [R8, #EIP_OFFSET]
    LDR R11, [R8, #EFLAGS_OFFSET]
    LDR R12, [R8, #SEG_CS_BASE] // CS.base
    ADD R10, R10, R12

    // Pick the DEFAULT mode record from CS.D. Both axes follow D: a 32-bit code
    // segment defaults to 32-bit operands and 32-bit addressing, a 16-bit one
    // to 16/16. The 66/67 prefixes move off that default per instruction.
    MOVW R12, #:lower16:mode_records_ptr
    MOVT R12, #:upper16:mode_records_ptr
    LDR R12, [R12]
    ADD.W R12, R12, #MODE_REC_TABLE_OFF
    LDR R1, [R8, #SEG_CS_SEL]      // CS sel+flags word
    TST R1, #(1 << 22)             // DESC_D_MASK
    ITT NE
    ADDNE R12, R12, #OPSIZE_XOR    // D=1 -> operand size 32
    ADDNE R12, R12, #ADDRSIZE_XOR  // ...and address size 32
    STR R12, [R8, #TABLE_BASE_OFFSET]

.fetch_decode_loop:
    // Decrement PIT cycle counter and check for hardware interrupts
    LDR R1, [R8, #CPU_PIT_COUNTER] // pit_counter
    // R6 carries a POINTER to the active segment's descriptor, not a base
    // value: limit and rights are then one load away, which is what makes
    // protected mode nearly free later. Reset to DS here because this is the
    // instruction boundary -- a segment override prefix re-enters at
    // .fetch_next_byte_no_reset and so survives into its own instruction, but
    // never into the next one. Placed in the PIT load-use shadow, so it costs
    // an instruction and should cost no cycle.
    //
    // R8 itself is the "no override" sentinel: R8+0 is eip, never a
    // descriptor. It has to be a sentinel rather than simply the DS pointer,
    // because the decoder must tell an absent prefix from an explicit 3E --
    // on a [BP] form the first means SS and the second means DS.
    MOV R6, R8
    // R2 = the active MODE RECORD's dispatch table. Loaded here, above the
    // prefix re-entry point, so 66/67 can simply EOR it and branch back into
    // the fetch: the override then lives exactly as long as the instruction
    // does. This load used to sit below, inside the fetch, which is why
    // operand size could only be chosen once per interp call.
    //
    // Record layout (see mode_records[] in the C side):
    //   [R2, #-8] 0F dispatch table for this operand size
    //   [R2, #-4] ModRM decoder for this ADDRESS size
    //   [R2]      256 entries for this OPERAND size
    // Records are 2 KB apart so each axis is one EOR: OPSIZE_XOR toggles
    // operand size, ADDRSIZE_XOR toggles address size.
    LDR.W R2, [R8, #TABLE_BASE_OFFSET]
    SUBS R1, R1, #1
    STR R1, [R8, #CPU_PIT_COUNTER]
    BEQ .handle_pit_or_irq

.resume_fetch:
.fetch_next_byte_no_reset:
    // Interleaved Fetch & Decode. The table base is issued first so its DTCM
    // load retires under the page-table walk.
    //
    // The tail is a single LDR with Rt=PC: an interworking branch straight out
    // of the table. Entries are absolute `.word handler + 1` (bit 0 set, or
    // ARMv7-M raises INVSTATE on the load), which replaces the old
    // LDRH/ADR/ADD/ORR/BX -- 4 fewer instructions on EVERY dispatched x86
    // instruction -- and dissolves the +/-64KB anchor constraint that forced
    // every handler to sit near .fetch_decode_loop.
    UBFX R0, R10, #12, #12
    LDR.W R0, [R9, R0, LSL #2]
    ADDS R0, R0, R10
    BMI .mem_fault
    LDRB R0, [R0]
    ADD R10, R10, #1
    LDR.W PC, [R2, R0, LSL #2]


#define CONCAT_HELPER(a, b) a ## b
#define CONCAT(a, b) CONCAT_HELPER(a, b)
#define L(name) CONCAT(name, OP_SUFFIX)

// ---------------------------------------------------------
// 32-Bit Handlers
// ---------------------------------------------------------
/* Operand-width macros. The two passes differ ONLY through these, so a handler
 * written against them is correct in both. The old MEM_READ_SZ/MEM_WRITE_SZ/
 * STACK_INC trio was never referenced by a single handler, which is why both
 * dispatch tables produced identical 32-bit behaviour.
 *
 * NSH is the flag-normalization shift: operands are moved to the top of the
 * word so the host PSR's N and V are already the x86 SF and OF for this width.
 */
#define OPSZ 4
#define NSH 0
    .macro LD_V dest, ptr
        LDR \dest, [\ptr]
    .endm
    .macro ST_V src, ptr
        STR \src, [\ptr]
    .endm
    /* Register file access. A 16-bit write must leave the high half alone --
     * that is architectural (AX writes do not clear the top of EAX) and STRH
     * gives it for free. */
    .macro LDREG_V dest, off
        LDR.W \dest, [R8, \off]
    .endm
    .macro STREG_V src, off
        STR.W \src, [R8, \off]
    .endm
    /* Stack. SP is narrowed and wrapped per operand size, and the access goes
       through SS.base -- neither of which the old code did: it used raw ESP as
       a linear address, which is only correct when SS.base is 0. */
    .macro EIP_ADV_V
        ADD.W R10, R10, #4            @ v-size immediate consumed
    .endm
    /* Signed v-size immediate: branch displacements are signed, so the 16-bit
       form must sign-extend. LD_V uses LDRH and would turn every backward
       jump into a forward one. */
    .macro LDS_V dest, ptr
        LDR \dest, [\ptr]
    .endm
    /* IP wrap. In a 32-bit segment EIP does not wrap at 16 bits, so this is
       empty and costs nothing. */
    .macro EIP_WRAP
    .endm
    .macro SP_DEC reg
        SUB.W \reg, \reg, #4
    .endm
    .macro SP_INC reg
        ADD.W \reg, \reg, #4
    .endm
    .macro SP_WRAP reg
    .endm
#define OP_SUFFIX _32
.set PASS_16_BIT, 0

.macro OP_32 name
\name\()_32:
.endm

.macro OP_16 name
.if PASS_16_BIT == 1
    .section .itcram_hot, "ax", %progbits
.endif
\name\()_16:
.endm

#define OP OP_32

// Define macros for agnostic instructions
.macro OP_AGNOSTIC name
.if PASS_16_BIT == 0
\name:
.endif
.endm

#include "cpu_exec_handlers.S"

#undef OPSZ
#undef NSH
#undef OP_SUFFIX
    .purgem LD_V
    .purgem ST_V
    .purgem LDREG_V
    .purgem STREG_V
    .purgem EIP_ADV_V
    .purgem LDS_V
    .purgem EIP_WRAP
    .purgem SP_DEC
    .purgem SP_INC
    .purgem SP_WRAP

// ---------------------------------------------------------
// 16-Bit Handlers
// ---------------------------------------------------------
.set PASS_16_BIT, 1
// ---------------------------------------------------------
#define OPSZ 2
#define NSH 16
    .macro LD_V dest, ptr
        LDRH \dest, [\ptr]
    .endm
    .macro ST_V src, ptr
        STRH \src, [\ptr]
    .endm
    .macro LDREG_V dest, off
        LDRH.W \dest, [R8, \off]
    .endm
    .macro STREG_V src, off
        STRH.W \src, [R8, \off]       @ preserves the high half of the 32-bit reg
    .endm
    .macro EIP_ADV_V
        ADD.W R10, R10, #2
    .endm
    .macro LDS_V dest, ptr
        LDRSH \dest, [\ptr]
    .endm
    /* R10 is the LINEAR eip (CS.base already folded in), so wrapping IP means
       going back to the offset, truncating, and re-adding the base. A 16-bit
       segment cannot execute past 0xFFFF -- control flow wraps to the bottom
       of the segment rather than running into the next one. */
    .macro EIP_WRAP
        LDR.W R12, [R8, #SEG_CS_BASE]
        SUB.W R10, R10, R12
        UXTH R10, R10
        ADD.W R10, R10, R12
    .endm
    .macro SP_DEC reg
        SUB.W \reg, \reg, #2
    .endm
    .macro SP_INC reg
        ADD.W \reg, \reg, #2
    .endm
    .macro SP_WRAP reg
        UXTH \reg, \reg              @ SP wraps inside the 64K stack segment
    .endm
#define OP_SUFFIX _16
#define USE_16_BIT
#undef OP
#define OP OP_16
/* This pass used to emit into .text.discard, which NO linker script discards --
   so it silently landed in .text, outside ITCM, and cross-section branches to
   .itcram_hot now overflow once ITCM is at 0x00000000. Co-locate it in ITCM.
   The duplication itself is removed by the OP_V rebuild; until then it is at
   least visible in the ITCM budget instead of hiding in flash. */
.section .itcram_hot, "ax", %progbits
#include "cpu_exec_handlers.S"

// ---------------------------------------------------------
// Error Handlers and Exits
// ---------------------------------------------------------
.section .itcram_hot, "ax", %progbits
// The PIT counter previously ran to zero and was never reloaded, so it fired
// exactly once per call and then counted down through the whole 32-bit range
// before it could fire again. Reload it here.
//
// The host decides whether to inject: delivering the interrupt in-guest needs
// the real-mode IVT dispatch that does not exist yet (PLAN.md Phase 2), and the
// existing HANDLE_INTERRUPT reads 8-byte protected-mode gates and pushes 32-bit
// frames unconditionally, which is wrong for real mode.
.handle_pit_or_irq:
    LDR R1, [R8, #CPU_PIT_RELOAD]
    STR R1, [R8, #CPU_PIT_COUNTER]

    /* Deliver a pending hardware interrupt IN-GUEST rather than exiting for
     * it. The host raises one by writing the vector to hard_int_pending and
     * dropping pit_counter to 1, so the check lands on the next instruction
     * boundary and costs nothing anywhere else -- there is no per-instruction
     * test for it.
     *
     * IF gates delivery, as the hardware does: a guest inside a critical
     * section with interrupts masked must not be interrupted, and the request
     * stays pending until it clears IF again.
     *
     * Dispatch goes to the 16-bit frame builder because delivery only happens
     * in real mode (the handler traps on CR0.PE). A 32-bit code segment in
     * real mode would want a 32-bit frame; that is unreal mode and nothing
     * here needs it yet. */
    LDR R1, [R8, #CPU_HARD_INT_PENDING]
    CMP R1, #0
    BEQ .exit_timer
    TST R11, #(1 << 9)             @ IF
    BEQ .exit_timer
    MOV R0, #0
    STR R0, [R8, #CPU_HARD_INT_PENDING]
    MOV R4, R1
    B.W HANDLE_INTERRUPT_16

.exit_timer:
    MOV R0, #X86_EXIT_TIMER
    STR R0, [R8, #CPU_EXIT_REASON]
    B .save_state_and_exit

// A guest access hit an unmapped page. Loud, not silent: the old code wrote
// into ITCM here and carried on until the corrupted instruction was executed.
.mem_fault:
    MOV R0, #X86_EXIT_MEM_FAULT
    STR R0, [R8, #CPU_EXIT_REASON]
    B .save_state_and_exit

.raise_gp_fault:
    MOV R0, #X86_EXIT_GPF
    STR R0, [R8, #CPU_EXIT_REASON]
    B .save_state_and_exit

// HLT, or an opcode with no handler. Distinct from the timer so the host can
// tell "ran out of budget" from "cannot execute this".
.exit_loop:
    /* Record the opcode. "Unimplemented" without saying which one sends you
       disassembling guest memory to guess; R0 still holds the byte that
       dispatched here. */
    STR R0, [R8, #CPU_EXIT_INFO]
    MOV R0, #X86_EXIT_UNIMPLEMENTED
    STR R0, [R8, #CPU_EXIT_REASON]

.save_state_and_exit:
    LDR R12, [R8, #SEG_CS_BASE] // CS.base
    SUB R10, R10, R12
    STR R10, [R8, #EIP_OFFSET]
    STR R11, [R8, #EFLAGS_OFFSET]
    POP {R4-R11, PC}
