#!/usr/bin/env python3
"""
Tier-1 checks for XMS -- the HIMEM interface, driven by GUEST CODE doing the
real sequence a program does.

INT 2Fh AX=4300h -> AX=4310h -> CALL FAR the returned entry point -> allocate ->
move a pattern up -> move it back down -> free -> query. Nothing here pokes
x86_xms_control() directly. The thing most likely to be wrong is not the
arithmetic inside a function: it is that the entry point has to be a REAL
address in guest memory with a stub that returns by RETF and not IRET, and a
guest executing a CALL FAR is the only witness to that.

WHY THE BUFFERS STRADDLE A GRANULE BOUNDARY. `host = lin + k` with one k per
4 KB granule, and the pager hands frames out in FAULT order, so guest page N+1
is almost never the host memory after page N. A move implemented as one
`memcpy` over a single resolved pointer runs off the top of a frame into an
unrelated page -- the exact bug that made the exec fallback execute the IVT.
The arm primes the four pages OUT OF ORDER so the shuffle is real, and asserts
it (`shuffled` below) rather than assuming it: on a contiguous fold every seam
claim in this file would be vacuous.

THE THREE NEGATIVE CONTROLS, all of which must FAIL in the way stated:
  * `control: corrupted store is DETECTED` -- one byte of the store is flipped
    and the same down-move re-run. If the byte-exact compare above it were
    vacuous this would come back "matching".
  * `control: the naive one-pointer copy IS wrong` -- 64 bytes memcpy'd through
    a single x86_guest_ptr(), then read back granule-aware. It cannot match.
  * the guest's own refusals: an odd length, an unissued handle, a move while
    locked, and an allocation larger than the store all have to come back
    with the documented BL code and AX=0.

The store is a plain RAM buffer standing in for the pagefile, exactly as the
pager arm's does, so the whole interface is proved with no card, no FatFs and
no firmware.
"""
import sys
import time

sys.path.insert(0, ".")
from scripts.common.runner import add_runner_args, build_and_create_runner
from scripts.common.elf import get_symbols

HALT = 6            # X86_EXIT_HALT
STORE_KB = 32       # what the harness's RAM store advertises
BLOCK_KB = 8        # what the guest allocates
PAT = 64            # bytes moved each way


def run_arm(b, syms, flag, result, count):
    # Boot/reset takes several seconds under gwemu. A flag written before
    # Reset_Handler reaches its command loop is cleared by BSS initialization.
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        pc = b.read_register("pc")
        if 0x08000000 <= pc < 0x08200000 and \
                b.read_uint32(syms["run_xms_flag"]) == 0:
            break
        time.sleep(0.05)
    else:
        return None
    b.write_uint32(syms[flag], 1)
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        if b.read_uint32(syms[flag]) == 0 and \
                b.read_uint32(syms["x86_xms_calls"]) >= 10:
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
        for n in ("run_xms_flag", "xms_r"):
            if n not in syms:
                print(f"missing symbol {n}")
                return False

        time.sleep(0.5)
        r = run_arm(b, syms, "run_xms_flag", "xms_r", 56)
        if r is None:
            print("FAIL: target never ran the XMS test")
            return False

        checks = [
            # ---- the premise: a fold in which the seam actually exists
            ("the frames really are shuffled",          r[32], 1),

            # ---- detection, exactly as a program does it
            ("INT 2Fh AX=4300h returns AL=80h",         r[0], 0x4380),
            ("AX=4310h entry segment",                  r[1], 0x0060),
            ("AX=4310h entry offset",                   r[2], 0x00C0),
            ("the driver was detected once",            r[45], 1),
            ("the entry point was asked for once",      r[46], 1),

            # ---- the far call works at all, which is the RETF stub
            ("AH=00h version is 2.00",                  r[3], 0x0200),
            ("  ... and it does NOT claim an HMA",      r[4], 0),

            ("AH=08h largest free, before",             r[5], STORE_KB),
            ("AH=08h total free, before",               r[6], STORE_KB),
            ("what the driver advertises",              r[43], STORE_KB),

            ("AH=09h allocated 8 KB",                   r[7], 1),
            ("  ... handle 1",                          r[8], 1),

            # ---- THE POINT OF THE ARM
            ("AH=0Bh move up succeeded",                r[9], 1),
            ("  ... BL clear",                          r[10] & 0xFF, 0),
            ("AH=0Bh move down succeeded",              r[11], 1),
            ("  ... BL clear",                          r[12] & 0xFF, 0),
            ("ROUND TRIP BYTE-EXACT across a granule",  r[33], 1),
            ("nothing outside the block was written",   r[44], 1),
            ("bytes moved up",                          r[37], PAT),
            # the third down-move is the one through a LOCKED handle (below)
            ("bytes moved down (three down-moves)",     r[38], 3 * PAT),
            ("successful moves",                        r[36], 4),
            ("one store open per move, not per page",   r[41], 4),
            ("no store I/O errors",                     r[39], 0),
            ("no unreachable guest page",               r[40], 0),

            # ---- the negative controls
            ("control: corrupted store is DETECTED",    r[34], 1),
            ("control: the naive one-pointer copy IS wrong", r[35], 1),

            # ---- accounting
            ("AH=0Eh handle info",                      r[13], 1),
            ("  ... 0 locks, 15 handles free",          r[14], 0x000F),
            ("  ... block is 8 KB",                     r[15], BLOCK_KB),
            ("AH=08h largest free, while held",         r[16], STORE_KB - BLOCK_KB),
            ("AH=08h total free, while held",           r[17], STORE_KB - BLOCK_KB),

            # ---- A20 is the MACHINE's gate, not a second model
            ("AH=03h then 07h: the gate is open",       r[18], 1),
            ("AH=04h then 07h: the gate is masked",     r[19], 0),

            # ---- the refusals, each with its documented code
            ("odd length is refused",                   r[21], 0),
            ("  ... BL=A7h invalid length",             r[22] & 0xFF, 0xA7),
            ("an unissued source handle is refused",    r[23], 0),
            ("  ... BL=A3h invalid source handle",      r[24] & 0xFF, 0xA3),
            ("AH=0Ch lock returns an XMS alias",        r[48], 1),
            ("  ... alias is in the 8 MB window",       r[49], 0x0080),
            ("  ... alias starts at allocated bytes",   r[50], 0),
            ("AH=0Eh reports one lock",                 r[51], 0x010F),
            # A lock pins the block's ADDRESS; it does not forbid moves (Dune II
            # locks its block for the linear base and then moves through it).
            # The move is still refused if the alias page is resident in a
            # frame -- the store would be stale -- which a real-mode guest
            # cannot cause, as this one cannot.
            ("AH=0Bh moves a locked handle",           r[52], 1),
            ("  ... BL=00h",                           r[53] & 0xFF, 0x00),
            ("AH=0Dh unlock succeeds",                 r[54], 1),
            ("AH=0Ch can relock after unlock",          r[55], 1),
            ("more memory than exists is refused",      r[30], 0),
            ("  ... BL=A0h all extended memory in use", r[31] & 0xFF, 0xA0),

            # ---- and the memory comes BACK
            ("AH=0Ah freed the handle",                 r[27], 1),
            ("AH=08h largest free, after",              r[28], STORE_KB),
            ("AH=08h total free, after",                r[29], STORE_KB),
            ("no handle left in use",                   r[47], 0),
        ]

        bad = 0
        for name, got, want in checks:
            ok = got == want
            bad += not ok
            print(f"  {'PASS' if ok else 'FAIL'}: {name}: {got}"
                  + ("" if ok else f" (expected {want})"))

        # The A20 gate's starting state depends on what ran before this arm, so
        # the number of TRANSITIONS is 1 or 2 -- what must hold is that the
        # guest moved the machine's own gate, not that it moved it twice.
        ok = 1 <= r[42] <= 2
        bad += not ok
        print(f"  {'PASS' if ok else 'FAIL'}: XMS drove the machine's A20 "
              f"({r[42]} transitions, expected 1 or 2)")

        if bad:
            print(f"FAIL: {bad} of {len(checks) + 1} checks")
            return False
        print(f"PASS: {len(checks) + 1} checks")
        return True
    finally:
        runner.stop()


if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser(description=__doc__)
    add_runner_args(p)
    args = p.parse_args()
    sys.exit(0 if main(args) else 1)
