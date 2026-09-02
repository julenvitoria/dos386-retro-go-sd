#!/usr/bin/env python3
"""
One command that fails when a budget or a generated document regressed.

Logic lives in scripts/common/budgets.py so other tooling can import it; that
module's docstring says why this guard exists and what it refuses to claim.

    python3 scripts/budget_gate.py                 # rebuild, then gate
    python3 scripts/budget_gate.py --clean         # after a build-FLAG change
    python3 scripts/budget_gate.py --no-build      # gate an existing ELF
    python3 scripts/budget_gate.py --update        # record today's numbers
    python3 scripts/budget_gate.py --json out.json

Exit status is 0 only when every gated check passed.
"""

import argparse
import json
import os
import sys

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from scripts.common import budgets  # noqa: E402


def _fmt(m):
    return [
        "ITCM   __CORE_ITCM_CODE_END__ 0x%-8X  %6d B used, %6d B free of %d"
        % (m["itcm_code_end"], m["itcm_code_end"], m["itcm_free"],
           budgets.ITCM_BYTES),
        "RAM    __CORE_BSS_END__       0x%-8X  %6d B free below "
        "__RAM_EMU_END__ 0x%X"
        % (m["core_bss_end"], m["ram_emu_headroom"], m["ram_emu_end"]),
        "PAGER  at most %d frames of stride %d (upper bound; %d frames = the "
        "whole 640 KB wants %d B)"
        % (m["frames_implied"], budgets.PG_STRIDE, budgets.PG_FULL_FRAMES,
           m["frames_wanted_bytes"]),
    ]


def main(argv=None):
    p = argparse.ArgumentParser(
        description="Budget and documentation-rot gate for the x86 core.")
    p.add_argument("--elf", default=budgets.DEFAULT_ELF,
                   help="core ELF to measure (default %(default)s)")
    p.add_argument("--thresholds", default=budgets.DEFAULT_THRESHOLDS,
                   help="recorded budgets (default %(default)s)")
    p.add_argument("--no-build", action="store_true",
                   help="do not rebuild; measure the ELF as it is. The gate "
                        "then REFUSES a stale ELF instead of reporting its "
                        "numbers.")
    p.add_argument("--clean", action="store_true",
                   help="drop objects before building (build flags are not "
                        "dependencies in this tree)")
    p.add_argument("--allow-stale", action="store_true",
                   help="with --no-build, report anyway. The numbers are then "
                        "NOT a measurement of this tree; say so if you quote "
                        "them.")
    p.add_argument("--make", action="append", default=[], metavar="VAR=VAL",
                   help="extra make variables. Thresholds are recorded for a "
                        "STOCK build, so this makes the comparison advisory.")
    p.add_argument("--skip-opcodes", action="store_true",
                   help="skip the docs/07 staleness check")
    p.add_argument("--asm", action="store_true",
                   help="also run the ADVISORY asm_metrics baseline report "
                        "(never affects exit status)")
    p.add_argument("--update", action="store_true",
                   help="write today's numbers into the thresholds file. "
                        "Commit the diff -- that is the review.")
    p.add_argument("--json", metavar="PATH", help="also write the numbers")
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args(argv)

    abs_elf = os.path.join(budgets.ROOT, args.elf)

    if args.no_build:
        stale = budgets.newer_than_elf(abs_elf)
        if stale is None:
            print("FAIL: %s does not exist and --no-build was given." % args.elf)
            return 2
        if stale:
            head = ", ".join(stale[:6]) + ("..." if len(stale) > 6 else "")
            msg = ("%d tracked source(s) are newer than %s: %s"
                   % (len(stale), args.elf, head))
            if not args.allow_stale:
                print("REFUSING TO REPORT: " + msg)
                print("  A stale ELF gives stale symbols, and stale symbols "
                      "have already been reported as real once. Drop "
                      "--no-build, or pass --allow-stale and do not quote the "
                      "numbers.")
                return 2
            print("WARNING (--allow-stale): " + msg)
    else:
        try:
            budgets.force_build(args.elf, make_args=args.make,
                                clean=args.clean, verbose=args.verbose)
        except RuntimeError as exc:
            print("FAIL: %s" % exc)
            return 2

    try:
        m = budgets.measure(abs_elf)
    except RuntimeError as exc:
        print("FAIL: %s" % exc)
        return 2

    print("\n".join(_fmt(m)))

    if args.json:
        with open(args.json, "w") as fh:
            json.dump(m, fh, indent=1, sort_keys=True)
            fh.write("\n")

    if args.update:
        thr = budgets.load_thresholds(args.thresholds)
        thr["itcm_code_end_max"] = m["itcm_code_end"]
        thr["core_bss_end_max"] = m["core_bss_end"]
        thr["frames_implied_min"] = m["frames_implied"]
        with open(os.path.join(budgets.ROOT, args.thresholds), "w") as fh:
            json.dump(thr, fh, indent=1)
            fh.write("\n")
        print("\nWrote %s. COMMIT THE DIFF and say in the message what grew "
              "and why -- that diff is the entire review." % args.thresholds)
        return 0

    thr = budgets.load_thresholds(args.thresholds)
    fails, notes = budgets.check(m, thr)

    if args.make:
        notes.append("--make %s was passed; the thresholds are recorded for a "
                     "STOCK build, so treat the comparison as advisory."
                     % " ".join(args.make))

    if not args.skip_opcodes:
        rc, out = budgets.run_opcode_check()
        if rc != 0:
            fails.append(
                "docs/07 IS STALE: either its generated block no longer "
                "matches the dispatch tables (regenerate with `python3 "
                "scripts/gen_opcode_coverage.py --write`) or a HAND-WRITTEN "
                "reachability claim contradicts that block (fix the prose, or "
                "fence it with `<!-- coverage-check: historical -->`). The "
                "detail below says which.\n    " +
                out.replace("\n", "\n    "))
        else:
            print("DOCS   " + out)

    if args.asm:
        rc, out = budgets.run_asm_advisory()
        tag = "skipped" if rc is None else ("drift" if rc else "clean")
        print("\nASM BASELINE (advisory, %s):\n%s" % (tag, out))

    print()
    for n in notes:
        print("note: " + n)
    if fails:
        print()
        for f in fails:
            print("FAIL: " + f)
        print("\n%d budget check(s) FAILED. A deliberate increase is a diff in "
              "%s -- run --update and explain it in the commit message."
              % (len(fails), args.thresholds))
        return 1
    print("OK: every gated budget is within its recorded threshold.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
