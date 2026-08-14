#!/usr/bin/env python3
"""
Tier-1 check of PIT channel 0 -- the SYSTEM TIMER's divisor -- driven by GUEST
code executing its own OUT instructions.

WHY THIS EXISTS. Channel 0 is the guest's entire sense of elapsed time. Until it
was modelled, `pit_write_control()` returned early for every channel but 2 and
port 0x40 was not in the write switch at all (x86_io.c), so a guest that
programmed its own timer rate got the 18.2065 Hz boot rate anyway. Wolfenstein 3D
does exactly that -- measured on our own target, `OUT 43h,36h` then `4Bh`/`21h`
to port 0x40, i.e. divisor 0x214B = 140 Hz -- and its tic counter, which is what
paces turning and menu key-repeat, therefore ran at 18.2/140 = 13% of real time.
That is a clock defect, not a keyboard one, and it produces exactly the reported
"rotating is slow and menu movement is sluggish".

It runs guest INSTRUCTIONS rather than calling the port handler from the host,
because the subject is what a program's instruction stream does to the machine.

Covered:
  * A cold machine reads 65536 -- the boot divisor -- before anything is written.
  * The real Wolf3D sequence lands 0x214B, and both halves of the lo/hi pair are
    consumed in the right order.
  * A LO-BYTE-ONLY access mode (control 0x14) leaves the HIGH byte alone. Storing
    the access field blindly, or treating every write as a lo/hi pair, silently
    halves or doubles the guest's clock.
  * Programming channel 2 -- the SPEAKER -- does not disturb channel 0, and vice
    versa. One control-word decoder serves both and a channel mix-up here retunes
    the timer every time a game beeps.
  * A reload of 0 means 65536, not 0. The frame loop divides by this value.

NEGATIVE CONTROL (`--negative`): rebuilds the core with the `case 0x40:` arm
deleted from `x86_port_write()` -- the exact state of the tree before this fix --
re-runs, and requires the result to be FAIL. The source is restored in a
`finally`, the way scripts/trace_diff.py restores x86_trace.inc. It is here
because a check that cannot fail proves nothing, and this area's bug was a
silently dropped write: the exact shape a green-by-construction test hides.
Measured: the mutated build reports divisor 65536 / 18.207 Hz where the guest
asked for 8523 / 140 Hz, and five assertions fire.
"""
import argparse
import sys
import time

sys.path.insert(0, ".")
from scripts.common.runner import add_runner_args, build_and_create_runner
from scripts.common.elf import get_symbols

WOLF3D_DIVISOR = 0x214B         # 8523 -> 140.0 Hz off the 1.193182 MHz input
LO_ONLY_DIVISOR = 0x21FF        # high byte kept, low byte replaced
SPEAKER_DIVISOR = 0x1234
BOOT_DIVISOR = 65536
PIT_HZ = 1193182


def hz(div):
    return PIT_HZ / div


def main(args):
    runner = build_and_create_runner(args)
    try:
        runner.start()
        b = runner.backend
        syms = get_symbols("test_core.elf")
        for n in ("run_pit_flag", "pit_r"):
            if n not in syms:
                print(f"missing symbol {n} -- rebuild tests/main.c")
                return False
        time.sleep(0.5)

        b.write_uint32(syms["run_pit_flag"], 1)
        for _ in range(600):
            if b.read_uint32(syms["run_pit_flag"]) == 0:
                break
            time.sleep(0.05)
        else:
            print("FAIL: the PIT arm never completed")
            return False

        r = [b.read_uint32(syms["pit_r"] + 4 * i) for i in range(10)]
        (cold, wolf, writes_a, lo_only, after_spk, spk, reload0, writes_all,
         exit_a, exit_b) = r

        print(f"cold machine        divisor {cold:6d}  "
              f"({hz(cold or 1):8.3f} Hz)   expect {BOOT_DIVISOR} / 18.207 Hz")
        print(f"OUT 43,36 / 4B / 21 divisor {wolf:6d}  "
              f"({hz(wolf or 1):8.3f} Hz)   expect {WOLF3D_DIVISOR} / 140.0 Hz")
        print(f"  port 0x40 writes  {writes_a}   (expect 2)")
        print(f"OUT 43,14 / FF      divisor {lo_only:6d}  "
              f"({hz(lo_only or 1):8.3f} Hz)   expect {LO_ONLY_DIVISOR}")
        print(f"after a CH2 program divisor {after_spk:6d}   "
              f"expect {LO_ONLY_DIVISOR} (unchanged)")
        print(f"  speaker divisor   {spk}   (expect {SPEAKER_DIVISOR})")
        print(f"OUT 43,36 / 00 / 00 divisor {reload0:6d}   "
              f"expect {BOOT_DIVISOR} -- a reload of 0 is the full period")
        print(f"  port 0x40 writes  {writes_all}   (expect 5)")
        print(f"exit reasons A/B: {exit_a}/{exit_b}   (6 = HLT)")

        ok = True

        def fail(msg):
            nonlocal ok
            print("FAIL: " + msg)
            ok = False

        if cold != BOOT_DIVISOR:
            fail(f"a machine that has never been programmed reports {cold}, "
                 f"not {BOOT_DIVISOR} -- every guest's clock starts wrong")
        if wolf != WOLF3D_DIVISOR:
            fail(f"the Wolf3D program left {wolf} ({hz(wolf or 1):.1f} Hz), "
                 f"expected {WOLF3D_DIVISOR} (140 Hz). At {BOOT_DIVISOR} the "
                 f"write was DROPPED, which is the original defect")
        if writes_a != 2:
            fail(f"port 0x40 saw {writes_a} writes, expected 2 -- the guest "
                 f"wrote a lo/hi pair")
        if lo_only != LO_ONLY_DIVISOR:
            fail(f"lo-byte-only access left {lo_only}, expected "
                 f"{LO_ONLY_DIVISOR}: the high byte must survive")
        if after_spk != LO_ONLY_DIVISOR:
            fail(f"programming the SPEAKER moved the timer divisor to "
                 f"{after_spk} -- the control word's channel field is being "
                 f"ignored, so every beep retunes the guest's clock")
        if spk != SPEAKER_DIVISOR:
            fail(f"the speaker divisor is {spk}, expected {SPEAKER_DIVISOR} -- "
                 f"channel 0 support broke channel 2")
        if reload0 != BOOT_DIVISOR:
            fail(f"a reload of 0 reports {reload0}; it means the full 16-bit "
                 f"period ({BOOT_DIVISOR}). A 0 here is a divide by zero in "
                 f"the frame loop")
        if writes_all != 5:
            fail(f"port 0x40 saw {writes_all} writes across the run, expected 5")
        for name, val in (("A", exit_a), ("B", exit_b)):
            if val != 6:
                fail(f"phase {name} exited with reason {val}, not 6 (HLT) -- "
                     f"the guest did not reach the end of its code")

        print("\nPASS" if ok else "\nFAIL")
        return ok
    finally:
        runner.stop()


# The line that carries the whole fix. Deleting it restores the defect exactly:
# the control word still latches an access mode, and the divisor bytes go
# nowhere.
MUTATE_OUT = "    case 0x40:\n        pit_write_ch0((uint8_t)value);\n        break;\n"


def negative_control(args):
    """Rebuild without the fix and require this test to FAIL."""
    import pathlib
    src = pathlib.Path("x86_io.c")
    original = src.read_text()
    if MUTATE_OUT not in original:
        print("FAIL: the mutation target is not in x86_io.c -- this control "
              "would pass without testing anything")
        return False
    try:
        src.write_text(original.replace(MUTATE_OUT, "", 1))
        print("-- negative control: rebuilt with the port 0x40 case DELETED --")
        args.negative = False
        failed = not main(args)
    finally:
        src.write_text(original)
        print("-- x86_io.c restored --")
    if failed:
        print("\nPASS (negative control): the test rejects the defective core")
        return True
    print("\nFAIL (negative control): the test PASSED without the fix, so it "
          "is not measuring anything")
    return False


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    add_runner_args(p)
    p.add_argument("--negative", action="store_true",
                   help="rebuild with the fix mutated out and require a FAIL")
    args = p.parse_args()
    if args.negative:
        sys.exit(0 if negative_control(args) else 1)
    sys.exit(0 if main(args) else 1)
