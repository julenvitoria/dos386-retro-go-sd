#!/usr/bin/env python3
"""
The NEGATIVE CONTROLS for the protected-mode descriptor walk.

    python3 scripts/run_pmode_controls.py [--target gwemu] [--only 6]

Each arm rebuilds the bare-metal harness with ONE check in x86_pmode.c compiled
out (X86_PMODE_NEGCTL=n) and requires `tests/test_pmode.py` to FAIL. An arm
that still passes is reported loudly: the check it names is not testing what it
claims, and the suite is green over nothing.

WHY THIS IS NOT OPTIONAL FOR THIS FEATURE IN PARTICULAR. The descriptor walk is
almost entirely checks -- there is very little "behaviour" to get wrong that is
not a rule to enforce. And the obvious reference to copy, `references/tiny386`,
DOES NOT IMPLEMENT THEM: its set_seg() carries `// TODO: various permission
checks` (i386.c:1097) and has no DPL/RPL/CPL validation on a data segment load
at all. An implementation modelled on it would pass every privilege test anyone
wrote against it, because the test and the code would share the same hole. The
only thing that separates "the check works" from "the check is absent" is
watching the test go red when the check is removed.

Two of these arms found real defects on their first run rather than merely
confirming the design, which is the argument for the whole exercise:

  * NEGCTL 8 (granularity) initially SURVIVED, because the arm that graded G
    used a descriptor whose limit was non-zero -- so an ignored G still gave a
    different number from the null-selector case and the check looked live. The
    test now uses a limit FIELD of zero with G set, where an ignored G yields 0
    and a working one yields 0xFFF.
  * NEGCTL 4 (the SS privilege rule) is graded only because the suite loads SS
    with a selector whose RPL and DPL are correct AND one where they are not.
    A single arm cannot distinguish "rejected for privilege" from "rejected for
    type".

Flags are not dependencies, so each arm builds CLEAN. That is nearly all of the
runtime and there is no way around it.
"""
import argparse
import os
import subprocess
import sys

# (n, name, what the check is, and what breaks in the guest if it is missing)
ARMS = [
    (1,  "TABLE BOUND",
     "read_desc's `off + 7 > limit`. Without it any selector indexes past the "
     "GDT into whatever follows the table and is decoded as a descriptor."),
    (2,  "NULL SS",
     "a null selector loaded into SS. Without it the stack segment silently "
     "gets base 0 limit 0 and the next push writes to linear 0."),
    (3,  "SYSTEM DESCRIPTOR",
     "the S bit. Without it an LDT, TSS or gate descriptor loads into a data "
     "segment register and its base/limit fields are read out of a structure "
     "that does not have them."),
    (4,  "SS PRIVILEGE",
     "RPL == DPL == CPL for SS. tiny386 does not have this."),
    (5,  "SS TYPE",
     "SS must be WRITABLE DATA. Without it a code or read-only selector "
     "becomes the stack."),
    (6,  "DATA DPL",
     "DPL >= max(CPL, RPL) for DS/ES/FS/GS. THE RULE TINY386 OMITS ENTIRELY -- "
     "without it a ring-3 selector reaches ring-0 data."),
    (7,  "PRESENT",
     "the P bit, and the #NP/#SS vector split. Without it a swapped-out "
     "segment loads as though it were resident."),
    (8,  "GRANULARITY",
     "the G bit's 4 KB shift. Without it every G=1 descriptor's limit is 4096 "
     "times too small."),
    (9,  "CS TYPE",
     "CS must be a code segment. Without it a far transfer to a DATA selector "
     "executes whatever the data segment holds."),
    (10, "LLDT TYPE",
     "LLDT's descriptor must be type 2. Without it the LDT base can be "
     "pointed at any segment, including a code segment."),
    # ---- the protected-mode INTERRUPT path ------------------------------
    # Numbered ON from the walk's rather than renumbered into it, so an arm's
    # number keeps meaning the same check across commits.
    (11, "IDT BOUND",
     "read_gate's `off + 7 > idtr_limit`. Without it a vector past the end "
     "of the IDT is decoded as a gate out of whatever follows the table. The "
     "arm places a WELL-FORMED gate one entry past the limit, so removing "
     "the bound DELIVERS rather than merely failing somewhere else."),
    (12, "GATE TYPE",
     "the gate must have S=0 and be one of the four int/trap gates. Without "
     "it a code/data descriptor -- or a TASK gate, which needs a task switch "
     "that is not built -- is read as an int gate and its base and limit "
     "fields are decoded as a selector and an offset."),
    (13, "GATE PRESENT",
     "the gate's P bit, and its #NP vector. Without it a vector the guest "
     "has deliberately left absent is delivered to whatever the empty gate "
     "happens to name."),
    (14, "GATE DPL",
     "a SOFTWARE interrupt may not reach a gate with DPL < CPL. Without it "
     "ring 3 enters a ring-0 handler by issuing INT by hand. This is the "
     "only arm that needs CPL != 0, and it is graded on x86_pmode_gates "
     "rather than on an exit code -- see the note in tests/test_pmode.py."),
    (15, "FRAME WIDTH",
     "the frame width comes from the GATE's type, not the operand size. "
     "Without it a 386 gate reached from a 16-bit code segment pushes six "
     "bytes where twelve are wanted, and the handler's IRET unwinds to the "
     "wrong place. Graded on ESP after the INT."),
    (16, "IRET CS DPL",
     "IRET's own CS rule -- DPL == RPL for a non-conforming target, DPL <= "
     "RPL for a conforming one. STRICTER than L(SET_SREG)'s CS arm, which "
     "checks only code-ness and present, so nothing else in the suite can "
     "grade it. Without it an IRET returns into a segment at the wrong "
     "privilege."),
    # ---- the DESCRIPTOR-INSPECTION instructions --------------------------
    (17, "LTR TYPE",
     "LTR's descriptor must be an AVAILABLE TSS, not a busy one and not "
     "anything else. The arm's target is a BUSY TSS -- present, S=0 and a "
     "TSS type -- so every weaker rule admits it and only this one refuses "
     "it. Without the check a task the guest believes is running is entered "
     "a second time."),
    (18, "LAR/LSL PRIVILEGE",
     "DPL >= max(CPL, RPL) for LAR, LSL, VERR and VERW. Without it an "
     "extender's `may I touch this` probe answers yes for a selector it may "
     "not touch, which is the one question these instructions exist to ask."),
    (19, "LAR TYPE LIST",
     "an INTERRUPT or TRAP gate is NOT accessible to LAR. `any present "
     "descriptor` is the intuitive rule and this is where it is wrong: the "
     "arm's gate is present and well-formed."),
    (20, "LSL TYPE LIST",
     "a CALL GATE is accessible to LAR and NOT to LSL -- a gate has no limit "
     "to load. This is the ONLY descriptor the two type lists disagree "
     "about, so the arm replaces LSL's list with LAR's and nothing else in "
     "the suite moves."),
    (21, "VERR READABLE",
     "VERR of EXECUTE-ONLY code must clear ZF. Without it a guest reads a "
     "segment the chip would have faulted on."),
    (22, "VERW WRITABLE",
     "VERW admits ONLY writable data -- no code type, conforming or not. "
     "Without it VERW becomes VERR, and both the read-only-data arm and the "
     "readable-code arm move."),
]


def run_arm(n, target, quiet):
    env = dict(os.environ, X86_PMODE_NEGCTL=str(n))
    cmd = [sys.executable, "tests/test_pmode.py", "--clean", "--target", target]
    kw = {"stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL} if quiet else {}
    return subprocess.run(cmd, env=env, **kw).returncode == 0


def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--target", default="gwemu")
    p.add_argument("--only", type=int, default=None,
                   help="run a single control by number")
    p.add_argument("--verbose", action="store_true",
                   help="show each arm's full test output")
    args = p.parse_args()

    arms = [a for a in ARMS if args.only is None or a[0] == args.only]
    if not arms:
        print(f"no such control: {args.only}")
        return 2

    # The POSITIVE control, and it is not a formality: if the suite does not
    # pass with everything ON, every arm below "fails" for a reason that has
    # nothing to do with the check it names.
    print("---- BASELINE: every check ON, the suite must PASS")
    if not run_arm(0, args.target, not args.verbose):
        print("BASELINE FAILS. Nothing below means anything until it passes.")
        return 1
    print("  baseline: pass")

    fail = 0
    for n, name, what in arms:
        print(f"---- NEGCTL {n} {name}: {what}")
        if run_arm(n, args.target, not args.verbose):
            print(f"NEGCTL {n} ({name}) SURVIVED -- tests/test_pmode.py passes "
                  f"with this check compiled OUT, so nothing in the suite is "
                  f"actually testing it.")
            fail = 1
        else:
            print(f"  negctl {n}: caught")

    if fail:
        print("PMODE CONTROLS FAIL")
        return 1
    print(f"PMODE CONTROLS PASS ({len(arms)}/{len(arms)} caught, baseline green)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
