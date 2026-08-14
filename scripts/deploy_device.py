#!/usr/bin/env python3
"""Update the real device: firmware to internal flash, core to the SD card.

Why this is not `sdcard.py --probe` plus a manual flash:

  * `sdcard.py` composes a WHOLE card -- sd_content, a ROM, and `/CONFIG`.
    On the device that is wrong twice over. The ROM library is already there
    and pushing it over the probe would take hours, and `/CONFIG` carries the
    owner's own settings (preselected title, overclock level, standby): a
    compose REWRITES it, silently resetting a level set through the launcher.
  * `HardwareTarget` flashes as a side effect of starting a MEASUREMENT run,
    so using it to deploy means also booting a timeline you did not want.

So this does the two things a core change actually needs, and nothing else:
flash internal flash, then push `/cores/<name>.bin`. It never touches
`/CONFIG`, `/roms`, or `sd_content`.

    # both halves (the usual case after a firmware ABI change)
    python3 scripts/deploy_device.py

    # only the core -- the common inner-loop case, seconds not minutes
    python3 scripts/deploy_device.py --no-firmware

Verify afterwards WITHOUT disturbing what is running:

    python3 scripts/probe.py --retrogo --target hardware --no-flash \\
        --watch core_ready,guest_insns_total,x86_vga13_ok
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

FW_DIR = "references/game-and-watch-retro-go-sd/build"


def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--firmware-dir", default=FW_DIR)
    p.add_argument("--core", default="dos.bin", help="packed core .bin")
    p.add_argument("--core-name", default="dos", help="-> /cores/<name>.bin")
    p.add_argument("--bank", type=int, default=1, choices=[1, 2],
                   help="internal flash bank the firmware was LINKED for")
    p.add_argument("--no-firmware", action="store_true",
                   help="push only the core; leave internal flash alone")
    p.add_argument("--no-core", action="store_true",
                   help="flash only the firmware; leave the SD card alone")
    args = p.parse_args()

    intflash = os.path.join(args.firmware_dir, "gw_retro_go_intflash.bin")
    if not args.no_firmware and not os.path.exists(intflash):
        sys.exit(f"missing {intflash} -- python3 scripts/build_retrogo.py")
    if not args.no_core and not os.path.exists(args.core):
        sys.exit(f"missing {args.core} -- make")

    from gnwmanager.gnw import GnW
    from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend
    from scripts.common.sdcard import HardwareProbeSDCardManager

    backend = OpenOCDBackend()
    backend.open()
    gnw = GnW(backend)
    gnw.start_gnwmanager()

    if not args.no_firmware:
        with open(intflash, "rb") as fh:
            data = fh.read()
        # Do NOT call gnw.erase() first. `flash()` -> `program(erase=True)`
        # already erases each sector before writing it, which is what keeps
        # the ECC story right: on STM32H7 the flash is ECC-protected in
        # 128-bit words, and programming a word that was not erased leaves an
        # UNCORRECTABLE error whose READ raises an imprecise bus fault -- one
        # that survives reflashing, because the bad word is just reprogrammed
        # over again. A separate erase of the whole bank up front is redundant
        # AND fails here: the on-target helper returns BAD_SEGFAULT.
        print(f"Flashing {intflash} -> bank {args.bank} "
              f"({len(data)} bytes, erase included)...", flush=True)
        gnw.flash(args.bank, 0, data)

    if not args.no_core:
        mgr = HardwareProbeSDCardManager(gnw)
        mgr.push_file(args.core, f"cores/{args.core_name}.bin")

    print("done. /CONFIG, /roms and sd_content were NOT touched.")
    print("Power-cycle or reset the device to boot the new firmware.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
