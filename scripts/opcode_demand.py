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
import json
import os
import re
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
                yield ("pfx", b[i]), None, "prefix"
                i += 1
            if i >= len(b):
                break
            if b[i] == 0x0F and i + 1 < len(b):
                op, kind, i = b[i + 1], "0f", i + 2
            else:
                op, kind, i = b[i], "1", i + 1
            reg = ((b[i] >> 3) & 7) if i < len(b) else None
            yield (kind, op), reg, ins.mnemonic
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
    for key, reg, _mn in decode_stream(bytes(shuf), bits):
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


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dsk", required=True,
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
        for key, reg, mn in decode_stream(code, args.bits):
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
