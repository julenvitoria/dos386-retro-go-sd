#!/usr/bin/env python3
"""
The controls for the Mode Y arm of tests/test_video.py -- four negative, one
positive. Each is a separate build of the bare-metal harness with one flag set,
run through `tests/test_video.py --modey-only`.

    python3 scripts/run_planar_controls.py [--target gwemu]

WHY THIS EXISTS AS ITS OWN SCRIPT. A default-parameter pass says nothing on its
own. docs and references/8086tiny/docs/traps.md carry three separate entries
about this project shipping checks that could not fail, so the checks are graded
by compiling the bug back IN and requiring the check to notice:

  CORE     -DX86_MODEY_CORE_NEGCTL=1    the chain-4 bit is inert, so guest
                                        writes land raw in the aperture. That
                                        is the pre-fix core, and it is what
                                        produced four squashed copies of
                                        Wolfenstein side by side.
  STRIDE   -DX86_MODEY_STRIDE_NEGCTL=1  the blit sources 40 plane offsets per
                                        row instead of 80. Bands A and C are
                                        transposes of each other precisely so
                                        that no single wrong stride can satisfy
                                        both.
  BLIT     -DX86_MODEY_BLIT_NEGCTL=1    the blit decodes four FLAT planes rather
                                        than four interleaved ones.
  PAGE     -DX86_MODEY_PAGE_NEGCTL=1    pages are folded with the fixed 16 KB
                                        AND mask and the off-screen cache goes
                                        back inside the alias -- the state
                                        before the page-stride work. The test
                                        spaces its pages 16,640 apart precisely
                                        so this control CAN fail; at 16,384 the
                                        mask is right by accident and it
                                        survives.

NOT A CONTROL, AND THE REASON IS WORTH KEEPING: "read the unchained aperture
linearly". The reference tried exactly that and measured it SURVIVING, because
at a display start of 0 and an Offset of 40 the correct de-interleave IS the
linear read. A mutation that is a no-op at the geometry under test cannot grade
anything.

  LEARN    -DX86_MODEY_PAGE0=16384      the POSITIVE control. The same 64,000
                                        pixels must still come out from a WRONG
                                        default, because the core is supposed to
                                        learn the stride from the guest's own
                                        two flips. The shipped default equals
                                        the right answer, so without this arm
                                        nothing tests the learning at all.

Flags are not dependencies, so every arm builds from CLEAN. That is most of the
runtime and there is no way around it.
"""
import argparse
import os
import subprocess
import sys

ARMS = [
    ("CORE",   "-DX86_MODEY_CORE_NEGCTL=1",   False),
    ("STRIDE", "-DX86_MODEY_STRIDE_NEGCTL=1", False),
    ("BLIT",   "-DX86_MODEY_BLIT_NEGCTL=1",   False),
    ("PAGE",   "-DX86_MODEY_PAGE_NEGCTL=1",   False),
    ("LEARN",  "-DX86_MODEY_PAGE0=16384",     True),
]


def run_arm(flag, target):
    env = dict(os.environ, X86_VIDEO_NEGCTL=flag)
    cmd = [sys.executable, "tests/test_video.py", "--modey-only", "--quiet",
           "--clean", "--target", target]
    return subprocess.run(cmd, env=env).returncode == 0


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--target", default="gwemu")
    args = p.parse_args()

    fail = 0
    for name, flag, want_pass in ARMS:
        print(f"---- {name}: {flag}")
        passed = run_arm(flag, args.target)
        if want_pass and not passed:
            print(f"LEARN FAIL -- with {flag} the core never picked up the "
                  f"16,640 stride its guest programmed. The two flips are the "
                  f"only place a guest ever states it.")
            fail = 1
        elif want_pass:
            print("  positive control: the stride is learned from a wrong default")
        elif passed:
            print(f"NEGCTL {name} SURVIVED -- the Mode Y check passes with the "
                  f"bug compiled IN, so it is not testing what it claims.")
            fail = 1
        else:
            print(f"  negctl {name}: caught")

    if fail:
        print("PLANAR CONTROLS FAIL")
        return 1
    print("PLANAR CONTROLS PASS (4/4 negative controls caught, stride learning "
          "proven from a wrong default)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
