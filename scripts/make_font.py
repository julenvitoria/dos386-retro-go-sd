#!/usr/bin/env python3
"""
Regenerate `font8x8.h` as a full 256-glyph CP437 table.

Why this exists: the renderer indexes the font with the raw character byte from
a text cell, and DOS text is CP437 -- box drawing at 0xB3-0xDA, shading at
0xB0-0xB2, blocks at 0xDB-0xDF. The tree previously carried only the 128 ASCII
glyphs of Daniel Hepper's public-domain `font8x8`, so `text_render` masked the
byte with 0x7F and every frame, shade and rule in a full-screen UI came out as
an unrelated lowercase letter. That is what made TOPBENCH's UI unreadable.

The table is assembled from Hepper's own blocks, which are indexed by Unicode:

    basic      U+0000-U+007F      ASCII
    ext_latin  U+00A0-U+00FF      accented latin, degree, plus-minus, ...
    greek      U+0390-U+03C9      alpha, beta, pi, sigma, Omega, ...
    box        U+2500-U+257F      single and double line drawing
    block      U+2580-U+259F      shades and half blocks
    misc       (two codepoints)   florin, pesetas

Python's `cp437` codec supplies the byte -> Unicode mapping for 0x20-0xFF. The
low 32 codes are control characters to the codec but graphics to a CGA, so they
are mapped explicitly here, and the 42 glyphs no Hepper block covers (the card
suits, arrows, and the maths operators at 0xEC-0xFE) are drawn below.

Bit order: the renderer takes the LEFTMOST pixel from bit 0, matching Hepper's
files. The hand-drawn glyphs are written most-significant-column-first as ASCII
art and reversed on emit, so what is written here reads the way it looks.

    python3 scripts/make_font.py                  # rewrite font8x8.h
    python3 scripts/make_font.py --check          # fail if it is out of date
    python3 scripts/make_font.py --cache build/fontwork
"""

import argparse
import os
import re
import sys
import urllib.request

BASE = "https://raw.githubusercontent.com/dhepper/font8x8/master"
BLOCKS = [
    ("font8x8_basic.h", 0x0000),
    ("font8x8_ext_latin.h", 0x00A0),
    ("font8x8_greek.h", 0x0390),
    ("font8x8_box.h", 0x2500),
    ("font8x8_block.h", 0x2580),
]
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "font8x8.h")

# CP437's low 32 codes, which the Python codec calls control characters.
CP437_LOW = ("\x00☺☻♥♦♣♠•◘○◙"
             "♂♀♪♫☼►◄↕‼¶"
             "§▬↨↑↓→←∟↔▲"
             "▼")

# Glyphs no Hepper block provides, drawn leftmost column first.
DRAWN = {
    0x263A: ".######.|#......#|#.#..#.#|#......#|#.####.#|#..##..#|#......#|.######.",
    0x263B: ".######.|########|##.##.##|########|##....##|###..###|########|.######.",
    0x2665: ".##.##..|#######.|#######.|#######.|.#####..|..###...|...#....|........",
    0x2666: "...#....|..###...|.#####..|#######.|.#####..|..###...|...#....|........",
    0x2663: "..###...|.#####..|..###...|#######.|#######.|...#....|...#....|..###...",
    0x2660: "...#....|..###...|.#####..|#######.|#######.|.#####..|...#....|..###...",
    0x2022: "........|........|...##...|..####..|..####..|...##...|........|........",
    0x25D8: "########|########|###..###|##....##|##....##|###..###|########|########",
    0x25CB: "........|..####..|.##..##.|.#....#.|.#....#.|.##..##.|..####..|........",
    0x25D9: "########|##....##|#..##..#|#.####.#|#.####.#|#..##..#|##....##|########",
    0x2642: "....####|.....###|....####|.#####.#|##..##..|##..##..|##..##..|.####...",
    0x2640: "..####..|.##..##.|.##..##.|.##..##.|..####..|...##...|.######.|...##...",
    0x266A: "..######|..##..##|..######|..##....|..##....|.###....|####....|###.....",
    0x266B: ".#######|.##...##|.#######|.##...##|.##...##|.##..###|###..##.|##......",
    0x263C: "#..##..#|.#.##.#.|..####..|###..###|###..###|..####..|.#.##.#.|#..##..#",
    0x25BA: "#.......|###.....|#####...|#######.|#####...|###.....|#.......|........",
    0x25C4: "......#.|.....###|..#####.|#######.|..#####.|.....###|......#.|........",
    0x2195: "...##...|..####..|.######.|...##...|...##...|.######.|..####..|...##...",
    0x203C: ".##..##.|.##..##.|.##..##.|.##..##.|.##..##.|........|.##..##.|........",
    0x25AC: "........|........|........|........|.######.|.######.|.######.|........",
    0x21A8: "...##...|..####..|.######.|...##...|.######.|..####..|...##...|########",
    0x2191: "...##...|..####..|.######.|...##...|...##...|...##...|...##...|........",
    0x2193: "...##...|...##...|...##...|...##...|.######.|..####..|...##...|........",
    0x2192: "........|...##...|....##..|#######.|....##..|...##...|........|........",
    0x2190: "........|..##....|.##.....|#######.|.##.....|..##....|........|........",
    0x221F: "........|........|##......|##......|##......|#######.|........|........",
    0x2194: "........|..#..#..|.##..##.|########|.##..##.|..#..#..|........|........",
    0x25B2: "........|...##...|..####..|.######.|########|########|........|........",
    0x25BC: "........|########|########|.######.|..####..|...##...|........|........",
    0x2302: "........|...#....|..###...|.##.##..|##...##.|##...##.|#######.|........",
    0x221E: "........|........|.###.##.|##.###..|##..#...|##.###..|.###.##.|........",
    0x2229: "..###...|.##.##..|##...##.|##...##.|##...##.|##...##.|........|........",
    0x2261: "........|#######.|........|#######.|........|#######.|........|........",
    0x2265: "##......|.##.....|..##....|...##...|..##....|.##.....|##......|#######.",
    0x2264: ".....##.|....##..|...##...|..##....|...##...|....##..|.....##.|#######.",
    0x2320: "....###.|...##.##|...##...|...##...|...##...|...##...|...##...|...##...",
    0x2321: "...##...|...##...|...##...|...##...|...##...|...##...|##.##...|.###....",
    0x2248: "........|.###.##.|##.###..|........|.###.##.|##.###..|........|........",
    0x2219: "........|........|........|...##...|...##...|........|........|........",
    0x221A: "....####|....##..|....##..|....##..|###.##..|.##.##..|..####..|...###..",
    0x207F: ".#####..|.##..##.|.##..##.|.##..##.|........|........|........|........",
    0x25A0: "........|........|..####..|..####..|..####..|..####..|........|........",
}


def drawn_rows(art):
    """ASCII art (leftmost column first) -> 8 bytes with bit 0 = leftmost."""
    rows = art.split("|")
    assert len(rows) == 8, art
    out = []
    for r in rows:
        assert len(r) == 8, r
        out.append(sum(1 << i for i, c in enumerate(r) if c == "#"))
    return out


def fetch(name, cache):
    path = os.path.join(cache, name)
    if not os.path.exists(path):
        os.makedirs(cache, exist_ok=True)
        with urllib.request.urlopen(f"{BASE}/{name}", timeout=30) as r:
            open(path, "wb").write(r.read())
    return open(path).read()


def parse_block(text, first):
    """dhepper's blocks are contiguous arrays starting at `first`."""
    out, n = {}, 0
    for body in re.findall(r"\{([^}]*)\}", text):
        vals = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
        if len(vals) != 8:
            continue
        out[first + n] = vals
        n += 1
    return out


def build(cache):
    glyphs = {}
    for name, first in BLOCKS:
        glyphs.update(parse_block(fetch(name, cache), first))
    misc = fetch("font8x8_misc.h", cache)
    cps = [int(m, 16) for m in re.findall(r"U\+([0-9A-F]{4})", misc)]
    rows = [[int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", b)]
            for b in re.findall(r"\{([^}]*)\}", misc)]
    for cp, row in zip(cps, rows):
        if len(row) == 8:
            glyphs[cp] = row
    for cp, art in DRAWN.items():
        glyphs[cp] = drawn_rows(art)

    table, names = [], []
    for byte in range(256):
        ch = CP437_LOW[byte] if byte < 0x20 else \
            ("⌂" if byte == 0x7F else bytes([byte]).decode("cp437"))
        cp = ord(ch)
        if cp not in glyphs:
            raise SystemExit(f"no glyph for 0x{byte:02X} U+{cp:04X}")
        table.append(glyphs[cp])
        names.append(f"U+{cp:04X}")
    return table, names


HEADER = '''/*
 * 8x8 monochrome bitmap font, CP437 -- all 256 codes, not just ASCII.
 *
 * GENERATED by scripts/make_font.py. Do not edit by hand.
 *
 * Glyph data from font8x8 by Daniel Hepper <daniel@hepper.net>, released into
 * the Public Domain (https://github.com/dhepper/font8x8), reindexed from
 * Unicode to CP437. The 42 codes no font8x8 block covers -- the card suits and
 * arrows below 0x20, and the maths operators at 0xEC-0xFE -- are drawn in the
 * generator and are likewise unencumbered.
 *
 * Indexed by the raw character byte of a text cell. DOS text is CP437: masking
 * to 7 bits turns every box-drawing rule and shading block in a full-screen UI
 * into an unrelated letter, which is exactly how this shipped broken.
 *
 * The leftmost pixel of a row is bit 0.
 */
#ifndef X86_FONT8X8_H
#define X86_FONT8X8_H

static const unsigned char font8x8_basic[256][8] = {
'''


def render(table, names):
    lines = [HEADER]
    for i, (rows, nm) in enumerate(zip(table, names)):
        body = ", ".join(f"0x{v:02X}" for v in rows)
        lines.append(f"    {{{body}}},   /* 0x{i:02X} {nm} */\n")
    lines.append("};\n\n#endif\n")
    return "".join(lines)


def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cache", default="build/fontwork",
                   help="where the upstream font8x8_*.h blocks are cached")
    p.add_argument("--out", default=os.path.normpath(OUT))
    p.add_argument("--check", action="store_true",
                   help="exit non-zero if the file on disk is not what this "
                        "generator would produce")
    a = p.parse_args()

    text = render(*build(a.cache))
    if a.check:
        cur = open(a.out).read() if os.path.exists(a.out) else ""
        if cur != text:
            print(f"{a.out} is out of date; run scripts/make_font.py")
            return 1
        print(f"{a.out} up to date (256 glyphs)")
        return 0
    open(a.out, "w").write(text)
    print(f"wrote {a.out}: 256 CP437 glyphs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
