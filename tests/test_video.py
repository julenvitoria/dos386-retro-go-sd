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


# ===========================================================================
# UNCHAINED VGA ("Mode Y") -- mode 13h with the Sequencer's chain-4 bit clear.
#
# This is what Wolfenstein 3-D runs in, and reading it as a chained mode is what
# put four horizontally-squashed copies side by side. Pixel (x,y) lives in PLANE
# x&3 at PLANE OFFSET y*80 + (x>>2), so each plane holds 80 bytes per scanline
# rather than 320.
#
# Ported from references/8086tiny/test286/{tmodey.asm,runmodey.sh,modeycheck.py}.
# Read references/8086tiny/docs/video/15-mode-y.md section 6 before changing the
# pattern -- several of its choices look arbitrary and are each the residue of a
# mutation that survived.
#
# THE FOUR BANDS. Each has a different geometric signature, chosen so that no
# single wrong stride, wrong plane order or wrong start address can satisfy more
# than one of them:
#
#   rows   0..49   x & 0xFF        VERTICAL bars. Written PLANE AT A TIME (four
#                                  Map Mask writes, each painting every fourth
#                                  column), so a wrong plane-to-column mapping
#                                  reorders the bars and a stride error slants
#                                  them.
#   rows  50..99   (x+y) & 0xFF    DIAGONALS. A stride error changes the SLOPE
#                                  rather than destroying it, so it disagrees
#                                  with band A in a DIFFERENT direction.
#   rows 100..149  y & 0xFF        HORIZONTAL bars -- the exact transpose of
#                                  band A. Written with the Map Mask at 0x0F, so
#                                  one store paints four pixels: this is the
#                                  band that proves the Map Mask is not ignored.
#   rows 150..199  (y-50) & 0xFF   A LATCH COPY of band C through write mode 1.
#                                  Latch copies are how unchained software
#                                  actually moves video memory, and they are the
#                                  one read path that must stay EXACT in Mode Y,
#                                  where a data read returns the wrong plane by
#                                  design.
#
# THE PAGE FLIPS. Between bands C and D the display start moves to 0x4100 and
# then 0x8200 and is LEFT there, and band D is addressed THROUGH PAGE 2. Without
# that last part the write-side fold is never exercised -- the reference's
# mutation that deletes it SURVIVED until band D was moved up there.
#
# THE STRIDE IS 16,640 AND THE NUMBER IS LOAD-BEARING. At 16,384 an AND mask is
# the correct fold BY ACCIDENT, so the page-fold control cannot fail and the
# shipped bug is invisible to the test written to catch it. 80 x 208 is what
# real Mode Y software uses.
#
# Rows 175..199 are staged through the OFF-SCREEN CACHE at 0xC300 and blitted
# back down, because that region has separate storage and an untested path is
# not a path.
# ===========================================================================
PLANAR_CODE_BASE = 0x3000
MODEY_STRIDE = 16640            # 80 x 208, the id Galaxy engine's SCREENSIZE
MODEY_PAGE2 = 2 * MODEY_STRIDE  # 33280
MODEY_CACHE = 0xC300            # 3 x 16,640: FREESTART


def modey_program():
    """The guest program, assembled here so it sits next to its expectation.

    Hand-encoded rather than run through an assembler because the harness has no
    assembler in the loop; every instruction carries its mnemonic. Only backward
    jumps are used, so a label is always known by the time it is referenced.
    """
    c = bytearray()

    def emit(*b):
        c.extend(b)

    def back(op, target):
        """A backward rel8 branch. Fails loudly rather than silently wrapping if
        a body ever grows past the reach of a short jump."""
        emit(op)
        d = target - (len(c) + 1)
        assert -128 <= d <= -1, f"rel8 out of range: {d}"
        emit(d & 0xFF)

    # ---- mode 13h through the BIOS. INT 10h and not a host call because the
    # chain-4 arm is GATED on BDA 0040:0049 being 0x13, and only the BIOS mode
    # set publishes that -- a blind Sequencer probe must not be able to
    # reinterpret some other mode as planar.
    emit(0xB8, 0x13, 0x00)                  # mov ax,0013h
    emit(0xCD, 0x10)                        # int 10h

    # ---- unchain. Sequencer index 4 = 0x06 clears Chain-4 and leaves Extended
    # Memory and Odd/Even set as a real Mode Y setup does -- byte for byte what
    # Wolfenstein 3-D writes. A WORD `out dx,ax`, which is how real software
    # programs these registers and is where a byte-only port path loses AH.
    emit(0xBA, 0xC4, 0x03)                  # mov dx,3C4h
    emit(0xB8, 0x04, 0x06)                  # mov ax,0604h
    emit(0xEF)                              # out dx,ax
    emit(0xB8, 0x00, 0xA0)                  # mov ax,0A000h
    emit(0x8E, 0xC0)                        # mov es,ax
    emit(0xFC)                              # cld

    # ---- band A: rows 0..49, pixel = x & 0xFF, one plane at a time.
    emit(0x31, 0xF6)                        # xor si,si        ; plane 0..3
    emit(0xB3, 0x01)                        # mov bl,1         ; Map Mask
    a_plane = len(c)
    emit(0xBA, 0xC4, 0x03)                  # mov dx,3C4h
    emit(0xB0, 0x02)                        # mov al,2
    emit(0xEE)                              # out dx,al
    emit(0x42)                              # inc dx
    emit(0x88, 0xD8)                        # mov al,bl
    emit(0xEE)                              # out dx,al
    emit(0x31, 0xFF)                        # xor di,di        ; plane offset 0
    emit(0xBD, 0x32, 0x00)                  # mov bp,50        ; rows to go
    a_row = len(c)
    emit(0x89, 0xF0)                        # mov ax,si        ; column 0's value
    emit(0xB9, 0x50, 0x00)                  # mov cx,80        ; offsets per row
    a_col = len(c)
    emit(0xAA)                              # stosb
    emit(0x04, 0x04)                        # add al,4         ; four pixels on
    back(0xE2, a_col)                       # loop a_col
    emit(0x4D)                              # dec bp
    back(0x75, a_row)                       # jnz a_row
    emit(0x46)                              # inc si
    emit(0xD0, 0xE3)                        # shl bl,1
    emit(0x83, 0xFE, 0x04)                  # cmp si,4
    back(0x72, a_plane)                     # jb a_plane

    # ---- band B: rows 50..99, pixel = (x + y) & 0xFF.
    emit(0x31, 0xF6)                        # xor si,si
    emit(0xB3, 0x01)                        # mov bl,1
    b_plane = len(c)
    emit(0xBA, 0xC4, 0x03)                  # mov dx,3C4h
    emit(0xB0, 0x02)                        # mov al,2
    emit(0xEE)                              # out dx,al
    emit(0x42)                              # inc dx
    emit(0x88, 0xD8)                        # mov al,bl
    emit(0xEE)                              # out dx,al
    emit(0xBF, 0xA0, 0x0F)                  # mov di,4000      ; row 50
    emit(0xBD, 0x32, 0x00)                  # mov bp,50        ; bp doubles as y
    b_row = len(c)
    emit(0x89, 0xF0)                        # mov ax,si
    emit(0x01, 0xE8)                        # add ax,bp        ; plane + y
    emit(0xB9, 0x50, 0x00)                  # mov cx,80
    b_col = len(c)
    emit(0xAA)                              # stosb
    emit(0x04, 0x04)                        # add al,4
    back(0xE2, b_col)                       # loop b_col
    emit(0x45)                              # inc bp
    emit(0x83, 0xFD, 0x64)                  # cmp bp,100
    back(0x72, b_row)                       # jb b_row
    emit(0x46)                              # inc si
    emit(0xD0, 0xE3)                        # shl bl,1
    emit(0x83, 0xFE, 0x04)                  # cmp si,4
    back(0x72, b_plane)                     # jb b_plane

    # ---- band C: rows 100..149, pixel = y & 0xFF, Map Mask 0x0F.
    # Every store paints all four planes, i.e. four pixels of the same value.
    # Constant ALONG a row, so this band is the transpose of band A and no
    # single wrong stride satisfies both.
    emit(0xBA, 0xC4, 0x03)                  # mov dx,3C4h
    emit(0xB8, 0x02, 0x0F)                  # mov ax,0F02h
    emit(0xEF)                              # out dx,ax
    emit(0xBF, 0x40, 0x1F)                  # mov di,8000      ; row 100
    emit(0xBB, 0x64, 0x00)                  # mov bx,100       ; y
    c_row = len(c)
    emit(0x88, 0xD8)                        # mov al,bl
    emit(0xB9, 0x50, 0x00)                  # mov cx,80
    emit(0xF3, 0xAA)                        # rep stosb
    emit(0x43)                              # inc bx
    emit(0x81, 0xFB, 0x96, 0x00)            # cmp bx,150
    back(0x72, c_row)                       # jb c_row

    # ---- the page flips, to 0x4100 and then 0x8200, and LEFT there.
    # Their DIFFERENCE is the only place a guest ever states its page stride,
    # which is what the POSITIVE control (a deliberately wrong X86_MODEY_PAGE0)
    # makes the core learn. Word OUTs again.
    emit(0xBA, 0xD4, 0x03)                  # mov dx,3D4h
    emit(0xB8, 0x0C, 0x41)                  # mov ax,410Ch     ; start hi = 0x41
    emit(0xEF)                              # out dx,ax
    emit(0xB8, 0x0D, 0x00)                  # mov ax,000Dh     ; start lo = 0
    emit(0xEF)                              # out dx,ax
    emit(0xB8, 0x0C, 0x82)                  # mov ax,820Ch     ; start hi = 0x82
    emit(0xEF)                              # out dx,ax
    emit(0xB8, 0x0D, 0x00)                  # mov ax,000Dh
    emit(0xEF)                              # out dx,ax

    # ---- band D: rows 150..199, a LATCH COPY of band C through write mode 1.
    # The READ is what loads the latches, and in Mode Y that read is the only
    # exact one there is -- the byte it returns in AL deliberately is not.
    emit(0xBA, 0xCE, 0x03)                  # mov dx,3CEh
    emit(0xB8, 0x05, 0x01)                  # mov ax,0105h     ; write mode 1
    emit(0xEF)                              # out dx,ax

    def latch_copy(src, dst, count):
        emit(0xBE, src & 0xFF, src >> 8)      # mov si,src
        emit(0xBF, dst & 0xFF, dst >> 8)      # mov di,dst
        emit(0xB9, count & 0xFF, count >> 8)  # mov cx,count
        top = len(c)
        emit(0x26, 0x8A, 0x04)              # mov al,[es:si]  ; loads the latches
        emit(0x26, 0x88, 0x05)              # mov [es:di],al  ; latches go back
        emit(0x46)                          # inc si
        emit(0x47)                          # inc di
        back(0xE2, top)                     # loop

    # Rows 100..124 -> rows 150..174, both addressed THROUGH PAGE 2,
    # deliberately: every plane offset folds into one page of storage, so page 2
    # + k must reach the same bytes as k -- on the READ and on the WRITE alike.
    # Addressing band D at its unaliased offset instead leaves the write-side
    # fold untested, and in the reference it did: the mutation that deletes the
    # fold SURVIVED until this copy was moved up here.
    latch_copy(MODEY_PAGE2 + 8000, MODEY_PAGE2 + 12000, 2000)
    # Rows 125..149 -> the OFF-SCREEN CACHE -> rows 175..199 of page 2. That
    # region has its own storage; folding it into the visible page is what
    # painted a band of garbage across Wolfenstein's setup screen.
    latch_copy(10000, MODEY_CACHE, 2000)
    latch_copy(MODEY_CACHE, MODEY_PAGE2 + 14000, 2000)

    emit(0xBA, 0xCE, 0x03)                  # mov dx,3CEh
    emit(0xB8, 0x05, 0x00)                  # mov ax,0005h     ; write mode 0
    emit(0xEF)                              # out dx,ax
    emit(0xF4)                              # hlt
    return bytes(c)


def modey_want_index(x, y):
    if y < 50:
        return x & 0xFF                     # A vertical bars, plane at a time
    if y < 100:
        return (x + y) & 0xFF               # B diagonals
    if y < 150:
        return y & 0xFF                     # C horizontal bars, Map Mask 0x0F
    return (y - 50) & 0xFF                  # D latch copy of band C


def expected_modey():
    """Computed from the PATTERN, never from a second copy of the emulator's
    decode -- two copies of one mistake agree with each other."""
    img = bytearray(FB_STRIDE * 240)
    for y in range(200):
        row = FB_TOP + y * FB_STRIDE
        for x in range(320):
            img[row + x] = modey_want_index(x, y)
    return bytes(img)


def modey_checks(b, syms, quiet=False):
    """Run the Mode Y program on the target and compare all 64,000 pixels."""
    def say(*a):
        if not quiet:
            print(*a)

    for n in ("run_planar_flag", "planar_r", "test_ram"):
        if n not in syms:
            print(f"FAIL: harness has no {n} -- rebuild tests/main.c")
            return False

    b.write_uint32(syms["run_planar_flag"], 1)
    if not wait_flag(b, syms["run_planar_flag"], 0, "planar init hung"):
        return False

    b.write_memory(syms["test_ram"] + PLANAR_CODE_BASE, modey_program())
    b.write_uint32(syms["run_planar_flag"], 2)
    if not wait_flag(b, syms["run_planar_flag"], 0, "Mode Y program hung",
                     tries=600):
        return False

    r = [b.read_uint32(syms["planar_r"] + 4 * i) for i in range(12)]
    mode, bda, unchained, page = r[0], r[1], r[2], r[3]
    stores, lost, flips = r[4], r[5], r[6]
    say(f"  mode={mode:#04x} bda449={bda:#04x} unchained={unchained} "
        f"page={page} stores={stores} lost={lost} flips={flips} "
        f"latch={r[8]}/{r[9]} exit={r[7]:#x}")

    ok = True
    if mode != 0x13 or bda != 0x13:
        print(f"FAIL: mode 13h was never adopted (mode {mode:#x}, BDA {bda:#x})")
        ok = False
    if not unchained:
        print("FAIL: the guest cleared Chain-4 and the adapter is still CHAINED."
              " Either the Sequencer write never reached "
              "x86_vga_planar_seq_write() or the BDA gate rejected it. This is "
              "the four-copies bug.")
        ok = False
    if page != MODEY_STRIDE:
        print(f"FAIL: page stride learned as {page}, want {MODEY_STRIDE}. The "
              f"guest's two flips are {MODEY_STRIDE} apart and that is the only "
              f"place it ever states the number.")
        ok = False
    if flips < 2:
        print(f"FAIL: only {flips} display-start changes seen; the guest made "
              f"two. Present-on-page-flip hangs off that edge.")
        ok = False
    if stores == 0:
        print("FAIL: not one store took the aperture hook -- cpu->vga_base was "
              "never armed, so every planar write landed raw in the aperture.")
        ok = False
    if not ok:
        return False

    b.write_uint32(syms["run_video_flag"], 3)
    if not wait_flag(b, syms["run_video_flag"], 0, "Mode Y blit did not finish"):
        return False
    got = bytes(b.read_memory(syms["cga_test_out"], FB_STRIDE * 240))
    want = expected_modey()

    bad = [i for i in range(FB_TOP, FB_TOP + 200 * FB_STRIDE)
           if got[i] != want[i]]
    if bad:
        i = bad[0]
        y, x = divmod(i - FB_TOP, FB_STRIDE)
        band = ["A vertical-bars/map-mask", "B diagonals", "C horizontal-bars",
                "D latch-copy"][min(y // 50, 3)]
        print(f"FAIL: Mode Y {len(bad)}/64000 pixels wrong; first ({x},{y}) "
              f"got {got[i]} want {want[i]}  [band {band}]")
        return False

    say("PASS: Mode Y 64000/64000 pixels exact (chain-4 off + map mask + "
        "interleave + CRTC start/stride + page fold + latch copy + off-screen "
        "cache)")

    # Put the fold back the way the harness set it up, so any arm that runs
    # after this one is not looking at the aperture through 0xA0000.
    b.write_uint32(syms["run_planar_flag"], 3)
    wait_flag(b, syms["run_planar_flag"], 0, "planar unmap hung")
    return True


# ===========================================================================
# EGA mode 0Dh -- 320x200, 16 colours, four bitplanes at 0xA0000.
#
# Ported from references/8086tiny/test286/tega.asm AND tega2.asm, MERGED,
# because the pair exists for a reason that is recorded and general
# (11-ega-planar.md, "Why the first test passed while a real game rendered
# garbage"). tega.asm compared all 64,000 pixels, was mutation-tested when it
# was written, and passed unchanged through ALL FIVE of the bugs that made
# Commander Keen 4 render as horizontal streaks of coloured dashes -- because it
# drove the adapter the way a TEST does:
#
#   tega.asm did                  real software does            bug it hid
#   byte OUT for index then data  word `out dx,ax`, one insn    word OUT drops AH
#   ATC index 0x01                ATC index 0x21 (bit 5 set)    index masked 0x3F
#   start 0, stride 40, no pan    reprograms all three a frame  blit ignores CRTC
#   stayed inside the first 16 KB uses the whole 64 KB window   writes land raw
#   never probed unknown ports    FreeDOS walks 0x3C0-0x3CF     the probe armed it
#
# So this program does the right-hand column: EVERY register write is a word
# OUT, the ATC index carries bit 5, the CRTC Offset is 22 (a 352-pixel virtual
# screen, not the default 40 bytes), the display start is 0x0100, there are
# three pixels of pel panning, there is a write above the 16 KB window whose
# read-back must be ZERO, and there is a blind 0xFF to port 0x3C2.
#
# The virtual screen is also deliberately WIDER THAN THE CARD CAN HOLD: at 8,192
# plane offsets the bottom right of the display runs past fitted plane memory and
# must come back BLACK. The card-size boundary is then visible in the picture
# instead of being an internal detail.
#
# FOUR BANDS, each isolating one mechanism so a wrong answer in one cannot be
# produced by a bug in another. u is the SOURCE pixel index within the virtual
# row, i.e. displayed x plus the pel pan:
#
#   rows   0..49   Map Mask     each plane filled separately    colour = u & 15
#   rows  50..99   Bit Mask +   repaint under mask 0x18, six    15 where u%8 is
#                  latches      bits coming from the latches    3 or 4, else 1
#   rows 100..149  Set/Reset    CPU byte is 0xAA and must be    colour = y & 15
#                               ignored entirely
#   rows 150..199  Write mode 1 a pure latch copy of rows 0-49  colour = u & 15
#
# The bands are chosen so a plausible-looking wrong answer is still a detectable
# one: if write mode 1 were unimplemented the bottom band would mirror AL --
# a pattern, not a blank -- which is why the comparison is pixel-exact rather
# than "does something appear".
# ===========================================================================
EGA_START = 0x0100      # non-zero display start, in plane offsets
EGA_OFFSET = 22         # CRTC 0x13, in WORDS -> 44 bytes per row
EGA_PITCH = 2 * EGA_OFFSET
EGA_PAN = 3             # ATC 0x13, horizontal pel pan
EGA_PLANE = 0x2000      # X86_EGA_PLANE_OFFSETS: 8,192 -> a 32 KB card
EGA_BAND = EGA_PITCH * 50
EGA_PROBE = 0x5000      # a window offset above the 16 KB CPU-visible shadow
EGA_PROBE_SLOT = 0x2F00  # guest DS:offset the read-back is parked in


def ega_program():
    """The guest program. Same hand-encoding as modey_program(); see there."""
    c = bytearray()

    def emit(*b):
        c.extend(b)

    def back(op, target):
        emit(op)
        d = target - (len(c) + 1)
        assert -128 <= d <= -1, f"rel8 out of range: {d}"
        emit(d & 0xFF)

    def wout(port, index, data):
        """`mov dx,port / mov ax,(data<<8)|index / out dx,ax` -- the word form,
        which is the only form real software uses and the one that loses AH if
        the port layer treats a 16-bit OUT as a byte."""
        emit(0xBA, port & 0xFF, port >> 8)          # mov dx,port
        emit(0xB8, index, data)                     # mov ax,(data<<8)|index
        emit(0xEF)                                  # out dx,ax

    emit(0xB8, 0x0D, 0x00)                  # mov ax,000Dh
    emit(0xCD, 0x10)                        # int 10h
    emit(0xB8, 0x00, 0xA0)                  # mov ax,0A000h
    emit(0x8E, 0xC0)                        # mov es,ax
    emit(0xFC)                              # cld

    # ---- CRTC: a non-default Offset and a non-zero display start.
    wout(0x3D4, 0x13, EGA_OFFSET)           # Offset = 22 words = 44 bytes
    wout(0x3D4, 0x0C, EGA_START >> 8)       # start high
    wout(0x3D4, 0x0D, EGA_START & 0xFF)     # start low

    # ---- ATC pel panning, through the real sequence: read 0x3DA to clear the
    # flip-flop, then index and data on the SAME port, with bit 5 of the index
    # set (Palette Address Source) exactly as real software leaves it.
    emit(0xBA, 0xDA, 0x03)                  # mov dx,3DAh
    emit(0xEC)                              # in al,dx
    emit(0xBA, 0xC0, 0x03)                  # mov dx,3C0h
    emit(0xB0, 0x13 | 0x20)                 # mov al,33h
    emit(0xEE)                              # out dx,al
    emit(0xB0, EGA_PAN)                     # mov al,3
    emit(0xEE)                              # out dx,al

    # ---- a blind 0xFF to Miscellaneous Output, which is what FreeDOS's boot
    # probe does and what armed planar and wiped VRAM on the reference port.
    emit(0xBA, 0xC2, 0x03)                  # mov dx,3C2h
    emit(0xB0, 0xFF)                        # mov al,0FFh
    emit(0xEE)                              # out dx,al

    def fill(mapmask, base, count, value):
        wout(0x3C4, 0x02, mapmask)          # Sequencer Map Mask
        emit(0xBF, base & 0xFF, base >> 8)  # mov di,base
        emit(0xB9, count & 0xFF, count >> 8)  # mov cx,count
        emit(0xB0, value)                   # mov al,value
        emit(0xF3, 0xAA)                    # rep stosb

    # ---- band A: colour = u & 15, one plane at a time.
    # Bit p of (u & 15) over eight consecutive u is a CONSTANT byte for planes
    # 0-2 (0x55, 0x33, 0x0F) and alternates 0x00/0xFF per group for plane 3.
    # Filling each plane separately is what makes this the Map Mask band: a Map
    # Mask that is ignored puts every constant into every plane.
    fill(0x01, EGA_START, EGA_BAND, 0x55)
    fill(0x02, EGA_START, EGA_BAND, 0x33)
    fill(0x04, EGA_START, EGA_BAND, 0x0F)
    wout(0x3C4, 0x02, 0x08)                 # Map Mask = plane 3 only
    emit(0xBF, EGA_START & 0xFF, EGA_START >> 8)    # mov di,start
    emit(0xB9, EGA_BAND & 0xFF, EGA_BAND >> 8)      # mov cx,2200
    emit(0xB0, 0x00)                        # mov al,0
    a_alt = len(c)
    emit(0xAA)                              # stosb
    emit(0x34, 0xFF)                        # xor al,0FFh
    back(0xE2, a_alt)                       # loop a_alt

    # ---- band B: colour 1 everywhere, then repaint under Bit Mask 0x18.
    # The six bits the mask does NOT cover have to come back from the LATCHES,
    # so this band fails if a read-modify-write does not load them.
    b_base = EGA_START + EGA_BAND
    fill(0x01, b_base, EGA_BAND, 0xFF)      # plane 0 only -> colour 1
    wout(0x3C4, 0x02, 0x0F)                 # every plane enabled
    wout(0x3CE, 0x08, 0x18)                 # Bit Mask = 0x18
    emit(0xBE, b_base & 0xFF, b_base >> 8)  # mov si,b_base
    emit(0xB9, EGA_BAND & 0xFF, EGA_BAND >> 8)      # mov cx,2200
    b_loop = len(c)
    emit(0x26, 0x8A, 0x1C)                  # mov bl,[es:si]  ; loads the latches
    emit(0xB0, 0xFF)                        # mov al,0FFh
    emit(0x26, 0x88, 0x04)                  # mov [es:si],al
    emit(0x46)                              # inc si
    back(0xE2, b_loop)                      # loop b_loop
    wout(0x3CE, 0x08, 0xFF)                 # Bit Mask back to 0xFF

    # ---- band C: Set/Reset supplies the colour and the CPU byte is 0xAA,
    # which must be ignored ENTIRELY. A path that quietly ORs the CPU byte in
    # renders 0xAA's bit pattern through it and this band says so.
    c_base = EGA_START + 2 * EGA_BAND
    wout(0x3CE, 0x01, 0x0F)                 # Enable Set/Reset, all four planes
    emit(0xBF, c_base & 0xFF, c_base >> 8)  # mov di,c_base
    emit(0xB3, 100)                         # mov bl,100      ; y
    c_row = len(c)
    emit(0xBA, 0xCE, 0x03)                  # mov dx,3CEh
    emit(0xB0, 0x00)                        # mov al,0        ; Set/Reset index
    emit(0x88, 0xDC)                        # mov ah,bl
    emit(0x80, 0xE4, 0x0F)                  # and ah,0Fh      ; colour = y & 15
    emit(0xEF)                              # out dx,ax
    emit(0xB0, 0xAA)                        # mov al,0AAh     ; must be ignored
    emit(0xB9, EGA_PITCH & 0xFF, EGA_PITCH >> 8)    # mov cx,44
    emit(0xF3, 0xAA)                        # rep stosb
    emit(0xFE, 0xC3)                        # inc bl
    emit(0x80, 0xFB, 150)                   # cmp bl,150
    back(0x72, c_row)                       # jb c_row
    wout(0x3CE, 0x01, 0x00)                 # Enable Set/Reset off

    # ---- band D: a PURE LATCH COPY of band A through write mode 1.
    d_base = EGA_START + 3 * EGA_BAND
    wout(0x3CE, 0x05, 0x01)                 # Mode register = write mode 1
    emit(0xBE, EGA_START & 0xFF, EGA_START >> 8)    # mov si,start
    emit(0xBF, d_base & 0xFF, d_base >> 8)          # mov di,d_base
    emit(0xB9, EGA_BAND & 0xFF, EGA_BAND >> 8)      # mov cx,2200
    d_loop = len(c)
    emit(0x26, 0x8A, 0x04)                  # mov al,[es:si]
    emit(0x26, 0x88, 0x05)                  # mov [es:di],al
    emit(0x46)                              # inc si
    emit(0x47)                              # inc di
    back(0xE2, d_loop)                      # loop d_loop
    wout(0x3CE, 0x05, 0x00)                 # back to write mode 0

    # ---- a write ABOVE the 16 KB CPU-visible window, then a read-back of it.
    # It must return ZERO. A non-zero answer means the access was treated as
    # plain memory: the raw guest byte landed somewhere the blit will render,
    # which is precisely the "horizontal streaks of coloured dashes" defect.
    emit(0xBF, EGA_PROBE & 0xFF, EGA_PROBE >> 8)    # mov di,5000h
    emit(0xB0, 0x5A)                        # mov al,5Ah
    emit(0x26, 0x88, 0x05)                  # mov [es:di],al
    emit(0x26, 0x8A, 0x05)                  # mov al,[es:di]
    emit(0xA2, EGA_PROBE_SLOT & 0xFF, EGA_PROBE_SLOT >> 8)   # mov [imm16],al
    emit(0xF4)                              # hlt
    return bytes(c)


def ega_want(x, y):
    """The colour index displayed at (x, y), from the PATTERN. u is the source
    pixel: the pel pan shifts the whole picture left by three, so displayed x
    reads source pixel x + 3."""
    u = x + EGA_PAN
    a = EGA_START + y * EGA_PITCH + (u >> 3)
    if a >= EGA_PLANE:
        # Past the plane memory this card has fitted. Reads as zero, exactly as
        # the store path discarded the writes there, so the card-size boundary
        # is a visible edge in the picture rather than an internal detail.
        return 0
    if y < 50 or y >= 150:
        return u & 15                       # A map mask; D is a latch copy of A
    if y < 100:
        return 15 if (u & 7) in (3, 4) else 1   # B bit mask + latches
    return y & 15                           # C set/reset


def expected_ega():
    img = bytearray(FB_STRIDE * 240)
    for y in range(200):
        row = FB_TOP + y * FB_STRIDE
        for x in range(320):
            img[row + x] = ega_want(x, y)
    return bytes(img)


def ega_checks(b, syms):
    """EGA 0Dh: the four write mechanisms, the CRTC window, and the card bound."""
    b.write_uint32(syms["run_planar_flag"], 1)
    if not wait_flag(b, syms["run_planar_flag"], 0, "planar init hung"):
        return False

    b.write_memory(syms["test_ram"] + PLANAR_CODE_BASE, ega_program())
    b.write_uint32(syms["run_planar_flag"], 2)
    if not wait_flag(b, syms["run_planar_flag"], 0, "EGA program hung",
                     tries=600):
        return False

    r = [b.read_uint32(syms["planar_r"] + 4 * i) for i in range(12)]
    print(f"  mode={r[0]:#04x} bda449={r[1]:#04x} ega_armed={r[10]} "
          f"stores={r[4]} lost={r[5]} arena_free={r[11]} exit={r[7]:#x}")
    if r[0] != 0x0D or r[1] != 0x0D:
        print(f"FAIL: mode 0Dh was not adopted (mode {r[0]:#x}, BDA {r[1]:#x}). "
              f"If the arena could not place 32,768 bytes the mode is REFUSED, "
              f"which is the intended behaviour but not a passing test.")
        return False
    if not r[10]:
        print("FAIL: mode 0Dh adopted but the planar adapter is not armed, so "
              "every store landed raw in the aperture.")
        return False
    if r[5] == 0:
        print("FAIL: no store was discarded, yet the program deliberately writes "
              "past the fitted card (offset 0x5000 and the bottom of band D). "
              "Nothing is enforcing the card bound.")
        return False

    probe = b.read_memory(syms["test_ram"] + EGA_PROBE_SLOT, 1)[0]
    if probe != 0:
        print(f"FAIL: a write at window offset {EGA_PROBE:#x} -- above the 16 KB "
              f"CPU-visible shadow -- read back as {probe:#04x}, not 0. The "
              f"access was treated as plain memory, so the raw guest byte is "
              f"sitting where the blit will render it.")
        return False
    print(f"PASS: a write above the 16 KB window is discarded and reads back 0")

    b.write_uint32(syms["run_video_flag"], 3)
    if not wait_flag(b, syms["run_video_flag"], 0, "EGA blit did not finish"):
        return False
    got = bytes(b.read_memory(syms["cga_test_out"], FB_STRIDE * 240))
    want = expected_ega()
    bad = [i for i in range(FB_TOP, FB_TOP + 200 * FB_STRIDE)
           if got[i] != want[i]]
    if bad:
        i = bad[0]
        y, x = divmod(i - FB_TOP, FB_STRIDE)
        band = ["A map-mask", "B bit-mask/latches", "C set/reset",
                "D write-mode-1"][min(y // 50, 3)]
        print(f"FAIL: EGA 0Dh {len(bad)}/64000 pixels wrong; first ({x},{y}) "
              f"got {got[i]} want {want[i]}  [band {band}]")
        return False
    print("PASS: EGA 0Dh 64000/64000 pixels exact (map mask + bit mask/latches "
          "+ set/reset + write mode 1 + word OUTs + CRTC Offset/start + pel pan "
          "+ the card bound)")

    b.write_uint32(syms["run_planar_flag"], 3)
    wait_flag(b, syms["run_planar_flag"], 0, "planar unmap hung")
    return True


def egafit_checks(b, syms):
    """The ENLARGED EGA card: the CGA loan, the compaction, and the refusal.

    Keen 4 programs a CRTC Offset of 0x20 -- 64 bytes per row -- so an 8,192
    offset plane is 128 rows and the bottom three quarters of the intro was
    black. The card is sized from the arena now, and this arm asserts the
    arithmetic on the layout the real core claims rather than on whatever the
    bonus pool happened to release.
    """
    for n in ("run_egafit_flag", "egafit_r"):
        if n not in syms:
            print(f"missing symbol {n}")
            return False
    b.write_uint32(syms["run_egafit_flag"], 1)
    if not wait_flag(b, syms["run_egafit_flag"], 0, "EGA fit arm hung"):
        return False
    r = [b.read_uint32(syms["egafit_r"] + 4 * i) for i in range(16)]
    print(f"  claims={r[0]}{r[1]} largest_before={r[2]} mode={r[3]:#04x} "
          f"card={r[4]} cga_held={r[5]} armed={r[6]} | back: mode={r[8]:#04x} "
          f"cga_held={r[9]} cell={r[10]:#06x} | control: mode={r[12]:#04x} "
          f"refused={r[13]} deficit={r[15]}")

    if not (r[0] and r[1]):
        print("FAIL: the harness could not make the core's own two claims")
        return False
    if r[2] != 45056:
        print(f"FAIL: expected 45,056 bytes free before the loan, got {r[2]}")
        return False
    if r[3] != 0x0D or not r[6] or r[7]:
        print(f"FAIL: mode 0Dh was not adopted (mode {r[3]:#x}, armed {r[6]}, "
              f"refusals {r[7]})")
        return False
    if r[4] != 19456:
        print(f"FAIL: the card is {r[4]} plane offsets, expected 19,456. "
              f"77,824 contiguous bytes / 4 planes is the whole point of "
              f"lending the CGA claim and compacting the aperture; "
              f"{r[4] * 4} bytes means one of the two did not happen.")
        return False
    if r[5]:
        print("FAIL: the CGA claim is still held while 0Dh is up, so the card "
              "got only the tail of the arena")
        return False
    if r[8] != 0x03 or not r[9] or r[11]:
        print(f"FAIL: returning to text did not give the CGA claim back "
              f"(mode {r[8]:#x}, held {r[9]}, ega_armed {r[11]})")
        return False
    if r[10] != 0x0720:
        print(f"FAIL: the text buffer came back as {r[10]:#06x}, not blank "
              f"(0x0720). Its pages held EGA planes and the contents were not "
              f"cleared, so the guest's screen is plane bytes read as glyphs.")
        return False
    # ---- the negative control
    if r[12] == 0x0D:
        print("FAIL: 0Dh was ADOPTED against an arena with no spare page. A "
              "mode whose stores have nowhere to land must be refused.")
        return False
    if r[13] < 1 or r[14] or r[15] == 0:
        print(f"FAIL: the refusal was silent -- refusals {r[13]}, armed "
              f"{r[14]}, deficit {r[15]}. A refusal must be counted and must "
              f"quote what it could not place.")
        return False
    print(f"PASS: EGA card 8,192 -> {r[4]} plane offsets ({r[4] * 4} B, "
          f"{r[4] // 64} rows at Keen's 64-byte pitch); CGA lent and returned "
          f"blank; a card that does not fit is refused and counted")
    b.write_uint32(syms["run_planar_flag"], 3)
    wait_flag(b, syms["run_planar_flag"], 0, "planar unmap hung")
    return True


def present_checks(b, syms):
    """The Mode Y PRESENTATION POLICY -- which frames the loop paints.

    The artefact this pins is invisible to every pixel test in this file, and
    that is the point of having it. Mode Y aliases all pages into ONE page of
    storage, so that storage holds whatever the guest is drawing at this
    instant; the screen is presented on the guest's page flip because that is
    the only moment it is a complete picture. The escape hatch that keeps a
    non-flipping title from being a frozen screen used to be a TIMER -- paint
    anyway every 8 frames -- and a timer fires mid-redraw. On hardware that was
    a raycast viewport split in half in gameplay and a menu highlight showing
    both the old item and the new one.

    The policy is now "paint when the aperture has gone QUIET", i.e. when the
    guest stored nothing for two whole slices, which is a completed picture by
    construction. Three scenarios, driven on the target with no LCD and no CPU.
    """
    if "run_present_flag" not in syms:
        print("FAIL: no run_present_flag; the target predates the policy test")
        return False

    def run(which):
        b.write_uint32(syms["run_present_flag"], which)
        if not wait_flag(b, syms["run_present_flag"], 0, "present policy hung"):
            return None
        return [b.read_uint32(syms["present_r"] + 4 * i) for i in range(8)]

    r = run(1)
    if r is None:
        return False
    if not r[7]:
        print("FAIL: Mode Y did not arm for the presentation policy test")
        return False
    # 200 frames, stores in every one, a flip every 20th. The page is complete
    # only at a flip, so the frame loop must paint on NONE of them.
    if r[0] != 0 or r[1] != 0:
        print(f"FAIL: the frame loop painted {r[0]} of 200 frames while the "
              f"guest was drawing ({r[1]} quiet-fallbacks). Each is the one "
              f"aliased page caught mid-redraw -- a split viewport on silicon.")
        return False
    if r[2] != 0:
        print(f"FAIL: the staleness bound fired {r[2]} times inside a title "
              f"that is flipping every 20 frames; it must never.")
        return False
    if r[3] != 20:
        print(f"FAIL: flip gap measured {r[3]}, expected 20")
        return False

    # THE SAME GUEST AT THE SPEED THE DEVICE RUNS IT. The arm above flips every
    # 20 frames, which is faster than the 60-frame staleness bound, so the bound
    # could never fire and the arm never asked what happens when it does. On
    # silicon a WOLF3D gameplay frame is much longer than 60 loop frames: the
    # bound fired mid-redraw, every guest frame, which is the split viewport and
    # the house of mirrors. 300-frame flips, stores in every frame, 1500 frames.
    r = run(4)
    if r is None:
        return False
    # Menus (fast flips), then a level entered: 1500 frames of continuous
    # drawing with a flip every 300. The estimate of the guest's frame time is
    # necessarily stale across that transition and nothing can distinguish a
    # slow frame from a stopped one, so the bound may fire a couple of times --
    # but it must BACK OFF, and it is graded on the run after.
    if r[0] > 2 or r[1] > 2:
        print(f"FAIL: entering a level the loop painted {r[0]} frames "
              f"({r[1]} of them mid-draw) and did not converge; the staleness "
              f"back-off is not working.")
        return False
    if r[2] != 0 or r[3] != 0 or r[4] != 0:
        print(f"FAIL: in a settled level the frame loop painted {r[2]} of 1500 "
              f"frames of a guest that draws continuously and flips every 300 "
              f"({r[3]} mid-draw, {r[4]} quiet). Every mid-draw paint is the "
              f"one aliased page caught with the raycaster still writing it -- "
              f"the split viewport, and the house of mirrors while turning.")
        return False
    if r[5] != 300:
        print(f"FAIL: flip gap measured {r[5]}, expected 300")
        return False
    # ...and it must not have bought that by freezing: the same guest then stops
    # flipping and keeps drawing, and has to be rescued eventually.
    if r[6] < 1:
        print("FAIL: a title that stopped flipping and kept drawing was never "
              "painted again in 12000 frames -- that is a frozen screen.")
        return False

    r = run(2)
    if r is None:
        return False
    # A menu: four frames of drawing, then quiet. Nothing while it draws; once,
    # and only once, after it stops.
    if r[0] != 0:
        print(f"FAIL: painted {r[0]} times while the menu was redrawing -- "
              f"that is the erase and the redraw both on the panel.")
        return False
    if r[1] != 1:
        print(f"FAIL: a quiet, non-flipping screen was painted {r[1]} times "
              f"in 30 frames; expected exactly 1 (0 is a FROZEN screen, more "
              f"is the whole frame wasted repainting a static picture).")
        return False
    if r[2] != 0:
        print(f"FAIL: the staleness bound fired {r[2]} times; the quiet path "
              f"should have presented long before it.")
        return False

    r = run(3)
    if r is None:
        return False
    # A guest that stores forever and never flips. Only the last-resort bound
    # may fire, and it must, or the screen is frozen for good.
    if r[1] != 0:
        print(f"FAIL: {r[1]} quiet-fallbacks in a guest that stored every "
              f"frame; the quiet test is not reading the store counter.")
        return False
    if r[2] != 3 or r[0] != 3:
        print(f"FAIL: 180 frames of unflipped drawing gave {r[0]} paints "
              f"({r[2]} from the staleness bound), expected 3 -- one per 60.")
        return False

    print("PASS: Mode Y presentation policy "
          "(0 mid-draw paints at 20- and 300-frame flip gaps, "
          "1 quiet paint, 3 stale paints, no freeze)")
    return True


def main(args):
    runner = build_and_create_runner(args)
    try:
        runner.start()
        b = runner.backend
        syms = get_symbols(getattr(args, "elf", None) or "test_core.elf")
        if getattr(args, "present_only", False):
            # The presentation-policy arm on its own, for the TIMER control in
            # scripts/run_planar_controls.py. It touches no pixels and needs no
            # guest program, so nothing else has to run first.
            time.sleep(0.5)
            return present_checks(b, syms)
        if getattr(args, "modey_only", False):
            # The single arm scripts/run_planar_controls.py builds and re-runs
            # once per control. Nothing else runs, so a control's verdict is
            # about the Mode Y pipeline and not about some other arm.
            time.sleep(0.5)
            return modey_checks(b, syms, quiet=getattr(args, "quiet", False))
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

        if not mode_switch_checks(b, syms):
            return False
        # LAST, deliberately: the planar arms remap guest 0xA0000/0xB8000 onto
        # the real aperture and VRAM for the duration, which is what the core
        # does and what the other arms do not expect.
        if not modey_checks(b, syms):
            return False
        if not present_checks(b, syms):
            return False
        if not ega_checks(b, syms):
            return False
        # LAST: it re-points the video layer at arena blocks of its own and
        # rebuilds the arena around them, so nothing may run after it that
        # expects the harness's standard buffers still to be claimed.
        return egafit_checks(b, syms)
    finally:
        runner.stop()


if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser(description=__doc__)
    add_runner_args(p)
    p.add_argument("--modey-only", action="store_true",
                   help="run only the Mode Y arm (the controls driver uses this)")
    p.add_argument("--present-only", action="store_true",
                   help="run only the Mode Y presentation-policy arm")
    p.add_argument("--quiet", action="store_true",
                   help="suppress the PASS chatter; the exit code is the verdict")
    args = p.parse_args()
    sys.exit(0 if main(args) else 1)
