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
    # 0x11 (640x480x2 VGA) is the stand-in now that 13h IS rendered. It has to
    # be a mode nothing in vmodes[] claims, or this arm silently tests nothing.
    r = guest_set_mode(b, syms, 0x11)
    if r is None:
        return False
    if r[0] != 0x00:
        print(f"FAIL: unsupported mode 11h was adopted ({r[0]:#x}); the "
              f"previous mode must stay up instead")
        return False
    print("PASS: mode 11h refused, mode 0 stays up (nothing renders it)")

    if not refusal_is_side_effect_free(b, syms):
        return False

    if not vga_reg_file_checks(b, syms):
        return False
    if not vga13_checks(b, syms):
        return False
    return ok


def read_mode_log(b, syms):
    """The mode ring, oldest-to-newest, as (tag, aux, value) triples."""
    idx = b.read_uint32(syms["x86_video_mode_log_idx"])
    raw = b.read_memory(syms["x86_video_mode_log"], 16 * 4)
    words = [int.from_bytes(raw[i * 4:i * 4 + 4], "little") for i in range(16)]
    n = min(idx, 16)
    order = [(idx - n + i) & 15 for i in range(n)]
    return [((words[o] >> 24) & 0xFF, (words[o] >> 8) & 0xFFFF, words[o] & 0xFF)
            for o in order]


def refusal_is_side_effect_free(b, syms):
    """A REFUSED mode set must leave the previous screen BYTE-IDENTICAL.

    The whole point of refusing a mode we cannot render is that the previous
    one stays up -- docs/08 states it and x86_bios.c's AH=1Ah comment leans on
    it. That promise is only worth what it is tested for: a refusal that
    cleared VRAM on the way out would leave the same picture a renderer bug
    does (2000 cells of 0x0720), and nothing else in this file would notice.

    Mode 0Dh is the case that matters -- it is what Commander Keen 4 asks for
    -- and it must come back as tag 5 in the mode ring AND leave both the
    guest's VRAM and the rendered frame exactly as they were.
    """
    for n in ("x86_video_mode_log", "x86_video_mode_log_idx", "cga_test_vram"):
        if n not in syms:
            print(f"missing symbol {n} -- rebuild tests/main.c")
            return False

    r = guest_set_mode(b, syms, 3)
    if r is None or r[0] != 3:
        print("FAIL: could not get back to text mode 3 for the refusal check")
        return False

    cells = bytearray()
    for i in range(80 * 25):
        cells += bytes([0x21 + (i % 90), 0x5D if (i % 7) else 0x5F])
    b.write_memory(syms["cga_test_vram"], bytes(cells))
    before_vram = bytes(b.read_memory(syms["cga_test_vram"], 80 * 25 * 2))
    before_frame = render(b, syms)
    if before_frame is None:
        return False

    r = guest_set_mode(b, syms, 0x0D)
    if r is None:
        return False
    if r[0] != 3 or r[1] != 1 or r[3] != 3:
        print(f"FAIL: mode 0Dh was not refused cleanly (mode {r[0]:#x} "
              f"is_text={r[1]} bda_mode={r[3]:#x})")
        return False
    log = read_mode_log(b, syms)
    if not any(tag == 5 and aux == 0x0D and value == 0x03 for tag, aux, value
               in log):
        print(f"FAIL: no tag-5 refusal of 0Dh in the mode ring: {log}")
        return False

    after_vram = bytes(b.read_memory(syms["cga_test_vram"], 80 * 25 * 2))
    after_frame = render(b, syms)
    if after_frame is None:
        return False
    if after_vram != before_vram:
        diff = sum(1 for g, w in zip(after_vram, before_vram) if g != w)
        blanked = after_vram[:4] == bytes([0x20, 0x07, 0x20, 0x07])
        extra = ("  -- VRAM holds 0x0720 spaces, i.e. the refusal ran the "
                 "mode-set CLEAR" if blanked else "")
        print(f"FAIL: a REFUSED mode set changed VRAM: {diff} of "
              f"{len(before_vram)} bytes{extra}")
        return False
    if after_frame != before_frame:
        print("FAIL: a REFUSED mode set changed the rendered frame")
        return False

    # Negative control. The check above can only be trusted if it FAILS on the
    # damage it claims to detect, so do that damage deliberately -- the exact
    # 0x0720 fill x86_video_set_mode() writes -- and require the comparison to
    # see it. A test that passes on a wiped screen is not a test.
    b.write_memory(syms["cga_test_vram"], bytes([0x20, 0x07]) * (80 * 25))
    poisoned = bytes(b.read_memory(syms["cga_test_vram"], 80 * 25 * 2))
    if poisoned == before_vram:
        print("FAIL: negative control -- a fully blanked VRAM compares EQUAL "
              "to the drawn screen, so the refusal check cannot fail")
        return False
    b.write_memory(syms["cga_test_vram"], before_vram)
    print("PASS: mode 0Dh refused (tag 5), previous text screen byte-identical "
          "(negative control: a 0x0720 wipe is detected)")
    return True


def expected_vga13(aperture):
    """Mode 13h is an IDENTITY copy: the guest's 320x200x8bpp aperture already
    IS the panel's LUT8 format. Computed here anyway rather than asserted as
    'the same bytes', because the letterbox offset and the stride are exactly
    what a wrong blit gets wrong."""
    img = bytearray(FB_STRIDE * 240)
    for y in range(200):
        img[FB_TOP + y * FB_STRIDE : FB_TOP + y * FB_STRIDE + 320] = \
            aperture[y * 320 : y * 320 + 320]
    return bytes(img)


def vga_reg_file_checks(b, syms):
    """Write-then-read-back on the CRTC/Sequencer/GC/Attribute register files.

    This is how a DOS program finds its display, and it is the probe WOLF3D
    failed: with no register file every readback was 0xFF, its detection
    concluded nothing was there, and the engine walked off into its own heap
    and halted on 0xFF filler -- three subsystems away from anything that
    looked like video. Measured with `probe.py --io-log`.
    """
    if "vga_regs_out" not in syms:
        print("missing symbol vga_regs_out -- rebuild tests/main.c")
        return False
    b.write_uint32(syms["run_video_flag"], 8)
    if not wait_flag(b, syms["run_video_flag"], 0, "VGA register-file probe"):
        return False
    got = [b.read_uint32(syms["vga_regs_out"] + 4 * i) for i in range(9)]
    want = [0x66, 0xFF, 0x06, 0x40, 0x41, 0x04, 0x66, 0x66, 0xFF]
    names = ["CRTC 0x0F via 0x3D5",
             "the 0x3B5 MONO alias while Misc Output selects colour "
             "(must be silent: answering both is what made WOLF3D poll a "
             "monochrome retrace that never comes)",
             "Sequencer 0x04", "Graphics Controller 0x05",
             "Attribute 0x10 (flip-flop resynced by 0x3DA)",
             "the Sequencer index latch itself",
             "the mono alias AFTER clearing Misc Output bit 0",
             "the colour address, same register file",
             "the mono alias once Misc Output selects colour again"]
    ok = True
    for g, w, n in zip(got, want, names):
        if g != w:
            print(f"FAIL: {n} read back {g:#04x}, expected {w:#04x}"
                  + (" -- 0xff is the no-device default, i.e. the register "
                     "file is missing" if g == 0xFF else ""))
            ok = False
    if ok:
        print("PASS: CRTC/Sequencer/GC/Attribute register files read back "
              "what was written, on both the colour and mono CRTC addresses")
    return ok


def vga13_checks(b, syms):
    """VGA mode 13h: 320x200x256 linear at 0xA0000, and its 256-entry DAC.

    Two things are checked and they fail differently. The BLIT is pixel-exact
    against a host-computed image, so a stride or letterbox error shows up as a
    slant or an offset. The PALETTE is compared against a host expansion of the
    same DAC bytes -- and it is streamed through the PORTS (0x3C8 then 768
    writes to 0x3C9), because a DAC modelled as a latch loses 767 of those 768
    and the whole screen comes out one colour. That failure is invisible in a
    blit check: the pixel INDICES would still be perfect.
    """
    for n in ("vga_test_aperture", "vga_dac_arg", "vga_clut_out",
              "vga_dac_readback", "x86_vga13_ok"):
        if n not in syms:
            print(f"FAIL: harness has no {n}; mode 13h arm cannot run")
            return False
    if b.read_uint32(syms["x86_vga13_ok"]) != 1:
        print("FAIL: x86_vga13_ok is 0 -- the aperture was never handed over, "
              "so 13h would be refused rather than rendered")
        return False

    r = guest_set_mode(b, syms, 0x13)
    if r is None:
        return False
    # (mode, is_text, cols, bda_mode, bda_cols)
    if r[0] != 0x13:
        print(f"FAIL: mode 13h not adopted (renderer says {r[0]:#x})")
        return False
    if r[1] != 0:
        print("FAIL: mode 13h reported as a TEXT mode")
        return False
    if r[3] != 0x13 or r[4] != 40:
        print(f"FAIL: BDA disagrees with the renderer: 0x449={r[3]:#x} "
              f"0x44A={r[4]} (want 0x13 / 40). The BDA is re-read every frame, "
              f"so a disagreement here undoes the mode set on the next one")
        return False
    print("PASS: mode 13h adopted, BDA 0x449=0x13 0x44A=40")

    # ---- the DAC, streamed through the ports ------------------------------
    # Index readable off the colour: R=i>>2, G=i&0x3F, B=63-(i>>2). No two
    # indices share a colour, so a wrong entry names itself.
    dac = bytearray(768)
    for i in range(256):
        dac[i * 3 + 0] = i >> 2
        dac[i * 3 + 1] = i & 0x3F
        dac[i * 3 + 2] = 63 - (i >> 2)
    b.write_memory(syms["vga_dac_arg"], bytes(dac))
    b.write_uint32(syms["run_video_flag"], 6)
    if not wait_flag(b, syms["run_video_flag"], 0, "DAC stream hung"):
        return False

    # Read-back: a fade loop read-modify-writes the DAC and must get its own
    # 6-bit values back, not the 8-bit expansion.
    got_rb = [b.read_uint32(syms["vga_dac_readback"] + 4 * i) for i in range(6)]
    if got_rb != [dac[i] for i in range(6)]:
        print(f"FAIL: DAC read-back {got_rb} != written {[dac[i] for i in range(6)]}"
              " -- a read-modify-write fade loop would corrupt the palette")
        return False
    print("PASS: DAC read-back exact through 0x3C7/0x3C9 (6-bit, as written)")

    b.write_uint32(syms["run_video_flag"], 7)
    if not wait_flag(b, syms["run_video_flag"], 0, "CLUT snapshot hung"):
        return False
    raw = bytes(b.read_memory(syms["vga_clut_out"], 256 * 4))
    got_clut = [int.from_bytes(raw[i * 4:i * 4 + 4], "little") for i in range(256)]

    def expand(v):
        # 63 must map to 255, not 252: a plain << 2 leaves every white 1.2%
        # grey and every fade-to-white short of it.
        return ((v << 2) | (v >> 4)) & 0xFF

    want_clut = [(expand(dac[i * 3]) << 16) | (expand(dac[i * 3 + 1]) << 8)
                 | expand(dac[i * 3 + 2]) for i in range(256)]
    bad = [i for i in range(256) if got_clut[i] != want_clut[i]]
    if bad:
        i = bad[0]
        print(f"FAIL: CLUT entry {i} is {got_clut[i]:#08x}, want {want_clut[i]:#08x}"
              f" ({len(bad)} of 256 wrong)")
        if len(bad) > 200:
            print("  nearly all wrong -- the classic shape of a DAC modelled "
                  "as a port LATCH, which keeps only the last of 768 bytes")
        return False
    print("PASS: 256 CLUT entries exact (6->8 bit as (v<<2)|(v>>4))")

    # ---- the write CURSOR: phase, wrap, and a negative control ------------
    #
    # 0x3C9 is a STREAM against an auto-incrementing byte cursor, and the whole
    # palette's meaning rests on that cursor's PHASE. Shift it by one byte and
    # every entry after it has r,g,b ROTATED -- red becomes blue -- while the
    # bytes themselves stay perfectly valid 6-bit values and every pixel INDEX
    # stays correct. That is the one palette failure a blit check, a read-back
    # check and a byte-count check all pass through.
    #
    # Wolf3D is why the wrap arm exists: VL_SetPalette is `mov cx,768 /
    # rep outsb`, and it runs TWICE with only one 0x3C8 between them, so the
    # second palette is positioned by the cursor wrapping at 768. If the wrap
    # landed anywhere but a multiple of 3, the SECOND palette would be rotated
    # and the first would not -- which reads as a defect that comes and goes.
    def stream_with_prewrite(n):
        b.write_uint32(syms["vga_dac_prewrite"], n)
        b.write_uint32(syms["run_video_flag"], 9)
        if not wait_flag(b, syms["run_video_flag"], 0, "DAC prewrite stream hung"):
            return None
        b.write_uint32(syms["run_video_flag"], 7)
        if not wait_flag(b, syms["run_video_flag"], 0, "CLUT snapshot hung"):
            return None
        raw2 = bytes(b.read_memory(syms["vga_clut_out"], 256 * 4))
        return [int.from_bytes(raw2[i * 4:i * 4 + 4], "little") for i in range(256)]

    if "vga_dac_prewrite" not in syms:
        print("FAIL: harness has no vga_dac_prewrite; the cursor arm cannot run")
        return False

    # 768 filler bytes: the cursor must wrap to exactly 0, so the palette lands
    # byte-for-byte where a fresh 0x3C8 would have put it.
    wrapped = stream_with_prewrite(768)
    if wrapped is None:
        return False
    if wrapped != want_clut:
        i = next(i for i in range(256) if wrapped[i] != want_clut[i])
        print(f"FAIL: after a 768-byte wrap the palette is mis-phased -- entry "
              f"{i} is {wrapped[i]:#08x}, want {want_clut[i]:#08x}. Wolf3D "
              f"streams two palettes back to back; the second would render in "
              f"rotated channels (red as blue)")
        return False
    print("PASS: the 768-byte write cursor wraps to phase 0 (two back-to-back "
          "palettes stay aligned)")

    # NEGATIVE CONTROL. One filler byte is a deliberate one-byte phase error;
    # the check above MUST reject it, or it is not checking anything.
    shifted = stream_with_prewrite(1)
    if shifted is None:
        return False
    if shifted == want_clut:
        print("FAIL: a deliberate ONE-BYTE cursor shift produced an identical "
              "CLUT -- the palette check cannot detect a channel rotation and "
              "therefore proves nothing")
        return False
    # Name the rotation rather than just "differs": a one-byte shift puts
    # (b[i-1], r[i], g[i]) in entry i, so red really does arrive as green and
    # the previous entry's blue as red. Asserting the exact shape stops the
    # control passing for some unrelated reason (a hung arm, a zeroed CLUT).
    def ch(v, s):
        return (v >> s) & 0xFF
    rot = [(ch(want_clut[(i - 1) % 256], 0) << 16)
           | (ch(want_clut[i], 16) << 8) | ch(want_clut[i], 8)
           for i in range(1, 256)]
    if shifted[1:] != rot:
        print(f"FAIL: the one-byte shift did change the CLUT, but not into the "
              f"expected channel rotation -- entry 1 is {shifted[1]:#08x}, a "
              f"rotation would give {rot[0]:#08x}. The control is not "
              f"exercising the cursor")
        return False
    print("PASS: negative control -- a one-byte cursor shift rotates every "
          "entry to (b[i-1], r[i], g[i]) and the CLUT check rejects it")

    # Put the correctly phased palette back for the arms below.
    if stream_with_prewrite(768) is None:
        return False

    # ---- the blit ---------------------------------------------------------
    # Vertical bars over the top half, diagonals over the bottom. A bar is
    # constant down a column, so a stride error slants it; a diagonal is
    # constant ALONG a diagonal, so the same error changes its slope instead.
    # One wrong stride cannot satisfy both.
    ap = bytearray(320 * 200)
    for y in range(200):
        for x in range(320):
            ap[y * 320 + x] = (x & 0xFF) if y < 100 else ((x + y) & 0xFF)
    b.write_memory(syms["vga_test_aperture"], bytes(ap))
    got = render(b, syms)
    if got is None:
        return False
    want = expected_vga13(bytes(ap))
    if got != want:
        for i, (g, w) in enumerate(zip(got, want)):
            if g != w:
                row, col = divmod(i, FB_STRIDE)
                print(f"FAIL: mode 13h first mismatch at ({col},{row}) "
                      f"got {g} expected {w}")
                break
        diff = sum(1 for g, w in zip(got, want) if g != w)
        print(f"  {diff} of {len(want)} bytes differ")
        return False
    print(f"PASS: mode 13h {320 * 200} pixels exact (bars + diagonals)")

    # The letterbox must be untouched black -- 200 rows into a 240-row panel.
    if any(got[:FB_TOP]) or any(got[FB_TOP + 200 * FB_STRIDE:]):
        print("FAIL: mode 13h wrote outside rows 20..219 (the letterbox bars "
              "are the OSK's surface and are not the guest's to touch)")
        return False
    print("PASS: letterbox bars untouched")

    # Leaving 13h must hand the 32-entry table back, or every later mode draws
    # through a palette nothing maintains.
    r = guest_set_mode(b, syms, 0x03)
    if r is None or r[0] != 0x03:
        print("FAIL: could not get back out of mode 13h into text mode 3")
        return False
    b.write_uint32(syms["run_video_flag"], 7)
    if not wait_flag(b, syms["run_video_flag"], 0, "CLUT snapshot hung"):
        return False
    raw = bytes(b.read_memory(syms["vga_clut_out"], 4))
    if int.from_bytes(raw, "little") != 0xDEADBEEF:
        print("FAIL: x86_video_clut256() still returns a palette outside a "
              "256-colour mode; the firmware would keep the core-owned table "
              "and retro-go's overlay would draw through it")
        return False
    print("PASS: 256-entry CLUT withdrawn on leaving 13h")
    return True


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
