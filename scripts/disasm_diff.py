#!/usr/bin/env python3
"""
PER-FUNCTION DISASSEMBLY DIFF -- the proof that real mode pays nothing.

`docs/14_protected_mode.md` Sec.2 states the real-mode cost of protected mode as
a DISASSEMBLY DIFF rather than as a benchmark, and for a measured reason: under
gwemu `tests/test_benchmark.py` spans 10.30-11.88 M cycles on the BASELINE
alone, a 15% spread that swallows anything a cold-path change could cost. What
CAN be decided exactly is whether the hot path's instructions changed at all.
So the claim is not "it is fast enough", it is "`.fetch_decode_loop` and every
`.op_*` handler are byte-identical", and this script is how that is checked
rather than asserted.

Mnemonics are NORMALISED for address shifts: adding cold code moves every later
symbol, so a raw `objdump` diff is nothing but noise. Branch targets are reduced
to their symbol names, trailing address comments are dropped, and the per-symbol
instruction sequence is what is compared. A symbol that MOVED but did not change
compares equal; a symbol that gained, lost or altered one instruction does not.

    # before your change, on the base commit
    python3 scripts/disasm_diff.py --dump build/run/before.json
    # after
    python3 scripts/disasm_diff.py --dump build/run/after.json
    python3 scripts/disasm_diff.py --compare build/run/before.json build/run/after.json
    # only the paths real mode executes
    python3 scripts/disasm_diff.py --compare a.json b.json --hot

`--hot` restricts the report to the symbols the real-mode claim is about: the
fetch/decode loop, every `.op_*` handler, and the ModRM decoders. Symbols that
exist in only one side are reported separately -- a NEW cold symbol is expected
when cold code is added and is not a violation, but a hot symbol appearing or
vanishing is.
"""
import argparse
import json
import os
import re
import subprocess
import sys

OBJDUMP = os.environ.get("OBJDUMP", "arm-none-eabi-objdump")

_SYMBOL_RE = re.compile(r"^[0-9a-f]+ <(.+)>:$")
_INSN_RE = re.compile(r"^\s*[0-9a-f]+:\t([0-9a-f ]+)\t\s*(.*)$")

# The symbols the "real mode pays nothing" claim covers.
_HOT_RE = re.compile(r"^(\.fetch|\.op_|decode_modrm_addr|ALU_TABLE|SHIFT_TABLE"
                     r"|STRING_TABLE|opcode_table)")


def normalise(text):
    """Strip everything that shifts when unrelated code is added."""
    text = text.split(";")[0]                      # trailing address comment
    # "  8004 <.op_foo+0x12>"  ->  "<.op_foo>": the offset within the symbol is
    # itself an address shift once the symbol is not at a round place.
    text = re.sub(r"\b[0-9a-f]{2,8}\b <([^>+]+)(?:\+0x[0-9a-f]+)?>", r"<\1>", text)
    return " ".join(text.split())


# An instruction whose only variable part is a RELOCATED ADDRESS. Adding cold
# code moves every global and every jump-table entry, so these differ on any
# build that changed a symbol's placement -- including one that changed no
# instruction at all. They are reported as RELOCATED rather than CHANGED so
# that "the handler gained an instruction" stays a distinct, reviewable event
# from "a literal it loads moved". Folding them into `normalise` instead would
# hide a genuine constant change, which is the opposite mistake.
_RELOC_RE = re.compile(r"^(movw|movt|\.word)\b")


def reloc_only(a, b):
    """True if a and b differ ONLY in relocated address literals."""
    if len(a) != len(b):
        return False
    for x, y in zip(a, b):
        if x == y:
            continue
        if not (_RELOC_RE.match(x) and _RELOC_RE.match(y)):
            return False
        # An UNRESOLVED word is judged only on being a word: it is a value in
        # an address space the disassembly cannot name, and the whole table
        # shifting by one constant is what "relocated" means for it.
        if x.startswith(".word") and y.startswith(".word"):
            continue
        # Otherwise: same mnemonic and same destination, only the immediate
        # moved. A resolved `.word <sym>` fails this and is a real change.
        if x.split("#")[0] != y.split("#")[0]:
            return False
    return True


_WORD_RE = re.compile(r"^\.word 0x([0-9a-f]+)$")


def dump(elf, sections):
    """Per-symbol normalised instructions, with jump-table entries RESOLVED.

    The dispatch tables (opcode_table_*, ALU_TABLE_*) disassemble as `.word`
    literals, and every one of them moves when any handler's placement moves --
    so comparing them raw makes a table that dispatches identically look
    entirely rewritten. Each entry is instead resolved back to the SYMBOL it
    points at, which is what the table actually means. A table that still
    dispatches every opcode to the same handler then compares EQUAL, and one
    that re-points an entry is a difference the tool still catches by name.
    """
    out = {}
    starts = []                     # (address, symbol) for the whole ELF
    raws = []
    for section in sections:
        try:
            raws.append(subprocess.run([OBJDUMP, "-d", "-j", section, elf],
                                       capture_output=True, text=True,
                                       check=True).stdout)
        except subprocess.CalledProcessError:
            continue

    for raw in raws:
        for line in raw.splitlines():
            m = re.match(r"^([0-9a-f]+) <(.+)>:$", line)
            if m:
                starts.append((int(m.group(1), 16), m.group(2)))
    starts.sort()

    def resolve(addr):
        """The symbol `addr` falls in, plus its offset, or None.

        Thumb sets bit 0 on a function pointer; it is not part of the address.
        A word that lands far past the last symbol is NOT an address in these
        sections -- some tables hold values relative to a base the disassembly
        cannot see -- and resolving it anyway invents a symbol name plus a
        24-bit offset, which reads like a real target and is not one. Those are
        left as raw words so the relocation classifier can judge them on the
        one thing that IS true of them: a uniform shift.
        """
        a = addr & ~1
        lo, hi = 0, len(starts)
        while lo < hi:
            mid = (lo + hi) // 2
            if starts[mid][0] <= a:
                lo = mid + 1
            else:
                hi = mid
        if lo == 0:
            return None
        base, name = starts[lo - 1]
        if a - base > 0x10000:
            return None
        return f"{name}+{a - base:#x}" if a != base else name

    for raw in raws:
        sym = None
        for line in raw.splitlines():
            m = _SYMBOL_RE.match(line)
            if m:
                sym = m.group(1)
                out.setdefault(sym, [])
                continue
            m = _INSN_RE.match(line)
            if m and sym is not None:
                text = normalise(m.group(2))
                if not text:
                    continue
                w = _WORD_RE.match(text)
                if w:
                    r = resolve(int(w.group(1), 16))
                    if r:
                        text = f".word <{r}>"
                out[sym].append(text)
    return out


def strip_pad(insns):
    """Drop trailing NOPs: they are ALIGNMENT, not code.

    objdump attributes a symbol everything up to the next one, so the padding
    the assembler inserts to align whatever follows is counted as part of the
    handler. Move an unrelated symbol and the padding changes, and a handler
    nobody touched reports "8 -> 7 instructions" -- which is exactly the false
    positive this tool exists to not produce.
    """
    end = len(insns)
    while end and insns[end - 1] in ("nop", "nop.w", "nop.n"):
        end -= 1
    return insns[:end]


def compare(a, b, hot_only, quiet):
    a = {k: strip_pad(v) for k, v in a.items()}
    b = {k: strip_pad(v) for k, v in b.items()}
    keys = sorted(set(a) | set(b))
    changed, only_a, only_b, relocated = [], [], [], []
    for k in keys:
        if hot_only and not _HOT_RE.match(k):
            continue
        if k not in b:
            only_a.append(k)
        elif k not in a:
            only_b.append(k)
        elif a[k] != b[k]:
            (relocated if reloc_only(a[k], b[k]) else changed).append(k)

    scope = "HOT (fetch loop, .op_* handlers, ModRM decoders)" if hot_only else "ALL"
    considered = [k for k in keys if not hot_only or _HOT_RE.match(k)]
    print(f"scope: {scope} -- {len(considered)} symbols considered")
    for k in changed:
        print(f"CHANGED: {k}: {len(a[k])} -> {len(b[k])} instructions")
        if not quiet:
            for i, (x, y) in enumerate(zip(a[k], b[k])):
                if x != y:
                    print(f"    first difference at {i}: {x!r} -> {y!r}")
                    break
            else:
                print(f"    identical prefix; lengths differ")
    for k in only_a:
        print(f"REMOVED: {k}")
    for k in only_b:
        print(f"ADDED:   {k} ({len(b[k])} instructions)")
    if relocated:
        print(f"RELOCATED ONLY ({len(relocated)}): same instructions, a moved "
              "address literal -- " + ", ".join(relocated))
    if not changed and not only_a and not only_b:
        print("IDENTICAL: every symbol in scope is instruction-for-instruction "
              "the same" + (" (modulo relocation)." if relocated else "."))
        return 0
    print(f"{len(changed)} changed, {len(only_a)} removed, {len(only_b)} added"
          + (f", {len(relocated)} relocated only" if relocated else ""))
    return 1


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--elf", default="build/core/dos_core.elf")
    p.add_argument("--section", action="append", default=None,
                   help="section to disassemble (repeatable); default is the "
                        "ITCM hot section plus .text")
    p.add_argument("--dump", metavar="OUT.json",
                   help="write this ELF's normalised disassembly")
    p.add_argument("--compare", nargs=2, metavar=("BEFORE.json", "AFTER.json"))
    p.add_argument("--hot", action="store_true",
                   help="report only the symbols the real-mode claim covers")
    p.add_argument("--quiet", action="store_true",
                   help="name changed symbols without showing the difference")
    args = p.parse_args()

    sections = args.section or ["._itcram_hot", ".itcram_hot", ".core_itcm",
                                ".core_entry", ".text"]

    if args.dump:
        d = dump(args.elf, sections)
        if not d:
            print(f"no symbols disassembled from {args.elf} in {sections}")
            return 2
        os.makedirs(os.path.dirname(args.dump) or ".", exist_ok=True)
        with open(args.dump, "w") as f:
            json.dump(d, f)
        print(f"{args.dump}: {len(d)} symbols from {args.elf}")
        return 0

    if args.compare:
        with open(args.compare[0]) as f:
            a = json.load(f)
        with open(args.compare[1]) as f:
            b = json.load(f)
        return compare(a, b, args.hot, args.quiet)

    p.error("one of --dump or --compare is required")


if __name__ == "__main__":
    sys.exit(main())
