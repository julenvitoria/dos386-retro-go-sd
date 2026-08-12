#!/usr/bin/env python3
"""
Tier-1 check of the CGA mode 4 blitter.

The expected image is computed here, independently of the target: the host
writes a pattern into VRAM, triggers the blit, reads the framebuffer back and
compares it against an expansion done in Python. Nothing on the device
contributes to the expectation, so a wrong table or a wrong interlace shows up
as a mismatch rather than as agreement between two copies of the same bug.
"""
import sys
import time

sys.path.insert(0, ".")
from scripts.common.runner import add_runner_args, build_and_create_runner
from scripts.common.elf import get_symbols

CGA_ROW_BYTES = 80
CGA_WIDTH = 320
CGA_HEIGHT = 200
PLANE_ODD = 0x2000
FB_STRIDE = 320
FB_TOP = ((240 - CGA_HEIGHT) // 2) * FB_STRIDE


def make_pattern():
    """A pattern where every byte differs from its neighbours and all four
    2-bit codes appear, so a shift/order error cannot alias into a pass."""
    vram = bytearray(32768)
    for row in range(CGA_HEIGHT):
        plane = PLANE_ODD if (row & 1) else 0
        base = plane + (row // 2) * CGA_ROW_BYTES
        for col in range(CGA_ROW_BYTES):
            vram[base + col] = (row * 31 + col * 7) & 0xFF
    return bytes(vram)


def expected_image(vram):
    """Expand 2bpp -> 8bpp exactly as the hardware defines it: leftmost pixel
    in the HIGH bits of each byte."""
    img = bytearray(FB_STRIDE * 240)
    for row in range(CGA_HEIGHT):
        plane = PLANE_ODD if (row & 1) else 0
        src = plane + (row // 2) * CGA_ROW_BYTES
        dst = FB_TOP + row * FB_STRIDE
        for col in range(CGA_ROW_BYTES):
            b = vram[src + col]
            img[dst + col * 4 + 0] = (b >> 6) & 3
            img[dst + col * 4 + 1] = (b >> 4) & 3
            img[dst + col * 4 + 2] = (b >> 2) & 3
            img[dst + col * 4 + 3] = (b >> 0) & 3
    return bytes(img)


def main(args):
    runner = build_and_create_runner(args)
    try:
        runner.start()
        b = runner.backend
        syms = get_symbols(getattr(args, "elf", None) or "test_core.elf")
        for n in ("run_video_flag", "cga_test_vram", "cga_test_out",
                  "video_blit_cycles"):
            if n not in syms:
                print(f"missing symbol {n}")
                return False

        # Let the C init settle before touching anything.
        time.sleep(0.5)

        # 1 -> the target initialises video (which zeroes VRAM) and answers 2.
        b.write_uint32(syms["run_video_flag"], 1)
        for _ in range(100):
            if b.read_uint32(syms["run_video_flag"]) == 2:
                break
            time.sleep(0.05)
        else:
            print("FAIL: target never acknowledged video init")
            return False

        vram = make_pattern()
        b.write_memory(syms["cga_test_vram"], vram)

        # 3 -> blit.
        b.write_uint32(syms["run_video_flag"], 3)
        for _ in range(200):
            if b.read_uint32(syms["run_video_flag"]) == 0:
                break
            time.sleep(0.05)
        else:
            print("FAIL: blit did not complete")
            return False

        got = bytes(b.read_memory(syms["cga_test_out"], FB_STRIDE * 240))
        want = expected_image(vram)
        cycles = b.read_uint32(syms["video_blit_cycles"])

        if got != want:
            for i, (g, w) in enumerate(zip(got, want)):
                if g != w:
                    row, col = divmod(i, FB_STRIDE)
                    print(f"FAIL: first mismatch at ({col},{row}) "
                          f"got {g} expected {w}")
                    break
            diff = sum(1 for g, w in zip(got, want) if g != w)
            print(f"  {diff} of {len(want)} bytes differ")
            return False

        px = CGA_WIDTH * CGA_HEIGHT
        print(f"PASS: {px} pixels exact")
        print(f"  blit: {cycles} cycles, {cycles / px:.2f} cyc/pixel")
        return True
    finally:
        runner.stop()


if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser(description=__doc__)
    add_runner_args(p)
    args = p.parse_args()
    sys.exit(0 if main(args) else 1)
