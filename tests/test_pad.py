#!/usr/bin/env python3
"""Host test for the per-title [gamepad] mapping and typematic repeat (x86_pad.c),
and for x86_cfg.c leaving that section alone.

Like test_settings.py this needs no target: x86_pad.c, x86_cfg.c and
x86_settings.c are freestanding, so the harness (tests/host_pad.c) is compiled
with the host cc and run natively. Build scratch goes under build/run/.

    python3 tests/test_pad.py              # the checks
    python3 tests/test_pad.py --negative   # every control, SEEN TO FAIL
"""

import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRCS = [os.path.join(ROOT, "tests", "host_pad.c"),
        os.path.join(ROOT, "x86_pad.c"),
        os.path.join(ROOT, "x86_cfg.c"),
        os.path.join(ROOT, "x86_settings.c")]
OUTDIR = os.path.join(ROOT, "build", "run", "host_pad")

CONTROLS = [
    ("X86_PAD_CTL_ALLOWGAME",
     "a .cfg can rebind GAME and lock the user out of the on-screen keyboard"),
]


def build_and_run(cc, exe, defines, quiet=False):
    cmd = [cc, "-O2", "-Wall", "-Wextra", "-Werror", "-std=c99",
           "-I", ROOT, "-o", exe] + [f"-D{d}=1" for d in defines] + SRCS
    if not quiet:
        print("$ " + " ".join(cmd))
    subprocess.run(cmd, check=True)
    res = subprocess.run([exe], capture_output=True, text=True)
    if not quiet:
        sys.stdout.write(res.stdout)
    return res.returncode


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--negative", action="store_true")
    p.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = p.parse_args()
    os.makedirs(OUTDIR, exist_ok=True)
    exe = os.path.join(OUTDIR, "host_pad")
    try:
        if not args.negative:
            rc = build_and_run(args.cc, exe, [])
            print("PASS" if rc == 0 else "FAIL")
            return rc
        failed_to_fail = 0
        for macro, what in CONTROLS:
            rc = build_and_run(args.cc, exe, [macro], quiet=True)
            state = "seen to fail" if rc != 0 else "PASSED -- the check tests nothing"
            print(f"  -D{macro}=1: {state}\n              ({what})")
            failed_to_fail += rc == 0
        print(f"\n{len(CONTROLS) - failed_to_fail}/{len(CONTROLS)} controls seen to fail")
        return 1 if failed_to_fail else 0
    finally:
        shutil.rmtree(OUTDIR, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
