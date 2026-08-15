#!/usr/bin/env python3
"""
Tier-1 check of SOUND CARD DETECTION -- the Sound Blaster DSP reset handshake
and the AdLib (OPL2) timer test -- driven by GUEST code executing its own IN and
OUT instructions.

THIS TESTS DETECTION, NOT AUDIO. Nothing in x86_io.c's sound block produces a
sample and this test does not pretend otherwise; what it grades is whether a
guest that asks "is there a card at 0x220 / 0x388" gets a truthful answer
instead of a floating 0xFF.

WHY THIS EXISTS, MEASURED RATHER THAN ASSUMED. With X86_PROBE=1 and
scripts/probe_report.py, on this tree, under gwemu:

    WOLF3D    IN 0x388 x348, OUT 0x388/0x389 x6 each   (an AdLib TIMER test)
    KEEN4     IN 0x388 x348, OUT 0x388/0x389 x6 each   (the same one)
    POP       OUT 0x226 x3, IN 0x22E x32, IN 0x22A x33 (a DSP RESET handshake),
              then the FM aliases 0x220/0x221/0x228/0x229, then a scan of the
              0x240, 0x280 and 0x2C0 bases when 0x220 did not answer
    everything else measured -- ALLEYCAT, SIMCITY, TOPBENCH, EPIC_PINBALL,
    JAZZ, TYRIAN, MORTAL_KOMBAT -- touched neither 0x22x nor 0x388 at all.

348 reads of a status port and 32 polls of a data-available bit are what a
detection routine looks like when the port floats: the loop runs to its bound
every time. Those two sequences are exactly what the guest program in
tests/main.c (sound_code) executes here.

Graded:
  * The DSP reset handshake. Write 1 then 0 to 0x226, poll 0x22E for bit 7, read
    0x22A: it MUST be 0xAA, and the poll must not have run to its bound.
  * The write-buffer status at 0x22C reports READY (bit 7 clear) rather than
    0xFF, which is a busy DSP that never becomes ready -- the shape that spins a
    guest forever.
  * DSP command E1h returns a version, 2.01, in two bytes and in that order.
  * The AdLib timer test: masked and flags reset -> status & 0xE0 == 0; timer 1
    started with preset FFh and given its 80 us -> status & 0xE0 == 0xC0; flags
    reset again -> back to 0. A status that cannot come UP is an absent card; a
    status that cannot go DOWN is not a flag.
  * The counters agree with the sequence, and NO DMA was started -- this arm
    must not be silently exercising an audio path that does not exist.

NEGATIVE CONTROL (`--negative`), and it is the whole point of the file: rebuild
with the `dsp_push(0xAAu)` line deleted from dsp_write_reset() -- the exact
state of the tree before this work, where 0x22A floats -- and require this test
to FAIL. `--negative-opl` does the same for the OPL timer-1 flag, which is the
half of the card WOLF3D and KEEN4 actually probe. The source is restored in a
finally, the way tests/test_pit.py does it.
"""
import argparse
import sys
import time

sys.path.insert(0, ".")
from scripts.common.runner import add_runner_args, build_and_create_runner
from scripts.common.elf import get_symbols

DSP_VERSION = (2, 1)            # DSP 2.01: a plain SB 2.0, no mixer, no SB16


def main(args):
    runner = build_and_create_runner(args)
    try:
        runner.start()
        b = runner.backend
        syms = get_symbols("test_core.elf")
        for n in ("run_sound_flag", "snd_r"):
            if n not in syms:
                print(f"missing symbol {n} -- rebuild tests/main.c")
                return False
        time.sleep(0.5)

        b.write_uint32(syms["run_sound_flag"], 1)
        for _ in range(600):
            if b.read_uint32(syms["run_sound_flag"]) == 0:
                break
            time.sleep(0.05)
        else:
            print("FAIL: the sound arm never completed")
            return False

        r = [b.read_uint32(syms["snd_r"] + 4 * i) for i in range(16)]
        (davail, polls_left, aa, wstat, major, minor,
         st1, st2, st3) = r[0:9]
        resets, cmds, dma, opl_w, opl_reads, expiry, exit_reason = (
            r[9], r[10], r[11], r[12], r[13], r[14], r[15])

        print(f"DSP reset  0x22E status {davail:#04x}  polls left {polls_left}"
              f"  (0 = the poll ran to its bound and gave up)")
        print(f"           0x22A data   {aa:#04x}   expect 0xaa")
        print(f"           0x22C wstat  {wstat:#04x}   expect bit 7 CLEAR")
        print(f"DSP E1h    version      {major}.{minor:02d}  expect "
              f"{DSP_VERSION[0]}.{DSP_VERSION[1]:02d}")
        print(f"OPL status masked {st1:#04x} / after timer 1 {st2:#04x} / "
              f"reset {st3:#04x}   expect &0xE0 = 0 / 0xC0 / 0")
        print(f"counters   sb_resets {resets} sb_cmds {cmds} dma_refused {dma} "
              f"opl_writes {opl_w} opl_status_reads {opl_reads} "
              f"opl_timer_expiry {expiry}")
        print(f"exit reason {exit_reason}   (6 = HLT)")

        ok = True

        def fail(msg):
            nonlocal ok
            print("FAIL: " + msg)
            ok = False

        # ---- the DSP reset handshake -----------------------------------
        if not (davail & 0x80):
            fail(f"0x22E never reported data available ({davail:#04x}); the "
                 f"guest polled 100 times and gave up. A card that resets but "
                 f"never says so is indistinguishable from an absent one")
        if polls_left == 0:
            fail("the data-available poll ran to its full bound -- the answer "
                 "arrived late or not at all")
        if aa != 0xAA:
            fail(f"0x22A returned {aa:#04x}, not 0xAA. THIS IS THE HANDSHAKE: "
                 f"0xFF here is the unmodelled port floating high, which is "
                 f"the state this work exists to fix")
        if wstat & 0x80:
            fail(f"0x22C reports the DSP BUSY ({wstat:#04x}). A guest waits for "
                 f"bit 7 to clear before every command byte -- 0xFF here is an "
                 f"infinite wait, which is worse than an absent card")
        if (major, minor) != DSP_VERSION:
            fail(f"DSP command E1h returned version {major}.{minor}, expected "
                 f"{DSP_VERSION[0]}.{DSP_VERSION[1]:02d} -- either the reply "
                 f"bytes are in the wrong order or the command was swallowed "
                 f"as an argument to the one before it")

        # ---- the AdLib timer test --------------------------------------
        if st1 & 0xE0:
            fail(f"with both timers masked and the flags reset the OPL status "
                 f"reads {st1:#04x}; a detection routine requires &0xE0 == 0 "
                 f"here and rejects the card outright when it is not")
        if (st2 & 0xE0) != 0xC0:
            fail(f"after timer 1 expired the OPL status reads {st2:#04x}, "
                 f"expected &0xE0 == 0xC0. 0xFF is the floating port WOLF3D "
                 f"and KEEN4 spin on 348 times; 0x06 is a card whose timer "
                 f"never fires, which fails detection just as hard")
        if st3 & 0xE0:
            fail(f"the flags did not clear on an IRQ-reset write ({st3:#04x}) "
                 f"-- a status bit that cannot go down is not a flag, and the "
                 f"second half of every detection sequence checks exactly this")

        # ---- the counters have to agree with the story -----------------
        if resets != 1:
            fail(f"x86_sb_resets is {resets}, expected exactly 1")
        if cmds < 1:
            fail("x86_sb_cmds is 0 -- the E1h command byte never reached the "
                 "DSP, so the version above came from somewhere else")
        if dma != 0:
            fail(f"x86_sb_dma_refused is {dma}: this arm started a DMA "
                 f"transfer, which it must not -- there is no audio path")
        if expiry < 1:
            fail("x86_opl_timer_expiry is 0 -- no timer ever fired, so any "
                 "0xC0 above did not come from a timer")
        if opl_reads < 2048:
            fail(f"only {opl_reads} OPL status reads for a 2048-iteration spin "
                 f"-- the guest program is not running what it claims")
        if exit_reason != 6:
            fail(f"the guest exited with reason {exit_reason}, not 6 (HLT) -- "
                 f"it did not reach the end of the program")

        print("\nPASS" if ok else "\nFAIL")
        return ok
    finally:
        runner.stop()


# The line that IS the handshake. Deleting it restores the defect exactly: the
# reset is still accepted, the buffer is still emptied, and 0x22A floats.
MUTATE_AA = "    dsp_push(0xAAu);                  /* THE handshake: \"a card is here\"   */\n"

# The OPL half: timer 1's flag. Without it the status can never come up, which
# is the state WOLF3D and KEEN4 measured 348 reads against.
MUTATE_T1 = """    if (opl.t1_run && !(opl.ctrl & 0x40u) &&
        (uint32_t)(now - opl.t1_start) >=
            (uint32_t)(256u - opl.t1_preset) * OPL_T1_STEP)
        f |= 0x40u;
"""


def _mutate_and_require_failure(args, target, what):
    import pathlib
    src = pathlib.Path("x86_io.c")
    original = src.read_text()
    if target not in original:
        print(f"FAIL: the mutation target ({what}) is not in x86_io.c -- this "
              f"control would pass without testing anything")
        return False
    try:
        src.write_text(original.replace(target, "", 1))
        print(f"-- negative control: rebuilt with {what} DELETED --")
        args.negative = False
        args.negative_opl = False
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
                   help="rebuild with the DSP's 0xAA push deleted, require FAIL")
    p.add_argument("--negative-opl", action="store_true",
                   help="rebuild with the OPL timer-1 flag deleted, require FAIL")
    args = p.parse_args()
    if args.negative:
        sys.exit(0 if _mutate_and_require_failure(
            args, MUTATE_AA, "the DSP reset's 0xAA") else 1)
    if args.negative_opl:
        sys.exit(0 if _mutate_and_require_failure(
            args, MUTATE_T1, "the OPL timer 1 expiry flag") else 1)
    sys.exit(0 if main(args) else 1)
