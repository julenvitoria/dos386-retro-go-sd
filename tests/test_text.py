#!/usr/bin/env python3
"""
Tier-1 check of the CGA text-mode (mode 3) renderer.

The text renderer had no coverage at all, which is how it shipped drawing a
full-screen DOS UI as gibberish. This is the mode-3 counterpart of
`test_video.py`: the host seeds an 80x25 (char, attribute) pattern straight
into the text buffer -- the path a real DOS program takes, poking B800:0 rather
than calling INT 10h -- renders one frame on the target, and compares the
framebuffer against an image computed here in Python.

What it pins down:

  * cell layout -- 2 bytes per cell, character low, attribute high. A stride
    error or a char/attribute swap shows up as a mismatch instead of as
    plausible-looking noise next to correct text.
  * the full CP437 range. The pattern uses all 256 character codes; a font
    indexed with only 7 bits (the original defect) fails here.
  * attributes -- all 16 foregrounds, all 8 backgrounds, and the blink bit,
    which must be masked out of the background rather than selecting colours
    8-15.
  * the 40-column window, at both ends of the 80-column buffer.
  * INT 10h AH=06 scrolling, checked on the buffer itself.
  * the text-mode CLUT, which must hold all 16 CGA colours (mode 4 needs only
    four, and the text branch is a separate path that can rot unnoticed).

The font is read out of `font8x8.h` for the pixel comparison -- the point of
that comparison is layout, not glyph art -- but a handful of well-known CP437
shapes are asserted against hard-coded bitmaps as well, so a font that is the
wrong size, wrong encoding or wrong bit order cannot pass.
"""
import sys
import time

sys.path.insert(0, ".")
from scripts.common.runner import add_runner_args, build_and_create_runner
from scripts.common.elf import get_symbols
from scripts.common import textmode

TEXT_COLS, TEXT_ROWS = textmode.TEXT_COLS, textmode.TEXT_ROWS
VIS_COLS = textmode.VIS_COLS
CGA_ARGB = textmode.CGA_ARGB
FB_STRIDE = 320

# Well-known CP437 cells, drawn leftmost column first. These are asserted
# against the header independently of anything the target does: they are the
# characters a DOS UI is actually made of, and they are exactly the ones a
# 128-glyph ASCII font gets wrong.
KNOWN = {
    0xC4: ["........", "........", "........", "........",
           "########", "........", "........", "........"],   # horizontal rule
    0xB3: ["...#....", "...#....", "...#....", "...#....",
           "...#....", "...#....", "...#....", "...#...."],   # vertical rule
    0xDB: ["########", "########", "########", "########",
           "########", "########", "########", "########"],   # full block
    0xB0: ["#.#.#.#.", "........", ".#.#.#.#", "........",
           "#.#.#.#.", "........", ".#.#.#.#", "........"],   # light shade
    0xDA: ["........", "........", "........", "........",
           "...#####", "...#....", "...#....", "...#...."],   # top-left corner
}


def check_font(font):
    if len(font) != 256:
        print(f"FAIL: font8x8.h has {len(font)} glyphs, need 256 (CP437). "
              f"A 128-entry font forces the renderer to mask the character "
              f"byte, so every box-drawing and shading code draws a letter.")
        return False
    ok = True
    for code, art in KNOWN.items():
        want = [sum(1 << i for i, c in enumerate(r) if c == "#") for r in art]
        if font[code] != want:
            print(f"FAIL: glyph 0x{code:02X} is "
                  f"{[hex(v) for v in font[code]]}, expected "
                  f"{[hex(v) for v in want]}")
            ok = False
    return ok


def make_cells():
    """Every character code and a wide spread of attributes, arranged so a row
    or column stride error cannot alias into a pass."""
    cells = bytearray(TEXT_COLS * TEXT_ROWS * 2)
    for r in range(TEXT_ROWS):
        for c in range(TEXT_COLS):
            ch = (r * TEXT_COLS + c) & 0xFF
            attr = (r * 29 + c * 11) & 0xFF      # includes the blink bit
            o = (r * TEXT_COLS + c) * 2
            cells[o] = ch
            cells[o + 1] = attr
    return bytes(cells)


def wait_flag(b, sym, want, what, tries=200):
    for _ in range(tries):
        if b.read_uint32(sym) == want:
            return True
        time.sleep(0.05)
    print(f"FAIL: {what}")
    return False


def compare(got, want, label):
    if got == want:
        return True
    for i, (gv, wv) in enumerate(zip(got, want)):
        if gv != wv:
            row, col = divmod(i, FB_STRIDE)
            print(f"FAIL: {label}: first mismatch at ({col},{row}) "
                  f"got {gv} expected {wv}")
            break
    print(f"  {sum(1 for a, bb in zip(got, want) if a != bb)} of {len(want)} "
          f"bytes differ")
    return False


def main(args):
    font = textmode.load_font()
    if not check_font(font):
        return False
    print(f"font: {len(font)} CP437 glyphs, known shapes exact")

    runner = build_and_create_runner(args)
    try:
        runner.start()
        b = runner.backend
        syms = get_symbols(getattr(args, "elf", None) or "test_core.elf")
        for n in ("run_video_flag", "cga_test_vram", "cga_test_out",
                  "cga_test_palette", "text_mode_arg", "text_scroll_arg",
                  "x86_video_pan"):
            if n not in syms:
                print(f"missing symbol {n}")
                return False
        time.sleep(0.5)

        b.write_uint32(syms["run_video_flag"], 1)
        if not wait_flag(b, syms["run_video_flag"], 2, "no video init"):
            return False

        # Mode 3. This clears the buffer and reloads the CLUT, so it must
        # happen before the pattern is written.
        b.write_uint32(syms["text_mode_arg"], 3)
        b.write_uint32(syms["run_video_flag"], 4)
        if not wait_flag(b, syms["run_video_flag"], 0, "mode 3 not selected"):
            return False

        clut = [b.read_uint32(syms["cga_test_palette"] + 4 * i)
                for i in range(16)]
        if clut != CGA_ARGB:
            print("FAIL: text-mode CLUT is not the 16 CGA colours")
            for i, (g, w) in enumerate(zip(clut, CGA_ARGB)):
                if g != w:
                    print(f"  entry {i}: {g:08x} expected {w:08x}")
            return False
        print("CLUT: 16 CGA colours exact")

        cells = make_cells()
        ok = True
        for left in (0, TEXT_COLS - VIS_COLS):
            b.write_memory(syms["cga_test_vram"], cells)
            b.write_uint32(syms["x86_video_pan"], left)
            b.write_uint32(syms["run_video_flag"], 3)
            if not wait_flag(b, syms["run_video_flag"], 0, "blit stalled"):
                return False
            got = bytes(b.read_memory(syms["cga_test_out"], FB_STRIDE * 240))
            if compare(got, textmode.render(cells, font, left),
                       f"pan={left}"):
                print(f"PASS: pan={left}: "
                      f"{VIS_COLS * TEXT_ROWS} cells pixel-exact")
            else:
                ok = False

        # Scrolling, checked on the buffer rather than the picture: it is a
        # memmove plus a fill and the picture would only obscure which.
        for lines, attr in ((1, 0x07), (3, 0x1E), (TEXT_ROWS, 0x4F)):
            b.write_memory(syms["cga_test_vram"], cells)
            b.write_uint32(syms["text_scroll_arg"], (attr << 8) | lines)
            b.write_uint32(syms["run_video_flag"], 5)
            if not wait_flag(b, syms["run_video_flag"], 0, "scroll stalled"):
                return False
            got = bytes(b.read_memory(syms["cga_test_vram"],
                                      TEXT_COLS * TEXT_ROWS * 2))
            want = textmode.scrolled(cells, lines, attr)
            if got == want:
                print(f"PASS: scroll {lines} line(s), attr 0x{attr:02X}")
            else:
                bad = next(i for i in range(len(want)) if got[i] != want[i])
                r, rem = divmod(bad // 2, TEXT_COLS)
                print(f"FAIL: scroll {lines}: first mismatch at row {r} "
                      f"col {rem} ({'char' if bad % 2 == 0 else 'attr'}) "
                      f"got 0x{got[bad]:02X} expected 0x{want[bad]:02X}")
                ok = False

        b.write_uint32(syms["x86_video_pan"], 0xFFFFFFFF)
        return ok
    finally:
        runner.stop()


if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser(description=__doc__)
    add_runner_args(p)
    args = p.parse_args()
    sys.exit(0 if main(args) else 1)
