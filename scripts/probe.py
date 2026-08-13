#!/usr/bin/env python3
"""
Sample a running target: PC, watched globals, pointer dereferences.

The "what is it actually doing" tool. Use it when a harness reports a value that
makes no sense and you need to see whether the CPU is in our interpreter, in
firmware, or wedged -- and whether the guest is advancing at all.

Examples:

    # Is our core progressing inside retro-go? Follow cpu_state_ptr to EAX.
    python3 scripts/probe.py --retrogo \\
        --watch core_ready,core_dtcm_ok,benchmark_insns \\
        --deref cpu_state_ptr+8=EAX

    # Bare-metal harness, on silicon
    python3 scripts/probe.py --target hardware --watch run_flag,benchmark_cycles

Timelines used here must stay short -- see CLAUDE.md. If you need a long window
to observe something, that is a bug to find, not a wait to sit through.
"""

import argparse
import os
import sys

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from scripts.common.probe import (                       # noqa: E402
    dump_arrays, dump_symbol_name, load_symbol_maps, parse_deref, parse_dump,
    parse_watch,
    sample, wait_for_value,
)
from scripts.common.rglog import LogDrainer                # noqa: E402
from scripts.common.target import Image, add_target_args, open_target  # noqa: E402

FW_DIR = "references/game-and-watch-retro-go-sd/build"
CORE_MAGIC = 0x54494E59   # 'TINY', set by src/main.c



# Named watch sets, so a subsystem's whole story is one word on the command
# line instead of a seventeen-symbol paste that goes stale the moment a counter
# is added. Keep these in step with the tables in TOOLING.md.
WATCH_GROUPS = {
    "nor": [
        # did the snapshot happen, and what did the firmware's flash cache do
        "nor_snap_result", "nor_snap_ms", "nor_snap_addr", "nor_snap_cb_bytes",
        # is the guest actually running out of NOR
        "x86_nor_attached", "x86_nor_attach_err",
        # the measurement: how much of guest RAM is genuinely read-only
        "x86_nor_pages_total", "x86_nor_pages_nor", "x86_nor_pages_axi",
        "x86_nor_axi_free",
        # what it costs, and who is paying it
        "x86_nor_reads", "x86_nor_writes", "x86_nor_cow_pulls",
        "x86_nor_host_cow", "x86_nor_slow_cycles",
        # the loud failures
        "x86_nor_cow_failures", "x86_nor_unmapped",
    ],
}

def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_target_args(p)
    p.add_argument("--retrogo", action="store_true",
                   help="boot retro-go + our packed core (default: the "
                        "bare-metal CPU harness)")
    p.add_argument("--firmware-dir", default=FW_DIR)
    p.add_argument("--sd", default="sdcard_retrogo.img")
    p.add_argument("--elf", default=None,
                   help="ELF for symbols (default depends on --retrogo)")
    p.add_argument("--watch", default="",
                   help="comma-separated globals to read as uint32; a GROUP "
                        f"name expands to a whole set ({', '.join(WATCH_GROUPS)})")
    p.add_argument("--dump", action="append", default=[],
                   metavar="SYM[:N][xG][=LABEL]",
                   help="hexdump N words of a symbol's own storage, grouped "
                        "G per line (arrays; --deref follows a pointer instead)")
    p.add_argument("--deref", action="append", default=[],
                   metavar="SYM[+OFF][=LABEL]",
                   help="read uint32 at (*SYM)+OFF; repeatable")
    p.add_argument("--set", action="append", default=[], metavar="SYM=VALUE",
                   help="write a uint32 before sampling, e.g. to trigger work "
                        "on the target; repeatable")
    p.add_argument("--zero", action="append", default=[], metavar="ADDR:BYTES",
                   help="write zeros over a memory range before sampling, e.g. "
                        "--zero 0x30000000:0x48000. Repeatable. For clearing "
                        "RETAINED state: retro-go keeps persistent_config_ram "
                        "in D2 AHB SRAM (0x30000000), which survives every "
                        "reset and reflash and is only lost on real power "
                        "removal -- so a bad value there outlives every "
                        "recovery step short of this")
    p.add_argument("--faults", action="store_true",
                   help="also dump ARMv7-M fault/exception state")
    p.add_argument("--samples", type=int, default=10)
    p.add_argument("--interval", type=float, default=1.0)
    p.add_argument("--wait-magic", action="store_true",
                   help="wait for core_magic before sampling (retro-go)")
    args = p.parse_args()

    if args.retrogo:
        fw = args.firmware_dir
        elf = args.elf or "build/core/dos_core.elf"
        image = Image(bank1=os.path.join(fw, "qemu_bank1.bin"),
                      bank2=os.path.join(fw, "qemu_bank2.bin"),
                      extflash=os.path.join(fw, "extflash.bin"),
                      sdcard=args.sd, elf=elf, intflash_bank=1,
                      intflash=os.path.join(fw, "gw_retro_go_intflash.bin"))
        if args.timeline is None:
            args.timeline = "timelines/start-preselected-game.tl"
        fw_elf = os.path.join(fw, "gw_retro_go.elf")
    else:
        elf = args.elf or "test_core.elf"
        image = Image(bank1="qemu_bank1.bin", bank2="qemu_bank2.bin",
                      extflash="extflash.bin", sdcard="sdcard.img",
                      elf=elf, intflash_bank=1)
        fw_elf = None

    syms, maps = load_symbol_maps(fw_elf, elf)
    watches = []
    for w in parse_watch(args.watch):
        watches.extend(WATCH_GROUPS.get(w, [w]))
    derefs = [parse_deref(d) for d in args.deref]
    dumps = [parse_dump(d) for d in args.dump]

    missing = [w for w in watches if w not in syms]
    missing += [s for s, _, _ in derefs if s not in syms]
    missing += [n for n in (dump_symbol_name(s) for s, _, _, _ in dumps)
                if n is not None and n not in syms]
    if missing:
        sys.exit(f"symbols not found in {elf}: {missing}")

    target = open_target(args, image)
    target.start()
    try:
        if args.wait_magic and "core_magic" in syms:
            print("waiting for core_magic...")
            if not wait_for_value(target.backend, syms["core_magic"],
                                  CORE_MAGIC, 30.0):
                print("core never started; sampling anyway")
            else:
                print("core running")
        for spec in args.zero:
            addr_s, _, len_s = spec.partition(":")
            addr, nbytes = int(addr_s, 0), int(len_s, 0)
            target.backend.halt()
            chunk = bytes(4096)
            done = 0
            while done < nbytes:
                n = min(len(chunk), nbytes - done)
                target.backend.write_memory(addr + done, chunk[:n])
                done += n
            target.backend.resume()
            print(f"zeroed 0x{addr:08x}..0x{addr + nbytes:08x} ({nbytes} bytes)")

        for spec in args.set:
            name, _, val = spec.partition("=")
            name = name.strip()
            if name not in syms:
                sys.exit(f"--set: symbol {name} not found")
            target.backend.write_uint32(syms[name], int(val, 0))
            print(f"set {name} = {int(val, 0)}")

        drainer = LogDrainer(fw_elf) if fw_elf else None
        if drainer and not drainer.available:
            drainer = None

        sample(target, syms, watches=watches, derefs=derefs,
               count=args.samples, interval=args.interval, symbol_maps=maps,
               on_sample=(drainer.drain if drainer else None))

        # After sampling, not before: an array read at t=0 shows the state
        # before the target has done anything, which is how a full log reads
        # as empty.
        if dumps:
            target.backend.halt()
            dump_arrays(target, syms, dumps, symbol_maps=maps)
            target.backend.resume()
        if args.faults:
            from scripts.common.probe import read_faults
            print("fault state:")
            target.backend.halt()
            read_faults(target.backend, symbol_maps=maps)
            target.backend.resume()
    finally:
        target.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
