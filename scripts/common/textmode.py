"""
Host-side model of the CGA text renderer.

`x86_video.c`'s text path turns an 80x25 buffer of (character, attribute) cells
into 8-bit palette indices. This recomputes the same thing in Python so a test
can assert the exact pixels the panel is being sent, rather than eyeballing 8x8
glyphs -- which is how a renderer that masked the character byte to 7 bits, and
so drew every box-drawing and shading code as a letter, went unnoticed.

Used by `tests/test_text.py` (tier 1, against a seeded pattern) and
`tests/test_screen.py` (tier 2, against whatever the real guest has drawn).
"""

import re

TEXT_COLS, TEXT_ROWS = 80, 25
VIS_COLS = 40                   # 320 px panel / 8 px cell
CELL_W = CELL_H = 8
CGA_HEIGHT = 200

# The 16 CGA colours in hardware index order, ARGB8888. Text attributes index
# all of them, which is why the CLUT pushed to the LCD must be 16 entries and
# not the four that mode 4 needs.
CGA_ARGB = [
    0xFF000000, 0xFF0000AA, 0xFF00AA00, 0xFF00AAAA,
    0xFFAA0000, 0xFFAA00AA, 0xFFAA5500, 0xFFAAAAAA,
    0xFF555555, 0xFF5555FF, 0xFF55FF55, 0xFF55FFFF,
    0xFFFF5555, 0xFFFF55FF, 0xFFFFFF55, 0xFFFFFFFF,
]


def load_font(path="font8x8.h"):
    """The generated CP437 table: 256 glyphs of 8 rows, leftmost pixel bit 0."""
    txt = open(path).read()
    rows = [[int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
            for body in re.findall(r"\{([^}]*)\}", txt)]
    return [r for r in rows if len(r) == 8]


def render(cells, font, left, stride=320, height=240):
    """The 8-bit palette-index image `text_render` should produce.

    `left` is the leftmost visible column: only 40 of the 80 columns fit a
    320 px panel, and the window slides. The picture is centred vertically in
    the panel, so there is a blank band above and below.
    """
    top = ((height - CGA_HEIGHT) // 2) * stride
    img = bytearray(stride * height)
    for r in range(TEXT_ROWS):
        for vc in range(VIS_COLS):
            col = left + vc
            if col >= TEXT_COLS:
                ch, attr = 0x20, 0x07
            else:
                o = (r * TEXT_COLS + col) * 2
                ch, attr = cells[o], cells[o + 1]
            fg = attr & 0x0F
            bg = (attr >> 4) & 0x07         # bit 7 is blink, not a colour
            glyph = font[ch]
            for gy in range(CELL_H):
                bits = glyph[gy]
                base = top + (r * CELL_H + gy) * stride + vc * CELL_W
                for gx in range(CELL_W):
                    img[base + gx] = fg if bits & (1 << gx) else bg
    return bytes(img)


def scrolled(cells, lines, attr):
    """INT 10h AH=06: shift up by `lines`, fill the vacated rows with spaces."""
    if lines == 0 or lines >= TEXT_ROWS:
        return bytes(bytearray([0x20, attr] * (TEXT_COLS * TEXT_ROWS)))
    out = bytearray(cells)
    keep = (TEXT_ROWS - lines) * TEXT_COLS * 2
    out[0:keep] = cells[lines * TEXT_COLS * 2:]
    out[keep:] = bytearray([0x20, attr] * (lines * TEXT_COLS))
    return bytes(out)


def dump(cells, cols=TEXT_COLS, rows=TEXT_ROWS):
    """The buffer as readable CP437 lines, blank rows dropped. Decoding as
    CP437 rather than ASCII matters: a DOS UI is mostly box drawing, and
    printing only 0x20-0x7E hides all of it."""
    out = []
    for r in range(rows):
        line = "".join(
            "." if cells[(r * cols + c) * 2] in (0x00, 0x20)
            else bytes([cells[(r * cols + c) * 2]]).decode("cp437")
            for c in range(cols))
        if line.strip("."):
            out.append((r, line))
    return out
