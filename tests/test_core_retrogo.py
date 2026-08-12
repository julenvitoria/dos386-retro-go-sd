#!/usr/bin/env python3
"""
Tier 2: boot retro-go under gwemu, launch our packed core, read its results.

Proves the pieces the whole architecture rests on, inside the real firmware:
  * the core loads and its ITCM segment is placed at 0x00000000
  * DTCM comes from mem_ctl(GW_MEM_DTC, ...) -- core_dtcm_ok
  * LUT8 releases the framebuffer bonus pool -- bonus_pool_size
  * the interpreter runs and reports the same 1,000,001 instructions as tier 2

Run:  python3 tests/test_core_retrogo.py [--gui] [--icount 0]
"""

import argparse
import os
import sys
import time

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from scripts.common.target import (Image, add_target_args, open_target,
                                   timeline_quit_time)
from scripts.common.elf import get_symbols
from scripts.common.rglog import dump_log
from scripts.common.core_abi import check as abi_check


CORE_MAGIC = 0x54494E59   # 'TINY', must match src/main.c

def wait_for_value(backend, addr, want, timeout, poll=0.25):
    """Wait for an EXACT value. Non-zero is not a usable signal here: until the
    firmware loads our core, these RAM_EMU addresses hold firmware data."""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = backend.read_uint32(addr)
        if last == want:
            return True
        time.sleep(poll)
    return False


def wait_for(backend, addr, timeout, poll=0.25):
    deadline = time.time() + timeout
    while time.time() < deadline:
        v = backend.read_uint32(addr)
        if v:
            return v
        time.sleep(poll)
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    add_target_args(p)
    p.add_argument("--firmware-dir",
                   default="references/game-and-watch-retro-go-sd/build",
                   help="directory holding the built firmware media")
    p.add_argument("--elf", default="build/core/dos_core.elf",
                   help="core ELF, for symbol lookup")
    p.add_argument("--sd", default="sdcard_retrogo.img",
                   help="SD image carrying /cores/dos.bin and /CONFIG")
    p.add_argument("--firmware-elf",
                   default="references/game-and-watch-retro-go-sd/build/"
                           "gw_retro_go.elf",
                   help="firmware ELF, for reading its log ring buffer")
    # Must stay INSIDE the timeline's own quit or gwemu exits mid-wait and the
    # GDB link drops -- which surfaces as "Connection closed by remote" and
    # reads like a target fault. Default is derived from the timeline below,
    # not hardcoded: the previous hardcoded 120s/150s outlived the 35s
    # start-preselected-game.tl and turned every tier-2 run into a traceback.
    p.add_argument("--timeout", type=float, default=None,
                   help="seconds to wait for the core to report ready "
                        "(default: the timeline's quit time, minus a margin)")
    args = p.parse_args()

    fw = args.firmware_dir
    args.firmware_elf = os.path.join(fw, "gw_retro_go.elf")
    if args.timeline is None:
        args.timeline = "timelines/start-preselected-game.tl"

    quit_at = timeline_quit_time(args.timeline)
    if args.timeout is None:
        args.timeout = max(5.0, (quit_at or 60.0) - 3.0)
    elif quit_at is not None and args.timeout > quit_at:
        print(f"NOTE: --timeout {args.timeout}s exceeds the timeline's quit at "
              f"{quit_at}s; clamping. gwemu will not be alive past that.")
        args.timeout = max(5.0, quit_at - 3.0)

    for path in (os.path.join(fw, "qemu_bank1.bin"), args.sd, args.elf):
        if not os.path.exists(path):
            sys.exit(f"missing {path} -- run: make retrogo_media && "
                     f"make -f Makefile.core pack")

    # Fail loudly BEFORE booting: a core the firmware will not accept simply
    # never appears, which looks exactly like every other kind of failure.
    if not abi_check("dos.bin", os.path.join(fw, "gw_retro_go.elf")):
        return 1

    syms = get_symbols(args.elf)
    for name in ("core_ready", "core_dtcm_ok", "benchmark_cycles",
                 "benchmark_insns", "bonus_pool_size", "core_magic",
                 "run_benchmark_flag"):
        if name not in syms:
            sys.exit(f"symbol {name} not in {args.elf}")

    image = Image(bank1=os.path.join(fw, "qemu_bank1.bin"),
                  bank2=os.path.join(fw, "qemu_bank2.bin"),
                  extflash=os.path.join(fw, "extflash.bin"),
                  sdcard=args.sd, elf=args.elf, intflash_bank=1)
    runner = open_target(args, image)
    runner.start()
    ok = False
    try:
        # No launcher navigation. The SD image carries a /CONFIG that
        # preselects /roms/dos/CAT.EXE, and the timeline just presses A twice.
        # Navigating menus by button choreography is not tractable and is
        # explicitly not how this is meant to be driven.
        print("Waiting for the preselected ROM to launch our core...")
        if not wait_for_value(runner.backend, syms["core_magic"], CORE_MAGIC,
                              args.timeout):
            got = runner.backend.read_uint32(syms["core_magic"])
            print(f"FAIL: core never ran (core_magic=0x{got:08x}, "
                  f"expected 0x{CORE_MAGIC:08x})")
            dump_log(runner.backend, args.firmware_elf, tail=40)
            return 1
        ready = wait_for(runner.backend, syms["core_ready"],
                         min(10.0, args.timeout))
        if not ready:
            print("FAIL: core never reported ready.")
            print(f"  core_dtcm_ok = "
                  f"{runner.backend.read_uint32(syms['core_dtcm_ok'])}")
            dump_log(runner.backend, args.firmware_elf, tail=40)
            return 1

        dtcm_ok = runner.backend.read_uint32(syms["core_dtcm_ok"])
        bonus = runner.backend.read_uint32(syms["bonus_pool_size"])
        # The core does NOT self-start the benchmark: auto-running it at boot
        # faulted and rebooted the device, wiping the very globals this test
        # reads. The host arms it, which is also why this test was previously
        # reporting a figure the core had stopped producing.
        runner.backend.write_uint32(syms["run_benchmark_flag"], 1)
        cycles = wait_for(runner.backend, syms["benchmark_cycles"], args.timeout)
        insns = runner.backend.read_uint32(syms["benchmark_insns"])

        print("\n--- core reported ---")
        print(f"core_ready       {ready}")
        print(f"core_dtcm_ok     {dtcm_ok}")
        print(f"bonus_pool_size  {bonus} bytes")
        print(f"cycles           {cycles}")
        print(f"insns            {insns}")
        if insns:
            print(f"cyc/insn         {cycles / insns:.2f}")

        ok = True
        if not dtcm_ok:
            print("FAIL: DTCM allocation failed"); ok = False
        if insns != 1000001:
            print(f"FAIL: expected 1000001 instructions, got {insns}"); ok = False
        if bonus == 0:
            print("FAIL: LUT8 released no bonus pool"); ok = False
        print("PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        try:
            runner.collect_artifacts()
        except Exception as exc:
            print(f"(could not collect artifacts: {exc})")
        runner.stop()


if __name__ == "__main__":
    sys.exit(main())
