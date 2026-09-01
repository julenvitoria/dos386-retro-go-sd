#!/usr/bin/env python3
"""
STATIC opcode demand scan: what does a title's binary actually CONTAIN, and
does our core have a correct handler for it?

Why this exists
---------------
Coverage work on the interpreter has been driven by SERIAL DISCOVERY: run a
title, hit `guest_exit_reason=4`, read `exit_info`, implement that one opcode,
run again. That costs one full tier-2 round trip per opcode and it only ever
finds the FIRST thing in the way. This answers the whole question in one pass:
disassemble the shipped binaries, count every opcode and every group `/reg`
sub-opcode, and cross-reference the result against what the core actually has.

Status comes from `scripts/gen_opcode_coverage.py`, which parses the two
dispatch tables and the handler bodies. It does NOT come from the prose in
`docs/07_opcode_coverage.md`: that column was hand-maintained, it rotted twice,
and it still listed opcodes as broken that had been fixed hundreds of commits
earlier. A scan that inherits a stale status column produces confident wrong
answers at 512 slots at once.

Two statuses are reported. `missing` means the dispatch slot goes to
`.exit_loop` -- the guest will stop dead on it, and this is hard fact. `REVIEW`
means the handler exists and a mechanical marker fired on its body (no width
primitive, open-coded stack, ModRM decoder called by symbol). `REVIEW` is a
prompt to read the handler, never a verdict that it is wrong.

HOW MUCH TO TRUST A ROW -- read this before quoting a number
------------------------------------------------------------
This is a LINEAR SWEEP. It starts at the load module's first byte and decodes
forward, so the instant it walks into data -- a jump table, a string, a
relocation blob, padding -- it decodes garbage and stays out of phase until it
happens to resynchronise. It cannot know which is which.

Therefore:

  * A HIGH count across MULTIPLE binaries is STRONG evidence. Real code is the
    bulk of an .EXE image, mis-decode noise is roughly uniform over 256 slots,
    and a compiler emits the same handful of forms constantly. Anything in the
    hundreds is real.
  * A count of 1--3 is WEAK evidence and may be pure noise. It is reported, but
    it must not be actioned on its own.
  * A count is an occurrence in the IMAGE, never an execution count. A hot loop
    is a handful of bytes; a never-called error path may be a kilobyte. Static
    demand ranks what a title COULD need, and dynamic profiling (`probe.py`,
    `profile_opcodes.py`) ranks what it DOES need. They answer different
    questions and neither substitutes for the other.

The `--min-count` and the `weak` marker in the output exist so that this
caveat travels with the numbers rather than being dropped in the retelling.

Usage
-----
    # rank every opcode Epic Pinball's binaries contain, 16-bit real mode
    python3 scripts/opcode_demand.py --dsk EPIC_PINBALL --bits 16

    # Dark Forces: the extender's load image is 32-bit protected-mode code
    python3 scripts/opcode_demand.py --dsk STAR_WARS_DARKFORCES \\
        --file DF/DOS4GW.EXE --bits 32 --pmode

    # just the rows where we are missing or wrong
    python3 scripts/opcode_demand.py --dsk EPIC_PINBALL --bits 16 --problems

`--dsk` takes a bare title name (resolved under the roms tree) or a path.
`--file` may be repeated; omitted, it scans every `.EXE` in the volume.
"""

import argparse
import collections
import io
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from common.dosimage import MZ, FatVolume          # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROMS = os.path.join(ROOT, "references",
                    "game-and-watch-retro-go-sd", "roms", "dos")
# references/ is untracked, so a worktree may not carry it; fall back to the
# main checkout, which is where the images actually live.
if not os.path.isdir(ROMS):
    ROMS = os.path.join("/home/doug/Nerd/git/tinyemu", "references",
                        "game-and-watch-retro-go-sd", "roms", "dos")

# Group opcodes: a count against the bare opcode says nothing, because the
# eight /reg members are eight different instructions with eight different
# statuses. These are split out per /reg.
GROUPS = {
    0x80: "Grp1", 0x81: "Grp1", 0x82: "Grp1", 0x83: "Grp1",
    0xC0: "Grp2", 0xC1: "Grp2", 0xD0: "Grp2", 0xD1: "Grp2",
    0xD2: "Grp2", 0xD3: "Grp2",
    0xF6: "Grp3", 0xF7: "Grp3", 0xFE: "Grp4", 0xFF: "Grp5",
}
GROUPS_0F = {0x00: "Grp6", 0x01: "Grp7", 0xBA: "Grp8"}

# Sub-opcode reachability is derived from the CMP/BEQ group dispatch chains by
# gen_opcode_coverage.py. Grp1/Grp2 use an 8-entry TBH and cannot fall through;
# Grp3/Grp4/Grp5 use a chain, and any /reg the chain does not name exits.
GROUP_CHAIN = {"Grp3": ("GRP3_F6_DISPATCH", "GRP3_F7_DISPATCH"),
               "Grp4": ("GRP4_DISPATCH",), "Grp5": ("GRP5_DISPATCH",)}

# Protected-mode instructions Task 2 cares about: what a scan must call out
# whatever its count, because one occurrence of LTR is architecturally decisive.
PMODE_1BYTE = {0x63: "ARPL Ew,Gw", 0x9A: "CALL far ptr16:v",
               0xCA: "RETF Iw", 0xCB: "RETF", 0xCF: "IRET",
               0xEA: "JMP far ptr16:v", 0xCD: "INT Ib", 0xCC: "INT3",
               0xF4: "HLT"}
PMODE_0F = {0x00: "Grp6 SLDT/STR/LLDT/LTR/VERR/VERW", 0x01: "Grp7",
            0x02: "LAR Gv,Ew", 0x03: "LSL Gv,Ew", 0x06: "CLTS",
            0x07: "LOADALL", 0x20: "MOV Rd,CRn", 0x22: "MOV CRn,Rd",
            0x21: "MOV Rd,DRn", 0x23: "MOV DRn,Rd", 0xB2: "LSS",
            0xB4: "LFS", 0xB5: "LGS", 0xA1: "POP FS", 0xA9: "POP GS"}


def parse_table(path):
    """opcode_table.S / opcode_0f_table.S -> {slot: handler symbol}."""
    out = {}
    pat = re.compile(r"\.word\s+(\S+)\s*\+\s*1\s*//\s*(0x[0-9a-fA-F]+)")
    with open(path) as f:
        for line in f:
            m = pat.search(line)
            if m:
                out[int(m.group(2), 16)] = m.group(1)
    return out


def parse_doc(path):
    """Mnemonics only, from docs/07's hand-written tables if they are present.

    **The status column of that document is NOT used and must not be.** It was
    hand-maintained, it rotted twice, and it still lists as DEFECT several
    opcodes that were repaired hundreds of commits ago. Status here comes from
    `scripts/gen_opcode_coverage.py`, i.e. from the dispatch tables and the
    handler bodies themselves. This function survives only to put a readable
    mnemonic next to an opcode number.
    """
    out = {}
    which = None
    row = re.compile(r"^\|\s*([0-9A-Fa-f]{2})\s*\|([^|]*)\|\s*([A-Za-z]+)\s*\|"
                     r"([^|]*)\|([^|]*)\|")
    with open(path) as f:
        for line in f:
            if line.startswith("## One-byte opcodes"):
                which = "1"
            elif line.startswith("## Two-byte (0F) opcodes"):
                which = "0f"
            elif line.startswith("## ") and which:
                which = None
            if not which:
                continue
            m = row.match(line)
            if m:
                out[(which, int(m.group(1), 16))] = (
                    m.group(3).strip(), m.group(2).strip(), m.group(5).strip())
    return out


PREFIXES = {0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67,
            0xF0, 0xF2, 0xF3}


def decode_stream(data, bits, base=0):
    """Linear sweep. Yields (opcode_key, modrm_reg_or_None) per instruction.

    opcode_key is ('1', op) or ('0f', op). Prefixes are consumed and not
    counted as instructions in their own right -- they are counted separately
    by the caller so `66`/`67` demand is still visible.
    """
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86,
                     capstone.CS_MODE_16 if bits == 16 else capstone.CS_MODE_32)
    md.detail = False
    pos = 0
    n = len(data)
    while pos < n:
        try:
            insns = list(md.disasm(data[pos:], base + pos, count=256))
        except Exception:
            insns = []
        if not insns:
            pos += 1
            continue
        for ins in insns:
            b = ins.bytes
            i = 0
            while i < len(b) and b[i] in PREFIXES:
                yield ("pfx", b[i]), None, "prefix", ins.address - base
                i += 1
            if i >= len(b):
                break
            if b[i] == 0x0F and i + 1 < len(b):
                op, kind, i = b[i + 1], "0f", i + 2
            else:
                op, kind, i = b[i], "1", i + 1
            reg = ((b[i] >> 3) & 7) if i < len(b) else None
            yield (kind, op), reg, ins.mnemonic, ins.address - base
        pos = insns[-1].address - base + insns[-1].size


def status_for(key, reg, cov, chains, tbl1, tbl0f, doc):
    """Status from the GENERATED coverage facts, never from docs/07's prose."""
    kind, op = key
    groups = GROUPS if kind == "1" else GROUPS_0F
    mnem = doc.get((kind, op), ("", "", ""))[1] or \
        (tbl1 if kind == "1" else tbl0f).get(op, "?")

    if op in groups and reg is not None:
        g = groups[op]
        label = "%s /%d" % (g, reg)
        # If the whole group's table slot exits, no /reg is reachable.
        if (tbl1 if kind == "1" else tbl0f).get(op) == ".exit_loop":
            return "missing", label, "whole %s slot is .exit_loop" % (
                "0F %02X" % op if kind == "0f" else "%02X" % op)
        for cname in GROUP_CHAIN.get(g, ()):
            regs = chains.get(cname)
            if regs is None:
                continue
            if reg not in regs:
                return "missing", label, ("%s does not name /%d; it falls to "
                                          ".exit_loop" % (cname, reg))
        return "ok", label, ""

    k = ("0F%02X" if kind == "0f" else "%02X") % op
    r = cov.get(k)
    if r is None:
        return "ok", mnem, ""
    if not r["reached"]:
        return "missing", mnem, "dispatch slot is .exit_loop"
    if r["warnings"]:
        real = [w for w in r["warnings"] if w.startswith("REVIEW")]
        if real:
            return "REVIEW", mnem, "; ".join(real)
    return "ok", mnem, ""


def name_of(key, reg, doc):
    kind, op = key
    if kind == "pfx":
        return "prefix %02X" % op
    groups = GROUPS if kind == "1" else GROUPS_0F
    base = doc.get((kind, op), ("", "", ""))[1] or "?"
    if op in groups and reg is not None:
        return "%s /%d" % (base, reg)
    return base


def scan_file(name, blob, bits, raw, from_entry=True):
    """Returns (code bytes, note). Skips the MZ header, and by default starts
    at the entry point rather than at image byte 0.

    Starting at byte 0 is what a naive sweep does and it is badly wrong for
    real DOS binaries. Epic Pinball's EP*.EXE are the extreme case: each is
    ~250 KB whose first ~200 KB is a data/BSS blob that begins with the literal
    string "STACK   " repeated, with only the last ~40 KB being code at
    CS:IP. Sweeping from byte 0 buries the answer in mis-decoded data --
    it reports 19,000 `DAS` because 0x2F is a common data byte, not because
    Epic Pinball does packed BCD.
    """
    if not raw and blob[:2] in (b"MZ", b"ZM"):
        mz = MZ(blob)
        img = mz.image()
        ep = mz.cs * 16 + mz.ip
        if from_entry and 0 < ep < len(img):
            return img[ep:], ("MZ, load module %d B, entry %04X:%04X = +%d; "
                              "scanning the %d B FROM THE ENTRY POINT"
                              % (len(img), mz.cs, mz.ip, ep, len(img) - ep))
        return img, "MZ, load module %d B, entry %04X:%04X" % (
            len(img), mz.cs, mz.ip)
    return blob, "raw, %d B" % len(blob)


def null_model(code, bits, seed=1234):
    """Decode a SHUFFLED copy of the same bytes and count the same way.

    Shuffling destroys every instruction boundary while preserving the byte
    frequency exactly, so the result is what this sweep would report if the
    region were pure data with these bytes in it. Subtracting it turns a raw
    count into an ENRICHMENT: an opcode that appears far more often in the real
    byte order than in the shuffled one is being decoded from genuine code.

    This is the quantitative form of the caveat at the top of the file. It does
    not make a count-of-1 trustworthy; it does separate "0x2F appears 19,000
    times because 0x2F is a common data byte" from "0x2F is really DAS here".
    """
    import random
    rnd = random.Random(seed)
    shuf = bytearray(code)
    rnd.shuffle(shuf)
    out = collections.Counter()
    for key, reg, _mn, _a in decode_stream(bytes(shuf), bits):
        kind, op = key
        groups = GROUPS if kind == "1" else GROUPS_0F
        out[(kind, op, reg if op in groups else None)] += 1
    return out


GRP6_NAMES = ["SLDT", "STR", "LLDT", "LTR", "VERR", "VERW", "inv/6", "inv/7"]
GRP7_NAMES = ["SGDT", "SIDT", "LGDT", "LIDT", "SMSW", "inv/5", "LMSW", "INVLPG"]


def privilege_census(blob, bits):
    """Exhaustive BYTE-LEVEL search for the privilege-relevant opcodes.

    This deliberately does NOT depend on the linear sweep's phase. The sweep
    can walk out of phase for a whole kilobyte and miss the one `LTR` in the
    file, and for the question this is built to answer -- does a DOS extender
    ever change privilege ring? -- a single genuine occurrence is decisive
    while a single absence is decisive the other way. So every `0F xx` pair in
    the file is examined, and each hit is then re-disassembled AT that offset
    in both widths so an accidental byte pair can be told from a real
    instruction.

    ABSENCE is the strong result here. A false positive shows up as a hit that
    disassembles to nonsense; a false negative is impossible, because the byte
    pair simply is not in the file.
    """
    import capstone
    dis = {b: capstone.Cs(capstone.CS_ARCH_X86,
                          capstone.CS_MODE_16 if b == 16 else capstone.CS_MODE_32)
           for b in (16, 32)}
    hits = collections.defaultdict(list)
    n = len(blob)
    for i in range(n - 2):
        if blob[i] != 0x0F:
            continue
        o = blob[i + 1]
        reg = (blob[i + 2] >> 3) & 7
        if o == 0x00:
            key = "0F 00 /%d %s" % (reg, GRP6_NAMES[reg])
        elif o == 0x01:
            key = "0F 01 /%d %s" % (reg, GRP7_NAMES[reg])
        elif o in (0x02, 0x03, 0x06, 0x07, 0x20, 0x21, 0x22, 0x23):
            key = "0F %02X %s" % (o, {0x02: "LAR", 0x03: "LSL", 0x06: "CLTS",
                                      0x07: "LOADALL", 0x20: "MOV r32,CRn",
                                      0x21: "MOV r32,DRn", 0x22: "MOV CRn,r32",
                                      0x23: "MOV DRn,r32"}[o])
        else:
            continue
        txt = ""
        for w in (bits, 16 if bits == 32 else 32):
            g = list(dis[w].disasm(blob[i:i + 12], 0, count=1))
            if g:
                txt = "%d-bit: %s %s" % (w, g[0].mnemonic, g[0].op_str)
                break
        hits[key].append((i, txt))
    return hits


# ---------------------------------------------------------------------------
# LIBRARY-WIDE SCAN (--library)
#
# The per-title invocation above answers "what does THIS binary demand". This
# answers "what does the SHIPPED LIBRARY demand", which is the question that
# actually ranks work. Three things make it more than a for-loop:
#
#  1. FORMAT CLASSIFICATION. The roms tree is not all DOS real-mode MZ. It also
#     holds Win32 PE (SCRIPTIN.EXE, JAZZ2.EXE), Win16 NE (Tyrian's installer,
#     all of WIN311), a Win386 W3 bundle, and DOS/4GW-bound LINEAR EXECUTABLES
#     (Dark Forces, Mortal Kombat). Feeding any of those to a 16-bit sweep
#     produces confident garbage. Each file is classified, and the non-DOS ones
#     are EXCLUDED WITH A REASON rather than silently scanned.
#
#  2. THE FORMAT PICKS THE WIDTH, per REGION rather than per file. An LE's code
#     lives in objects flagged executable, and the object's BIG bit says
#     whether it is 32-bit. That is a fact recorded in the file, not a guess,
#     and it is the only reliable way to avoid decoding 32-bit code as 16-bit.
#
#  3. A RAW SIGNATURE SEARCH FOR "LE\0\0" IS NOT A FORMAT CHECK. Searching
#     PRINCE.EXE for that byte pair "finds" an LE header at +111628 whose page
#     size reads 0x414f4259 -- it is four bytes of graphics data. The detector
#     here follows e_lfanew and then VALIDATES (page size 4096, sane object
#     count, object table inside the file). Epic Pinball's EP*.EXE fail that
#     check and are plain 16-bit MZ, which is what the earlier lane concluded
#     by other means.
# ---------------------------------------------------------------------------

LE_EXEC, LE_BIG = 0x4, 0x2000

# x87 ESC. These slots are NOT missing: they reach `.op_esc_x87`, the
# no-coprocessor stub, which counts into `x86_x87_escapes` and advances EIP.
X87 = set(range(0xD8, 0xE0))


def le_valid(blob, off):
    """Is there a REAL linear-executable header at `off`?

    This is the whole difference between finding MK1.EXE's payload and
    "finding" one in PRINCE.EXE. Both contain the byte pair `LE\\0\\0`; only
    one of them has a 4096-byte page size, a plausible object count and an
    object table that lies inside the file. Prince of Persia's candidate reads
    a page size of 0x414f4259 -- four bytes of graphics data.
    """
    if off + 0x84 > len(blob) or blob[off:off + 2] not in (b"LE", b"LX"):
        return False
    psize, = struct.unpack_from("<I", blob, off + 0x28)
    objoff, objcnt = struct.unpack_from("<II", blob, off + 0x40)
    dpo, = struct.unpack_from("<I", blob, off + 0x80)
    return (psize == 0x1000 and 0 < objcnt < 64 and objoff > 0
            and off + objoff + objcnt * 24 <= len(blob)
            and 0 < dpo <= len(blob))


def exe_kind(blob):
    """(kind, detail, le_off). kind in MZ, LE, LX, NE, PE, W3, W4, COM, TINY."""
    if len(blob) < 0x40:
        return "TINY", "%d B" % len(blob), 0
    if blob[:2] not in (b"MZ", b"ZM"):
        return "COM", "no MZ magic", 0
    lfa = struct.unpack_from("<I", blob, 0x3C)[0]
    # A pre-Windows MZ has no e_lfanew: offset 0x3C is inside the relocation
    # table or the load module, so the dword there is arbitrary. Bound-check
    # before believing it.
    if 0x40 <= lfa <= len(blob) - 4:
        sig = blob[lfa:lfa + 2]
        if sig in (b"NE", b"PE", b"W3", b"W4"):
            return sig.decode(), "@%d" % lfa, 0
        if le_valid(blob, lfa):
            return sig.decode(), "@%d (e_lfanew)" % lfa, lfa
    # A DOS/4GW-bound game built with an OLD linker has a stub whose 0x3C is
    # not an e_lfanew at all -- MK1.EXE reads 162,791,424 there -- yet the LE
    # payload really is appended. Fall back to a signature search, but accept
    # only a candidate that passes le_valid(); that is what keeps PRINCE.EXE
    # out. Missing this cost the first library scan Mortal Kombat's entire
    # 32-bit game image, leaving only the extender stub.
    i = 0x40
    while True:
        i = min((x for x in (blob.find(b"LE\x00\x00", i),
                             blob.find(b"LX\x00\x00", i)) if x >= 0),
                default=-1)
        if i < 0:
            break
        if le_valid(blob, i):
            return blob[i:i + 2].decode(), "@%d (appended payload)" % i, i
        i += 1
    return "MZ", "plain DOS load module", 0


def le_code_regions(blob, lfa):
    """[(label, bytes, bits)] for each EXECUTABLE object of an LE/LX image.

    Data objects are excluded outright, which is a much better filter than the
    entry-point heuristic an MZ needs: the format itself records which pages
    are code, so no data blob is left for the sweep to mis-decode.
    """
    sig = blob[lfa:lfa + 2]
    psize, = struct.unpack_from("<I", blob, lfa + 0x28)
    shift, = struct.unpack_from("<I", blob, lfa + 0x2C)
    objoff, objcnt = struct.unpack_from("<II", blob, lfa + 0x40)
    mapoff, = struct.unpack_from("<I", blob, lfa + 0x48)
    datapages, = struct.unpack_from("<I", blob, lfa + 0x80)
    out = []
    for i in range(objcnt):
        o = lfa + objoff + i * 24
        vsize, base, flags, pmi, pmc, _r = struct.unpack_from("<6I", blob, o)
        if not (flags & LE_EXEC):
            continue
        bits = 32 if (flags & LE_BIG) else 16
        buf = bytearray()
        for p in range(pmc):
            e = lfa + mapoff + (pmi - 1 + p) * 4
            if e + 4 > len(blob):
                break
            if sig == b"LE":
                num = (blob[e] << 16) | (blob[e + 1] << 8) | blob[e + 2]
                off, n = datapages + (num - 1) * psize, psize
            else:
                po, n, _fl = struct.unpack_from("<IHH", blob, e)
                off = datapages + (po << shift)
            buf += blob[off:off + n]
        if buf:
            out.append(("obj%d exec %d-bit base %#x" % (i + 1, bits, base),
                        bytes(buf), bits))
    return out


def regions_for(blob, from_entry=False):
    """([(label, code, bits)], note). An empty list means excluded, and the
    note says why -- an exclusion is a result, not a silent skip.

    ON WHERE TO START A 16-BIT MZ. The earlier lane's rule was "start at
    CS:IP", and for Epic Pinball that is right: EP*.EXE is ~200 KB of data
    followed by ~45 KB of code, and sweeping from byte 0 reported 19,000 DAS.
    But it is the WRONG rule for most of this library, because the majority
    of these binaries are Borland or Microsoft C images whose startup stub sits
    at the END of the module with the game's code BEFORE it. Applied to
    WOLF3D.EXE it scans 2,793 bytes of a 109,927-byte module; to KEEN4E.EXE
    3,414 of 105,076; to PRINCE.EXE 3,461 of 122,823. Ninety-seven per cent of
    each title, discarded.

    So the default here is the WHOLE load module, and the data that gets swept
    with it is handled where it should be -- by the two controls, the shuffle
    null model and `ctx_diversity`, which is exactly what they are for. The
    entry-point rule stays available as `--from-entry`; run both if a row
    matters, since agreement across the two is itself evidence.
    """
    kind, detail, le_off = exe_kind(blob)
    if kind in ("PE", "NE", "W3", "W4"):
        return [], ("EXCLUDED: %s executable (%s) -- Windows code, not a DOS "
                    "guest binary" % (kind, detail))
    if kind == "TINY":
        return [], "EXCLUDED: %s" % detail
    if kind in ("LE", "LX"):
        rs = le_code_regions(blob, le_off)
        if not rs:
            return [], "EXCLUDED: %s with no executable object" % kind
        return rs, "%s %s -- %s" % (kind, detail,
                                    "; ".join(r[0] for r in rs))
    if kind == "COM":
        return [("whole file", blob, 16)], "COM/raw, 16-bit"
    mz = MZ(blob)
    img = mz.image()
    ep = mz.cs * 16 + mz.ip
    if from_entry and 0 < ep < len(img):
        return ([("from entry %04X:%04X" % (mz.cs, mz.ip), img[ep:], 16)],
                "MZ 16-bit, %d B module, scanning the %d B FROM CS:IP"
                % (len(img), len(img) - ep))
    return ([("load module", img, 16)],
            "MZ 16-bit, whole %d B load module (entry at +%d)" % (len(img), ep))


def ctx_diversity(code, addr, half=12):
    """How many DISTINCT byte values surround this offset.

    The shuffle null model controls for byte FREQUENCY but not for byte
    CLUSTERING, and that gap produced a real false positive: `0F 0D`
    (PREFETCHW) scored count 37 at enrichment 5.3 in Epic Pinball, which no
    1993 DOS game should contain. Every occurrence turned out to sit at the
    edge of a long run of `0D` bytes inside an asset blob; shuffling breaks
    the runs, so a run-structured region is UNDER-represented in the control
    and the enrichment comes out spuriously high.

    Byte diversity in a small window catches exactly that. Real 16- or 32-bit
    code uses most of the byte range within 24 bytes; a table, a run, a string
    or a bitmap uses a handful.
    """
    lo = max(0, addr - half)
    return len(set(code[lo:addr + half]))


# Opcodes that dominate real 16- and 32-bit compiler output. Membership does
# not have to be exact -- what matters is that the set is large in real code
# and near-uniformly rare in data.
COMMON_OPS = frozenset([
    0x8B, 0x89, 0x8A, 0x88, 0xE8, 0xE9, 0xEB, 0xC3, 0xC2, 0x55, 0x5D,
    0x50, 0x51, 0x52, 0x53, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5E, 0x5F,
    0x83, 0x81, 0x8D, 0xB8, 0x3B, 0x39, 0x33, 0x31, 0x85, 0x84,
    0x74, 0x75, 0x72, 0x73, 0x76, 0x77, 0x7C, 0x7D, 0x7E, 0x7F,
    0xFF, 0xC7, 0xC6, 0x03, 0x2B, 0x01, 0x29, 0x0B, 0x23,
    0x40, 0x48, 0x46, 0x4E, 0x6A, 0x68, 0x90, 0x8E, 0x8C])

WINDOW = 4096
CODE_FLOOR = 0.30


def code_windows(code, bits, floor=CODE_FLOOR, win=WINDOW):
    """Keep only the windows that decode like CODE. Returns (bytes, kept, all).

    This replaces guessing where the code is. The earlier lane's rule was
    "start at CS:IP", which is right for Epic Pinball -- 200 KB of data then
    45 KB of code -- and badly wrong for Wolf3D, Keen 4, Prince of Persia and
    Battle Chess, whose startup stub is at the END of the module with the
    game's code before it; applied there it discards 97% of the title. Neither
    "from the entry point" nor "the whole module" is right for both.

    So neither is used. Each 4 KB window is decoded and scored by the fraction
    of its instructions whose opcode is in COMMON_OPS, and the separation is
    wide enough to make the choice mechanical rather than a judgement call:

        Epic Pinball EP9, the pre-entry data blob   median 0.07, max 0.21
        Epic Pinball EP9, the post-entry code       median 0.32
        Dark Forces DF.EXE, the LE exec object      median 0.81
        Mortal Kombat MK1.EXE, LE exec object       median 0.19  (table-heavy)
        Wolf3D WOLF3D.EXE, whole module             median 0.28  -- see below

    WOLF3D.EXE scores 0.28 and is therefore REJECTED ENTIRELY, and that is the
    correct answer rather than a failure: the file is an LZEXE-packed image, so
    the bytes on disk are compressed and there is no x86 in them to count. Its
    demand cannot be established statically by any method, and a scan that
    reports numbers for it is reporting numbers about a decompressor's output
    format. The same holds for every packed binary here.
    """
    keep, kept, total = bytearray(), 0, 0
    for s in range(0, len(code), win):
        seg = code[s:s + win]
        if len(seg) < 512:
            break
        total += 1
        n = c = 0
        for k, _r, _m, _a in decode_stream(seg, bits):
            if k[0] == "pfx":
                continue
            n += 1
            if k[0] == "1" and k[1] in COMMON_OPS:
                c += 1
        if n and c / n >= floor:
            keep += seg
            kept += 1
    return bytes(keep), kept, total


def scan_regions(regions, cov, chains, tbl1, tbl0f, doc, null=True):
    counts, noise = collections.Counter(), collections.Counter()
    mnems = collections.defaultdict(collections.Counter)
    ctxs = collections.defaultdict(list)
    for _label, code, bits in regions:
        for key, reg, mn, addr in decode_stream(code, bits):
            kind, op = key
            groups = GROUPS if kind == "1" else GROUPS_0F
            r = reg if op in groups else None
            counts[(kind, op, r)] += 1
            mnems[(kind, op, r)][mn] += 1
            if len(ctxs[(kind, op, r)]) < 400:
                ctxs[(kind, op, r)].append(ctx_diversity(code, addr))
        if null:
            noise.update(null_model(code, bits))
    rows = {}
    for (kind, op, reg), n in counts.items():
        if kind == "pfx":
            st, mnem, note = "ok", "prefix %02X" % op, ""
        else:
            st, mnem, note = status_for((kind, op), reg, cov, chains,
                                        tbl1, tbl0f, doc)
        nz = noise.get((kind, op, reg), 0)
        obs = mnems[(kind, op, reg)].most_common(1)
        if obs and (mnem in ("?", "") or mnem.startswith(".op_")
                    or mnem.startswith(".exit")):
            mnem = obs[0][0].upper() + (" /%d" % reg if reg is not None else "")
        c = sorted(ctxs[(kind, op, reg)])
        rows[(kind, op, reg)] = dict(kind=kind, op=op, reg=reg, mnem=mnem,
                                     status=st, count=n, noise=nz, note=note,
                                     ctx=(c[len(c) // 2] if c else 0),
                                     ctxn=len(c))
    return rows


# A 24-byte window of real x86 uses most of the byte range. Below this many
# distinct values the "instruction" is sitting in a table, a run or a bitmap.
CTX_FLOOR = 14

# x87 byte sequences that are essentially never anything else. Each is a
# no-operand or fixed-operand form, so it is one or three FIXED bytes rather
# than an opcode plus a variable ModRM, and it can be searched for WITHOUT
# decoding -- no phase, no sweep, no mis-decode.
#
# This exists because neither statistical control can settle the x87 question
# on its own. The shuffle model says no title's D8-DF clears the noise floor,
# and `ctx` says WOLF3D.EXE is full of high-diversity x87 -- but WOLF3D.EXE is
# an LZEXE-packed image, so its ctx is 22 everywhere for the same reason a JPEG
# would be, and its enrichment is 0.9 because compressed bytes ARE their own
# shuffle. The two controls have opposite blind spots and BOTH are blind to a
# few hundred real instructions in a 240,000-instruction image. An idiom count
# is not a statistic at all: `DF E0 9E` is FNSTSW AX; SAHF, and finding it
# fifty times means the file computes in floating point.
X87_IDIOM = {
    b"\xdf\xe0\x9e": "FNSTSW AX; SAHF", b"\xdb\xe3": "FNINIT",
    b"\xd9\x7c\x24": "FNSTCW [ESP]", b"\xd9\x3c\x24": "FNSTCW [ESP]",
    b"\xd9\xee": "FLDZ", b"\xd9\xe8": "FLD1", b"\xd9\xfa": "FSQRT",
    b"\xd9\xf2": "FPTAN", b"\xd9\xff": "FCOS", b"\xd9\xfe": "FSIN",
    b"\xde\xf9": "FDIVP", b"\xde\xc1": "FADDP", b"\xde\xc9": "FMULP",
    b"\xdb\xe2": "FNCLEX", b"\xd9\xfd": "FSCALE", b"\xd9\xf1": "FYL2X",
}


def x87_idioms(regions):
    """{idiom name: count} over the code regions, by byte search."""
    blob = b"".join(c for _l, c, _b in regions)
    out = {}
    for pat, name in X87_IDIOM.items():
        n = blob.count(pat)
        if n:
            out[name] = out.get(name, 0) + n
    return out


def grade(rows, null=True):
    """Attach enrichment, a CLASS-RELATIVE enrichment, and a verdict.

    Three conditions, because each of the first two has been caught being
    wrong on this library:

    1. COUNT alone was fooled by `0x2F`'s frequency as a data byte: 19,000
       `DAS` in Epic Pinball.
    2. COUNT + shuffle enrichment was fooled twice. Once by run structure --
       `0F 0D` at the edge of a run of `0D` bytes -- which `ctx` now catches.
       And once, far more seriously, by OPCODE LENGTH.

    That second bias invalidates every cross-length comparison the raw
    `enrich` number invites, and it is large. Shuffling a region preserves
    single byte frequencies exactly, so a ONE-byte opcode is barely disturbed
    and its enrichment tends to 1 however real it is -- `8B` MOV, the single
    most common instruction in x86, scores 1.27 in Epic Pinball. But a TWO-byte
    `0F xx` needs an exact ORDERED PAIR, and shuffling destroys ordered pairs,
    so the control finds almost none and the ratio explodes for ANY 0F pair
    that survives, real or not: Epic Pinball's `0F 0F` PI2FD scores 217. A
    1993 pinball game does not contain 3DNow!.

    So enrichment is only meaningful WITHIN an opcode-length class, and `rel`
    is the number to read: each row's enrichment divided by the median
    enrichment of its own class in the same scan. `rel` near 1 means "no more
    enriched than every other opcode of this length here", i.e. nothing.
    """
    for cls in ("1", "0f"):
        es = sorted(r["enrich"] for r in rows.values()
                    if r["kind"] == cls and r["count"] >= 10
                    and r.get("enrich"))
        med = es[len(es) // 2] if es else 1.0
        for r in rows.values():
            if r["kind"] != cls:
                continue
            r["class_median_enrich"] = med
            r["rel"] = (r["enrich"] / med) if (r["enrich"] and med) else None
    for r in rows.values():
        rel, e = r.get("rel"), r.get("enrich")
        r["strong"] = (r["count"] >= 20 and r.get("ctx", 0) >= CTX_FLOOR
                       and (not null or rel is None or rel >= 4.0))
        r["weak"] = (r["count"] < 4 or r.get("ctx", 99) < CTX_FLOOR
                     or (null and rel is not None and rel < 2.0)
                     or (null and e is not None and e < 1.0))
    return rows


def opkey(kind, op, reg):
    s = ("0F %02X" if kind == "0f" else "%02X") % op
    return s + ("" if reg is None else " /%d" % reg)


def library(args):
    tbl1 = parse_table(os.path.join(ROOT, "opcode_table.S"))
    tbl0f = parse_table(os.path.join(ROOT, "opcode_0f_table.S"))
    doc = parse_doc(os.path.join(ROOT, "docs", "07_opcode_coverage.md"))
    import gen_opcode_coverage as gen
    gen_rows, chains, _a = gen.build()
    cov = {("0F%02X" if k == "0f" else "%02X") % o:
           {kk: vv for kk, vv in r.items() if kk != "detail"}
           for (k, o), r in gen_rows.items()}

    titles = args.title or sorted(f[:-4] for f in os.listdir(ROMS)
                                  if f.endswith(".dsk"))
    null = not args.no_null_model
    result = {}
    for t in titles:
        vol = FatVolume(open(os.path.join(ROMS, t + ".dsk"), "rb").read())
        files = sorted(p for p, s in vol.walk()
                       if p.upper().endswith((".EXE", ".COM"))
                       and s >= args.min_size)
        tinfo = {"files": {}, "rows": {}}
        agg = {}
        for fn in files:
            if any(re.search(p, fn, re.I) for p in args.skip):
                tinfo["files"][fn] = {"note": "EXCLUDED: --skip pattern"}
                continue
            try:
                blob = vol.read(fn)
            except Exception as e:                      # noqa: BLE001
                tinfo["files"][fn] = {"note": "unreadable: %s" % e}
                continue
            regions, note = regions_for(blob, args.from_entry)
            if regions and not args.no_code_filter:
                filt, kk, tt = [], 0, 0
                for lbl, code, bits in regions:
                    c, k1, t1 = code_windows(code, bits)
                    kk += k1
                    tt += t1
                    if c:
                        filt.append((lbl, c, bits))
                note += "; code-like windows %d/%d (%d KB kept)" % (
                    kk, tt, len(b"".join(c for _l, c, _b in filt)) // 1024)
                if not filt:
                    note += " -- NO CODE-LIKE WINDOW: packed, encrypted or " \
                            "pure data; demand is not statically knowable"
                regions = filt
            tinfo["files"][fn] = {"note": note, "size": len(blob),
                                  "x87": x87_idioms(regions) if regions else {}}
            sys.stderr.write("  %-14s %-30s %s\n" % (t, fn, note))
            if not regions:
                continue
            for k, r in scan_regions(regions, cov, chains, tbl1, tbl0f, doc,
                                     null=null).items():
                a = agg.get(k)
                if a is None:
                    agg[k] = dict(r, files=[os.path.basename(fn)])
                else:
                    # Keep the BEST context seen: one binary's real use of an
                    # opcode must not be masked by another's blob noise.
                    a["ctx"] = max(a["ctx"], r["ctx"])
                    a["count"] += r["count"]
                    a["noise"] += r["noise"]
                    a["files"].append(os.path.basename(fn))
        for a in agg.values():
            a["enrich"] = (a["count"] / a["noise"]) if a["noise"] else None
        grade(agg, null)
        for k, a in agg.items():
            tinfo["rows"][opkey(*k)] = a
        result[t] = tinfo
    return result


def library_report(result, out):
    def p(s=""):
        out.write(s + "\n")

    p("# Library-wide static opcode demand")
    p()
    p("Generated by `scripts/opcode_demand.py --library`. Do not hand-edit.")
    p()
    p("Every count is an occurrence in the shipped IMAGE, never an execution")
    p("count, and every count is paired with `enrich` = count divided by the")
    p("same count over a SHUFFLED copy of the same bytes. `enrich <= 1` means")
    p("the opcode appears LESS often than it would in pure data with the same")
    p("byte frequencies, and is conclusively noise.")
    p()
    p("`ctx` is the median number of DISTINCT byte values in the 24 bytes")
    p("around each occurrence. The shuffle control catches byte FREQUENCY but")
    p("not byte CLUSTERING, and that gap produced a real false positive: see")
    p("`ctx_diversity()`. Real code scores 18-24; a table, a run or a bitmap")
    p("scores under 14.")
    p()
    p("`rel` is that enrichment divided by the MEDIAN enrichment of opcodes of")
    p("the SAME LENGTH in the same scan. Raw `enrich` is not comparable across")
    p("lengths: shuffling preserves single bytes but destroys ordered pairs, so")
    p("one-byte opcodes always score near 1 (`8B` MOV scores 1.27) and two-byte")
    p("`0F xx` opcodes always score huge (`0F 0F` PI2FD scores 217 in a 1993")
    p("pinball game). Read `rel`, never raw `enrich`. See grade().")
    p()
    p("STRONG means count >= 20 AND ctx >= %d AND rel >= 4 -- all three."
      % CTX_FLOOR)
    p()
    p("## Per-title gaps")
    p()
    order = sorted(result, key=lambda t: -sum(
        r["count"] for r in result[t]["rows"].values()
        if r["status"] == "missing" and r["strong"]))
    for t in order:
        info = result[t]
        rows = info["rows"]
        p("### %s" % t)
        p()
        for fn, f in sorted(info["files"].items()):
            p("  * `%s` -- %s" % (fn, f["note"]))
            if f.get("x87"):
                p("    * x87 idioms: %s"
                  % ", ".join("%s x%d" % (k, v)
                              for k, v in sorted(f["x87"].items(),
                                                 key=lambda x: -x[1])))
        p()
        if not rows:
            p("  NOTHING SCANNABLE: every binary on this image was excluded.")
            p()
            continue
        miss = sorted((r for r in rows.values() if r["status"] == "missing"),
                      key=lambda r: -r["count"])
        strong = [r for r in miss if r["strong"]]
        if not miss:
            p("  **NO MISSING OPCODE.** Every opcode and every group `/reg` "
              "found in this title's code reaches a handler.")
        else:
            p("  | op | mnemonic | count | enrich | rel | ctx | evidence |")
            p("  |---|---|---|---|---|---|---|")
            for r in miss[:20]:
                e = "inf" if r["enrich"] is None else "%.1f" % r["enrich"]
                rl = r.get("rel")
                p("  | `%s` | %s | %d | %s | %s | %d | %s |"
                  % (opkey(r["kind"], r["op"], r["reg"]), r["mnem"],
                     r["count"], e,
                     "-" if rl is None else "%.1f" % rl, r["ctx"],
                     "STRONG" if r["strong"] else
                     ("weak" if r["weak"] else "moderate")))
            if not strong:
                p()
                p("  **No strong evidence of any gap.** Every missing opcode "
                  "found is at or below the data-noise floor.")
        x = sorted((r for r in rows.values()
                    if r["kind"] == "1" and r["op"] in X87),
                   key=lambda r: -r["count"])
        p()
        if x:
            p("  x87 ESC `D8`-`DF`: %d occurrences across %d of 8 slots, %d "
              "of them strong. (These slots are NOT missing: they reach the "
              "no-coprocessor stub `.op_esc_x87`.)"
              % (sum(r["count"] for r in x), len(x),
                 len([r for r in x if r["strong"]])))
        else:
            p("  x87 ESC `D8`-`DF`: no occurrences at all.")
        p()

    p("## Cross-title ranking of MISSING opcodes")
    p()
    tally = {}
    for t, info in result.items():
        for k, r in info["rows"].items():
            if r["status"] != "missing":
                continue
            e = tally.setdefault(k, dict(mnem=r["mnem"], strong=[], weak=[],
                                         count=0))
            e["count"] += r["count"]
            (e["strong"] if r["strong"] else e["weak"]).append(t)
    p("| op | mnemonic | titles with STRONG evidence | titles, weak only | "
      "total count |")
    p("|---|---|---|---|---|")
    for k in sorted(tally, key=lambda k: (-len(tally[k]["strong"]),
                                          -tally[k]["count"])):
        e = tally[k]
        p("| `%s` | %s | %s | %s | %d |"
          % (k, e["mnem"], ", ".join(sorted(e["strong"])) or "--",
             ", ".join(sorted(e["weak"])) or "--", e["count"]))
    p()
    clean = [t for t, i in result.items()
             if i["rows"] and not any(r["status"] == "missing" and r["strong"]
                                      for r in i["rows"].values())]
    p("## Titles with no strongly-evidenced gap")
    p()
    p(", ".join("`%s`" % t for t in sorted(clean)) or "(none)")
    p()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--library", action="store_true",
                    help="scan EVERY .dsk in the roms tree: classify each "
                         "binary by executable format, take the decode width "
                         "from the format, and emit a ranked cross-title "
                         "report. Ignores --dsk/--file/--bits")
    ap.add_argument("--title", action="append", default=[],
                    help="--library: restrict to these titles; repeatable")
    ap.add_argument("--min-size", type=int, default=2048,
                    help="--library: skip binaries smaller than this")
    ap.add_argument("--skip", action="append", default=[],
                    help="--library: regex of paths to exclude; repeatable")
    ap.add_argument("--from-entry", action="store_true",
                    help="--library: for a 16-bit MZ, scan only from CS:IP "
                         "instead of the whole load module. Right for Epic "
                         "Pinball, wrong for most of the library -- see "
                         "regions_for()")
    ap.add_argument("--no-code-filter", action="store_true",
                    help="--library: do not drop windows that decode like "
                         "data rather than code. See code_windows()")
    ap.add_argument("--report",
                    help="--library: write the markdown report here")
    ap.add_argument("--dsk",
                    help="title name (resolved under the roms tree) or a path")
    ap.add_argument("--file", action="append", default=[],
                    help="file inside the volume; repeatable. "
                         "Default: every .EXE")
    ap.add_argument("--bits", type=int, choices=(16, 32), default=16,
                    help="decode width. 16 = DOS real mode (default); "
                         "32 = protected-mode load image. Mixing these is the "
                         "main way this scan goes wrong")
    ap.add_argument("--raw", action="store_true",
                    help="do not parse an MZ header; scan the whole file")
    ap.add_argument("--whole-image", action="store_true",
                    help="scan the load module from byte 0 instead of from "
                         "CS:IP. Usually much noisier -- see scan_file()")
    ap.add_argument("--no-null-model", action="store_true",
                    help="skip the shuffled-bytes baseline (faster, but then "
                         "every count is raw and data noise is not separated)")
    ap.add_argument("--min-count", type=int, default=1,
                    help="hide rows below this count (default 1; rows under 4 "
                         "are marked 'weak' regardless)")
    ap.add_argument("--problems", action="store_true",
                    help="only rows whose status is missing or DEFECT")
    ap.add_argument("--pmode", action="store_true",
                    help="also print the protected-mode instruction census "
                         "(group 6, LAR/LSL, CLTS, ARPL, far transfers, IRET, "
                         "INT, MOV CRn) whatever the count")
    ap.add_argument("--top", type=int, default=60, help="rows to print")
    ap.add_argument("--json", help="write the full result here")
    args = ap.parse_args()

    if args.library:
        res = library(args)
        buf = io.StringIO()
        library_report(res, buf)
        sys.stdout.write(buf.getvalue())
        if args.report:
            os.makedirs(os.path.dirname(args.report) or ".", exist_ok=True)
            with open(args.report, "w") as f:
                f.write(buf.getvalue())
            sys.stderr.write("wrote %s\n" % args.report)
        if args.json:
            os.makedirs(os.path.dirname(args.json) or ".", exist_ok=True)
            with open(args.json, "w") as f:
                json.dump(res, f, indent=1)
            sys.stderr.write("wrote %s\n" % args.json)
        return

    if not args.dsk:
        ap.error("--dsk is required unless --library is given")
    path = args.dsk
    if not os.path.exists(path):
        path = os.path.join(ROMS, args.dsk if args.dsk.endswith(".dsk")
                            else args.dsk + ".dsk")
    with open(path, "rb") as f:
        vol = FatVolume(f.read())

    files = args.file
    if not files:
        files = sorted(p for p, _s in vol.walk() if p.upper().endswith(".EXE"))

    tbl1 = parse_table(os.path.join(ROOT, "opcode_table.S"))
    tbl0f = parse_table(os.path.join(ROOT, "opcode_0f_table.S"))
    doc = parse_doc(os.path.join(ROOT, "docs", "07_opcode_coverage.md"))
    import gen_opcode_coverage as gen
    gen_rows, chains, _gen_ana = gen.build()
    cov = {("0F%02X" if k == "0f" else "%02X") % o:
           {kk: vv for kk, vv in r.items() if kk != "detail"}
           for (k, o), r in gen_rows.items()}

    counts = collections.Counter()          # (kind, op, reg_or_None) -> n
    noise = collections.Counter()           # same key, shuffled-byte baseline
    per_file = collections.defaultdict(set)  # key -> {filenames}
    # capstone's own mnemonic, so the table reads as x86 rather than as our
    # handler symbols. Most-common wins where a slot decodes several ways.
    seen_mnem = collections.defaultdict(collections.Counter)
    print("Scanned as %d-bit code:" % args.bits)
    for fn in files:
        blob = vol.read(fn)
        code, note = scan_file(fn, blob, args.bits, args.raw,
                               from_entry=not args.whole_image)
        print("  %-22s %s" % (fn, note))
        for key, reg, mn, _a in decode_stream(code, args.bits):
            kind, op = key
            groups = GROUPS if kind == "1" else GROUPS_0F
            r = reg if op in groups else None
            counts[(kind, op, r)] += 1
            per_file[(kind, op, r)].add(os.path.basename(fn))
            seen_mnem[(kind, op, r)][mn] += 1
        if not args.no_null_model:
            noise.update(null_model(code, args.bits))

    rows = []
    for (kind, op, reg), n in counts.items():
        if kind == "pfx":
            st, mnem, note = "ok", "prefix %02X" % op, ""
        else:
            st, mnem, note = status_for((kind, op), reg, cov, chains,
                                        tbl1, tbl0f, doc)
        nz = noise.get((kind, op, reg), 0)
        # Enrichment over the shuffled-byte baseline. >=3x with a real count is
        # code; <=1x means the sweep is just finding this byte value in data.
        enr = (n / nz) if nz else (float("inf") if n else 0.0)
        obs = seen_mnem[(kind, op, reg)].most_common(1)
        if obs and (mnem in ("?", "") or mnem.startswith(".op_")
                    or mnem.startswith(".exit")):
            mnem = obs[0][0].upper() + (" /%d" % reg if reg is not None else "")
        rows.append(dict(op=("0F %02X" % op) if kind == "0f" else "%02X" % op,
                         reg=reg, mnem=mnem, status=st, count=n, noise=nz,
                         enrich=enr, weak=(n < 4 or (noise and enr < 2.0)),
                         note=note,
                         files=sorted(per_file[(kind, op, reg)])))
    rows.sort(key=lambda r: -r["count"])

    def show(rs, title):
        print("\n" + title)
        print("%-7s %-4s %-26s %-8s %7s %6s %6s %-6s %s"
              % ("op", "/reg", "mnemonic", "status", "count", "noise",
                 "enrich", "conf", "files"))
        print("-" * 118)
        for r in rs:
            fl = ",".join(f.replace(".EXE", "") for f in r["files"])
            if len(fl) > 24:
                fl = "%d files" % len(r["files"])
            e = r["enrich"]
            es = "inf" if e == float("inf") else "%.1f" % e
            print("%-7s %-4s %-26s %-8s %7d %6d %6s %-6s %s"
                  % (r["op"], "" if r["reg"] is None else "/%d" % r["reg"],
                     r["mnem"][:26], r["status"], r["count"], r["noise"], es,
                     "weak" if r["weak"] else "strong", fl))
            if r["note"] and r["status"] != "ok":
                print("        -> %s" % r["note"][:100])

    sel = [r for r in rows if r["count"] >= args.min_count]
    if args.problems:
        sel = [r for r in sel if r["status"] != "ok"]
    show(sel[:args.top], "RANKED BY STATIC OCCURRENCE COUNT")

    prob = [r for r in rows if r["status"] != "ok"]
    if not args.problems:
        show(prob[:args.top], "MISSING or DEFECTIVE, ranked -- what this title "
                              "needs from us")

    if args.pmode:
        print("\nBYTE-LEVEL PRIVILEGE CENSUS (whole file, phase-independent)")
        print("-" * 118)
        for fn in files:
            blob = vol.read(fn)
            h = privilege_census(blob, args.bits)
            print("  %s  (%d B)" % (fn, len(blob)))
            for k in sorted(h):
                offs = h[k]
                print("    %-20s %4d   e.g. +0x%x  %s"
                      % (k, len(offs), offs[0][0], offs[0][1]))
            for want in ("0F 00 /3 LTR", "0F 00 /2 LLDT", "0F 02 LAR",
                         "0F 03 LSL", "0F 06 CLTS"):
                if want not in h:
                    print("    ABSENT: %s" % want)
            for op, nm in ((0x63, "ARPL"), (0x9A, "CALL far ptr"),
                           (0xEA, "JMP far ptr"), (0xCB, "RETF"),
                           (0xCA, "RETF Iw"), (0xCF, "IRET"), (0xF4, "HLT")):
                print("    byte %02X %-14s %6d (raw byte frequency, "
                      "NOT instruction count)" % (op, nm, blob.count(bytes([op]))))

        pm = []
        for r in rows:
            k = r["op"]
            if k.startswith("0F "):
                if int(k[3:], 16) in PMODE_0F:
                    pm.append(r)
            elif int(k, 16) in PMODE_1BYTE:
                pm.append(r)
        show(pm, "PROTECTED-MODE CENSUS (printed whatever the count)")
        for op, nm in sorted(PMODE_0F.items()):
            if not any(r["op"] == "0F %02X" % op for r in rows):
                print("  ABSENT: 0F %02X  %s" % (op, nm))
        for op, nm in sorted(PMODE_1BYTE.items()):
            if not any(r["op"] == "%02X" % op for r in rows):
                print("  ABSENT: %02X     %s" % (op, nm))

    print("\nCAVEAT: linear sweep, so it mis-decodes wherever it walks into "
          "data.\n'noise' is the same count over a SHUFFLED copy of the same "
          "bytes -- the count this\nsweep would report if the region were pure "
          "data. 'enrich' = count/noise.\n  enrich >= 3 with count >= 20  -> "
          "strong, this is real code\n  enrich < 2                    -> "
          "'weak', probably data bytes, do NOT action alone\nCounts are "
          "occurrences in the IMAGE, never execution counts.")

    if args.json:
        os.makedirs(os.path.dirname(args.json) or ".", exist_ok=True)
        with open(args.json, "w") as f:
            json.dump(dict(dsk=path, bits=args.bits, files=files, rows=rows),
                      f, indent=1)
        print("wrote %s" % args.json)


if __name__ == "__main__":
    main()
