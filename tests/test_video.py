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
# Graphics pixels index their own four-entry CLUT block; 0-15 stay the fixed
# CGA palette in every mode. See x86_video_refresh_palette().
GFX_CLUT_BASE = 16
TEXT_ROWS = 25
CELL_H = 8


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
            img[dst + col * 4 + 0] = GFX_CLUT_BASE + ((b >> 6) & 3)
            img[dst + col * 4 + 1] = GFX_CLUT_BASE + ((b >> 4) & 3)
            img[dst + col * 4 + 2] = GFX_CLUT_BASE + ((b >> 2) & 3)
            img[dst + col * 4 + 3] = GFX_CLUT_BASE + ((b >> 0) & 3)
    return bytes(img)


def expected_mode6(vram):
    """640x200 at 1 bpp, decimated to the 320 px panel by OR-ing pixel PAIRS:
    a set bit in either lights the output pixel, so a one-pixel rule survives
    where dropping every other column would erase half of them."""
    img = bytearray(FB_STRIDE * 240)
    for row in range(CGA_HEIGHT):
        plane = PLANE_ODD if (row & 1) else 0
        src = plane + (row // 2) * CGA_ROW_BYTES
        dst = FB_TOP + row * FB_STRIDE
        for col in range(CGA_ROW_BYTES):
            b = vram[src + col]
            for p in range(4):
                # Source bits 7-2p and 6-2p, leftmost first.
                lit = ((b >> (7 - 2 * p)) | (b >> (6 - 2 * p))) & 1
                img[dst + col * 4 + p] = GFX_CLUT_BASE + lit
    return bytes(img)


def expected_glyph_mode4(glyph, colour, col, row):
    """What one INT 10h AH=0Eh character must look like in VRAM in mode 4.

    This is the whole reported bug in one expectation: in a graphics mode the
    BIOS rasterises the 8x8 glyph as PIXELS, and storing a char/attr pair here
    instead is what turned Alley Cat's joystick prompt into confetti. Computed
    from the font the target hands the guest, so a wrong bit order or a missed
    interleave bank cannot agree with a matching mistake in the renderer."""
    vram = bytearray(32768)
    pix = colour & 3 or 3
    for line in range(CELL_H):
        y = row * CELL_H + line
        off = (PLANE_ODD if (y & 1) else 0) + (y // 2) * CGA_ROW_BYTES + col * 2
        bits = glyph[line]
        for half in range(2):
            b = 0
            for p in range(4):
                if bits & (1 << (half * 4 + p)):   # bit 0 is the LEFTMOST pixel
                    b |= pix << (6 - 2 * p)
            vram[off + half] = b
    return bytes(vram)


def wait_flag(b, addr, want, what, tries=200):
    for _ in range(tries):
        if b.read_uint32(addr) == want:
            return True
        time.sleep(0.05)
    print(f"FAIL: {what}")
    return False


def render(b, syms):
    b.write_uint32(syms["run_video_flag"], 3)
    if not wait_flag(b, syms["run_video_flag"], 0, "blit did not complete"):
        return None
    return bytes(b.read_memory(syms["cga_test_out"], FB_STRIDE * 240))


def guest_set_mode(b, syms, mode):
    """Set the mode the way a program does: MOV AX,000m / INT 10h in GUEST
    code. Returns (mode, is_text, cols, bda_mode, bda_cols)."""
    b.write_uint32(syms["msw_mode_arg"], mode)
    b.write_uint32(syms["run_modeswitch_flag"], 2)
    if not wait_flag(b, syms["run_modeswitch_flag"], 0, f"mode {mode:#x} set hung"):
        return None
    return [b.read_uint32(syms["msw_r"] + 4 * i) for i in range(6)]


def mode_switch_checks(b, syms):
    """A guest switching modes at RUNTIME, in both directions, plus the modes
    brought over from the previous DOS core.

    The reported bug ("Alley Cat doesn't switch from CGA to text mode
    correctly, it just becomes a bunch of pixels") was measured to be none of
    mode detection, palette or blit: the mode switch was followed exactly, and
    the CHARACTERS printed afterwards were still stored as char/attr pairs into
    2 bpp pixel memory. So this walks 3 -> 4 -> 3 and checks the rendering after
    each, and then checks the character path IN a graphics mode, which is where
    the defect actually was."""
    for n in ("run_modeswitch_flag", "msw_mode_arg", "msw_char_arg", "msw_r"):
        if n not in syms:
            print(f"missing symbol {n} -- rebuild tests/main.c")
            return False

    b.write_uint32(syms["run_modeswitch_flag"], 1)
    if not wait_flag(b, syms["run_modeswitch_flag"], 0, "mode-switch init hung"):
        return False

    # The bare-metal harness links BOTH text renderers and defaults to the
    # anti-aliased one; the per-frame CLUT allocation under it is test_text.py's
    # subject, not this file's. Pin it off so a text expectation here is the
    # bilevel one.
    b.write_uint32(syms["x86_text_aa"], 0)

    import scripts.common.textmode as textmode
    font = textmode.load_font()                     # 4x8, the 80-column face
    font8 = textmode.load_font("font8x8.h")         # 8x8, 40 columns and the
    ok = True                                       # graphics-mode rasteriser

    # ---- text 3 -> graphics 4 -> text 3, each rendered and compared --------
    cells = bytearray()
    for i in range(80 * 25):
        cells += bytes([0x41 + (i % 26), 0x07 if (i % 3) else 0x1E])
    pattern = make_pattern()

    for step, (mode, kind) in enumerate(((3, "text"), (4, "cga"), (3, "text"))):
        r = guest_set_mode(b, syms, mode)
        if r is None:
            return False
        got_mode, is_text, cols, bda_mode, bda_cols = r[0], r[1], r[2], r[3], r[4]
        want_text = 1 if kind == "text" else 0
        if got_mode != mode or is_text != want_text:
            print(f"FAIL: step {step}: INT 10h AX={mode:04x} left mode "
                  f"{got_mode:#x} is_text={is_text} (want {mode:#x}/{want_text})")
            return False
        if bda_mode != mode or bda_cols not in (40, 80):
            print(f"FAIL: step {step}: BDA says mode {bda_mode:#x} cols "
                  f"{bda_cols} -- the renderer re-reads these every frame")
            return False

        if kind == "text":
            b.write_memory(syms["cga_test_vram"], bytes(cells))
            want = textmode.render(bytes(cells), font, 0)
        else:
            b.write_memory(syms["cga_test_vram"], pattern)
            want = expected_image(pattern)
        got = render(b, syms)
        if got is None:
            return False
        if got != want:
            diff = sum(1 for g, w in zip(got, want) if g != w)
            print(f"FAIL: step {step} (mode {mode:#x}, {kind}): "
                  f"{diff} of {len(want)} bytes differ")
            return False
        print(f"PASS: step {step}: guest INT 10h -> mode {mode:#x} "
              f"({cols} cols), rendered exact")

    # ---- the actual defect: a character printed in a GRAPHICS mode ---------
    guest_set_mode(b, syms, 4)
    b.write_memory(syms["cga_test_vram"], bytes(32768))
    b.write_uint32(syms["msw_char_arg"], ord("A"))
    b.write_uint32(syms["run_modeswitch_flag"], 3)
    if not wait_flag(b, syms["run_modeswitch_flag"], 0, "graphics teletype hung"):
        return False
    got_vram = bytes(b.read_memory(syms["cga_test_vram"], 32768))
    want_vram = expected_glyph_mode4(font8[ord("A")], 3, 0, 0)
    if got_vram != want_vram:
        pair = got_vram[0] | (got_vram[1] << 8)
        extra = ""
        if pair == (0x0741 if got_vram[1] == 7 else pair) or got_vram[0] == 0x41:
            extra = ("  -- VRAM holds the CHARACTER BYTE, so the glyph was "
                     "never rasterised: this is the Alley Cat defect")
        print(f"FAIL: INT 10h AH=0Eh in mode 4 did not rasterise the glyph{extra}")
        return False
    print("PASS: INT 10h AH=0Eh in mode 4 rasterises an 8x8 glyph into both "
          "interleave banks")

    # ---- mode 6: 640x200 at 1 bpp -----------------------------------------
    r = guest_set_mode(b, syms, 6)
    if r is None:
        return False
    if r[0] != 6 or r[1] != 0:
        print(f"FAIL: mode 6 not adopted (mode {r[0]:#x} is_text={r[1]})")
        return False
    b.write_memory(syms["cga_test_vram"], pattern)
    got = render(b, syms)
    want = expected_mode6(pattern)
    if got != want:
        diff = sum(1 for g, w in zip(got, want) if g != w)
        print(f"FAIL: mode 6: {diff} of {len(want)} bytes differ")
        return False
    print("PASS: mode 6 (640x200x1, OR-decimated to 320) pixel-exact")

    # ---- mode 0: 40 columns, which selects the 8x8 font --------------------
    r = guest_set_mode(b, syms, 0)
    if r is None:
        return False
    if r[0] != 0 or r[1] != 1 or r[2] != 40:
        print(f"FAIL: mode 0 not adopted as 40-column text "
              f"(mode {r[0]:#x} is_text={r[1]} cols={r[2]})")
        return False
    cells40 = bytearray()
    for i in range(40 * 25):
        cells40 += bytes([0x30 + (i % 40), 0x0F if (i % 2) else 0x47])
    b.write_memory(syms["cga_test_vram"], bytes(cells40))
    got = render(b, syms)
    want = expected_text40(bytes(cells40), font8)
    if got != want:
        diff = sum(1 for g, w in zip(got, want) if g != w)
        print(f"FAIL: 40-column text: {diff} of {len(want)} bytes differ")
        return False
    print("PASS: mode 0 (40x25 text at the 8x8 cell) pixel-exact")

    # ---- a mode we cannot render must be REFUSED, not adopted -------------
    r = guest_set_mode(b, syms, 0x13)
    if r is None:
        return False
    if r[0] != 0:
        print(f"FAIL: unsupported mode 13h was adopted ({r[0]:#x}); the "
              f"previous mode must stay up instead")
        return False
    print("PASS: mode 13h refused, mode 0 stays up (nothing renders it)")
    return ok


def expected_text40(cells, font):
    """40 columns of the authentic 8x8 cell: 40 x 8 = 320, the whole panel."""
    img = bytearray(FB_STRIDE * 240)
    for row in range(TEXT_ROWS):
        for col in range(40):
            cell = cells[(row * 40 + col) * 2] | (cells[(row * 40 + col) * 2 + 1] << 8)
            g = font[cell & 0xFF]
            fg, bg = (cell >> 8) & 0x0F, (cell >> 12) & 0x07
            for gy in range(CELL_H):
                base = FB_TOP + (row * CELL_H + gy) * FB_STRIDE + col * 8
                for p in range(8):
                    img[base + p] = fg if (g[gy] >> p) & 1 else bg
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

        return mode_switch_checks(b, syms)
    finally:
        runner.stop()


if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser(description=__doc__)
    add_runner_args(p)
    args = p.parse_args()
    sys.exit(0 if main(args) else 1)
