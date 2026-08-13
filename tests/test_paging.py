#!/usr/bin/env python3
"""
Tier-1 checks for the guest memory ARCHITECTURE: the A20 gate, the real-mode
wrap it controls, and (as later stages land) the resident set, demand paging
and eviction.

Why this is its own harness rather than more arms on test_nor.py: test_nor
covers one BACKING TIER and its copy-on-write. What is checked here is the
address space those tiers live in -- which pages exist at all, and which page a
guest address resolves to. Those fail in different ways and the counters that
name them are different.

The A20 arms exist because of a specific defect. x86_mem_wrap_1mb() used to
alias every fold entry from 1 MB to 16 MB onto the low megabyte -- an 8086's
20-bit address space. That is not merely inaccurate: with all 3,840 entries
above 1 MB held as aliases there IS no memory above 1 MB, and the 8 MB guest
space cannot exist. It was invisible because nothing had ever mapped anything
up there, so no test could tell a correct wrap from a total one.
"""
import sys
import time

sys.path.insert(0, ".")
from scripts.common.runner import add_runner_args, build_and_create_runner
from scripts.common.elf import get_symbols

MEM_FAULT = 5   # X86_EXIT_MEM_FAULT
HALT = 6        # X86_EXIT_HALT -- the guest ran to its HLT


def run_arm(b, syms, flag, result, count):
    """Trigger one target-side arm and read its result block back."""
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
        for n in ("run_a20_flag", "a20_r"):
            if n not in syms:
                print(f"missing symbol {n}")
                return False

        time.sleep(0.5)
        r = run_arm(b, syms, "run_a20_flag", "a20_r", 24)
        if r is None:
            print("FAIL: target never ran the A20 test")
            return False

        checks = [
            # ---- the wrap's SHAPE, which is where the defect was
            ("gate masked at boot",                     r[0], 0),
            ("HMA page 0 aliases linear 0",             r[1], 1),
            ("HMA page 15 aliases linear 0xF000",       r[2], 1),
            # These two fail on the old whole-space wrap and are the reason
            # the harness exists: 0x110000 and 0x200000 are extended memory,
            # not another copy of the bottom of RAM.
            ("0x110000 is NOT aliased",                 r[3], 1),
            ("0x200000 is NOT aliased",                 r[4], 1),

            # ---- the wrap WORKS, from guest code through a segment override
            ("guest ran to HLT",                        r[5], HALT),
            ("FFFF:0020 read the byte at 0000:0010",    r[6], 0x5A),
            ("  ... and that byte really was written",  r[7], 0x5A),

            # ---- the gate, opened by the guest through port 0x92
            ("guest OUT 92h opened the gate",           r[8], 1),
            ("exactly one transition",                  r[9], 1),
            ("HMA access now faults cleanly",           r[10], MEM_FAULT),
            ("  ... because the HMA is unmapped",       r[11], 1),
            ("port 0x92 reads back open",               r[12], 0x02),

            # ---- and CLOSES again. A gate tested only in the opening
            # direction passes while the closing half does nothing.
            ("masking it again",                        r[13], 0),
            ("two transitions",                         r[14], 2),
            ("the alias is restored",                   r[15], 1),

            # ---- the 8042 doors
            ("0x64=D1 then 0x60=02 opens it",           r[16], 1),
            ("0x64=D0 reports it on 0x60",              r[17], 0x02),
            ("0x64=DD masks it",                        r[18], 0),
            ("0x64=DF opens it",                        r[19], 1),
            ("a non-D1 command clears the latch",       r[20], 1),
            ("probing an open gate is not a change",    r[21], 1),

            ("left masked for the next test",           r[22], 0),
            ("host-readable mirror agrees",             r[23], 0),
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
