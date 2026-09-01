#!/usr/bin/env python3
"""
Tier-1 checks for PROTECTED MODE: the descriptor walk.

Scope is deliberately narrow and matches what is built. LGDT/LIDT/LLDT, the
descriptor fetch, the type/DPL/present rules, the per-segment descriptor cache,
and the CR0.PE transition in both directions. FAR TRANSFERS, CALL GATES, IRET
ACROSS PRIVILEGE AND PAGING ARE NOT BUILT AND ARE NOT TESTED HERE -- a test
that passes over unbuilt code is worse than no test.

Every arm runs GUEST CODE. The walk is reachable from exactly one place --
L(SET_SREG) in cpu_exec_handlers.S -- so calling x86_pmode_load_seg() from the
host would leave the path that reaches it untested, and that path is where a
register-contract violation would live.

WHY THE NEGATIVE CONTROLS MATTER MORE THAN THE POSITIVE ONES HERE. This
feature is almost entirely checks, and a check that cannot be seen failing has
not been shown to be a check at all. `references/tiny386` is the object lesson:
its set_seg() carries `// TODO: various permission checks` (i386.c:1097) and
has NO DPL validation, so a suite modelled on it would pass every privilege
test it wrote. Each rule here is compiled out individually by
X86_PMODE_NEGCTL=n (see x86_pmode.c), and `scripts/run_pmode_controls.py`
builds each arm and requires this file to FAIL on it.

    python3 tests/test_pmode.py
    python3 scripts/run_pmode_controls.py       # the controls
"""
import sys
import time

sys.path.insert(0, ".")
from scripts.common.runner import add_runner_args, build_and_create_runner
from scripts.common.elf import get_symbols

GPF = 13    # X86_EXIT_GPF
HALT = 6    # X86_EXIT_HALT -- the guest ran to its HLT
UNIMP = 4   # X86_EXIT_UNIMPLEMENTED

EX_NP = 11
EX_SS = 12
EX_GP = 13


def run_arm(b, syms, flag, result, count):
    b.write_uint32(syms[flag], 1)
    for _ in range(400):
        if b.read_uint32(syms[flag]) == 0:
            break
        time.sleep(0.05)
    else:
        return None
    return [b.read_uint32(syms[result] + 4 * i) for i in range(count)]


def main(args):
    runner = build_and_create_runner(args)
    try:
        runner.start()
        b = runner.backend
        syms = get_symbols(getattr(args, "elf", None) or "test_core.elf")
        for n in ("run_pmode_flag", "pmode_r"):
            if n not in syms:
                print(f"missing symbol {n}")
                return False

        time.sleep(0.5)
        r = run_arm(b, syms, "run_pmode_flag", "pmode_r", 192)
        if r is None:
            # A stall here is the arm hard-faulting or a guest that never
            # reaches its HLT, and "never ran" alone cannot tell those apart.
            # The partial results say HOW FAR it got, which names the arm.
            print("FAIL: target never ran the protected-mode test")
            part = [b.read_uint32(syms["pmode_r"] + 4 * i) for i in range(192)]
            last = max([i for i, v in enumerate(part) if v] or [-1])
            print(f"  last slot written: {last}")
            for n in ("x86_pmode_loads", "x86_pmode_faults"):
                if n in syms:
                    print(f"  {n} = {b.read_uint32(syms[n])}")
            print("  " + " ".join(f"{v:#x}" for v in part[:max(last + 2, 8)]))
            return False

        checks = [
            # ---- A. LGDT/LIDT parse their operand -----------------------
            # LGDT used to check CPL and then branch away having parsed
            # NOTHING, so every LGDT a guest issued was discarded and the GDT
            # stayed at base 0. Invisible until something walks the table.
            ("guest ran LGDT/LIDT to HLT",          r[0], HALT),
            ("LGDT set the GDT base",               r[1], 0x2000),
            ("  ... and its limit",                 r[2], 0x27),
            ("LIDT set the IDT base",               r[3], 0x4000),
            ("  ... and its limit",                 r[4], 0x1FF),
            # THE COST CHECK. A real-mode guest must never reach the walk.
            ("real mode walked NO descriptor",      r[5], 0),

            # ---- B. the BASE takes effect -------------------------------
            ("PE on, DS=0x08, ran to HLT",          r[6], HALT),
            # 0x5A lives at linear 0x30010 and nowhere else, so reading it
            # through DS:0x0010 can only mean the base was applied.
            ("read through the descriptor base",    r[7], 0x5A),
            ("  ... base cached",                   r[8], 0x30000),
            ("  ... limit cached",                  r[9], 0xFFFF),
            ("  ... selector cached",               r[10], 0x08),
            ("  ... marked usable (P)",             r[11], 1),
            ("  ... type is data RW (2)",           r[12], 2),
            ("  ... DPL 0",                         r[13], 0),
            ("exactly one descriptor walk",         r[14], 1),
            ("  ... and no fault",                  r[15], 0),

            # ---- C. the LIMIT, and the G bit ----------------------------
            ("two loads ran to HLT",                r[16], HALT),
            ("byte-granular limit is verbatim",     r[17], 0x100),
            # G=1 with a limit FIELD of zero. If G were ignored this reads 0,
            # which is also what a null selector leaves -- hence both arms.
            ("G=1 expands 0 to 0xFFF",              r[18], 0xFFF),
            ("  ... and its base",                  r[19], 0x31000),

            # ---- D. null selectors: three different behaviours ----------
            ("null into DS is LEGAL",               r[20], HALT),
            ("  ... but the segment is UNUSABLE",   r[21], 0),
            ("  ... and its limit is cleared",      r[22], 0),
            # NEGATIVE CONTROL 2.
            ("null into SS FAULTS",                 r[23], GPF),
            ("  ... with #GP",                      r[24], EX_GP),
            ("  ... error code 0, not the selector", r[25], 0),

            # ---- E. past the descriptor table limit --------------------
            # GDT limit 0x3F; 0x40 is one entry past. Graded properly in arm M
            # below -- this arm alone cannot, because with nothing at 0x40 the
            # fault merely moves to the S-bit check when the bound is removed.
            ("selector past the GDT limit FAULTS",  r[26], GPF),
            ("  ... with #GP",                      r[27], EX_GP),
            ("  ... error code is the selector",    r[28], 0x40),
            # The control for the control: the last IN-bounds selector works,
            # so the arm above failed for the bound and not for the table.
            ("the last in-bounds selector loads",   r[29], HALT),

            # ---- F. WRONG DPL ------------------------------------------
            # NEGATIVE CONTROL 6, and the rule tiny386 does not have at all.
            # DPL 0 descriptor loaded with RPL 3: max(CPL,RPL)=3 > DPL.
            ("RPL 3 against a DPL 0 selector FAULTS", r[30], GPF),
            ("  ... with #GP",                      r[31], EX_GP),
            ("  ... error code has RPL cleared",    r[32], 0x18),
            # Same descriptor, RPL 0. Proves the fault was the PRIVILEGE.
            ("  ... the same selector at RPL 0 loads", r[33], HALT),

            # ---- G. not present -> #NP, a DIFFERENT vector --------------
            # NEGATIVE CONTROL 7.
            ("a not-present selector FAULTS",       r[34], GPF),
            ("  ... with #NP, not #GP",             r[35], EX_NP),
            ("  ... error code is the selector",    r[36], 0x20),

            # ---- H. type rules ------------------------------------------
            # NEGATIVE CONTROL 3.
            ("a SYSTEM descriptor into DS FAULTS",  r[37], GPF),
            ("  ... with #GP",                      r[38], EX_GP),
            ("  ... error code",                    r[39], 0x28),
            ("EXECUTE-ONLY code into DS FAULTS",    r[40], GPF),
            ("  ... with #GP",                      r[41], EX_GP),
            ("  ... error code",                    r[42], 0x30),
            # NEGATIVE CONTROL 5.
            ("READ-ONLY data into SS FAULTS",       r[43], GPF),
            ("  ... with #GP",                      r[44], EX_GP),
            ("  ... error code",                    r[45], 0x38),
            # And a legal SS load, so the three above failed for their TYPES
            # and not merely for being SS.
            ("writable data into SS loads",         r[46], HALT),
            ("  ... with the descriptor's base",    r[47], 0x30000),

            # ---- I. LLDT and an LDT-relative selector -------------------
            ("LLDT then a TI=1 selector ran to HLT", r[48], HALT),
            ("LLDT cached the LDT base",            r[49], 0x2800),
            ("  ... and its limit",                 r[50], 0x0F),
            # 0xA5 is at linear 0x32010 only, so reading it proves the TI bit
            # routed the second walk through the LDT the first one installed.
            ("read through the LDT descriptor",     r[51], 0xA5),
            ("  ... base cached",                   r[52], 0x32000),
            # NEGATIVE CONTROL 10.
            ("LLDT of a non-LDT descriptor FAULTS", r[53], GPF),
            ("  ... error code",                    r[54], 0x08),

            # ---- J. LEAVING protected mode ------------------------------
            ("PE on, load, PE off, load, HLT",      r[55], HALT),
            ("the real-mode base is sel<<4 again",  r[56], 0x77),
            ("  ... 0x0300 -> 0x3000",              r[57], 0x3000),
            # The real-mode store was an STRH that wrote only the selector, so
            # a D bit cached by the protected-mode load survived into real mode
            # and would have left the interpreter decoding 32-bit operands.
            ("  ... and the stale FLAGS are cleared", r[58], 0),
            ("  ... PE really is off",              r[59], 0),

            # ---- K. the segment-limit predicate -------------------------
            # Not on the access path yet; settled and tested first.
            ("expand-up limit, both ends and size", r[60], 1),
            ("an offset that WRAPS is refused",     r[61], 1),
            ("expand-down is inverted, not offset", r[62], 1),
            ("  ... and B raises the ceiling",      r[63], 1),

            # ---- L. the SS privilege rule, isolated ---------------------
            # NEGATIVE CONTROL 4. Selector 0x08 is writable, present data, so
            # the ONLY thing wrong with 0x0B is its RPL -- arm H's selectors
            # fail on type first and cannot grade this.
            ("writable data into SS at RPL 3 FAULTS", r[64], GPF),
            ("  ... with #GP",                      r[65], EX_GP),
            ("  ... error code",                    r[66], 0x08),

            # ---- M. the table bound, against a VALID descriptor ---------
            # NEGATIVE CONTROL 1. GDT+0x40 holds a well-formed present data
            # descriptor that the limit puts outside the table, so removing
            # the bound LOADS it rather than failing somewhere else.
            ("a valid descriptor past the limit FAULTS", r[67], GPF),
            ("  ... with #GP",                      r[68], EX_GP),
            ("  ... error code",                    r[69], 0x40),
            ("  ... and its base was NOT loaded",   r[70], 0),
            # Raise the limit by one entry and the same selector loads, so the
            # arm above failed for the BOUND and not for the descriptor.
            ("  ... limit +8: the same selector loads", r[71], HALT),
            ("  ... with its base",                 r[72], 0x33000),

            # ---- N. the far jump that installs CS -----------------------
            # Call gates, privilege changes and IRET across rings are NOT
            # built and are NOT tested. A same-privilege far jump is the whole
            # scope, and it is the last step of every real entry sequence.
            ("far JMP to a code selector ran to HLT", r[73], HALT),
            ("  ... CS base came from the descriptor", r[74], 0x30000),
            ("  ... CS selector cached",            r[75], 0x30),
            # 1, not 0: the HLT at offset 0 has retired, so EIP is past it.
            # The number that matters is that it is an offset in the NEW segment
            # and not the linear address 0x30001.
            ("  ... and EIP rebuilt from the new base", r[76], 1),
            ("  ... CPL is CS.RPL",                 r[77], 0),
            # NEGATIVE CONTROL 9. Selector 0x08 is DATA and shares the code
            # selector's base, so without the check this jump would find the
            # very same HLT and look like the success above.
            ("far JMP to a DATA selector FAULTS",   r[78], GPF),
            ("  ... error code",                    r[79], 0x08),

            # ---- O. INT through a 386 INTERRUPT GATE --------------------
            # The measured demand: Dark Forces and Mortal Kombat both stop on
            # `INT imm8` with PE set, and this is the walk that answers it.
            ("INT through a 386 gate ran to HLT",   r[80], HALT),
            ("  ... CS came from the GATE",         r[81], 0x48),
            ("  ... with the descriptor's base",    r[82], 0x30000),
            # Gate offset 0x20; the HLT there has retired, so EIP is 0x21.
            ("  ... EIP is the GATE's offset",      r[83], 0x21),
            # NEGATIVE CONTROL 15. THE FRAME WIDTH IS THE GATE'S, not the
            # operand size -- CS.D is 0 here, so a width taken from the
            # operand size would push six bytes and leave SP at 0x7FFA.
            ("  ... a 386 gate pushed 12 bytes",    r[84], 0x7FF4),
            ("  ... the pushed EIP is the NEXT insn", r[85], 0x0D),
            ("  ... and the pushed CS is the old one", r[86], 0x00),
            ("  ... an INTERRUPT gate cleared IF",  r[87], 0),
            ("exactly one gate walked",             r[88], 1),

            # ---- P. TRAP vs INTERRUPT gate ------------------------------
            # One bit of the gate type, and it is the whole difference. Both
            # arms STI first, or neither number can fail.
            ("INT through a TRAP gate ran to HLT",  r[89], HALT),
            ("  ... and a TRAP gate KEPT IF",       r[90], 1),
            ("the same INT through an INT gate ran", r[91], HALT),
            ("  ... and cleared IF",                r[92], 0),

            # ---- Q. a 286 gate builds a 16-bit frame --------------------
            ("INT through a 286 gate ran to HLT",   r[93], HALT),
            ("  ... pushed a 16-bit IP",            r[94], 0x103),
            ("  ... and a 16-bit CS",               r[95], 0x48),

            # ---- R. the gate's own refusals -----------------------------
            # NEGATIVE CONTROL 12. Each gate below is well-formed except for
            # the one thing tested, so removing the check DELIVERS rather
            # than moving the fault somewhere else.
            ("an S=1 descriptor in the IDT FAULTS", r[96], GPF),
            ("  ... with #GP",                      r[97], EX_GP),
            # The IDT error code is vec*8+2, NOT a selector: 0x23*8+2.
            ("  ... error code is the IDT form",    r[98], 282),
            # NEGATIVE CONTROL 13.
            ("a NOT-PRESENT gate FAULTS",           r[99], GPF),
            ("  ... with #NP, not #GP",             r[100], EX_NP),
            ("  ... error code",                    r[101], 290),
            # NEGATIVE CONTROL 11.
            ("a vector past the IDT limit FAULTS",  r[102], GPF),
            ("  ... with #GP",                      r[103], EX_GP),
            ("  ... error code",                    r[104], 514),
            # And the same vector with the limit raised by one gate
            # delivers, so the arm above failed for the BOUND.
            ("  ... limit +8: the same vector delivers", r[105], HALT),

            # ---- S. a software INT may not reach a more privileged gate --
            # NEGATIVE CONTROL 14, and the only arm that needs CPL != 0.
            #
            # NEITHER ARM IS GRADED ON ITS EXIT REASON. The twin's handler
            # cannot end in a HLT: HLT is PRIVILEGED, so at CPL 3 it is
            # itself a #GP -- a perfectly delivered interrupt then exits 13
            # on the halt and reads exactly like a refusal. What is graded
            # is DELIVERY, x86_pmode_gates, which is the sharper check
            # anyway: with the DPL rule compiled out the refused arm
            # delivers and its counter moves off 0.
            ("INT 26h at CPL 3 through a DPL 0 gate FAULTS", r[106], GPF),
            ("  ... and CPL really was 3",          r[107], 3),
            ("  ... with #GP",                      r[108], EX_GP),
            ("  ... error code",                    r[109], 306),
            ("  ... and NOTHING was delivered",     r[110], 0),
            # The twin: a DPL 3 gate at CPL 3 IS delivered. Without it,
            # "refused" cannot be told from "protected mode broke at CPL 3".
            ("  ... a DPL 3 gate at CPL 3 delivers", r[111], 1),
            ("  ... with CS.RPL forced to CPL",     r[112], 0x53),

            # ---- T. THE ROUND TRIP --------------------------------------
            # Delivery without a return moves the hang rather than fixing it.
            ("gate in, IRET out, ran to HLT",       r[113], HALT),
            ("  ... EIP is the instruction after the INT", r[114], 0x104),
            ("  ... and ESP is back where it began", r[115], 0x8000),
            ("  ... CS restored from the frame",    r[116], 0x48),
            ("one gate walked",                     r[117], 1),
            ("  ... and one IRET completed",        r[118], 1),
            # STI set it, the gate cleared it, the IRET put it back: three
            # separate things must be right for this to read 1.
            ("  ... and IF came back from the frame", r[119], 1),

            # ---- U. IRET's OWN CS rule ----------------------------------
            # NEGATIVE CONTROL 16. DPL == RPL for a non-conforming target is
            # STRICTER than L(SET_SREG)'s CS arm, which checks only code and
            # present -- so this cannot be graded by the far-jump arms.
            ("IRET to a DPL 3 selector at RPL 0 FAULTS", r[120], GPF),
            ("  ... with #GP",                      r[121], EX_GP),
            ("  ... error code is the selector",    r[122], 0x50),
            # The pops are SPECULATIVE until the CS is known good.
            ("  ... and ESP was not left consumed", r[123], 0x7FFA),
            ("  ... the same frame with DPL == RPL returns", r[124], HALT),
            ("  ... to the offset the frame named", r[125], 0x61),

            # ---- V. LAR -------------------------------------------------
            # 58 static sites in DOS4GW.EXE. Every ZF=0 arm below sets ZF
            # first (XOR AX,AX), or it could not fail: PM_ENTER's OR leaves
            # ZF clear and a handler that never ran would read as a refusal.
            ("LAR of a valid selector ran to HLT",  r[126], HALT),
            ("  ... ZF set",                        r[127], 1),
            # The access byte at bits 8-15, which is NOT where our flags
            # packing keeps it (docs/14 Sec.4).
            ("  ... AR is the access byte, shifted", r[128], 0x9200),
            # NEGATIVE CONTROL 18: the same descriptor at RPL 3.
            ("LAR at RPL 3 against DPL 0 clears ZF", r[129], 0),
            ("  ... and leaves the destination alone", r[130], 0xDEAD),
            # NEGATIVE CONTROL 19.
            ("LAR of an INTERRUPT GATE clears ZF",  r[131], 0),
            ("  ... but a CALL GATE is accessible", r[132], 1),
            ("LAR of a null selector clears ZF",    r[133], 0),
            # And in REAL mode it is not an instruction at all.
            ("LAR in real mode is UNIMPLEMENTED",   r[134], UNIMP),
            ("  ... and the exit names 0F 02",      r[135], 0x0F02),

            # ---- W. LSL -------------------------------------------------
            ("LSL of a valid selector set ZF",      r[136], 1),
            ("  ... and loaded the limit",          r[137], 0xFFFF),
            # The G bit, through LSL: control 8 grades this arm too.
            ("  ... G=1 expands 0 to 0xFFF",        r[138], 0xFFF),
            # NEGATIVE CONTROL 20. The call gate LAR accepted: the ONLY
            # descriptor the two type lists disagree about.
            ("LSL of a CALL GATE clears ZF",        r[139], 0),
            ("  ... but a TSS has a limit",         r[140], 1),
            ("  ... and LSL reads it",              r[141], 0x67),

            # ---- X. VERR and VERW ---------------------------------------
            ("VERR of writable data set ZF",        r[142], 1),
            # NEGATIVE CONTROL 21.
            ("VERR of EXECUTE-ONLY code clears ZF", r[143], 0),
            ("VERW of writable data set ZF",        r[144], 1),
            # NEGATIVE CONTROL 22, from both sides: removing it turns VERW
            # into VERR and both of these move.
            ("VERW of READ-ONLY data clears ZF",    r[145], 0),
            ("VERW of readable CODE clears ZF",     r[146], 0),

            # ---- Y. LTR and STR -----------------------------------------
            # The task register is LOADED AND STORED and no task switch is
            # implemented: DOS/4GW runs entirely at CPL 0 (x86_pmode.c).
            ("LTR then STR ran to HLT",             r[147], HALT),
            ("  ... the task register holds the selector", r[148], 0x58),
            ("  ... with the descriptor's base",    r[149], 0x35000),
            ("  ... and its limit",                 r[150], 0x67),
            ("  ... STR read it back",              r[151], 0x58),
            ("  ... and the TSS is marked BUSY in memory", r[152], 0x8B),
            ("  ... one task register loaded",      r[153], 1),
            # NEGATIVE CONTROL 17: a BUSY TSS is present, S=0 and a TSS.
            ("LTR of a BUSY TSS FAULTS",            r[154], GPF),
            ("  ... with #GP",                      r[155], EX_GP),
            ("  ... error code is the selector",    r[156], 0x60),
            ("LTR of a DATA selector FAULTS",       r[157], GPF),
            ("  ... error code",                    r[158], 0x08),

            # ---- Z. CLTS ------------------------------------------------
            ("LMSW really set CR0.TS",              r[159], 8),
            ("CLTS ran to HLT",                     r[160], HALT),
            ("  ... and cleared TS",                r[161], 0),
            ("  ... leaving PE alone",              r[162], 1),

            # LAR/LSL/VERR/VERW NEVER FAULT. The only two refusals in the
            # whole V-Z block are LTR's.
            ("the inspection arms raised nothing",  r[163], 2),
        ]

        bad = 0
        for name, got, want in checks:
            ok = got == want
            bad += not ok
            print(f"  {'PASS' if ok else 'FAIL'}: {name}: {got}"
                  + ("" if ok else f" (expected {want})"))
        if bad:
            print(f"FAIL: {bad} of {len(checks)} checks")
            return False
        print(f"PASS: {len(checks)} checks")
        return True
    finally:
        runner.stop()


if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser(description=__doc__)
    add_runner_args(p)
    args = p.parse_args()
    sys.exit(0 if main(args) else 1)
