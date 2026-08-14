#!/usr/bin/env python3
"""
The controls for the Mode Y arm of tests/test_video.py -- six negative, one
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

  TIMER    -DX86_MODEY_TIMER_NEGCTL=1   the frame loop's escape hatch goes back
                                        to a plain timer -- paint every 8 frames
                                        without a flip, with no test for whether
                                        the guest had FINISHED drawing. Mode Y
                                        aliases every page into one page of
                                        storage, so a timed blit lands mid
                                        redraw: on hardware that was a raycast
                                        viewport split in half and a menu
                                        highlight showing the old item and the
                                        new one at once. No pixel test can see
                                        it -- it is a question about WHICH
                                        FRAMES are painted -- so it is graded
                                        here, by tests/test_video.py
                                        --present-only.

  START    -DX86_MODEY_START_NEGCTL=1   the display start is recomputed and
                                        ACTED ON per byte OUT again. The start
                                        is 16 bits written as 0x0C (high) then
                                        0x0D (low), so between them the register
                                        file holds the new page's high byte over
                                        the previous page's low byte -- an
                                        address no guest ever programmed. The
                                        Mode Y arm's guest now changes both
                                        halves on its first flip precisely so
                                        this control CAN fail; with every low
                                        byte zero, as it was, the hybrid is
                                        identical to the real value and the
                                        mutation is a no-op.
  RECENCY  -DX86_MODEY_RECENCY_NEGCTL=1 "this guest flips" decays against the
                                        HIGH-WATER flip gap and the anti-freeze
                                        slack -- 8 x 34 = 272 loop frames on
                                        hardware's numbers. That is the ~4.5 s
                                        the menu spends governed by the
                                        raycaster's rule after escaping a level,
                                        with its whole draw hidden, before it
                                        "catches up". Graded by the escape arm
                                        of --present-only.

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

# (name, flag, must_pass, which arm of tests/test_video.py grades it)
ARMS = [
    ("CORE",   "-DX86_MODEY_CORE_NEGCTL=1",   False, "--modey-only"),
    ("STRIDE", "-DX86_MODEY_STRIDE_NEGCTL=1", False, "--modey-only"),
    ("BLIT",   "-DX86_MODEY_BLIT_NEGCTL=1",   False, "--modey-only"),
    ("PAGE",   "-DX86_MODEY_PAGE_NEGCTL=1",   False, "--modey-only"),
    ("TIMER",  "-DX86_MODEY_TIMER_NEGCTL=1",  False, "--present-only"),
    ("START",  "-DX86_MODEY_START_NEGCTL=1",  False, "--modey-only"),
    ("RECENCY", "-DX86_MODEY_RECENCY_NEGCTL=1", False, "--present-only"),
    ("LEARN",  "-DX86_MODEY_PAGE0=16384",     True,  "--modey-only"),
]


def run_arm(flag, target, arm):
    env = dict(os.environ, X86_VIDEO_NEGCTL=flag)
    cmd = [sys.executable, "tests/test_video.py", arm, "--quiet",
           "--clean", "--target", target]
    return subprocess.run(cmd, env=env).returncode == 0


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--target", default="gwemu")
    args = p.parse_args()

    fail = 0
    for name, flag, want_pass, arm in ARMS:
        print(f"---- {name}: {flag}")
        passed = run_arm(flag, args.target, arm)
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
    n = sum(1 for a in ARMS if not a[2])
    print(f"PLANAR CONTROLS PASS ({n}/{n} negative controls caught, stride "
          f"learning proven from a wrong default)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
