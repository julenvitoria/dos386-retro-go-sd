#!/usr/bin/env python3
"""
Capture what the panel is showing, as a PNG.

gwemu's own screendump is useless here (it returns black even for Retro-Go's
menu), so this reconstructs the frame from the LTDC's Layer-1 registers and the
firmware's CLUT instead -- see scripts/common/screen.py. That works headless,
under gwemu and on silicon.

Unlike tests/test_screen.py, this asserts nothing about the content: it is for
looking at a guest that is running, not for checking a fixed pattern. It also
prints a coarse histogram, because a PNG that is entirely one palette index is
a black screen no matter how good it looks in a viewer.

    python3 scripts/screenshot.py --wait-frames 200 --out build/artifacts/game.png
    python3 scripts/screenshot.py --shots 4 --interval 2   # a short sequence
"""

import argparse
import os
import sys
import time

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from scripts.common.screen import (                      # noqa: E402
    SCREEN_H, SCREEN_W, capture, write_png,
)
from scripts.common.target import Image, add_target_args, open_target  # noqa: E402
from scripts.common.elf import get_symbols                # noqa: E402

FW_DIR = "references/game-and-watch-retro-go-sd/build"
CORE_MAGIC = 0x54494E59


def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    add_target_args(p)
    p.add_argument("--firmware-dir", default=FW_DIR)
    p.add_argument("--sd", default="sdcard_retrogo.img")
    p.add_argument("--core-elf", default="build/core/dos_core.elf")
    p.add_argument("--out", default="build/artifacts/screen.png")
    p.add_argument("--shots", type=int, default=1,
                   help="capture N frames; the Nth keeps --out, the others "
                        "get a -NN suffix")
    p.add_argument("--interval", type=float, default=1.0)
    p.add_argument("--wait-frames", type=int, default=3,
                   help="wait until the core has rendered this many frames")
    args = p.parse_args()

    fw = args.firmware_dir
    image = Image(bank1=os.path.join(fw, "qemu_bank1.bin"),
                  bank2=os.path.join(fw, "qemu_bank2.bin"),
                  extflash=os.path.join(fw, "extflash.bin"),
                  sdcard=args.sd, elf=args.core_elf, intflash_bank=1)
    if args.timeline is None:
        args.timeline = "timelines/start-preselected-game.tl"

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    target = open_target(args, image)
    target.start()
    try:
        b = target.backend
        core = get_symbols(args.core_elf)
        fw_syms = get_symbols(os.path.join(fw, "gw_retro_go.elf"))

        deadline = time.time() + 30
        while time.time() < deadline:
            if b.read_uint32(core["core_magic"]) == CORE_MAGIC and \
               b.read_uint32(core["frames_rendered"]) >= args.wait_frames:
                break
            time.sleep(0.25)
        else:
            print("core never reached the requested frame count")
            return 1

        ok = False
        for n in range(args.shots):
            if n:
                time.sleep(args.interval)
            b.halt()
            info, indices, clut, rgb = capture(b, fw_syms, core)
            frames = b.read_uint32(core["frames_rendered"])
            b.resume()
            if info is None:
                print("this target does not model the LTDC (CFBAR reads 0)")
                return 1
            path = args.out if n == args.shots - 1 else \
                f"{os.path.splitext(args.out)[0]}-{n:02d}.png"
            write_png(path, SCREEN_W, SCREEN_H, rgb)
            hist = {}
            for i in indices:
                hist[i] = hist.get(i, 0) + 1
            top = sorted(hist.items(), key=lambda kv: -kv[1])[:6]
            print(f"frame {frames}: fb=0x{info['fb']:08x} "
                  f"pitch={info['pitch']} lines={info['lines']} "
                  f"clut={info['clut_src']}")
            print("  palette index histogram: " +
                  " ".join(f"{i}:{c}" for i, c in top))
            print(f"  wrote {path}")
            ok = ok or len(hist) > 1
        if not ok:
            print("WARNING: every pixel has the same palette index -- this is "
                  "a blank screen, not a picture")
        return 0
    finally:
        target.stop()


if __name__ == "__main__":
    sys.exit(main())
