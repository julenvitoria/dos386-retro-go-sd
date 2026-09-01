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
    /* NAME THE OPCODE, NOT THE ESCAPE.
     *
     * .exit_loop reports CPU_EXIT_INFO from R0, and R0 still held 0x0F here --
     * so EVERY unimplemented two-byte opcode reported the identical
     * exit_info=0x0F, and three unrelated titles "converged" on one byte that
     * was never the byte any of them executed. The escape is not the
     * instruction. (They really were on three different opcodes: 0F 01 /6,
     * 0F 01 /6 and 0F AF. docs/13.)
     *
     * R0 is dead across the whole 0F path -- the contract's "R0 is the opcode"
     * ends at this second dispatch, PROF_DISPATCH already takes R0 as scratch,
     * and every 0F handler writes R0 before reading it -- so it costs one
     * instruction to make it carry the truth: 0x0Fnn, the escape in the high
     * byte and the SECOND byte in the low one. exit_info > 0xFF therefore reads
     * as "two-byte opcode 0F nn" with no ambiguity against the one-byte table.
     *
     * The reported EIP is likewise PAST the whole two-byte opcode: R10 was
     * advanced by the dispatch and again just above, so the instruction that
     * exited starts at EIP-2. */
    ORR.W R0, R1, #0x0F00
    LDR.W PC, [R3, R1, LSL #2]
L(opcode_0f_dispatch_target):


/* 0F 00 -- group 6, now ALL SIX members: SLDT /0, STR /1, LLDT /2, LTR /3,
 * VERR /4, VERW /5. (/6 and /7 do not exist and keep .exit_loop.)
 *
 * STR and LTR used to sit in the .exit_loop with them, on the reasoning that
 * INT 21h is HLE'd so there is no task state to point a task register at.
 * MEASUREMENT SUPERSEDES THAT: scripts/opcode_demand.py counts 5 STR sites and
 * 2 LTR sites in DOS4GW.EXE, one of them at +0x476e in the PE/paging re-entry
 * path. A TSS at DPL 0 is needed for SS0:ESP0 and the I/O permission bitmap
 * and is NOT evidence of a ring change -- the same scan found all 256 IDT
 * entries at DPL 0 and no DPL 3 code descriptor anywhere in the image. So the
 * task register is loaded and stored and NO TASK SWITCH IS IMPLEMENTED.
 *
 * The operand is a 16-bit selector however the operand size is set, for every
 * member of the group. */
OP .op_0f_00
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    UBFX R3, R1, #3, #3
    CMP R3, #0
    BNE.N 1f
    LDR.W R5, [R8, #CPU_LDT_SEL]
    B.W L(OP_STORE_SEL)
1:  CMP R3, #1
    BNE.N 2f
    LDR.W R5, [R8, #CPU_TR_SEL]
    B.W L(OP_STORE_SEL)
2:  CMP R3, #6                     @ /6 and /7 do not exist
    BHS.W .exit_loop
    CMP R3, #4
    BHS.W L(OP_VERRW)              @ /4 VERR, /5 VERW
    CMP R3, #3
    BEQ.W L(OP_LTR)
    /* ---- LLDT (/2) ---- */
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

/* SLDT (/0) and STR (/1): store a 16-bit selector. Both are UNPRIVILEGED in
 * both references -- CPU_STR is a bare `return cpu_tss.selector`
 * (dosbox-x src/cpu/cpu.cpp:2441) with no CPL test at all -- and they differ
 * only in which selector the caller put in R5, so they share this tail.
 *
 * The register form writes a HALFWORD and leaves the high half of a 32-bit
 * destination alone. That matches SMSW below and is what SLDT shipped with;
 * Intel's zero-extension for the r32 form buys nothing for a guest that is
 * only ever going to compare the selector. */
L(OP_STORE_SEL):
    CMP R1, #192
    BLO.W L(OP_STORE_SEL_MEM)
    AND.W R3, R1, #7
    ADD.W R3, R8, R3, LSL #2
    STRH.W R5, [R3, #CPU_REGS]
    B.W .fetch_decode_loop
L(OP_STORE_SEL_MEM):
    /* R5 is the selector and must survive DECODE_MODRM, which clobbers R2-R5
       (CLAUDE.md). It does not, so it is re-read after the walk -- and it is
       re-read from the same place the dispatch chose, carried in R4, because
       R1 alone cannot say whether /0 or /1 got here. */
    LDR.W R12, [R2, #REC_MODRM_FN]
    BLX R12
    /* R1 is the ModRM byte and survives the walk; R4 would NOT -- the decoder
       clobbers R2-R5 -- so the /reg field is recomputed here rather than
       carried across. */
    TST.W R1, #0x08                @ /reg bit 0: 0 = SLDT, 1 = STR
    ITE EQ
    LDREQ.W R5, [R8, #CPU_LDT_SEL]
    LDRNE.W R5, [R8, #CPU_TR_SEL]
    MEM_GET_PTR R3, R7
    VGA_ST_H R5, R3, R12
    B.W .fetch_decode_loop

/* LTR (/3). The selector is fetched exactly as LLDT's is; only the C function
 * differs, and so does nothing else -- validation, the busy write-back and the
 * refusal to switch tasks all live in x86_pmode_ltr(). CPL 0 in protected mode
 * only, the same predicate LLDT uses (dosbox-x src/cpu/cpu.cpp:2456). */
L(OP_LTR):
    BL L(LXXX_CPL_CHECK)
    CMP R1, #192
    BLO.W L(OP_LTR_MEM)
    AND.W R5, R1, #7
    ADD.W R5, R8, R5, LSL #2
    LDRH.W R5, [R5, #CPU_REGS]
    B.W L(OP_LTR_GO)
L(OP_LTR_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN]
    BLX R12
    SEAM_CHECK R3, R7
    UBFX R3, R7, #12, #12
    LDR.W R3, [R9, R3, LSL #2]
    ADDS R3, R3, R7
    MEM_TRAP_R R3
    LDRH R5, [R3, #0]
L(OP_LTR_GO):
    PUSH {R0-R3, R12, LR}
    MOV R0, R8
    UXTH R1, R5
    BL x86_pmode_ltr
    CBNZ R0, 1f
    POP {R0-R3, R12, LR}
    B.W .fetch_decode_loop
1:  POP {R0-R3, R12, LR}
    B.W L(PMODE_EXIT_GPF)

/* VERR (/4) and VERW (/5). Pure predicates: they set ZF and touch nothing
 * else, and they NEVER FAULT -- an inaccessible selector is reported as ZF=0
 * (dosbox-x src/cpu/cpu.cpp:2904, :2937), which is why nothing here has an
 * exit path. R3 still holds the /reg field, and bit 0 of it is exactly the
 * VERR/VERW distinction. */
L(OP_VERRW):
    /* NOT AN INSTRUCTION IN REAL MODE. VERR/VERW (and LAR/LSL below) are
       invalid outside protected mode, and without this a real-mode title
       that lands on one would walk a GDT it never installed and come back
       with an answer. .exit_loop names the opcode instead. */
    LDR.W R3, [R8, #CPU_CR0]
    TST.W R3, #1
    BEQ.W .exit_loop
    CMP R1, #192
    BLO.W L(OP_VERRW_MEM)
    AND.W R5, R1, #7
    ADD.W R5, R8, R5, LSL #2
    LDRH.W R5, [R5, #CPU_REGS]
    B.W L(OP_VERRW_GO)
L(OP_VERRW_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN]
    BLX R12
    SEAM_CHECK R3, R7
    UBFX R3, R7, #12, #12
    LDR.W R3, [R9, R3, LSL #2]
    ADDS R3, R3, R7
    MEM_TRAP_R R3
    LDRH R5, [R3, #0]
L(OP_VERRW_GO):
    /* From R1, not from R3: R3 held the /reg field on entry and DECODE_MODRM
       clobbers R2-R5, so anything carried across the walk in it is gone. */
    UBFX R4, R1, #3, #1            @ 0 = VERR, 1 = VERW
    PUSH {R0-R3, R12, LR}
    MOV R0, R8
    UXTH R1, R5
    MOV R2, R4
    BL x86_pmode_verrw
    CMP R0, #0
    ITE EQ
    BICEQ.W R11, R11, #0x40        @ ZF
    ORRNE.W R11, R11, #0x40
    POP {R0-R3, R12, LR}
    B.W .fetch_decode_loop

/* The shared protected-mode fault exit. x86_pmode_* left the vector in the
   return value and the error code in cpu->pmode_err; the exit carries the
   error code, matching what the walk's callers already do. */
L(PMODE_EXIT_GPF):
    MOV.W R0, #X86_EXIT_GPF
    STR.W R0, [R8, #CPU_EXIT_REASON]
    LDR.W R0, [R8, #CPU_PMODE_ERR]
    STR.W R0, [R8, #CPU_EXIT_INFO]
    B.W .save_state_and_exit

/* 0F 02 LAR and 0F 03 LSL -- read a descriptor's ACCESS RIGHTS or its LIMIT
 * into a register and report accessibility in ZF. 58 and 28 static sites in
 * DOS4GW.EXE respectively, which is what put them on this lane's list; they
 * are how an extender asks "may I touch this selector" without risking a
 * fault, so a title that uses them uses them constantly.
 *
 * They are the SAME handler but for the C function, so they share everything
 * below R0. Declared OP rather than OP_AGNOSTIC because the DESTINATION is
 * operand-sized -- LAR r16 writes a halfword and LAR r32 a word -- and
 * STREG_V is what makes that true in both passes rather than in neither.
 *
 * THE SOURCE IS ALWAYS 16 BITS whatever the operand size: it is a selector.
 *
 * ZF=0 leaves the destination UNMODIFIED, which is architectural and is why
 * the store is inside the taken arm rather than after it.
 */
OP .op_0f_lar_02
OP .op_0f_lsl_03
    /* Real mode: not an instruction. See L(OP_VERRW). */
    LDR.W R3, [R8, #CPU_CR0]
    TST.W R3, #1
    BEQ.W .exit_loop
    /* R0 carries 0x0F02 or 0x0F03 from the two-byte dispatch and DECODE_MODRM
       does not clobber it (R2-R5 only), so bit 0 of R0 IS the LAR/LSL choice
       and nothing has to be stashed across the walk. */
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192
    BLO.W L(OP_LAR_MEM)
    AND.W R5, R1, #7
    ADD.W R5, R8, R5, LSL #2
    LDRH.W R5, [R5, #CPU_REGS]
    B.W L(OP_LAR_GO)
L(OP_LAR_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN]
    BLX R12
    /* A HALFWORD load is a multi-byte fold site and needs SEAM_CHECK: two
       bytes can straddle a 4 KB granule whose neighbour is not the host memory
       after it (CLAUDE.md, docs/10). */
    SEAM_CHECK R3, R7
    UBFX R3, R7, #12, #12
    LDR.W R3, [R9, R3, LSL #2]
    ADDS R3, R3, R7
    MEM_TRAP_R R3
    LDRH R5, [R3, #0]
L(OP_LAR_GO):
    /* R1 -- the ModRM byte -- survives DECODE_MODRM by contract, so the
       destination register is recomputed from it AFTER the walk rather than
       read before it. Reading it early is the bug MOV Ev,Gv shipped with. */
    PUSH {R0-R3, R12, LR}
    SUB SP, SP, #8                 @ the out slot; 8 keeps SP 8-byte aligned
    MOV R0, R8
    UXTH R1, R5
    MOV R2, SP
    LDR R4, [SP, #8]               @ R0 as pushed: the opcode
    TST.W R4, #1
    BNE.N 1f
    BL x86_pmode_lar
    B.N 2f
1:  BL x86_pmode_lsl
2:  LDR R5, [SP]                   @ the value, valid only when R0 != 0
    ADD SP, SP, #8
    CMP R0, #0
    ITE EQ
    BICEQ.W R11, R11, #0x40        @ ZF
    ORRNE.W R11, R11, #0x40
    MOV R4, R0
    POP {R0-R3, R12, LR}
    CBZ R4, 3f
    UBFX R3, R1, #3, #3
    ADD.W R3, R3, #(CPU_REGS / 4)
    LSLS R3, R3, #2
    STREG_V R5, R3
3:  B.W .fetch_decode_loop

/* 0F 06 CLTS -- clear CR0.TS. Five static sites in DOS4GW.EXE and it exited
 * today, which is the whole reason it is here; it is the cheapest thing on
 * this lane's list and it was still a wall.
 *
 * TS is bit 3 of CR0, set by a task switch to make the next x87 instruction
 * trap so a kernel can swap the FPU state lazily. We never set it -- there is
 * no task switch -- so clearing it is genuinely all there is to do, and that
 * is the CORRECT emulation rather than a stub: the guest's own LMSW and MOV
 * CR0 can set TS, and CLTS is the only instruction that clears it alone.
 *
 * Privileged: CPL 0 in protected mode, unprivileged in real mode, the same
 * predicate LGDT/LIDT/LLDT/LTR/LMSW share. No ModRM byte at all. */
OP_AGNOSTIC .op_0f_clts_06
    BL L(LXXX_CPL_CHECK)
    LDR.W R3, [R8, #CPU_CR0]
    BIC.W R3, R3, #8
    STR.W R3, [R8, #CPU_CR0]
    B.W .fetch_decode_loop

OP .op_0f_01
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    /* SMSW (/4) is decided BEFORE the ModRM walk, because it is the one member
     * of this group with a REGISTER form -- `0F 01 E0`, SMSW AX, which is what
     * a Microsoft/Borland C startup issues to read CR0.EM before it probes for
     * an x87. LGDT/LIDT are memory-only, so the walk was unconditional here and
     * a mod=11 encoding fell through it into .exit_loop: the whole title died
     * on the first instruction of its runtime's coprocessor detection.
     * Measured on MORTAL_KOMBAT (docs/13). */
    UBFX R3, R1, #3, #3
    CMP R3, #4
    BEQ.W L(OP_SMSW)
    /* LMSW (/6) is decided here for the SAME reason SMSW is: it has a register
     * form, `0F 01 F0` = LMSW AX, and that is the encoding a DOS extender
     * actually issues. Deciding it after the memory-only walk would send the
     * mod=11 form into .exit_loop exactly as SMSW's used to go. */
    CMP R3, #6
    BEQ.W L(OP_LMSW)
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
    /* A GROUP OPCODE IS NOT AN OPCODE. "0F 01 is unimplemented" is not a
       reportable fact -- /2, /3, /4 and /6 are implemented and /0, /1, /7 are
       not, and which one a guest issued is the entire question. R1 is the
       ModRM byte and is still live, so the exit carries it in the high half:
       exit_info reads 0xMM0F01. Cold path, one instruction. */
    ORR.W R0, R0, R1, LSL #16
    B.W .exit_loop

/* SMSW: store the low 16 bits of CR0. Unprivileged at every CPL on a 286 and
 * later (tiny386 i386.c, DOSBox-X src/cpu/cpu.cpp CPU_SMSW), which is the whole
 * point of it -- LMSW is the privileged half. A halfword in both operand sizes,
 * matching SLDT above; Intel's `SMSW r32` zero-extension buys nothing for a
 * guest that only ever tests the bottom four bits. */
L(OP_SMSW):
    LDR.W R5, [R8, #CPU_CR0]
    CMP R1, #192
    BLO.W L(OP_SMSW_MEM)
    AND.W R3, R1, #7
    ADD.W R3, R8, R3, LSL #2
    STRH.W R5, [R3, #CPU_REGS]
    B.W .fetch_decode_loop
L(OP_SMSW_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN]
    BLX R12
    LDR.W R5, [R8, #CPU_CR0]
    MEM_GET_PTR R3, R7
    VGA_ST_H R5, R3, R12
    B.W .fetch_decode_loop

/* LMSW -- 0F 01 /6. THE INSTRUCTION THAT TURNS PROTECTED MODE ON, and the one
 * that was really stopping Dark Forces and Mortal Kombat.
 *
 * Both were reported as "unimplemented opcode 0x0F" at one address shared with
 * a third title, which was an artefact of the exit reporting twice over:
 * .exit_loop named the ESCAPE byte rather than the opcode, and "0F 01" names a
 * GROUP rather than a member. Both are fixed above; what they were really
 * executing is `0F 01 F0` = LMSW AX -- the 286 entry sequence, which is what a
 * DOS extender uses because it must also run on a 286.
 *
 * SEMANTICS, and the two references AGREE here (they do not always):
 *   references/tiny386/i386.c:3292-3294  cr0 = (cr0 & ((~0xf) | 1)) | (src & 0xf)
 *   references/dosbox-x/src/cpu/cpu.cpp:2788-2795  same, with the rule spelled
 *   out: "low 4 bits only, cannot change PE ... stuck on, cannot exit
 *   protected mode, 286 style".
 * So it writes PE/MP/EM/TS and NOTHING above bit 3, and PE is SET-ONLY: the
 * `BIC #0xE` below keeps bit 0 of the old CR0, which is exactly tiny386's
 * `(~0xf)|1` mask. Clearing PE is MOV CR0's job, not this one.
 *
 * The operand is 16 bits in BOTH operand sizes -- LMSW has no 32-bit form --
 * so this is deliberately NOT written against the width macros.
 *
 * CPL 0, but only in protected mode: L(LXXX_CPL_CHECK) is the same predicate
 * LGDT/LIDT/LLDT use, and it is DOSBox-X's rule (`cpu.pmode && cpu.cpl>0`)
 * rather than tiny386's unconditional one -- in real mode there is no CPL.
 *
 * The PE-transition tail is MOV CR0's, byte for byte and for its reasons: the
 * segment caches are deliberately NOT reloaded (the far jump that follows is
 * what installs a real descriptor and flushes the pipeline), and
 * x86_pmode_sync_mode() is called only when PE actually MOVED, because the
 * interpreter picks its dispatch tables once from CS.D. */
L(OP_LMSW):
    BL L(LXXX_CPL_CHECK)
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(OP_LMSW_MEM)
    AND.W R3, R1, #7
    ADD.W R3, R8, R3, LSL #2
    LDRH.W R5, [R3, #CPU_REGS]
    B.N L(OP_LMSW_APPLY)
L(OP_LMSW_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN]
    BLX R12
    /* A HALFWORD load is a multi-byte fold site, so it needs SEAM_CHECK --
       CLAUDE.md's rule, and the reason is that the two bytes can straddle a
       4 KB granule whose neighbour is not the host memory after it. */
    SEAM_CHECK R2, R7
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LDRH R5, [R2, #0]
L(OP_LMSW_APPLY):
    LDR.W R4, [R8, #CPU_CR0]
    AND.W R5, R5, #0xF             @ PE, MP, EM, TS -- nothing else
    BIC.W R3, R4, #0xE             @ keep everything above bit 3, AND old PE
    ORR.W R5, R3, R5
    STR.W R5, [R8, #CPU_CR0]
    EOR.W R4, R4, R5
    TST.W R4, #1                   @ did PE actually move?
    BEQ.W .fetch_decode_loop
    PUSH {R0-R3, R12, LR}
    MOV R0, R8
    BL x86_pmode_sync_mode
    POP {R0-R3, R12, LR}
    B.W .fetch_decode_loop

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
 * Protected mode walks an 8-byte IDT GATE -- L(INT_PMODE) below, and every
 * instruction of it is out of line, so the real-mode arm from here down is
 * byte-for-byte what it was. Only the branch TARGET of the CR0 test changed,
 * which is the same move that made the descriptor walk free (docs/14 Sec.2);
 * that test already stood here to trap protected mode out.
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
    BNE.W L(INT_PMODE)

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

/* ---------------------------------------------------------------------------
 * The PROTECTED-MODE arm. COLD and out of line, so the real-mode fall-through
 * above keeps its straight-line layout and its exact instruction sequence.
 *
 * All of it is C (x86_pmode_int_gate, x86_pmode.c): eight descriptor bytes,
 * a dozen predicates and a frame whose WIDTH comes from the gate's type rather
 * than from the operand size -- which is precisely why PUSH_V could not build
 * it. None of that is anything Thumb-2 would do better, and it runs once per
 * delivered interrupt in a protected-mode guest and never at all in a
 * real-mode one.
 *
 * THE STATE HANDOFF. R10 is LINEAR EIP and the frame wants the SEGMENT OFFSET,
 * so CS.base comes off before the call and back on after; R11 is the live
 * EFLAGS the frame carries. Both are synced through the context fields the
 * exit path already uses, so there is one convention rather than two.
 *
 * Every path out of here LEAVES -- to the fetch loop or to the exit -- so
 * nothing is pushed and LR is free to be clobbered by the BL. A fault exits
 * with X86_EXIT_GPF carrying the error code, the way an undeliverable
 * segmentation fault already does, and .save_state_and_exit restores SP from
 * cpu->host_sp so the depth this was taken at does not matter (docs/14 Sec.5).
 *
 * R4 = vector. The interrupt is delivered as a SOFTWARE one, which is what
 * every site that reaches here is: INT imm8, INT3, INTO, and #UD from the
 * hypercall stub with HLE off. A hardware IRQ arrives through the same label
 * from .handle_pit_or_irq and is treated as software too -- the ONLY thing
 * that distinguishes them is the gate-DPL check, which cannot fire at CPL 0,
 * and measuring whether a title ever leaves CPL 0 is the step after this one.
 * Nothing is guessed here: if it does, this is the line to change.
 */
L(INT_PMODE):
    LDR.W R3, [R8, #SEG_CS_BASE]
    SUB.W R3, R10, R3
    STR.W R3, [R8, #EIP_OFFSET]        @ the offset the frame must carry
    STR.W R11, [R8, #EFLAGS_OFFSET]
    MOV R0, R8
    MOV R1, R4
    MOV.W R2, #X86_INT_SOFTWARE
    MOV.W R3, #0                       @ no error code
    BL x86_pmode_int_gate
    CBNZ R0, 1f
    LDR.W R11, [R8, #EFLAGS_OFFSET]
    LDR.W R10, [R8, #EIP_OFFSET]
    LDR.W R3, [R8, #SEG_CS_BASE]
    ADD.W R10, R10, R3
    B.W .fetch_decode_loop
1:  MOV.W R0, #X86_EXIT_GPF
    STR.W R0, [R8, #CPU_EXIT_REASON]
    LDR.W R0, [R8, #CPU_PMODE_ERR]
    STR.W R0, [R8, #CPU_EXIT_INFO]
    B.W .save_state_and_exit

OP .op_iret_CF
    /* PROTECTED MODE goes to L(IRET_PMODE) below. These three instructions are
       the only thing this work costs real mode anywhere, and they are here
       rather than folded into the mode record because the record block would
       have to double -- 8 KB to 16 KB of DTCM against 5,856 B free (STATUS.md
       Budgets), which does not fit. IRET is per-interrupt, not per-instruction.

       They cannot go below the pops either: a protected-mode IRET's CS has to
       be validated by IRET's OWN rules (DPL == RPL for a non-conforming
       target), which are stricter than the ones L(SET_SREG)'s CS arm applies,
       and the 16-bit form must CARRY the high half of EFLAGS rather than let
       POP_V zero it. Both decisions have to be made before anything commits. */
    LDR.W R3, [R8, #CPU_CR0]
    TST.W R3, #1
    BNE.W L(IRET_PMODE)
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

/* ---------------------------------------------------------------------------
 * IRET in PROTECTED MODE, same privilege. COLD and out of line.
 *
 * x86_pmode_iret() does the pops, the validation and the install, for the same
 * reason the gate walk is in C: it is a dozen predicates that run once per
 * return in a protected-mode guest and never in a real-mode one. It also has
 * to do the pops ITSELF rather than take three popped values, because the
 * three are SPECULATIVE -- a CS that fails IRET's DPL/RPL rule must leave ESP
 * reading exactly as it did before the instruction, and that unwind is only
 * possible where the pops happened.
 *
 * The OPERAND SIZE is the frame width and it is a per-pass constant, so it is
 * an immediate here and this handler is declared OP, not OP_AGNOSTIC.
 *
 * EIP is NOT advanced past the IRET: it is replaced wholesale by the popped
 * value, so R10 is rebuilt from cpu->eip and the new CS.base. R11 likewise
 * comes back from cpu->eflags, which the C side has masked by CPL.
 */
L(IRET_PMODE):
    STR.W R11, [R8, #EFLAGS_OFFSET]
    MOV R0, R8
#if OPSZ == 4
    MOV.W R1, #1
#else
    MOV.W R1, #0
#endif
    BL x86_pmode_iret
    CBNZ R0, 1f
    LDR.W R11, [R8, #EFLAGS_OFFSET]
    LDR.W R10, [R8, #EIP_OFFSET]
    LDR.W R3, [R8, #SEG_CS_BASE]
    ADD.W R10, R10, R3
    B.W .fetch_decode_loop
1:  MOV.W R0, #X86_EXIT_GPF
    STR.W R0, [R8, #CPU_EXIT_REASON]
    LDR.W R0, [R8, #CPU_PMODE_ERR]
    STR.W R0, [R8, #CPU_EXIT_INFO]
    B.W .save_state_and_exit

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
/* 0F A2 -- CPUID. Leaf 0 and leaf 1, and nothing else.
 *
 * We ANSWER it rather than refusing it because the guest has already proved it
 * may: the same routine toggles EFLAGS.ID (bit 21) with `0F BA` above and finds
 * it writable, which on real silicon is the definition of "CPUID is present".
 * The alternative -- masking ID in POPF so the guest concludes 386 -- puts a
 * test on the hot flag path to buy the same outcome, and would then be a lie
 * about a machine whose 0F opcodes are all 486-era anyway.
 *
 * The signature is a 486SX: family 4, EDX feature bits ALL ZERO, and bit 0 of
 * EDX is the on-chip FPU bit, so this agrees with .op_esc_x87 -- one machine
 * with no coprocessor, described the same way twice. Do not set a feature bit
 * here without the instruction behind it.
 *
 * MEASURED on MORTAL_KOMBAT (docs/13): `66 B8 01 00 00 00 / 0F A2 / 8A C4 /
 * 25 0F 00` -- leaf 1, then AH masked to the family nibble. Its THIRD wall. */
OP_AGNOSTIC .op_0f_cpuid_A2
    LDR.W R3, [R8, #CPU_REGS]
    CBNZ R3, 1f
    MOV.W R4, #1                   @ EAX: highest leaf we answer
    STR.W R4, [R8, #(CPU_REGS + 0)]
    MOVW R4, #0x6547               @ EBX "Genu"
    MOVT R4, #0x756E
    STR.W R4, [R8, #(CPU_REGS + 12)]
    MOVW R4, #0x6E69               @ EDX "ineI"
    MOVT R4, #0x4965
    STR.W R4, [R8, #(CPU_REGS + 8)]
    MOVW R4, #0x746E               @ ECX "ntel"
    MOVT R4, #0x6C65
    STR.W R4, [R8, #(CPU_REGS + 4)]
    B.W .fetch_decode_loop
1:  MOVW R4, #0x0400               @ family 4, model 0, stepping 0
    STR.W R4, [R8, #(CPU_REGS + 0)]
    MOV.W R4, #0
    STR.W R4, [R8, #(CPU_REGS + 4)]
    STR.W R4, [R8, #(CPU_REGS + 8)]    @ no FPU, no TSC, no MSR, no CX8
    STR.W R4, [R8, #(CPU_REGS + 12)]
    B.W .fetch_decode_loop

/* 0F BA -- Group 8: BT/BTS/BTR/BTC with an IMMEDIATE bit number.
 *
 * The imm8 forms only. The register-source forms (0F A3/AB/B3/BB) address a bit
 * STRING -- a signed offset that walks off the effective address into adjacent
 * words -- and are deliberately left at .exit_loop rather than half-built.
 * These four take the offset MODULO the operand size (Intel SDM, Group 8), so
 * the address never moves and the whole opcode is a mask.
 *
 * /0../3 are undefined encodings and stay loud.
 *
 * Only CF is architecturally defined; OF/SF/ZF/AF/PF are undefined, so R11's
 * other bits are left exactly as they were rather than invented.
 *
 * MEASURED on MORTAL_KOMBAT (docs/13): its runtime's CPU identification is
 * `66 0F BA FA 15 / 66 0F BA FA 12` -- toggling EFLAGS.ID and EFLAGS.AC in EDX
 * to tell a 386 from a 486 from a 586 -- and it was the title's SECOND wall,
 * three instructions after the x87 detection. */
OP .op_0f_ba
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    UBFX R3, R1, #3, #3
    CMP R3, #4
    BLO.W .exit_loop
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.W L(BTGRP_MEM)
    AND.W R4, R1, #7
    MOV.W R4, R4, LSL #2
    ADD.W R4, R4, #CPU_REGS
    LDREG_V R5, R4
    CODE_PTR R3
    LDRB R3, [R3, #0]
    ADD.W R10, R10, #1
    AND.W R3, R3, #(OPSZ * 8 - 1)
    LSR.W R12, R5, R3
    BFI R11, R12, #0, #1           @ CF = the bit as it was
    UBFX R12, R1, #3, #3
    CMP R12, #4
    BEQ.W .fetch_decode_loop       @ BT: no write-back
    MOV.W R0, #1
    LSL.W R0, R0, R3
    CMP R12, #5
    BEQ.N 1f
    CMP R12, #6
    BEQ.N 2f
    EOR.W R5, R5, R0               @ BTC
    B.N 3f
1:  ORR.W R5, R5, R0               @ BTS
    B.N 3f
2:  BIC.W R5, R5, R0               @ BTR
3:  STREG_V R5, R4
    B.W .fetch_decode_loop

L(BTGRP_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    CODE_PTR R3                    @ the imm8 comes AFTER the displacement
    LDRB R3, [R3, #0]
    ADD.W R10, R10, #1
    AND.W R3, R3, #(OPSZ * 8 - 1)
    UBFX R12, R1, #3, #3           @ R1 survives DECODE_MODRM; R2-R5 do not
    CMP R12, #4
    BNE.N 4f
    /* BT: a pure read, so it takes the READ walk. Going through the RMW walk
       would record a VGA latch for an access that never writes. */
    SEAM_CHECK R2, R7
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LD_V R5, R2
    LSR.W R12, R5, R3
    BFI R11, R12, #0, #1
    B.W .fetch_decode_loop
4:  MEM_GET_PTR_RMW R2, R7
    LD_V R5, R2
    LSR.W R4, R5, R3
    BFI R11, R4, #0, #1
    MOV.W R0, #1
    LSL.W R0, R0, R3
    CMP R12, #5
    BEQ.N 5f
    CMP R12, #6
    BEQ.N 6f
    EOR.W R5, R5, R0
    B.N 7f
5:  ORR.W R5, R5, R0
    B.N 7f
6:  BIC.W R5, R5, R0
7:  VGA_ST_V R5, R2, R7
    B.W .fetch_decode_loop

/* 0F A3 BT / 0F AB BTS / 0F B3 BTR / 0F BB BTC -- the REGISTER-source forms.
 *
 * These are NOT 0F BA with a different source, and that is the whole trap.
 * The imm8 group takes its bit number MODULO the operand size, so its address
 * never moves; here the offset is a SIGNED Gv that indexes a BIT STRING and
 * can range far outside the addressed operand. The effective operand is
 *
 *     EA + (offset >> log2(bits)) * OPSZ,   bit = offset & (bits - 1)
 *
 * with an ARITHMETIC shift, so a negative offset walks BACKWARDS. Written here
 * as `(offset ASR 3) BIC (OPSZ-1)`, which is the same floor division in two
 * instructions and needs no per-pass constant. An implementation that instead
 * masks the offset to the operand width works for every small offset and
 * corrupts memory for the large ones -- which is exactly the case a bitmap
 * allocator, the only thing that uses these, generates.
 *
 * The REGISTER form has no address to move, so there the offset IS taken
 * modulo the operand size (Intel SDM, BT/BTS/BTR/BTC).
 *
 * Only CF is architecturally defined; OF/SF/ZF/AF/PF are undefined and R11's
 * other bits are left exactly as they were, as 0F BA already does.
 *
 * The moved address is a NEW FOLD SITE and the access is 2 or 4 bytes wide, so
 * it goes through MEM_GET_PTR_R / MEM_GET_PTR_RMW, both of which carry
 * SEAM_CHECK. Doing the fold by hand off R7 would be silently wrong at one
 * address in 2,048 (CLAUDE.md, the granule seam).
 *
 * The address is formed LINEARLY, on the post-segment R7. A 16-bit address size
 * should wrap the offset within the segment before adding the base; that costs
 * a base subtract and re-add on every one of these, and no offset a real bit
 * string produces comes near the wrap. Noted rather than paid for.
 *
 * R0 carries which of the four this is (0=BT, 1=BTS, 2=BTR, 3=BTC) and survives
 * DECODE_MODRM by the same contract LAR/LSL rely on above -- the decoder
 * clobbers R2-R5 only. R5, the offset, does NOT, so it is pushed. */
OP .op_0f_bt_A3
    MOV.W R0, #0
    B.N L(BTSTR)
OP .op_0f_bts_AB
    MOV.W R0, #1
    B.N L(BTSTR)
OP .op_0f_btr_B3
    MOV.W R0, #2
    B.N L(BTSTR)
OP .op_0f_btc_BB
    MOV.W R0, #3
L(BTSTR):
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    UBFX R3, R1, #3, #3            @ Gv -- the bit offset, SIGNED
    LSL.W R3, R3, #2
    ADD.W R3, R3, #CPU_REGS
    LDREG_V R5, R3
    VSEXT(R5)                      @ LDREG_V zero-extends at 16 bits
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(BTSTR_MEM)
    AND.W R3, R5, #(OPSZ * 8 - 1)  @ register form: modulo the operand size
    AND.W R4, R1, #7
    LSL.W R4, R4, #2
    ADD.W R4, R4, #CPU_REGS
    LDREG_V R5, R4
    LSR.W R12, R5, R3
    BFI R11, R12, #0, #1           @ CF = the bit as it was
    CMP R0, #0
    BEQ.W .fetch_decode_loop       @ BT: no write-back
    MOV.W R12, #1
    LSL.W R12, R12, R3
    CMP R0, #1
    BEQ.N 1f
    CMP R0, #2
    BEQ.N 2f
    EOR.W R5, R5, R12              @ BTC
    B.N 3f
1:  ORR.W R5, R5, R12              @ BTS
    B.N 3f
2:  BIC.W R5, R5, R12              @ BTR
3:  STREG_V R5, R4
    B.W .fetch_decode_loop

L(BTSTR_MEM):
    PUSH {R5}                      @ the offset does not survive the walk
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    POP {R5}
    ASR.W R4, R5, #3               @ floor(offset / 8), sign-propagating
    BIC.W R4, R4, #(OPSZ - 1)      @ ...rounded down to a whole operand
    ADD.W R7, R7, R4
    AND.W R3, R5, #(OPSZ * 8 - 1)  @ the bit within that operand
    CMP R0, #0
    BNE.N 4f
    /* BT: a pure read, so it takes the READ walk -- the RMW walk would record
       a VGA latch for an access that never writes. */
    MEM_GET_PTR_R R2, R7
    LD_V R5, R2
    LSR.W R4, R5, R3
    BFI R11, R4, #0, #1
    B.W .fetch_decode_loop
4:  MEM_GET_PTR_RMW R2, R7
    LD_V R5, R2
    LSR.W R4, R5, R3
    BFI R11, R4, #0, #1
    MOV.W R12, #1
    LSL.W R12, R12, R3
    CMP R0, #1
    BEQ.N 5f
    CMP R0, #2
    BEQ.N 6f
    EOR.W R5, R5, R12
    B.N 7f
5:  ORR.W R5, R5, R12
    B.N 7f
6:  BIC.W R5, R5, R12
7:  VGA_ST_V R5, R2, R7
    B.W .fetch_decode_loop

/* 0F BC BSF / 0F BD BSR -- Gv <- index of the lowest/highest set bit of Ev.
 *
 * ZF is what software branches on: it is SET when the source is zero, and the
 * DESTINATION IS THEN LEFT ALONE. The SDM calls the destination undefined in
 * that case, but every real part and both references leave it unmodified
 * (dosbox-x src/cpu/core_normal/prefix_0f.h, the 0xbc/0xbd cases), so a guest
 * that pre-seeds the destination and only tests ZF gets the answer it expects.
 * CF/OF/SF/AF/PF are undefined and are not written.
 *
 * Thumb-2 does both in two instructions: RBIT+CLZ is the index of the lowest
 * set bit, CLZ alone the distance from the top. At 16 bits LD_V/LDREG_V
 * zero-extend, so CLZ lands in 16..31 and 31-CLZ is still the right index. */
OP .op_0f_bsf_BC
    MOV.W R0, #0
    B.N L(BSFR)
OP .op_0f_bsr_BD
    MOV.W R0, #1
L(BSFR):
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(BSFR_MEM)
    AND.W R3, R1, #7
    LSL.W R3, R3, #2
    ADD.W R3, R3, #CPU_REGS
    LDREG_V R5, R3
    B.N L(BSFR_DO)
L(BSFR_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    MEM_GET_PTR_R R2, R7
    LD_V R5, R2
L(BSFR_DO):
    CMP R5, #0
    BNE.N 1f
    ORR.W R11, R11, #(1 << 6)      @ ZF = 1, destination untouched
    B.W .fetch_decode_loop
1:  BIC.W R11, R11, #(1 << 6)
    CBZ R0, 2f
    CLZ R5, R5
    RSB.W R5, R5, #31              @ BSR
    B.N 3f
2:  RBIT R5, R5
    CLZ R5, R5                     @ BSF
3:  UBFX R4, R1, #3, #3
    LSL.W R4, R4, #2
    ADD.W R4, R4, #CPU_REGS
    STREG_V R5, R4
    B.W .fetch_decode_loop

/* 0F A4/A5 SHLD Ev,Gv,imm8|CL and 0F AC/AD SHRD Ev,Gv,imm8|CL.
 *
 * The bits shifted into Ev come from Gv, which is why a "shift Ev and OR in
 * something" 32-bit implementation silently gets the 16-bit forms wrong: at 16
 * bits the pair is a 32-BIT CONCATENATION and the bits that arrive come from
 * the far end of it. The 16-bit pass below therefore builds that dword
 * explicitly and takes the half the direction dictates, following
 * dosbox-x src/cpu/instructions.h DSHLW/DSHRW including its `count > 16`
 * fixup. A count above the operand size is architecturally UNDEFINED and is
 * not graded anywhere; matching the reference is the point, not inventing one.
 *
 * THE COUNT IS MASKED TO FIVE BITS BEFORE THE ZERO TEST, and a masked-to-zero
 * count must move NO FLAG AT ALL -- the same rule the Grp2 shifts follow, and
 * the reason `SHRD x,y,CL` with CL==0 is safe inside a loop that is still
 * carrying a CF it needs. The imm8 is consumed either way, so EIP advances
 * before the test.
 *
 * CF is the last bit shifted out. SF/ZF/PF come from the result. OF is defined
 * only for a count of one and is the Intel rule -- the destination's sign
 * changed -- which is why the original Ev is kept in R7 across the shift.
 *
 * R0 carries the form: bit 0 selects SHRD, bit 1 says the count is an imm8.
 * R4 carries the destination: a nonzero register-file offset, or 0 meaning
 * "memory, through the pointer in R2". The two paths converge with R10 on the
 * imm8 in both cases, so the count is fetched once. */
OP .op_0f_shld_imm_A4
    MOV.W R0, #2
    B.N L(DSH)
OP .op_0f_shrd_imm_AC
    MOV.W R0, #3
    B.N L(DSH)
OP .op_0f_shld_cl_A5
    MOV.W R0, #0
    B.N L(DSH)
OP .op_0f_shrd_cl_AD
    MOV.W R0, #1
L(DSH):
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(DSH_MEM)
    AND.W R4, R1, #7
    LSL.W R4, R4, #2
    ADD.W R4, R4, #CPU_REGS
    LDREG_V R3, R4                 @ Ev, the destination
    B.N L(DSH_SRC)
L(DSH_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    MEM_GET_PTR_RMW R2, R7         @ loaded and stored through one pointer
    LD_V R3, R2
    MOV.W R4, #0                   @ 0 = the destination is memory, at R2
L(DSH_SRC):
    UBFX R5, R1, #3, #3            @ Gv, the source of the shifted-in bits
    LSL.W R5, R5, #2
    ADD.W R5, R5, #CPU_REGS
    LDREG_V R5, R5
    TST.W R0, #2
    BEQ.N 1f
    CODE_PTR R12                   @ the imm8 comes AFTER the displacement
    LDRB R12, [R12, #0]
    ADD.W R10, R10, #1
    B.N 2f
1:  LDRB.W R12, [R8, #(CPU_REGS + 4)]   @ CL
2:  ANDS R12, R12, #31
    BEQ.N L(DSH_NOFLAGS)
    MOV.W R7, R3                   @ the original Ev, for OF
#if NSH
    TST.W R0, #1
    BNE.N 3f
    /* SHLD 16: the pair is (Ev << 16) | Gv, shifted LEFT, top half kept. */
    ORR.W R1, R5, R3, LSL #16
    RSB.W R0, R12, #32
    LSR.W R0, R1, R0               @ CF = bit (32 - n) of the pair
    LSL.W R1, R1, R12
    SUB.W R3, R12, #16             @ n > 16: an ARM shift of >= 32 yields 0,
    LSL.W R3, R5, R3               @ so this needs no test of its own
    ORR.W R1, R1, R3
    LSR.W R3, R1, #16
    B.N L(DSH_FLAGS)
    /* SHRD 16: the pair is (Gv << 16) | Ev, shifted RIGHT, low half kept. */
3:  ORR.W R1, R3, R5, LSL #16
    SUB.W R0, R12, #1
    LSR.W R0, R1, R0               @ CF = bit (n - 1) of the pair
    LSR.W R1, R1, R12
    RSB.W R3, R12, #32
    LSL.W R3, R5, R3               @ the same fixup; it can only reach bit 15
    ORR.W R3, R1, R3               @ when n > 16
#else
    RSB.W R1, R12, #32
    TST.W R0, #1
    BNE.N 3f
    /* SHLD 32 */
    LSR.W R0, R3, R1               @ CF = the last bit out of the destination
    LSL.W R3, R3, R12
    LSR.W R5, R5, R1
    ORR.W R3, R3, R5
    B.N L(DSH_FLAGS)
    /* SHRD 32 */
3:  SUB.W R0, R12, #1
    LSR.W R0, R3, R0               @ CF = the last bit out of the destination
    LSR.W R3, R3, R12
    LSL.W R5, R5, R1
    ORR.W R3, R3, R5
#endif
L(DSH_FLAGS):
    BFI R11, R0, #0, #1            @ CF
    EOR.W R1, R3, R7
    LSR.W R1, R1, #VMSB
    BFI R11, R1, #11, #1           @ OF, defined only for a count of 1
    VMASK(R3)
    VTEST(R1, R3)
    MRS R12, APSR
    X86_SF_ZF R12, R1
    X86_PF R3, R1
L(DSH_NOFLAGS):
    /* A masked-to-zero count: the value is unchanged and no flag moved. The
       write-back still happens, exactly as the Grp2 shifts' does. */
    CBZ R4, 4f
    STREG_V R3, R4
    B.W .fetch_decode_loop
4:  VGA_ST_V R3, R2, R1
    B.W .fetch_decode_loop

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

/* 0F AF /r -- IMUL Gv, Ev. The two-operand form: Gv = Gv * Ev, low half only.
 *
 * MEASURED on EPIC_PINBALL (docs/13), and it is the whole reason that title's
 * "same exit as the other two" reading was wrong -- with the exit naming the
 * opcode instead of the 0F escape, its exit_info reads 0x0FAF where Dark
 * Forces and Mortal Kombat read 0x0F01. Three titles, three different walls.
 *
 * Structurally .op_imul_imm32_69 with the immediate replaced by the
 * destination register's own current value.
 *
 * CF AND OF ARE SET HERE, and they are the whole point of the instruction's
 * flag contract: the destination keeps only the LOW half, so CF=OF=1 is the
 * only thing that says the discarded half mattered. Software multiplies and
 * then branches on JC/JO to detect the overflow, so an unset pair is not a
 * missing nicety, it is a branch that silently takes the wrong arm.
 *
 * The test is "the full product sign-extends from the destination width":
 * at 32 bits SMULL's high word must equal lo>>31 arithmetic; at 16 bits the
 * full 32-bit product must equal SXTH of its own low half. SF/ZF/AF/PF are
 * architecturally UNDEFINED for IMUL and are left exactly as they were rather
 * than invented.
 *
 * `69`/`6B` in cpu_exec_handlers.S have the IDENTICAL gap and are NOT fixed
 * here -- that file belongs to another lane. docs/07 rows 69/6B still read
 * DEFECT. */
OP .op_0f_imul_AF
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(IMULAF_MEM)
    AND.W R2, R1, #7
    ADD.W R2, R8, R2, LSL #2
    ADD.W R2, R2, #8
    LD_V R3, R2
    B.N L(IMULAF_DO)
L(IMULAF_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    SEAM_CHECK R2, R7
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LD_V R3, R2
L(IMULAF_DO):
    UBFX R4, R1, #3, #3
    MOV.W R4, R4, LSL #2
    ADD.W R4, R4, #8
    LDREG_V R5, R4
    /* Both operands arrived through LD_V/LDREG_V, which ZERO-extend at 16 bits.
       The low half of the product does not care, but the overflow test does,
       so the 16-bit pass signs them first. R0 ends up 0 iff the full product
       fits the destination. */
#if NSH
    SXTH R3, R3
    SXTH R5, R5
    MUL.W R3, R3, R5               @ the FULL 32-bit signed product
    SXTH R0, R3
    SUB.W R0, R0, R3
#else
    SMULL R3, R0, R3, R5           @ R3 = low, R0 = high
    SUB.W R0, R0, R3, ASR #31
#endif
    STREG_V R3, R4                 @ destination keeps the low half only
    CMP R0, #0
    IT NE
    MOVNE R0, #1
    BFI R11, R0, #0, #1            @ CF
    BFI R11, R0, #11, #1           @ OF
    B.W .fetch_decode_loop

/* 0F A0/A8 PUSH FS,GS   0F A1/A9 POP FS,GS
 *
 * MEASURED on EPIC_PINBALL (docs/13): its wall was `0F A0`, exit_info 0x0FA0
 * at 443,391 instructions. Watcom's real-mode runtime pushes FS around a
 * routine that borrows it, which is also why 0F B4 (LFS) is next door.
 *
 * These are the 06/0E/16/1E forms with a two-byte opcode and a segment index
 * of 4 or 5, so they REUSE L(PUSH_SREG) / L(POP_SREG) rather than restating
 * them: the stack slot is operand-size wide (PUSH_V/POP_V are the only stack
 * primitives that apply SS.base and wrap SP at the stack width), the selector
 * is zero-extended on the way out and truncated on the way in, and the load
 * goes through L(SET_SREG) so a protected-mode guest gets the descriptor walk
 * for free. Three instructions each, per pass. */
OP .op_0f_push_fs_A0
    MOV.W R4, #4                   @ FS
    B.W L(PUSH_SREG)
OP .op_0f_push_gs_A8
    MOV.W R4, #5                   @ GS
    B.W L(PUSH_SREG)
OP .op_0f_pop_fs_A1
    MOV.W R0, #4
    B.W L(POP_SREG)
OP .op_0f_pop_gs_A9
    MOV.W R0, #5
    B.W L(POP_SREG)

/* 0F B2 LSS / 0F B4 LFS / 0F B5 LGS -- Gv <- [mem], Sreg <- [mem + operand
 * size]. Exactly LES/LDS (C4/C5) with a different segment index, so they enter
 * the SAME L(LOAD_FAR_PTR): it fetches the ModRM itself, refuses a register
 * source as #UD, SEAM_CHECKs both halves of the far pointer, writes Gv with
 * STREG_V and installs the selector through L(SET_SREG). R0 carries the index
 * in, which is the contract C4/C5 already use.
 *
 * LSS is the one that matters beyond FS/GS convenience: it is how code loads
 * SS:SP in a single instruction so no interrupt can land on a half-switched
 * stack. We do not model that inhibit -- `MOV SS,x` does not either, and it is
 * the same known gap (docs/07), not a new one. */
OP .op_0f_lss_B2
    MOV.W R0, #2                   @ SS
    B.W L(LOAD_FAR_PTR)
OP .op_0f_lfs_B4
    MOV.W R0, #4                   @ FS
    B.W L(LOAD_FAR_PTR)
OP .op_0f_lgs_B5
    MOV.W R0, #5                   @ GS
    B.W L(LOAD_FAR_PTR)

#ifndef USE_16_BIT

.align 2
.set .op_0f_0F, .op_0f_0F_32
.set .op_0f_00, .op_0f_00_32
.set .op_0f_lar_02, .op_0f_lar_02_32
.set .op_0f_lsl_03, .op_0f_lsl_03_32
.set .op_0f_01, .op_0f_01_32
.set .op_0f_ba, .op_0f_ba_32
.set .op_0f_bt_A3, .op_0f_bt_A3_32
.set .op_0f_bts_AB, .op_0f_bts_AB_32
.set .op_0f_btr_B3, .op_0f_btr_B3_32
.set .op_0f_btc_BB, .op_0f_btc_BB_32
.set .op_0f_bsf_BC, .op_0f_bsf_BC_32
.set .op_0f_bsr_BD, .op_0f_bsr_BD_32
.set .op_0f_shld_imm_A4, .op_0f_shld_imm_A4_32
.set .op_0f_shld_cl_A5, .op_0f_shld_cl_A5_32
.set .op_0f_shrd_imm_AC, .op_0f_shrd_imm_AC_32
.set .op_0f_shrd_cl_AD, .op_0f_shrd_cl_AD_32
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
.set .op_0f_imul_AF, .op_0f_imul_AF_32
.set .op_0f_push_fs_A0, .op_0f_push_fs_A0_32
.set .op_0f_push_gs_A8, .op_0f_push_gs_A8_32
.set .op_0f_pop_fs_A1, .op_0f_pop_fs_A1_32
.set .op_0f_pop_gs_A9, .op_0f_pop_gs_A9_32
.set .op_0f_lss_B2, .op_0f_lss_B2_32
.set .op_0f_lfs_B4, .op_0f_lfs_B4_32
.set .op_0f_lgs_B5, .op_0f_lgs_B5_32
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
.set .op_0f_lar_02, .op_0f_lar_02_16
.set .op_0f_lsl_03, .op_0f_lsl_03_16
.set .op_0f_01, .op_0f_01_16
.set .op_0f_ba, .op_0f_ba_16
.set .op_0f_bt_A3, .op_0f_bt_A3_16
.set .op_0f_bts_AB, .op_0f_bts_AB_16
.set .op_0f_btr_B3, .op_0f_btr_B3_16
.set .op_0f_btc_BB, .op_0f_btc_BB_16
.set .op_0f_bsf_BC, .op_0f_bsf_BC_16
.set .op_0f_bsr_BD, .op_0f_bsr_BD_16
.set .op_0f_shld_imm_A4, .op_0f_shld_imm_A4_16
.set .op_0f_shld_cl_A5, .op_0f_shld_cl_A5_16
.set .op_0f_shrd_imm_AC, .op_0f_shrd_imm_AC_16
.set .op_0f_shrd_cl_AD, .op_0f_shrd_cl_AD_16
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
.set .op_0f_imul_AF, .op_0f_imul_AF_16
.set .op_0f_push_fs_A0, .op_0f_push_fs_A0_16
.set .op_0f_push_gs_A8, .op_0f_push_gs_A8_16
.set .op_0f_pop_fs_A1, .op_0f_pop_fs_A1_16
.set .op_0f_pop_gs_A9, .op_0f_pop_gs_A9_16
.set .op_0f_lss_B2, .op_0f_lss_B2_16
.set .op_0f_lfs_B4, .op_0f_lfs_B4_16
.set .op_0f_lgs_B5, .op_0f_lgs_B5_16
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
