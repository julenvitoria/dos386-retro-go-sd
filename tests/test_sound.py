#!/usr/bin/env python3
"""
Tier-1 check of SOUND CARD DETECTION -- the Sound Blaster DSP reset handshake
and the AdLib (OPL2) timer test -- driven by GUEST code executing its own IN and
OUT instructions.

THIS TESTS DETECTION, NOT AUDIO. Nothing in x86_io.c's sound block produces a
sample and this test does not pretend otherwise; what it grades is whether a
guest that asks "is there a card at 0x220 / 0x388" gets a truthful answer
instead of a floating 0xFF.

WHY THIS EXISTS, MEASURED RATHER THAN ASSUMED. With X86_PROBE=1 and
scripts/probe_report.py, on this tree, under gwemu:

    WOLF3D    IN 0x388 x348, OUT 0x388/0x389 x6 each   (an AdLib TIMER test)
    KEEN4     IN 0x388 x348, OUT 0x388/0x389 x6 each   (the same one)
    POP       OUT 0x226 x3, IN 0x22E x32, IN 0x22A x33 (a DSP RESET handshake),
              then the FM aliases 0x220/0x221/0x228/0x229, then a scan of the
              0x240, 0x280 and 0x2C0 bases when 0x220 did not answer
    everything else measured -- ALLEYCAT, SIMCITY, TOPBENCH, EPIC_PINBALL,
    JAZZ, TYRIAN, MORTAL_KOMBAT -- touched neither 0x22x nor 0x388 at all.

348 reads of a status port and 32 polls of a data-available bit are what a
detection routine looks like when the port floats: the loop runs to its bound
every time. Those two sequences are exactly what the guest program in
tests/main.c (sound_code) executes here.

Graded:
  * The DSP reset handshake. Write 1 then 0 to 0x226, poll 0x22E for bit 7, read
    0x22A: it MUST be 0xAA, and the poll must not have run to its bound.
  * The write-buffer status at 0x22C reports READY (bit 7 clear) rather than
    0xFF, which is a busy DSP that never becomes ready -- the shape that spins a
    guest forever.
  * DSP command E1h returns a version, 2.01, in two bytes and in that order.
  * The AdLib timer test: masked and flags reset -> status & 0xE0 == 0; timer 1
    started with preset FFh and given its 80 us -> status & 0xE0 == 0xC0; flags
    reset again -> back to 0. A status that cannot come UP is an absent card; a
    status that cannot go DOWN is not a flag.
  * The counters agree with the sequence, and NO DMA was started -- this arm
    must not be silently exercising an audio path that does not exist.

NEGATIVE CONTROL (`--negative`), and it is the whole point of the file: rebuild
with the `dsp_push(0xAAu)` line deleted from dsp_write_reset() -- the exact
state of the tree before this work, where 0x22A floats -- and require this test
to FAIL. `--negative-opl` does the same for the OPL timer-1 flag, which is the
half of the card WOLF3D and KEEN4 actually probe. The source is restored in a
finally, the way tests/test_pit.py does it.
"""
import argparse
import sys
import time

sys.path.insert(0, ".")
from scripts.common.runner import add_runner_args, build_and_create_runner
from scripts.common.elf import get_symbols

DSP_VERSION = (4, 5)            # DSP 4.05: an SB16, which is what we implement

# The playback arm's fixed numbers. 6,000 Hz into a 48,000 Hz buffer is EXACTLY
# eight output samples per source byte, which is why that rate was chosen: the
# expected waveform is arithmetic, not a simulation of the resampler.
SBD_SRC_RATE = 6000
SBD_OUT_RATE = 48000
SBD_HOLD = SBD_OUT_RATE // SBD_SRC_RATE     # 8
SBD_BYTES = 256
SBD_AMP = 32767


def _s16(v):
    return v - 0x10000 if v & 0x8000 else v


def playback(args, runner):
    """The DMA transfer, the terminal count, the IRQ 5, and the SAMPLES."""
    b = runner.backend
    syms = get_symbols("test_core.elf")
    for n in ("run_sbdma_flag", "sbd_r", "sbd_pcm"):
        if n not in syms:
            print(f"missing symbol {n} -- rebuild tests/main.c")
            return False

    b.write_uint32(syms["run_sbdma_flag"], 1)
    for _ in range(600):
        if b.read_uint32(syms["run_sbdma_flag"]) == 0:
            break
        time.sleep(0.05)
    else:
        print("FAIL: the playback arm never completed")
        return False

    r = [b.read_uint32(syms["sbd_r"] + 4 * i) for i in range(29)]
    wbusy = r[28]
    if not (0 < wbusy < 32):
        print(f"FAIL: the DSP write-status port (0x22C) read busy {wbusy} times in "
              f"32. A real DSP's bit 7 comes and goes; Prince of Persia waits to "
              f"SEE it busy before halting a playing sound, and a port that never "
              f"reads busy froze the game for the rest of each sound.")
        return False
    print(f"PASS: 0x22C reads busy {wbusy}/32 times (bit 7 comes and goes)")
    (aa, major, minor, polls_left, isr_count, dma_status, dsp_stat,
     mix82, mix80, mix81, starts, blocks, dma_bytes, samples, irqs,
     delivered, acked, eois, underruns, rate, maps, guard, captured,
     reason) = r[0:24]
    f2_isr, f2_polls, f2_requests, f2_unknown = r[24:28]
    # Packed two samples per word: this harness has no RAM_EMU to spare.
    pcm = []
    for i in range(32):
        w = b.read_uint32(syms["sbd_pcm"] + 4 * i)
        pcm.append(_s16(w & 0xFFFF))
        pcm.append(_s16((w >> 16) & 0xFFFF))

    print()
    print("=== DIGITISED PLAYBACK ===")
    print(f"handshake  0x22A {aa:#04x}   version {major}.{minor:02d}")
    print(f"mixer      0x80 IRQ sel {mix80:#04x} (expect 0x02 = IRQ 5)   "
          f"0x81 DMA sel {mix81:#04x} (expect 0x22 = D1 + H5)")
    print(f"transfer   starts {starts} blocks {blocks} dma_bytes {dma_bytes} "
          f"rate {rate} Hz  granule maps {maps}")
    print(f"           8237 status {dma_status:#04x} (bit 1 = TC on ch1)")
    print(f"IRQ 5      asserted {irqs} delivered {delivered} acked {acked} "
          f"EOIs {eois}   guest ISR entered {isr_count}x")
    print(f"           mixer 0x82 in the handler {mix82:#04x} "
          f"(bit 0 = the 8-bit line)   0x22E after the ack {dsp_stat:#04x}")
    print(f"           the guest's bounded wait had {polls_left} polls left "
          f"(0 = it gave up)")
    print(f"audio      {samples} samples rendered, underruns {underruns}, "
          f"first 64 captured {captured}")
    print(f"           " + " ".join(f"{v:6d}" for v in pcm[:16]))
    print(f"DSP F2h    ISR entered {f2_isr}x with {f2_polls} of 10 polls left"
          f"   irq_requests {f2_requests}  cmds_unknown {f2_unknown}")
    print(f"host loop  {guard} iterations, exit reason {reason} (6 = HLT)")

    ok = True

    def fail(msg):
        nonlocal ok
        print("FAIL: " + msg)
        ok = False

    # ---- DSP F2h: THE IRQ A DRIVER ASKS FOR, AND HOW FAST -------------
    if f2_unknown:
        fail(f"the DSP counted {f2_unknown} UNKNOWN commands. F2h is a real "
             f"SB command -- request an 8-bit interrupt -- and answering "
             f"'unknown' is what makes a driver conclude the card's IRQ is "
             f"dead and fall back to the PC speaker")
    if f2_requests != 1:
        fail(f"x86_sb_irq_requests is {f2_requests}, expected 1: the F2h "
             f"command was not recognised as an IRQ request at all")
    if f2_isr != 1:
        fail(f"the guest's IRQ 5 handler ran {f2_isr} times after F2h, "
             f"expected exactly 1. THIS IS A LATENCY TEST: the window is ten "
             f"iterations, and this harness offers the interrupt from its host "
             f"loop only every 1,200 PIT ticks, so an F2h whose interrupt is "
             f"merely QUEUED for the next slice boundary cannot arrive in "
             f"time -- which is precisely how a real detect routine misses it")
    if f2_polls == 0:
        fail("the F2h wait ran its whole ten-iteration window without the "
             "handler running -- the interrupt was late, not absent")

    # ---- identity: 4.05 is a PROMISE, and the rest of this arm is it ----
    if aa != 0xAA:
        fail(f"0x22A returned {aa:#04x}, not 0xAA -- the card is not even found")
    if (major, minor) != DSP_VERSION:
        fail(f"E1h returned {major}.{minor}, expected "
             f"{DSP_VERSION[0]}.{DSP_VERSION[1]:02d}. A title uses this number "
             f"to decide whether the 16-bit channel and the 0xBx/0xCx command "
             f"block exist, so it must match what is implemented")
    if mix80 != 0x02:
        fail(f"the mixer's IRQ select reads {mix80:#04x}, expected 0x02 (IRQ 5) "
             f"-- 0xFF is the unmodelled port, and a driver that believes it "
             f"hooks a vector nothing will ever raise")
    if mix81 != 0x22:
        fail(f"the mixer's DMA select reads {mix81:#04x}, expected 0x22 "
             f"(channel 1 and channel 5) -- this is what BLASTER's D1/H5 "
             f"advertises and the two must agree")

    # ---- the transfer ---------------------------------------------------
    if starts != 1:
        fail(f"x86_sb_starts is {starts}, expected exactly 1")
    if rate != SBD_SRC_RATE:
        fail(f"the DSP took the rate as {rate} Hz, not {SBD_SRC_RATE} -- "
             f"command 41h's two argument bytes are big-endian and a swap "
             f"here plays everything at the wrong pitch")
    if dma_bytes != SBD_BYTES:
        fail(f"the DMA moved {dma_bytes} bytes, expected exactly {SBD_BYTES}. "
             f"Fewer means the walk stopped early (a count off by one, or a "
             f"granule boundary it could not cross); more means terminal "
             f"count never fired and it ran off the end of the buffer")
    if not (dma_status & 0x02):
        fail(f"the 8237 status register reads {dma_status:#04x}: channel 1's "
             f"terminal count bit is CLEAR after a completed transfer. A "
             f"driver that polls this instead of taking the interrupt waits "
             f"for ever")

    # ---- the interrupt, end to end --------------------------------------
    if isr_count < 1:
        fail("THE GUEST'S OWN IRQ 5 HANDLER NEVER RAN. This is the failure "
             "the whole task exists to remove: the transfer completes and the "
             "title waits for a completion interrupt that never arrives")
    if polls_left == 0:
        fail("the guest's bounded wait ran to its full bound -- the interrupt "
             "arrived late or not at all, which on a real title is a hang")
    if irqs < 1 or delivered < 1:
        fail(f"IRQ 5 asserted {irqs} / delivered {delivered}: the card never "
             f"raised the line, or the 8259 never handed it over")
    if acked < 1:
        fail("the DSP's interrupt was never acknowledged -- a read of 0x22E "
             "must drop the line, and without that a second block never "
             "interrupts")
    if eois < 1:
        fail("no EOI reached the 8259, so the in-service bit is still set and "
             "IRQ 5 can never be delivered again")
    if not (mix82 & 0x01):
        fail(f"the mixer's IRQ status read {mix82:#04x} inside the handler; "
             f"bit 0 (the 8-bit DMA line) must be set. An SB16 handler reads "
             f"this to decide WHICH acknowledge to issue, and a constant here "
             f"sends it to acknowledge the line that did not fire")
    if dsp_stat & 0x80:
        fail(f"0x22E still reports data available ({dsp_stat:#04x}) after the "
             f"handler acknowledged")

    # ---- THE SAMPLES. No counter can fake this one. ---------------------
    if not captured:
        fail("no audio was captured at all -- x86_sb_render_mix() returned 0 "
             "on every call, so nothing was ever played")
    else:
        bad = 0
        first_bad = None
        for k, got in enumerate(pcm):
            src = k // SBD_HOLD                 # the guest wrote byte == index
            want = ((src - 128) << 8)
            want = (want * SBD_AMP) >> 15
            if abs(got - want) > 2:
                bad += 1
                if first_bad is None:
                    first_bad = (k, got, want, src)
        if bad:
            k, got, want, src = first_bad
            fail(f"{bad} of 64 rendered samples are not what the guest wrote. "
                 f"First at output sample {k}: got {got}, expected {want} "
                 f"(the guest's byte {src} of the ramp, unsigned 8-bit). At "
                 f"{SBD_SRC_RATE} Hz into {SBD_OUT_RATE} Hz each source byte "
                 f"must be held for exactly {SBD_HOLD} output samples -- so "
                 f"this catches a wrong DMA address, a wrong decode AND a "
                 f"wrong resampler step")
        else:
            print(f"           all 64 samples match the guest's ramp, held "
                  f"{SBD_HOLD} apiece -- data path AND rate conversion")
    if underruns:
        fail(f"x86_sb_underruns is {underruns}: the source ran dry mid-render, "
             f"so some of the audio above is whatever was held over")
    if samples < 1:
        fail("x86_sb_samples is 0")
    if reason != 6:
        fail(f"the guest exited with reason {reason}, not 6 (HLT)")

    print("\nPLAYBACK PASS" if ok else "\nPLAYBACK FAIL")
    return ok



# ---------------------------------------------------------------------------
# CREATIVE ADPCM. Three formats, two reference spellings, graded on the SAMPLES.
#
# THE RULE, and its source: Creative ADPCM is a TABLE WALK, not IMA/DVI ADPCM
# and not a filter. Each code is ADDED to a running step index, the sum is
# clamped to the table, the step table entry is added to a running unsigned
# 8-bit reference (clamped to 0..255) and IS the sample, and the adjust table
# entry is added to the step index modulo 256. The "+reference" commands
# (0x17 / 0x75 / 0x77 and the auto-init forms) spend the FIRST byte of the
# transfer on an absolute sample and reset the step. Checked against
# DOSBox-X's decode_ADPCM_{2,3,4}_sample() in
# references/dosbox-x/src/hardware/sblaster.cpp; the PACKING is stated below in
# our own terms because a bit index copied out of a reference whose byte order
# differs is exactly how this goes silently wrong.
#
# This decoder is written HERE from that rule. It is not a copy of x86_io.c's
# control flow -- a test carrying a second copy of the implementation would
# agree with it however wrong both were.

APC_SCALE4 = [
    0, 1, 2, 3, 4, 5, 6, 7, 0, -1, -2, -3, -4, -5, -6, -7,
    1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15,
    2, 6, 10, 14, 18, 22, 26, 30, -2, -6, -10, -14, -18, -22, -26, -30,
    4, 12, 20, 28, 36, 44, 52, 60, -4, -12, -20, -28, -36, -44, -52, -60,
]
APC_ADJ4 = [
    0, 0, 0, 0, 0, 16, 16, 16,
    0, 0, 0, 0, 0, 16, 16, 16,
    240, 0, 0, 0, 0, 16, 16, 16,
    240, 0, 0, 0, 0, 16, 16, 16,
    240, 0, 0, 0, 0, 16, 16, 16,
    240, 0, 0, 0, 0, 16, 16, 16,
    240, 0, 0, 0, 0, 0, 0, 0,
    240, 0, 0, 0, 0, 0, 0, 0,
]
APC_SCALE3 = [
    0, 1, 2, 3, 0, -1, -2, -3,
    1, 3, 5, 7, -1, -3, -5, -7,
    2, 6, 10, 14, -2, -6, -10, -14,
    4, 12, 20, 28, -4, -12, -20, -28,
    5, 15, 25, 35, -5, -15, -25, -35,
]
APC_ADJ3 = [
    0, 0, 0, 8, 0, 0, 0, 8,
    248, 0, 0, 8, 248, 0, 0, 8,
    248, 0, 0, 8, 248, 0, 0, 8,
    248, 0, 0, 8, 248, 0, 0, 8,
    248, 0, 0, 0, 248, 0, 0, 0,
]
APC_SCALE2 = [
    0, 1, 0, -1, 1, 3, -1, -3,
    2, 6, -2, -6, 4, 12, -4, -12,
    8, 24, -8, -24, 16, 48, -16, -48,
]
APC_ADJ2 = [
    0, 4, 0, 4,
    252, 4, 252, 4, 252, 4, 252, 4,
    252, 4, 252, 4, 252, 4, 252, 4,
    252, 0, 252, 0,
]

# command -> (label, codes-per-byte format, has a reference byte)
APC_CMDS = {
    0x75: ("4-bit  +ref", 4, True),
    0x74: ("4-bit  no ref", 4, False),
    0x77: ("2.6-bit +ref", 3, True),
    0x17: ("2-bit  +ref", 2, True),
}

APC_BYTES = 20                  # must match APC_BYTES in tests/main.c
APC_SRC_RATE = 24000
APC_OUT_RATE = 48000
APC_HOLD = APC_OUT_RATE // APC_SRC_RATE     # 2


def _apc_src_bytes():
    """The block tests/main.c generates. Same closed formula, derived here."""
    return [(i * 37 + 11) & 0xFF for i in range(APC_BYTES)]


def _apc_codes(byte, fmt):
    """OUR packing, stated explicitly.

    4-bit:   high nibble first, then the low nibble.
    2.6-bit: THREE codes -- bits 7:5, bits 4:2, and the low TWO bits SHIFTED UP
             one so a 2-bit field indexes a 3-bit table. Reading that last
             field as three bits, or without the shift, is the classic error.
    2-bit:   four codes, 7:6 down to 1:0.
    """
    if fmt == 4:
        return [byte >> 4, byte & 0x0F]
    if fmt == 3:
        return [(byte >> 5) & 7, (byte >> 2) & 7, (byte & 3) << 1]
    return [(byte >> 6) & 3, (byte >> 4) & 3, (byte >> 2) & 3, byte & 3]


def _apc_decode(data, fmt, have_ref):
    scale, adj, top = {
        4: (APC_SCALE4, APC_ADJ4, 63),
        3: (APC_SCALE3, APC_ADJ3, 39),
        2: (APC_SCALE2, APC_ADJ2, 23),
    }[fmt]
    i = 0
    ref, step = 128, 0
    if have_ref:
        ref, step = data[0], 0
        i = 1
    out = []
    for b in data[i:]:
        for code in _apc_codes(b, fmt):
            idx = min(max(code + step, 0), top)
            ref = min(max(ref + scale[idx], 0), 255)
            step = (step + adj[idx]) & 0xFF
            out.append(ref)
    return out


def _apc_expect(cmd):
    _, fmt, have_ref = APC_CMDS[cmd]
    dec = _apc_decode(_apc_src_bytes(), fmt, have_ref)
    pcm = []
    for d in dec:
        v = ((d - 128) << 8)
        v = (v * SBD_AMP) >> 15
        for _ in range(APC_HOLD):
            pcm.append(v)
    return dec, pcm[:64]


def adpcm(args, runner):
    """Creative ADPCM: the transport AND the decoded waveform, per format."""
    b = runner.backend
    syms = get_symbols("test_core.elf")
    for n in ("run_sbadpcm_flag", "apc_in", "apc_r", "apc_pcm"):
        if n not in syms:
            print(f"missing symbol {n} -- rebuild tests/main.c")
            return False

    print()
    print("=== CREATIVE ADPCM ===")
    ok = True

    def fail(msg):
        nonlocal ok
        print("FAIL: " + msg)
        ok = False

    for cmd in (0x75, 0x74, 0x77, 0x17):
        name, fmt, have_ref = APC_CMDS[cmd]
        b.write_uint32(syms["apc_in"], cmd)
        b.write_uint32(syms["run_sbadpcm_flag"], 1)
        for _ in range(600):
            if b.read_uint32(syms["run_sbadpcm_flag"]) == 0:
                break
            time.sleep(0.05)
        else:
            fail(f"DSP {cmd:#04x} ({name}): the arm never completed")
            return False

        r = [b.read_uint32(syms["apc_r"] + 4 * i) for i in range(12)]
        (aa, isr, polls_left, dma_status, starts, blocks, dma_bytes,
         samples, underruns, silenced, captured, reason) = r
        pcm = []
        for i in range(32):
            w = b.read_uint32(syms["apc_pcm"] + 4 * i)
            pcm.append(_s16(w & 0xFFFF))
            pcm.append(_s16((w >> 16) & 0xFFFF))

        dec, want = _apc_expect(cmd)
        print(f"-- DSP {cmd:#04x}  {name} --")
        print(f"   transfer starts {starts} blocks {blocks} "
              f"dma_bytes {dma_bytes} ISR {isr}x polls left {polls_left} "
              f"8237 status {dma_status:#04x}")
        print(f"   samples {samples} underruns {underruns} "
              f"adpcm_silenced {silenced} captured {captured} reason {reason}")
        print("   decoded  " + " ".join(f"{v:4d}" for v in dec[:12]))
        print("   got      " + " ".join(f"{v:6d}" for v in pcm[:10]))
        print("   want     " + " ".join(f"{v:6d}" for v in want[:10]))

        if aa != 0xAA:
            fail(f"{name}: 0x22A returned {aa:#04x} -- the card is not found")
        if silenced:
            fail(f"{name}: x86_sb_adpcm_silenced is {silenced}. THIS IS THE "
                 f"ORIGINAL STUB: the ADPCM commands took the deliberate "
                 f"silent path instead of decoding")
        if starts != 1:
            fail(f"{name}: x86_sb_starts is {starts}, expected 1")
        if dma_bytes != APC_BYTES:
            fail(f"{name}: the DMA moved {dma_bytes} bytes, expected exactly "
                 f"{APC_BYTES}. An ADPCM block length is in BYTES, not in "
                 f"decoded samples -- a decoder that ticks the block per "
                 f"OUTPUT sample interrupts several times too early and reads "
                 f"correspondingly too few bytes")
        if blocks != 1:
            fail(f"{name}: {blocks} blocks completed, expected exactly 1")
        if isr < 1:
            fail(f"{name}: the guest's IRQ 5 handler never ran")
        if polls_left == 0:
            fail(f"{name}: the guest's bounded wait ran to its bound")
        if underruns:
            fail(f"{name}: x86_sb_underruns is {underruns}")
        if reason != 6:
            fail(f"{name}: the guest exited with reason {reason}, not 6 (HLT)")
        if not captured:
            fail(f"{name}: no audio was captured at all")
            continue

        # ---- THE WAVEFORM. Three assertions, and they fail differently. ----
        #
        # (1) It must not be silence.
        # (2) It must not be a CONSTANT -- a DC offset and real silence are
        #     different failures, and a decoder that consumed the reference
        #     byte as data, or never consumed it, collapses toward a constant
        #     that is NOT zero. An all-zero expectation would accept both.
        # (3) It must be the RIGHT waveform, sample for sample, held APC_HOLD
        #     apiece -- which no counter and no energy measure can fake.
        if all(v == 0 for v in pcm):
            fail(f"{name}: every one of the 64 rendered samples is ZERO -- "
                 f"the transfer ran and emitted silence")
            continue
        distinct = len(set(pcm))
        if distinct < 4:
            fail(f"{name}: the 64 rendered samples take only {distinct} "
                 f"distinct values -- this is a CONSTANT (or near-constant), "
                 f"not a decoded waveform. A reference byte consumed as data, "
                 f"or a step table that never adapts, looks exactly like this")
        want_distinct = len(set(want))
        if want_distinct < 4:
            fail(f"{name}: the EXPECTATION itself has only {want_distinct} "
                 f"distinct values, so this arm could be passed by a decoder "
                 f"that does nothing. The source block is wrong for this "
                 f"format and the arm proves nothing")
        bad = [(k, pcm[k], want[k]) for k in range(64) if pcm[k] != want[k]]
        if bad:
            k, got, exp = bad[0]
            fail(f"{name}: {len(bad)} of 64 rendered samples differ from the "
                 f"reference decode. First at output sample {k}: got {got}, "
                 f"expected {exp} (decoded sample {k // APC_HOLD} = "
                 f"{dec[k // APC_HOLD]}). At {APC_SRC_RATE} Hz into "
                 f"{APC_OUT_RATE} Hz each decoded sample is held exactly "
                 f"{APC_HOLD} outputs, so this catches the step table, the "
                 f"packing, the reference byte AND the resampler at once")
        else:
            print(f"   all 64 samples match the reference decode "
                  f"({distinct} distinct values, held {APC_HOLD} apiece)")

    print("\nADPCM PASS" if ok else "\nADPCM FAIL")
    return ok


# ---------------------------------------------------------------------------
# THE FM ARM. The OPL2 tone generator, graded on the SAMPLES.
#
# Everything the expectation needs is computed HERE, from the register bytes
# tests/main.c's fm_code[] writes, using the chip's published frequency
# formula. Nothing below re-implements the synthesiser -- a test that carried a
# second copy of x86_opl.c would agree with it however wrong both were.

FM_FNUM = 0x200
FM_BLOCK = 7
FM_CHIP_HZ = 49716              # 14.31818 MHz / 288, the YM3812's sample clock
FM_OUT_RATE = 48000
FM_SAMPLES = 512
# f = f_num * chip_rate / 2^(20 - block). Ritchie's formula, and the one every
# AdLib note table is built from.
FM_EXPECT_HZ = FM_FNUM * FM_CHIP_HZ / float(1 << (20 - FM_BLOCK))


def _goertzel(x, freq, rate):
    """Magnitude-squared of one DFT bin, no numpy on this host."""
    import math
    w = 2.0 * math.pi * freq / rate
    coeff = 2.0 * math.cos(w)
    s1 = s2 = 0.0
    for v in x:
        s0 = v + coeff * s1 - s2
        s2, s1 = s1, s0
    return s1 * s1 + s2 * s2 - coeff * s1 * s2


def fm(args, runner):
    """A known note in, that note out."""
    b = runner.backend
    syms = get_symbols("test_core.elf")
    for n in ("run_fm_flag", "fm_r", "fm_pcm"):
        if n not in syms:
            print(f"missing symbol {n} -- rebuild tests/main.c")
            return False

    b.write_uint32(syms["run_fm_flag"], 1)
    for _ in range(600):
        if b.read_uint32(syms["run_fm_flag"]) == 0:
            break
        time.sleep(0.05)
    else:
        print("FAIL: the FM arm never completed")
        return False

    r = [b.read_uint32(syms["fm_r"] + 4 * i) for i in range(12)]
    cyc = b.read_uint32(syms["fm_cycles"]) if "fm_cycles" in syms else 0
    slots = b.read_uint32(syms["fm_slots"]) if "fm_slots" in syms else 0
    (reason_on, rendered, peak_on, active_on, reason_off, peak_off,
     active_off, keyons, chip_steps, slots_run, rhythm_muted, writes) = r

    pcm = []
    for i in range(FM_SAMPLES // 2):
        w = b.read_uint32(syms["fm_pcm"] + 4 * i)
        pcm.append(_s16(w & 0xFFFF))
        pcm.append(_s16((w >> 16) & 0xFFFF))

    zc = sum(1 for i in range(1, len(pcm)) if (pcm[i - 1] < 0) != (pcm[i] < 0))
    zc_expect = 2.0 * FM_EXPECT_HZ * FM_SAMPLES / FM_OUT_RATE
    m_fund = _goertzel(pcm, FM_EXPECT_HZ, FM_OUT_RATE)
    m_oct_up = _goertzel(pcm, FM_EXPECT_HZ * 2, FM_OUT_RATE)
    m_oct_dn = _goertzel(pcm, FM_EXPECT_HZ / 2, FM_OUT_RATE)

    print()
    print("=== OPL2 FM SYNTHESIS ===")
    print(f"programmed f_num {FM_FNUM:#05x} block {FM_BLOCK} mult 1 -> "
          f"{FM_EXPECT_HZ:.2f} Hz  ({writes} register writes accepted)")
    print(f"key-on     guest exit {reason_on} (6 = HLT), {rendered} samples "
          f"rendered, peak {peak_on}, chip active {active_on}")
    print(f"pitch      {zc} zero crossings in {FM_SAMPLES} samples, "
          f"expected {zc_expect:.1f}  ->  {zc * FM_OUT_RATE / (2.0 * FM_SAMPLES):.1f} Hz")
    print(f"spectrum   |X(f)| {m_fund:.3e}   |X(2f)| {m_oct_up:.3e}   "
          f"|X(f/2)| {m_oct_dn:.3e}")
    print(f"key-off    guest exit {reason_off}, peak after 20 frames "
          f"{peak_off}, chip active {active_off}")
    print(f"counters   keyons {keyons} chip_steps {chip_steps} "
          f"slot updates {slots_run} rhythm_muted {rhythm_muted}")
    if cyc and slots:
        # WOLF3D at tier 2 averages 10.11 operator updates per chip sample
        # (12,738,872 / 1,259,455 -- about five of the nine channels sounding),
        # and the chip runs at 49,716 Hz. One 60 fps frame at 280 MHz is
        # 4,666,666 cycles.
        per_op = cyc / float(slots)
        frame = 49716.0 / 60.0 * 10.11 * per_op
        print(f"COST       {cyc:,} cycles for {slots:,} operator updates = "
              f"{per_op:.1f} cyc/operator")
        print(f"           -> {frame:,.0f} cyc/frame at WOLF3D's measured 10.11 "
              f"operators/sample = {100.0 * frame / 4666666.0:.2f}% of a "
              f"280 MHz 60 fps frame")
        print("           (a HARDWARE run's number is the only one to quote; "
              "gwemu does not model timing)")
    print("           " + " ".join(f"{v:6d}" for v in pcm[:16]))

    ok = True

    def fail(msg):
        nonlocal ok
        print("FAIL: " + msg)
        ok = False

    if reason_on != 6 or reason_off != 6:
        fail(f"the guest exited with {reason_on} / {reason_off}, not 6 (HLT) -- "
             f"it did not finish writing the register set")
    if writes < 12:
        fail(f"x86_opl_writes is {writes}: fewer than the 12 registers the "
             f"guest program writes reached the card at all")
    if rendered != FM_SAMPLES:
        fail(f"x86_opl_render_mix returned {rendered}, not {FM_SAMPLES} -- "
             f"NO TONE WAS PRODUCED. This is the whole point of the arm: the "
             f"registers were accepted (see the count above) and discarded")
    if not keyons:
        fail("x86_opl_keyons is 0 -- register B0h's key-on bit never reached "
             "the voice, so nothing was ever asked to sound")
    if chip_steps < 1:
        fail("x86_opl_chip_steps is 0: the synthesiser never advanced")
    elif not (0 < slots_run <= 2 * chip_steps):
        fail(f"{slots_run} slot updates over {chip_steps} chip samples. One "
             f"channel is two operators, so this can never exceed twice the "
             f"sample count -- more means a retired or unkeyed voice is being "
             f"synthesised")
    elif slots_run >= 2 * chip_steps:
        fail(f"{slots_run} slot updates over {chip_steps} chip samples: the "
             f"channel was synthesised on EVERY sample, including the ones "
             f"after its release finished. The idle-channel skip is what makes "
             f"the cycle budget work and it never engaged")
    if rhythm_muted:
        fail(f"x86_opl_rhythm_muted is {rhythm_muted}: percussion mode was "
             f"entered, which this program never asks for")

    # ---- THE PITCH. Two independent measures of the same claim. --------
    if abs(zc - zc_expect) > 4:
        fail(f"{zc} zero crossings against the {zc_expect:.1f} the programmed "
             f"f_num and block require. That is "
             f"{zc * FM_OUT_RATE / (2.0 * FM_SAMPLES):.1f} Hz where the chip's "
             f"own formula f_num * {FM_CHIP_HZ} / 2^(20-block) says "
             f"{FM_EXPECT_HZ:.1f} Hz -- an octave out is the classic missing "
             f"or extra shift in the phase increment")
    if not (m_fund > 4.0 * m_oct_up and m_fund > 4.0 * m_oct_dn):
        fail(f"the strongest component is not the programmed note: |X(f)| "
             f"{m_fund:.3e} against |X(2f)| {m_oct_up:.3e} and |X(f/2)| "
             f"{m_oct_dn:.3e}. Zero crossings alone cannot tell a note from "
             f"its octave when the envelope is noisy; this can")

    # ---- THE ENVELOPE, both ends of it. --------------------------------
    if not active_on:
        fail("x86_opl_active() is 0 while the note is held")
    if not (800 <= peak_on <= 1300):
        fail(f"the held note peaks at {peak_on}. One operator at total level 0 "
             f"through the music gain (x86_opl.h) is ~1,012, so a peak far "
             f"under that is an envelope that decayed when EG-TYPE said "
             f"sustain, and one far over is a level or a gain that is wrong")
    if peak_off > 16:
        fail(f"twenty frames after KEY OFF the note still peaks at {peak_off}. "
             f"Release rate 15 is the fastest the chip has and must be over "
             f"inside 333 ms -- a note that never releases is a stuck voice, "
             f"and nine of them is the whole card jammed")
    if active_off:
        fail("the channel is still active after a completed release, so it is "
             "being synthesised for ever and the idle-channel skip -- which is "
             "what makes the cycle budget work -- never engages")

    print("\nFM PASS" if ok else "\nFM FAIL")
    return ok


def main(args):
    runner = build_and_create_runner(args)
    try:
        runner.start()
        b = runner.backend
        syms = get_symbols("test_core.elf")
        for n in ("run_sound_flag", "snd_r"):
            if n not in syms:
                print(f"missing symbol {n} -- rebuild tests/main.c")
                return False
        time.sleep(0.5)

        b.write_uint32(syms["run_sound_flag"], 1)
        for _ in range(600):
            if b.read_uint32(syms["run_sound_flag"]) == 0:
                break
            time.sleep(0.05)
        else:
            print("FAIL: the sound arm never completed")
            return False

        r = [b.read_uint32(syms["snd_r"] + 4 * i) for i in range(16)]
        (davail, polls_left, aa, wstat, major, minor,
         st1, st2, st3) = r[0:9]
        resets, cmds, dma, opl_w, opl_reads, expiry, exit_reason = (
            r[9], r[10], r[11], r[12], r[13], r[14], r[15])

        print(f"DSP reset  0x22E status {davail:#04x}  polls left {polls_left}"
              f"  (0 = the poll ran to its bound and gave up)")
        print(f"           0x22A data   {aa:#04x}   expect 0xaa")
        print(f"           0x22C wstat  {wstat:#04x}   expect bit 7 CLEAR")
        print(f"DSP E1h    version      {major}.{minor:02d}  expect "
              f"{DSP_VERSION[0]}.{DSP_VERSION[1]:02d}")
        print(f"OPL status masked {st1:#04x} / after timer 1 {st2:#04x} / "
              f"reset {st3:#04x}   expect &0xE0 = 0 / 0xC0 / 0")
        print(f"counters   sb_resets {resets} sb_cmds {cmds} dma_refused {dma} "
              f"opl_writes {opl_w} opl_status_reads {opl_reads} "
              f"opl_timer_expiry {expiry}")
        print(f"exit reason {exit_reason}   (6 = HLT)")

        ok = True

        def fail(msg):
            nonlocal ok
            print("FAIL: " + msg)
            ok = False

        # ---- the DSP reset handshake -----------------------------------
        if not (davail & 0x80):
            fail(f"0x22E never reported data available ({davail:#04x}); the "
                 f"guest polled 100 times and gave up. A card that resets but "
                 f"never says so is indistinguishable from an absent one")
        if polls_left == 0:
            fail("the data-available poll ran to its full bound -- the answer "
                 "arrived late or not at all")
        if aa != 0xAA:
            fail(f"0x22A returned {aa:#04x}, not 0xAA. THIS IS THE HANDSHAKE: "
                 f"0xFF here is the unmodelled port floating high, which is "
                 f"the state this work exists to fix")
        if wstat & 0x80:
            fail(f"0x22C reports the DSP BUSY ({wstat:#04x}). A guest waits for "
                 f"bit 7 to clear before every command byte -- 0xFF here is an "
                 f"infinite wait, which is worse than an absent card")
        if (major, minor) != DSP_VERSION:
            fail(f"DSP command E1h returned version {major}.{minor}, expected "
                 f"{DSP_VERSION[0]}.{DSP_VERSION[1]:02d} -- either the reply "
                 f"bytes are in the wrong order or the command was swallowed "
                 f"as an argument to the one before it")

        # ---- the AdLib timer test --------------------------------------
        if st1 & 0xE0:
            fail(f"with both timers masked and the flags reset the OPL status "
                 f"reads {st1:#04x}; a detection routine requires &0xE0 == 0 "
                 f"here and rejects the card outright when it is not")
        if (st2 & 0xE0) != 0xC0:
            fail(f"after timer 1 expired the OPL status reads {st2:#04x}, "
                 f"expected &0xE0 == 0xC0. 0xFF is the floating port WOLF3D "
                 f"and KEEN4 spin on 348 times; 0x06 is a card whose timer "
                 f"never fires, which fails detection just as hard")
        if st3 & 0xE0:
            fail(f"the flags did not clear on an IRQ-reset write ({st3:#04x}) "
                 f"-- a status bit that cannot go down is not a flag, and the "
                 f"second half of every detection sequence checks exactly this")

        # ---- the counters have to agree with the story -----------------
        if resets != 1:
            fail(f"x86_sb_resets is {resets}, expected exactly 1")
        if cmds < 1:
            fail("x86_sb_cmds is 0 -- the E1h command byte never reached the "
                 "DSP, so the version above came from somewhere else")
        if dma != 0:
            fail(f"x86_sb_dma_refused is {dma}: this arm started a DMA "
                 f"transfer, which it must not -- there is no audio path")
        if expiry < 1:
            fail("x86_opl_timer_expiry is 0 -- no timer ever fired, so any "
                 "0xC0 above did not come from a timer")
        if opl_reads < 2048:
            fail(f"only {opl_reads} OPL status reads for a 2048-iteration spin "
                 f"-- the guest program is not running what it claims")
        if exit_reason != 6:
            fail(f"the guest exited with reason {exit_reason}, not 6 (HLT) -- "
                 f"it did not reach the end of the program")

        print("\nDETECTION PASS" if ok else "\nDETECTION FAIL")

        # The playback arm runs on the SAME target: rebuilding for it would
        # double every negative control's cost for nothing.
        ok = playback(args, runner) and ok
        ok = adpcm(args, runner) and ok
        ok = fm(args, runner) and ok

        print("\nPASS" if ok else "\nFAIL")
        return ok
    finally:
        runner.stop()


# The line that IS the handshake. Deleting it restores the defect exactly: the
# reset is still accepted, the buffer is still emptied, and 0x22A floats.
MUTATE_AA = "    dsp_push(0xAAu);                  /* THE handshake: \"a card is here\"   */\n"

# The OPL half: timer 1's flag. Without it the status can never come up, which
# is the state WOLF3D and KEEN4 measured 348 reads against.
MUTATE_T1 = """    if (opl.t1_run && !(opl.ctrl & 0x40u) &&
        (uint32_t)(now - opl.t1_start) >=
            (uint32_t)(256u - opl.t1_preset) * OPL_T1_STEP)
        f |= 0x40u;
"""


# ---------------------------------------------------------------------------
# THE PLAYBACK CONTROLS. Each deletes ONE link in the chain and requires the
# test to notice -- a green suite over a broken engine is the failure mode these
# exist to make impossible.

# TERMINAL COUNT. Without the borrow test the count never reaches zero, the DMA
# walks past the end of the guest's buffer and no block ever completes.
MUTATE_TC = """    if (c->cur_count-- == 0u) {        /* the borrow IS terminal count      */
        c->tc = 1;
        if (c->mode & 0x10u) {         /* auto-init: reload from the latches */
            c->cur_addr  = c->base_addr;
            c->cur_count = c->base_count;
        } else {
            c->masked = 1;
        }
        sb_win_drop();                 /* the address just jumped           */
    }
"""

# IRQ DELIVERY. The line still comes up and the counters still move; what does
# not happen is the guest's handler running. This is exactly the shape of the
# hang the task is about, so the test MUST catch it.
MUTATE_IRQ = """    pic_isr |= 0x20u;
    cpu->hard_int_pending = 0x0Du;    /* IRQ 5 -> vector 0Dh                */
    x86_sb_irqs_delivered++;
"""

# RATE CONVERSION. Replacing the fractional step with a whole source sample per
# output sample plays the buffer six times too fast: every counter stays green
# and only the SAMPLES say so.
MUTATE_RATE = ("    uint32_t step = (uint32_t)(((uint64_t)sbp.rate << 16) "
               "/ sample_rate);\n")
MUTATE_RATE_TO = "    uint32_t step = 0x10000u;\n"

# THE F2h IRQ REQUEST ITSELF. Without it the command falls through to default
# and is counted unknown, which is the state PRINCE OF PERSIA meets today.
MUTATE_F2 = """    case 0xF2:
    case 0xF3: {"""
MUTATE_F2_TO = """    case 0xF2u & 0u:              /* never matches 0xF2 */
    case 0xF3u & 1u: {"""

# THE IMMEDIATE DELIVERY, and NOT the assert. Every counter stays green -- the
# line is raised, the kind bits are right, the request is counted -- and only
# the guest's ten-iteration window catches that it arrived too late.
MUTATE_F2_NOW = "            x86_pit_counter_forced(1);\n        break;\n    }\n"
MUTATE_F2_NOW_TO = "            (void)0;\n        break;\n    }\n"


# ---------------------------------------------------------------------------
# THE FM CONTROLS. Both live in x86_opl.c, and each breaks ONE of the two
# things the arm above measures independently.

# THE ENVELOPE'S RELEASE, in the synthesiser itself. Without the state change
# a keyed-off operator never leaves the stage it was in, so the note sustains
# for ever: the PITCH assertions still pass, x86_opl_keyons still counts, and
# only the key-off peak and the idle-channel skip say so.
#
# BOTH FM CONTROLS MUTATE THE VENDORED lib/emu8950/emu8950.c. That is the point
# -- they grade the code that actually makes the sound, not our adapter around
# it -- and the file is restored in the same `finally` every other control uses.
MUTATE_FM_ENV = """static INLINE void slotOff(OPL *opl, int i) {
    OPL_SLOT *slot = &opl->slot[i];
    slot->eg_state = RELEASE;
    request_update(slot, UPDATE_EG);
}"""
MUTATE_FM_ENV_TO = """static INLINE void slotOff(OPL *opl, int i) {
    OPL_SLOT *slot = &opl->slot[i];
    request_update(slot, UPDATE_EG);
}"""

# THE FREQUENCY. ml_table is the frequency-multiple the phase increment is
# scaled by; doubling it puts every note an octave up. Every counter stays
# green, the envelope is untouched, and the tune is simply in the wrong key --
# exactly the class of defect a counter-only test cannot see.
MUTATE_FM_FREQ = ("static uint32_t ml_table[16] = {1, 1 * 2, 2 * 2, 3 * 2, "
                  "4 * 2, 5 * 2, 6 * 2, 7 * 2,\n")
MUTATE_FM_FREQ_TO = ("static uint32_t ml_table[16] = {2, 1 * 4, 2 * 4, 3 * 4, "
                     "4 * 4, 5 * 4, 6 * 4, 7 * 4,\n")


# ---- the ADPCM controls -------------------------------------------------
#
# THE STUB, restored. Before this work every ADPCM command called sb_start()
# with silent=1: the transfer ran, the block completed, the interrupt was
# raised, and the samples were zero. This control puts 0x75 back on that path,
# so the arm must reject it -- both through x86_sb_adpcm_silenced and, more to
# the point, through the samples themselves.
MUTATE_ADPCM = "        sb_start_fmt(0, 0, 0, 0, 0, dsp_len() + 1u, 4, 1);\n"
MUTATE_ADPCM_TO = "        sb_start(0, 0, 0, 0, 1, dsp_len() + 1u);\n"

# THE REFERENCE BYTE, never consumed as a reference. The first byte of the
# block is then decoded as ordinary codes and the predictor starts at
# mid-scale. Every counter stays green, the output is NOT silent and NOT a
# constant -- so only the sample-exact comparison can see it. This is the exact
# failure mode the arm's three-layer expectation exists to separate.
MUTATE_ADPCM_REF = "    sbp.adpcm_ref_pending = adpcm_fmt ? ref : 0u;\n"
MUTATE_ADPCM_REF_TO = "    sbp.adpcm_ref_pending = 0u;\n"

# THE 2.6-BIT PACKING. Its third code is the byte's low TWO bits shifted UP one
# so a 2-bit field indexes a 3-bit table; dropping the shift is the classic
# error and it changes only every third sample.
MUTATE_ADPCM_PACK = """        sbp.adpcm_out[2] = adpcm_step_one((uint8_t)((b & 0x03u) << 1),
"""
MUTATE_ADPCM_PACK_TO = """        sbp.adpcm_out[2] = adpcm_step_one((uint8_t)(b & 0x03u),
"""

# THE BLOCK LENGTH UNIT. An ADPCM block is counted in DMA BYTES; ticking it per
# OUTPUT sample instead interrupts two to four times too early and stops the
# DMA short. The waveform's first 64 samples survive that, so this one is
# caught by dma_bytes and the block count -- a different layer again.
MUTATE_ADPCM_BLK = """            adpcm_expand((uint8_t)u);
            sb_block_tick();
        }
"""
MUTATE_ADPCM_BLK_TO = """            adpcm_expand((uint8_t)u);
        }
        sb_block_tick();
"""


def _mutate_and_require_failure(args, target, what, replacement="",
                                path="x86_io.c"):
    import pathlib
    src = pathlib.Path(path)
    original = src.read_text()
    # AN OCCURRENCE ASSERT, not a presence check. A target that matches twice
    # patches whichever copy comes first, which is how a control ends up
    # mutating something other than what it names -- and a control that
    # mutates nothing passes for free.
    n = original.count(target)
    if n != 1:
        print(f"FAIL: the mutation target ({what}) occurs {n} times in "
              f"{path}, expected exactly 1 -- this control would mutate the "
              f"wrong thing, or nothing at all")
        return False
    try:
        src.write_text(original.replace(target, replacement, 1))
        verb = "REPLACED" if replacement else "DELETED"
        print(f"-- negative control: rebuilt with {what} {verb} --")
        for a in ("negative", "negative_opl", "negative_tc",
                  "negative_irq", "negative_rate", "negative_f2",
                  "negative_f2_now", "negative_fm_env", "negative_fm_freq",
                  "negative_adpcm", "negative_adpcm_ref",
                  "negative_adpcm_pack", "negative_adpcm_blk"):
            setattr(args, a, False)
        failed = not main(args)
    finally:
        src.write_text(original)
        print(f"-- {path} restored --")
    if failed:
        print("\nPASS (negative control): the test rejects the defective core")
        return True
    print("\nFAIL (negative control): the test PASSED without the fix, so it "
          "is not measuring anything")
    return False


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    add_runner_args(p)
    p.add_argument("--negative", action="store_true",
                   help="rebuild with the DSP's 0xAA push deleted, require FAIL")
    p.add_argument("--negative-opl", action="store_true",
                   help="rebuild with the OPL timer-1 flag deleted, require FAIL")
    p.add_argument("--negative-tc", action="store_true",
                   help="rebuild with the 8237's terminal count deleted")
    p.add_argument("--negative-irq", action="store_true",
                   help="rebuild with IRQ 5 delivery deleted")
    p.add_argument("--negative-rate", action="store_true",
                   help="rebuild with the resampler step forced to 1:1")
    p.add_argument("--negative-f2", action="store_true",
                   help="rebuild with the DSP's F2h IRQ request removed")
    p.add_argument("--negative-f2-now", action="store_true",
                   help="rebuild with F2h's IMMEDIATE delivery removed -- the "
                        "line is still asserted, so every counter stays green "
                        "and only the guest's short window catches it")
    p.add_argument("--negative-fm-env", action="store_true",
                   help="rebuild with the FM envelope's RELEASE deleted")
    p.add_argument("--negative-adpcm", action="store_true",
                   help="rebuild with 4-bit ADPCM back on the SILENT stub")
    p.add_argument("--negative-adpcm-ref", action="store_true",
                   help="rebuild with the ADPCM REFERENCE BYTE never consumed "
                        "as a reference -- output stays non-silent and "
                        "non-constant, so only the sample-exact arm sees it")
    p.add_argument("--negative-adpcm-pack", action="store_true",
                   help="rebuild with the 2.6-bit format's third code read "
                        "without its shift")
    p.add_argument("--negative-adpcm-blk", action="store_true",
                   help="rebuild with the ADPCM block ticked per OUTPUT "
                        "sample instead of per DMA byte")
    p.add_argument("--negative-fm-freq", action="store_true",
                   help="rebuild with the FM phase increment an octave out")
    args = p.parse_args()
    if args.negative:
        sys.exit(0 if _mutate_and_require_failure(
            args, MUTATE_AA, "the DSP reset's 0xAA") else 1)
    if args.negative_opl:
        sys.exit(0 if _mutate_and_require_failure(
            args, MUTATE_T1, "the OPL timer 1 expiry flag") else 1)
    if args.negative_tc:
        sys.exit(0 if _mutate_and_require_failure(
            args, MUTATE_TC, "the 8237's TERMINAL COUNT") else 1)
    if args.negative_irq:
        sys.exit(0 if _mutate_and_require_failure(
            args, MUTATE_IRQ, "IRQ 5 DELIVERY") else 1)
    if args.negative_rate:
        sys.exit(0 if _mutate_and_require_failure(
            args, MUTATE_RATE, "the RESAMPLER STEP", MUTATE_RATE_TO) else 1)
    if args.negative_f2:
        sys.exit(0 if _mutate_and_require_failure(
            args, MUTATE_F2, "the DSP's F2h IRQ REQUEST",
            MUTATE_F2_TO) else 1)
    if args.negative_f2_now:
        sys.exit(0 if _mutate_and_require_failure(
            args, MUTATE_F2_NOW, "F2h's IMMEDIATE DELIVERY",
            MUTATE_F2_NOW_TO) else 1)
    if args.negative_adpcm:
        sys.exit(0 if _mutate_and_require_failure(
            args, MUTATE_ADPCM, "4-bit ADPCM back on the SILENT STUB",
            MUTATE_ADPCM_TO) else 1)
    if args.negative_adpcm_ref:
        sys.exit(0 if _mutate_and_require_failure(
            args, MUTATE_ADPCM_REF, "the ADPCM REFERENCE BYTE",
            MUTATE_ADPCM_REF_TO) else 1)
    if args.negative_adpcm_pack:
        sys.exit(0 if _mutate_and_require_failure(
            args, MUTATE_ADPCM_PACK, "the 2.6-bit ADPCM PACKING",
            MUTATE_ADPCM_PACK_TO) else 1)
    if args.negative_adpcm_blk:
        sys.exit(0 if _mutate_and_require_failure(
            args, MUTATE_ADPCM_BLK, "the ADPCM BLOCK LENGTH UNIT",
            MUTATE_ADPCM_BLK_TO) else 1)
    if args.negative_fm_env:
        sys.exit(0 if _mutate_and_require_failure(
            args, MUTATE_FM_ENV, "the FM envelope's RELEASE",
            MUTATE_FM_ENV_TO, path="lib/emu8950/emu8950.c") else 1)
    if args.negative_fm_freq:
        sys.exit(0 if _mutate_and_require_failure(
            args, MUTATE_FM_FREQ, "the FM FREQUENCY MULTIPLE", MUTATE_FM_FREQ_TO,
            path="lib/emu8950/emu8950.c") else 1)
    sys.exit(0 if main(args) else 1)
