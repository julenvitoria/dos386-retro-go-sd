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
#include "x86_trace.inc"

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

    /* ---------------------------------------------------------------- *
     * The NOR cold-page trap.
     *
     * A page backed by memory-mapped NOR lives at 0x9xxxxxxx, so bit 31 of
     * `lin + k` is SET and the fold check traps on EVERY access to it. That is
     * deliberate and the sentinel encoding is not negotiable: the check is two
     * instructions on the hottest path in the project, and a cold page paying a
     * slow path is the entire point of putting it in NOR.
     *
     * So the FAST PATH BELOW IS UNCHANGED, instruction for instruction. All
     * that moves is where the BMI goes: to a per-site stub that can service the
     * access, instead of straight to .mem_fault, which killed the guest.
     *
     * The stub is out of line but still in ITCM. It has to be: BMI reaches
     * +/-1 MB and .text is linked into RAM_EMU at 0x24xxxxxx, ~600 MB away.
     * It costs ~28 bytes per site against 29 KB of ITCM headroom.
     *
     * READ vs WRITE is a property of the SITE and nothing the helper can
     * recover, which is why there are two macros. A read is served straight out
     * of NOR; a write copies the page into AXI first (copy-on-write) and
     * repoints the fold entry. A site that does both is a WRITE site -- COW up
     * front is always CORRECT there, merely pessimistic, so the conservative
     * default can never corrupt a guest.
     *
     * The helper leaves its answer in cpu->nor_ptr rather than returning it in
     * R0, because the stub has to restore R0-R3 before it can use the value and
     * the destination register may be any of them (or R12). One LDR off R8
     * reaches it; a global would need a literal pool inside ITCM. Zero means
     * "genuinely unmapped", and then the stub takes the old .mem_fault exit --
     * an absent page is still a clean, reported fault, not a hard fault.
     * ---------------------------------------------------------------- */
    .macro MEM_TRAP ptr, wr
        BMI 99f
98:
        .pushsection .itcram_hot.norslow, "ax", %progbits
99:     PUSH {R0-R3, R12, LR}
        MOV R0, \ptr                  @ PUSH stores, it does not clobber
    .if \wr
        BL x86_nor_slow_w
    .else
        BL x86_nor_slow_r
    .endif
        POP {R0-R3, R12, LR}
        LDR \ptr, [R8, #CPU_NOR_PTR]
        CMP \ptr, #0
        BEQ .mem_fault
        B 98b
        .popsection
    .endm

    .macro MEM_TRAP_R ptr
        MEM_TRAP \ptr, 0
    .endm

    .macro MEM_TRAP_W ptr
        MEM_TRAP \ptr, 1
    .endm

    /* Translate a guest linear address to a host pointer, CHECKED.
     *
     * host = lin + k, where k comes from the fold table. A mapped AXI page
     * yields a RAM pointer (bit 31 clear); an unmapped page's FOLD_INVALID and
     * a NOR-backed page's 0x9xxxxxxx both drive the result negative, so
     * ADDS/BMI catches both and the stub tells them apart. Same four
     * instructions as the old UBFX/LDR/BFC/BFI, but that version trusted the
     * entry: a zero entry produced `0 | offset`, and address 0 is ITCM -- so a
     * guest write to an unmapped page silently overwrote the interpreter's own
     * instructions. */
    .macro MEM_GET_PTR_R dest_ptr, vaddr
        UBFX \dest_ptr, \vaddr, #12, #12
        LDR \dest_ptr, [R9, \dest_ptr, LSL #2]
        ADDS \dest_ptr, \dest_ptr, \vaddr
        MEM_TRAP_R \dest_ptr
    .endm

    .macro MEM_GET_PTR_W dest_ptr, vaddr
        UBFX \dest_ptr, \vaddr, #12, #12
        LDR \dest_ptr, [R9, \dest_ptr, LSL #2]
        ADDS \dest_ptr, \dest_ptr, \vaddr
        MEM_TRAP_W \dest_ptr
    .endm

    /* Unqualified = WRITE, i.e. the safe answer. A site that only ever loads
       through the pointer should say MEM_GET_PTR_R and get to stay in NOR;
       forgetting to only costs it a needless pull-back into AXI. */
    .macro MEM_GET_PTR dest_ptr, vaddr
        MEM_GET_PTR_W \dest_ptr, \vaddr
    .endm

    /* ---------------------------------------------------------------- *
     * The VGA aperture hook.
     *
     * A planar VGA mode (EGA 0Dh, and the unchained "Mode Y" Wolfenstein 3D
     * runs in) is not memory. A store to 0xA0000-0xAFFFF goes through the map
     * mask, four latches and one of four write modes, so what the guest
     * presents is not what any plane receives -- and a byte store and a dword
     * store are different operations, not the same one repeated.
     *
     * The fold trap cannot serve this and that is settled: MEM_TRAP fires at
     * pointer RESOLUTION and hands back a host pointer; the store then happens
     * inline at the site, so the trap never sees the VALUE or the SIZE and
     * there is no post-store hook. Hence a check AT the store, per site, with
     * the size baked in at assembly time.
     *
     * cpu->vga_base is the host base of the aperture while a planar mode is
     * armed and 0 otherwise. Host pointers are 0x24xxxxxx, so with a zero base
     * `ptr - 0` is never unsigned-below 0x10000 and the check cannot fire:
     * disarmed is the reset state with no special case anywhere.
     *
     * Cost when disarmed: LDR + SUBS + CMP + a not-taken BLO at a store, and
     * LDR + SUBS + CMP + a not-executed IT/STR at a load.
     *
     * THE STORE STUB REPLACES THE STORE and must not fall through to it. In
     * Mode Y the raw byte would land on top of a pixel written four stores ago.
     *
     * The stub is out of line but stays in ITCM, for MEM_TRAP's reason: a
     * conditional branch reaches +/-1 MB and .text is in RAM_EMU ~600 MB away.
     * (The BL to the C helper is fine -- the linker plants a veneer in ITCM,
     * exactly as it already does for x86_nor_slow_r/w.)
     *
     * BOTH macros CLOBBER THE PSR. x86 flags live in R11, so that is normally
     * free; a site that consumes host flags across the access must not use
     * them, and none does.
     *
     * The STACK is deliberately NOT hooked (PUSH_V/POP_V in
     * cpu_exec_handlers.S). SS pointing into the A segment is not something a
     * DOS guest can legally do, and skipping it saves four instructions on
     * every PUSH, POP, CALL and RET.
     * ---------------------------------------------------------------- */

    /* Store: \src is the value, \ptr the host pointer, \op the store opcode
       that would have been used, \sz its size in bytes, and \t a register that
       is DEAD at this site. \t and \src must be different registers. */
    .macro VGA_ST op, sz, src, ptr, t
        LDR \t, [R8, #CPU_VGA_BASE]
        SUBS \t, \ptr, \t
        CMP \t, #0x10000
        BLO 99f
        \op \src, [\ptr]
98:
        .pushsection .itcram_hot.norslow, "ax", %progbits
99:     PUSH {R0-R3, R12, LR}
        /* PUSH stores, it does not clobber, so \src and \t are still live.
           Marshalling order is the only subtlety: R3 is the staging register,
           so a site whose dead scratch IS R3 takes the other order. */
    .ifc \t, R3
        MOV R1, \src
        MOV R0, R3
    .else
        MOV R3, \src
        MOV R0, \t                @ window offset, 0..0xFFFF
        MOV R1, R3                @ value
    .endif
        MOV R2, #\sz
        BL x86_vga_planar_store
        POP {R0-R3, R12, LR}
        B 98b
        .popsection
    .endm

    .macro VGA_ST_B src, ptr, t
        VGA_ST STRB, 1, \src, \ptr, \t
    .endm
    .macro VGA_ST_H src, ptr, t
        VGA_ST STRH, 2, \src, \ptr, \t
    .endm

    /* Load: record the window offset of a guest read that lands in the
       aperture, then let the site's own load run unchanged. Width-agnostic --
       the latch is an OFFSET, so there is nothing per-size about it, and the
       caller keeps its own LDRB/LDRH/LD_V. */
    .macro VGA_LATCH ptr, t
        LDR \t, [R8, #CPU_VGA_BASE]
        SUBS \t, \ptr, \t
        CMP \t, #0x10000
        IT LO
        STRLO \t, [R8, #CPU_VGA_LATCH]
    .endm

/* ------------------------------------------------------------------ *
 * Per-opcode DWT cycle profiling. OFF unless built with X86_PROFILE=1.
 *
 * Read x86_profile.h first: it owns the block layout, the slot numbering and
 * the "ranking, not truth" caveat. What lives here is the dispatch-tail
 * insert, which is the part with a register contract to respect.
 *
 * Registers: this macro takes every register it touches as a parameter and
 * uses NOTHING else. R2/R6/R8/R9/R10/R11 are reserved (CLAUDE.md) and are
 * never passed to it -- the callers hand it only registers that are dead at
 * that exact point. The ARM PSR is clobbered (ADDS/ADC for the 64-bit
 * accumulate), which is safe because the fetch's own `ADDS Rd,Rd,R10` already
 * destroys the flags two instructions earlier: nothing consumes the host PSR
 * across a dispatch, x86 flags live in R11.
 *
 * CYCCNT is 32-bit and WRAPS every ~15 s at 280 MHz. `SUB new, old` is exact
 * modulo 2^32 and a dispatch-to-dispatch delta is tiny, so the wrap needs no
 * handling. Do not "fix" this with a comparison -- that would be the bug.
 * ------------------------------------------------------------------ */
#if X86_PROFILE
    .equ PROF_HDR,       16          @ header words before entry[0]
    .equ PROF_ENTRY_LOG2, 4          @ 16 bytes per entry
    .equ DWT_CYCCNT_ADDR, 0xE0001004
    .equ PROF_HOST_SLOT, 512     @ must match X86_PROFILE_HOST_SLOT

    /* base/a/b/c are scratch; \idx holds the slot number and SURVIVES. */
    .macro PROF_DISPATCH idx, base, a, b, c, bias=0
        MOVW \base, #:lower16:x86_profile_ptr
        MOVT \base, #:upper16:x86_profile_ptr
        LDR  \base, [\base]
        MOVW \c, #:lower16:DWT_CYCCNT_ADDR
        MOVT \c, #:upper16:DWT_CYCCNT_ADDR
        LDR  \a, [\c]                    @ t0
        LDR  \c, [\base, #4]             @ prev_ts
        LDR  \b, [\base]                 @ prev_idx
        SUB  \a, \a, \c                  @ delta, wrap-exact
        ADD  \b, \base, \b, LSL #PROF_ENTRY_LOG2
        ADD  \b, \b, #PROF_HDR
        LDR  \c, [\b, #0]
        ADDS \c, \c, \a
        STR  \c, [\b, #0]
        LDR  \c, [\b, #4]
        ADC  \c, \c, #0
        STR  \c, [\b, #4]
        LDR  \c, [\b, #8]
        ADD  \c, \c, #1
        STR  \c, [\b, #8]
        ADD  \a, \idx, #\bias
        STR  \a, [\base]                 @ this dispatch's slot
        MOVW \c, #:lower16:DWT_CYCCNT_ADDR
        MOVT \c, #:upper16:DWT_CYCCNT_ADDR
        LDR  \c, [\c]                    @ t1: bookkeeping is NOT charged to
        STR  \c, [\base, #4]             @ the opcode about to run
    .endm
#endif

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

#if X86_PROFILE
    /* Open the HOST slot. Without this the first dispatch of every call would
       charge the whole host frame -- blit, audio, HLE -- to whichever opcode
       happened to run last before the previous exit, which is a made-up
       number attached to a real opcode. Slot 512 says "not in the guest". */
    MOVW R0, #:lower16:x86_profile_ptr
    MOVT R0, #:upper16:x86_profile_ptr
    LDR  R0, [R0]
    MOVW R1, #512
    STR  R1, [R0]
    MOVW R1, #:lower16:DWT_CYCCNT_ADDR
    MOVT R1, #:upper16:DWT_CYCCNT_ADDR
    LDR  R1, [R1]
    STR  R1, [R0, #4]
#endif

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
#ifdef X86_TRACE_ODDSP
    /* Catch the FIRST instruction boundary at which guest SP is ODD, and hold
       it. A 16-bit DOS guest moves SP by two for its whole life, so an odd SP
       is a defect in whatever ran last -- and slot [1] is that instruction's
       EIP, which is the answer this trap exists to produce. It records rather
       than samples on purpose: the interpreter's instruction count varies by
       ~1500 between identical builds (the wall-clock tick steers the slice), so
       every count-based bisection of a rare event is unsound. This one is not.
       cpu_state.regs are written back as the interpreter goes, so REG_ESP is
       live here; cpu_state.eip is NOT, which is why R10 is used for the EIP. */
#ifndef X86_TRACE_ODDREG
#define X86_TRACE_ODDREG REG_ESP
#endif
    PUSH {R0, R1, R2, R3}
    MOVW R0, #:lower16:x86_oddsp_log
    MOVT R0, #:upper16:x86_oddsp_log
    LDR R3, [R0, #288]               @ [72] free-running instruction count
    ADD.W R3, R3, #1
    STR R3, [R0, #288]
    LDR R2, [R0, #0]                 @ caught already? the ring is frozen
    CMP R2, #0                       @ not CBNZ: 3f is out of its +126 reach
    BNE.W 3f
    /* X86_TRACE_SKIP arms the trap late. A register that is legitimately odd
       during startup -- BP is a general register until a frame is set up, and
       the unpacker loads it with a segment value at instruction 28 -- makes an
       ungated "first odd" trap fire on a non-defect and hide the real one. The
       ring keeps filling while disarmed, so the history is still there. */
#if X86_TRACE_SKIP
    MOVW R2, #(X86_TRACE_SKIP & 0xFFFF)
    MOVT R2, #(X86_TRACE_SKIP >> 16)
    CMP R3, R2
    BLO.N 2f
#endif
#ifdef X86_TRACE_EIPREG
    /* "The guest reached THIS instruction with THIS value in a register."
       Aimed at an HLE entry stub: freezing the ring on the first INT 21h whose
       AH is a chosen function names the CALLER, which no existing log records --
       x86_dos_file_log says what was asked and what we answered, never who
       asked. Mask + value, so a single byte of a register can be selected. */
    MOVW R1, #(X86_TRACE_EIPREG & 0xFFFF)
    MOVT R1, #(X86_TRACE_EIPREG >> 16)
    CMP R10, R1
    BNE.W 2f
    LDR.W R1, [R8, #X86_TRACE_ODDREG]
    MOVW R2, #(X86_TRACE_REGMASK & 0xFFFF)
    ANDS R1, R1, R2
    MOVW R2, #(X86_TRACE_MEMVAL & 0xFFFF)
    CMP R1, R2
    BNE.W 2f
#elif defined(X86_TRACE_MEMEQ)
    /* "Something wrote THIS value to THIS guest address." A hardware
       watchpoint cannot do this job here: the host address of a guest page is
       only known once the fold entry is live, and this guest halts about a
       second after it starts, so there is no window in which a host-side tool
       can resolve and arm one. Checking the word at an instruction boundary
       costs one fold lookup and fires on the instruction AFTER the store, with
       the ring naming the writer -- which is the same answer. */
    MOVW R1, #(X86_TRACE_MEMEQ & 0xFFFF)
    MOVT R1, #(X86_TRACE_MEMEQ >> 16)
    UBFX R2, R1, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R1
    BMI.W 2f                         @ page not mapped yet
    LDRH R1, [R2]
    MOVW R2, #(X86_TRACE_MEMVAL & 0xFFFF)
    CMP R1, R2
    BNE.W 2f
#elif defined(X86_TRACE_LOWSP)
    /* "The stack has leaked past a level it should never reach." Aimed at a
       push/pop imbalance repeated thousands of times, where the individual
       leak is invisible and only the drift is not. */
    LDR.W R1, [R8, #REG_ESP]
    MOVW R2, #(X86_TRACE_LOWSP & 0xFFFF)
    MOVT R2, #(X86_TRACE_LOWSP >> 16)
    CMP R1, R2
    BHS.N 2f
#elif defined(X86_TRACE_LOWEIP)
    /* "The guest is executing somewhere it can never legitimately be." A linear
       EIP below the DOS load area is not a subtle invariant like an odd SP --
       it is the IVT and the BDA being run as code -- and it fires EARLIER in a
       derailment than any register predicate, which is what makes it the one to
       reach for first. */
    MOV.W R1, R10
    MOVW R2, #(X86_TRACE_LOWEIP & 0xFFFF)
    MOVT R2, #(X86_TRACE_LOWEIP >> 16)
    CMP R1, R2
    BHS.W 2f
#else
    LDR.W R1, [R8, #X86_TRACE_ODDREG]
    LSLS R2, R1, #31                 @ bit 0 into C/Z: Z set == even
    BEQ.W 2f
#endif
    MOV.W R2, #1
    STR R2, [R0, #0]
    STR R3, [R0, #4]
    STR.W R10, [R0, #8]
    STR R1, [R0, #12]
    LDR.W R2, [R8, #SEG_SS_BASE]
    STR R2, [R0, #16]
    LDR.W R2, [R8, #REG_ESP]
    STR R2, [R0, #20]
    LDR.W R2, [R8, #REG_EBP]
    STR R2, [R0, #24]
    LDR.W R2, [R8, #SEG_CS_BASE]
    STR R2, [R0, #292]               @ [73]
    /* Eight stack words from SS:(SP-8). A far RETF has already popped by the
       time this fires, so the frame it used sits just BELOW the live SP -- and
       nowhere else will it still be readable. One fold lookup per word: the
       span can cross a 4 KB granule. */
    LDR.W R1, [R8, #REG_ESP]
    LDR.W R2, [R8, #SEG_SS_BASE]
    ADD.W R1, R1, R2
    SUB.W R1, R1, #8
    ADD.W R2, R0, #296               @ [74]
4:  UBFX R3, R1, #12, #12
    LDR.W R3, [R9, R3, LSL #2]
    ADDS R3, R3, R1
    BMI.N 5f                         @ unmapped: leave the slot zero
    LDRH R3, [R3]
    STR R3, [R2]
5:  ADD.W R1, R1, #2
    ADD.W R2, R2, #4
    SUB.W R3, R2, R0
    CMP R3, #328                     @ [82] -- eight words done
    BLO.N 4b
    B.W 3f                           @ freeze the ring where it stands
2:
#ifdef X86_TRACE_ATEIP
    /* Record SP at ONE fixed point in the guest's loop instead of the EIP of
       every instruction. A slow stack leak cannot be found with a threshold --
       the drift is smaller than the normal frame churn, so any bound low enough
       to miss a legitimate `sub sp,N` prologue is already past the start. One
       sample per iteration, at the same instruction every time, makes the
       per-iteration delta the reading. */
    MOVW R3, #(X86_TRACE_ATEIP & 0xFFFF)
    MOVT R3, #(X86_TRACE_ATEIP >> 16)
    CMP R10, R3
    BNE.W 3f
#ifndef X86_TRACE_ATREG
#define X86_TRACE_ATREG REG_ESP
#endif
    LDR.W R3, [R8, #X86_TRACE_ATREG]
#else
    MOV.W R3, R10
#endif
    LDR R1, [R0, #28]                @ [7] ring write index
    AND.W R1, R1, #63
    ADD.W R2, R0, #32                @ [8..71] the ring itself
    STR.W R3, [R2, R1, LSL #2]
    ADD.W R1, R1, #1
    STR R1, [R0, #28]
3:  POP {R0, R1, R2, R3}
#endif
#ifdef X86_TRACE_EIP
    /* Diagnostic sampler, off unless x86_trace.inc turns it on. See
       scripts/trace_diff.py and the alignment traps in TOOLING.md.
       R1 becomes the 0-BASED index of the instruction about to run, rebased on
       X86_TRACE_SKIP -- sampling the post-increment counter instead put every
       slot one instruction ahead of the reference's.

       Position matters as much as the arithmetic. This sits BELOW the PIT
       check, so an instruction the interpreter samples and then does not
       execute (the slice expired) is not counted, and is not counted a second
       time when the host re-enters. Above the check, each frame boundary added
       one to our index and the diff against the reference drifted by one per
       slice. It is also below the prefix re-entry point, so a prefixed
       instruction counts once. */
    PUSH {R0, R1, R2}
    MOVW R0, #:lower16:x86_eip_log_idx
    MOVT R0, #:upper16:x86_eip_log_idx
    LDR R1, [R0]
    ADD.W R2, R1, #1
    STR R2, [R0]
    MOVW R2, #(X86_TRACE_SKIP & 0xFFFF)
    MOVT R2, #(X86_TRACE_SKIP >> 16)
    SUBS R1, R1, R2
    BLO.N 1f
#if X86_TRACE_SHIFT
    LSLS R2, R1, #(32 - X86_TRACE_SHIFT)
    BNE.N 1f
    LSR.W R1, R1, #X86_TRACE_SHIFT
#endif
    CMP R1, #64                    @ saturating window, NOT a ring
    BHS.N 1f
    MOVW R0, #:lower16:x86_eip_log
    MOVT R0, #:upper16:x86_eip_log
#ifdef X86_TRACE_FLAGS
    /* EFLAGS is CACHED IN R11 and only written back to the struct on exit, so
       reading CPU_EFLAGS here returns whatever the last exit stored. */
    STR.W R11, [R0, R1, LSL #2]
#elif defined(X86_TRACE_REG)
    LDR.W R2, [R8, #X86_TRACE_REG]   @ a guest register instead of the EIP
    STR.W R2, [R0, R1, LSL #2]
#else
    STR.W R10, [R0, R1, LSL #2]
#endif
1:  POP {R0, R1, R2}
#endif
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
    MEM_TRAP_R R0
    LDRB R0, [R0]
    ADD R10, R10, #1
#if X86_PROFILE
    /* R1/R3/R4/R12 are all dead here: R1 last held the PIT counter, R0 is the
       opcode a handler receives, R2 the mode record. Nothing else is touched. */
    PROF_DISPATCH R0, R12, R1, R3, R4
#endif
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
    /* ST_V through the VGA aperture hook. Per-pass because the SIZE handed to
       the hook must be this pass's operand size, not a runtime value. */
    .macro VGA_ST_V src, ptr, t
        VGA_ST STR, 4, \src, \ptr, \t
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
    .purgem VGA_ST_V
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
    .macro VGA_ST_V src, ptr, t
        VGA_ST STRH, 2, \src, \ptr, \t
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

#if X86_PROFILE
/* uint32_t x86_profile_calibrate(uint32_t iterations)
 *
 * Measure the measurement, on the target, with the SAME macro the dispatch
 * tail uses -- no fudge factor, and nothing on the target ever subtracts it.
 * Two loops: an empty one and an instrumented one. The difference divided by
 * the iteration count is the cycles per instrumented dispatch.
 *
 * It is a LOWER BOUND, deliberately: this loop is tiny, its branch predicts
 * perfectly and its DTCM block is hot, so the real cost inside a 3,000-handler
 * interpreter is at least this. It accumulates into the HOST slot, which
 * x86_profile_init clears immediately afterwards.
 */
    .global x86_profile_calibrate
    .type x86_profile_calibrate, %function
x86_profile_calibrate:
    PUSH {R4-R7, LR}
    MOV  R6, R0
    CMP  R6, #0
    BEQ  9f

    MOVW R12, #:lower16:DWT_CYCCNT_ADDR
    MOVT R12, #:upper16:DWT_CYCCNT_ADDR
    LDR  R4, [R12]
    MOV  R3, R6
1:  SUBS R3, R3, #1
    BNE  1b
    LDR  R5, [R12]
    SUB  R5, R5, R4                  @ empty-loop cost

    MOVW R7, #PROF_HOST_SLOT
    MOVW R12, #:lower16:DWT_CYCCNT_ADDR
    MOVT R12, #:upper16:DWT_CYCCNT_ADDR
    LDR  R4, [R12]
    MOV  R3, R6
2:  PROF_DISPATCH R7, R12, R0, R1, R2
    SUBS R3, R3, #1
    BNE  2b
    MOVW R12, #:lower16:DWT_CYCCNT_ADDR
    MOVT R12, #:upper16:DWT_CYCCNT_ADDR
    LDR  R0, [R12]
    SUB  R0, R0, R4
    SUB  R0, R0, R5
    UDIV R0, R0, R6
9:  POP {R4-R7, PC}
#endif
