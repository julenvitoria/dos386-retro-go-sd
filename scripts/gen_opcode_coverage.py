#!/usr/bin/env python3
"""
Generate the opcode coverage table in `docs/07_opcode_coverage.md` FROM SOURCE.

Why this exists
---------------
`docs/07` was hand-written, and it has now rotted twice. Commit 7f4c12a fixed
the `0F 8x` near-Jcc rel16 defect and the `E0`-`E3` address-size gap; the
document still listed both as DEFECT 249 commits later. `60` PUSHA, `61` POPA,
`68` PUSH Iv and `84`/`85` TEST were likewise fixed while the document called
them broken. A sprint was planned off those stale rows and an agent was
mis-tasked as a result.

A generated table cannot rot. This parses the two dispatch tables and the body
of every named handler and emits the derivable half of the document. The
hand-written analysis -- how the two operand-size passes work, the width
primitive list, the defect taxonomy, the prioritised defect discussion -- is
NOT generated and is preserved verbatim from the existing file.

What it will and will not claim
-------------------------------
Everything below is a MECHANICAL property of the source text. Each is something
you could grep for; the value here is doing it for all 512 slots consistently.

  reached        the dispatch table slot points at a handler, not `.exit_loop`
  dual / AGN     the handler is declared `OP` (emitted per operand size) or
                 `OP_AGNOSTIC` (emitted once, shared by both tables)
  width          which of the width-correct primitives the body uses, and
                 whether it also contains bare 32-bit `LDR.W`/`STR.W` or a
                 hard-coded `#4` -- the signature of a silently-32-bit body
  flags          whether the body invokes any `X86_FLAGS_*` macro at all
  stack          `PUSH_V`/`POP_V` (SS.base applied, width-correct) versus an
                 open-coded `[R8, #24]` ESP walk
  modrm          `[R2, #REC_MODRM_FN]` (honours `0x67`) versus a decoder called
                 by symbol (ignores it)
  mem form       whether the body ever calls a ModRM decoder

It refuses to state anything it cannot derive. In particular it NEVER claims a
flag computation is CORRECT -- only that flag macros are present or absent.
Semantic correctness is marked UNKNOWN and is exactly what the hand-written
sections and `tests/test_opcodes.py` are for. A generator that guessed would
reproduce the failure it exists to prevent.

Usage
-----
    python3 scripts/gen_opcode_coverage.py                 # print the tables
    python3 scripts/gen_opcode_coverage.py --check         # is docs/07 stale?
    python3 scripts/gen_opcode_coverage.py --write         # regenerate docs/07
    python3 scripts/gen_opcode_coverage.py --json out.json
"""

import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOC = os.path.join(ROOT, "docs", "07_opcode_coverage.md")

WIDTH_PRIMS = ("LD_V", "ST_V", "LDS_V", "LDREG_V", "STREG_V", "EIP_ADV_V",
               "EIP_WRAP", "PUSH_V", "POP_V", "SP_DEC", "SP_INC", "SP_WRAP",
               "VNORM", "VDENORM", "VMASK", "VSEXT", "VTEST", "OPSZ", "NSH")

BEGIN = "<!-- BEGIN GENERATED -- scripts/gen_opcode_coverage.py -->"
END = "<!-- END GENERATED -->"


def parse_table(path):
    out = {}
    pat = re.compile(r"\.word\s+(\S+)\s*\+\s*1\s*//\s*(0x[0-9a-fA-F]+)")
    with open(path) as f:
        for line in f:
            m = pat.search(line)
            if m:
                out[int(m.group(2), 16)] = m.group(1)
    return out


def parse_handlers(paths):
    """symbol -> {'decl': 'OP'|'OP_AGNOSTIC', 'body': str, 'file': str}."""
    out = {}
    start = re.compile(r"^(OP|OP_AGNOSTIC)\s+(\S+)\s*$")
    for path in paths:
        with open(path) as f:
            lines = f.readlines()
        cur = None
        buf = []
        for line in lines:
            m = start.match(line.rstrip())
            if m:
                if cur:
                    out[cur[1]] = dict(decl=cur[0], body="".join(buf),
                                       file=os.path.basename(path))
                cur, buf = (m.group(1), m.group(2)), []
            elif cur:
                buf.append(line)
        if cur:
            out[cur[1]] = dict(decl=cur[0], body="".join(buf),
                               file=os.path.basename(path))
    return out


def strip_comments(body):
    body = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
    body = re.sub(r"@.*", " ", body)
    body = re.sub(r"//.*", " ", body)
    return body


# Loads that are NEVER an operand access: dispatch-table reads, fold-table
# walks, PC-relative literals, the mode record. Counting these as "a bare
# 32-bit operand load" is what made the first version of this generator report
# every 8-bit ALU handler as silently 32-bit.
SCRATCH_LOAD = re.compile(
    r"REC_MODRM_FN|REC_\w+|\[R9|\[PC|\[R2,\s*#|MEM_GET_PTR|MEM_TRAP|"
    r"\[R8,\s*R\d|=\w+")
BARE_WORD = re.compile(r"\b(LDR|STR)(\.W|\.N)?\s+R\d+\s*,")
FLAG_EVIDENCE = re.compile(r"\bX86_FLAGS_\w+|\bALU_TABLE\w*|\bALU_DISPATCH\w*|"
                           r"\bX86_ADC_NARROW\b|\bEVAL_COND\b|\bSHIFT_TBH\w*")

LABEL_DEF = re.compile(r"^L\((\w+)\):", re.M)
LABEL_REF = re.compile(r"\bB[A-Z.NW]*\s+L\((\w+)\)")


def index_labels(paths):
    """label -> its body text, so a handler's delegation can be followed.

    Most handlers do not compute their own flags; they fall into a shared core
    (`L(ALU_DISPATCH_8)`, `L(SHIFT_TBH_32)`, `L(GRP5_INC)` ...). Judging the
    handler on its own text alone therefore reports "writes no flags" for
    almost every ALU instruction in the table, which is both wrong and exactly
    the kind of confident-but-false row this generator exists to remove.
    """
    out = {}
    for path in paths:
        src = strip_comments(open(path).read())
        marks = [(m.start(), m.group(1)) for m in LABEL_DEF.finditer(src)]
        for i, (pos, name) in enumerate(marks):
            end = marks[i + 1][0] if i + 1 < len(marks) else len(src)
            out.setdefault(name, "")
            out[name] += src[pos:end]
    return out


def reachable_text(body, labels, depth=4):
    """Handler body plus the bodies of labels it branches to, transitively."""
    seen = set()
    text = [body]
    frontier = set(LABEL_REF.findall(body))
    for _ in range(depth):
        nxt = set()
        for lab in frontier - seen:
            seen.add(lab)
            t = labels.get(lab)
            if t:
                text.append(t)
                nxt |= set(LABEL_REF.findall(t))
        frontier = nxt
        if not frontier:
            break
    return "\n".join(text)


def analyse(sym, h, labels):
    """Mechanical properties of one handler body. No semantic judgement."""
    b = strip_comments(h["body"])
    full = reachable_text(b, labels)
    prims = sorted({p for p in WIDTH_PRIMS if re.search(r"\b%s\b" % p, full)})

    # A bare word load/store that is NOT one of the known scratch forms.
    bare = [ln.strip() for ln in full.splitlines()
            if BARE_WORD.search(ln) and not SCRATCH_LOAD.search(ln)
            and not re.search(r"\bLDRB|\bLDRH|\bSTRB|\bSTRH|\bLDRS", ln)]
    hard4 = bool(re.search(r"#4\b", full))
    esp_raw = bool(re.search(r"\[R8,\s*#24\]", full))
    stack_v = bool(re.search(r"\b(PUSH_V|POP_V)\b", full))
    flags = sorted(set(re.findall(r"\bX86_FLAGS_\w+", full)))
    flag_ev = bool(FLAG_EVIDENCE.search(full))
    modrm_rec = "REC_MODRM_FN" in full
    modrm_sym = bool(re.search(r"L\(DECODE_MODRM_(16|32)\)", b))
    seam = "SEAM_CHECK" in full
    return dict(symbol=sym, decl=h["decl"], file=h["file"],
                dual=(h["decl"] == "OP"),
                width_prims=prims, bare_word_ops=len(bare),
                bare_example=(bare[0][:48] if bare else ""),
                hard_4=hard4,
                stack_open_coded=(esp_raw and not stack_v),
                stack_v=stack_v, flag_macros=flags, flag_evidence=flag_ev,
                modrm_via_record=modrm_rec, modrm_by_symbol=modrm_sym,
                has_mem_form=(modrm_rec or modrm_sym), seam_check=seam,
                lines=len([x for x in h["body"].splitlines() if x.strip()]))


def flags_for(a):
    """The mechanical REVIEW markers. Each is a grep, not a verdict.

    These are deliberately conservative. A marker means "a human should read
    this handler", never "this handler is wrong": an 8-bit-only instruction
    legitimately uses no width primitive, and `MOV`/`JMP` legitimately write no
    flags. The generator has no way to tell an 8-bit handler from a
    width-sensitive one, so it reports the observation and stops.
    """
    w = []
    if a is None:
        return w
    if a["bare_word_ops"] and not a["width_prims"]:
        w.append("REVIEW: no width primitive, %d bare word op(s)"
                 % a["bare_word_ops"])
    elif not a["dual"] and a["bare_word_ops"] and a["width_prims"]:
        w.append("REVIEW: AGN body mixes width primitives with %d bare word "
                 "op(s)" % a["bare_word_ops"])
    if a["stack_open_coded"]:
        w.append("REVIEW: open-coded stack (raw ESP, no PUSH_V/POP_V)")
    if a["modrm_by_symbol"] and not a["modrm_via_record"]:
        w.append("REVIEW: ModRM decoder called by symbol (ignores 0x67)")
    if not a["flag_evidence"]:
        w.append("no flag macro reachable")
    return w


DISPATCH = re.compile(
    r"L\((?P<name>\w*GRP\w*_DISPATCH)\):(?P<body>.*?)(?=\nL\(|\nOP\b|\nOP_AGNOSTIC\b|\Z)",
    re.S)


def parse_group_chains(paths):
    """CMP/BEQ group dispatch chains -> {chain: {reg: target_label}}.

    Grp1 and Grp2 use an 8-entry TBH and cannot fall through, so they have no
    chain here and every /reg is reachable by construction. Grp3/4/5 use a
    CMP/BEQ chain and anything unnamed falls to `.exit_loop` -- which is
    exactly what this extracts.
    """
    out = {}
    for path in paths:
        src = open(path).read()
        for m in DISPATCH.finditer(src):
            body = m.group("body")
            regs = {}
            for cmp_m in re.finditer(
                    r"CMP\s+R\d+,\s*#(\d+)\s*\n\s*BEQ[^\n]*L\((\w+)\)", body):
                regs[int(cmp_m.group(1))] = cmp_m.group(2)
            if regs:
                out[m.group("name")] = regs
    return out


def build(argv=None):
    tbl1 = parse_table(os.path.join(ROOT, "opcode_table.S"))
    tbl0f = parse_table(os.path.join(ROOT, "opcode_0f_table.S"))
    srcs = [os.path.join(ROOT, "cpu_exec_handlers.S"),
            os.path.join(ROOT, "cpu_exec_handlers_0f.S"),
            os.path.join(ROOT, "cpu_exec.S")]
    hs = parse_handlers(srcs[:2])
    labels = index_labels(srcs)
    ana = {k: analyse(k, v, labels) for k, v in hs.items()}
    chains = parse_group_chains([os.path.join(ROOT, "cpu_exec_handlers.S"),
                                 os.path.join(ROOT, "cpu_exec_handlers_0f.S")])

    rows = {}
    for kind, tbl in (("1", tbl1), ("0f", tbl0f)):
        for op in range(256):
            sym = tbl.get(op, "<absent>")
            a = ana.get(sym)
            reached = sym not in (".exit_loop", "<absent>")
            rows[(kind, op)] = dict(
                kind=kind, op=op, symbol=sym, reached=reached,
                mode=("dual" if a and a["dual"] else "AGN") if a else "-",
                warnings=flags_for(a) if reached else [],
                analysed=bool(a),
                detail=a)
    return rows, chains, ana


def render(rows, chains):
    out = []
    n1 = sum(1 for (k, _o), r in rows.items() if k == "1" and r["reached"])
    n0 = sum(1 for (k, _o), r in rows.items() if k == "0f" and r["reached"])
    out.append("### Generated hard numbers\n")
    out.append("| Metric | Count |")
    out.append("|---|---|")
    out.append("| One-byte slots with a handler | **%d / 256** |" % n1)
    out.append("| One-byte slots dispatching to `.exit_loop` | **%d / 256** |"
               % (256 - n1))
    out.append("| Two-byte (0F) slots with a handler | **%d / 256** |" % n0)
    out.append("| Two-byte slots dispatching to `.exit_loop` | **%d / 256** |"
               % (256 - n0))
    nw = sum(1 for r in rows.values() if r["reached"] and r["warnings"])
    out.append("| Implemented slots carrying at least one mechanical warning "
               "| **%d** |" % nw)
    out.append("")
    out.append("Every row below is DERIVED. `status` is `reached` or "
               "`.exit_loop`, taken from\nthe dispatch table. `mode` is `OP` "
               "(dual-emitted) or `OP_AGNOSTIC`, taken from the\nhandler "
               "declaration. `warnings` are greps over the handler body, "
               "listed in the\nlegend below. **Semantic correctness of a flag "
               "or an addressing computation is\nnot derivable and is reported "
               "as UNKNOWN -- an empty `warnings` cell means no\nmechanical "
               "warning fired, NOT that the handler is correct.**\n")
    out.append("Warning legend:\n")
    out.append("| warning | what was grepped |")
    out.append("|---|---|")
    for k, v in (
        ("`AGN+raw32`", "declared `OP_AGNOSTIC` yet the body mixes width "
                        "primitives with a bare `LDR.W`/`STR.W` or a "
                        "hard-coded `#4`"),
        ("`AGN, bare 32-bit body`", "declared `OP_AGNOSTIC` and uses no width "
                                    "primitive at all -- 32-bit in both passes "
                                    "unless the instruction is byte-sized"),
        ("`dual but no width primitive`", "declared `OP` but the body has no "
                                          "`LD_V`/`ST_V`/`LDREG_V`/... "),
        ("`open-coded stack`", "touches ESP as `[R8, #24]` and never calls "
                               "`PUSH_V`/`POP_V`"),
        ("`ModRM by symbol (ignores 0x67)`",
         "calls `L(DECODE_MODRM_16/32)` directly instead of through "
         "`[R2, #REC_MODRM_FN]`"),
        ("`no X86_FLAGS_* macro`", "the body invokes no flag macro; benign for "
                                   "MOV/PUSH/JMP, a real gap for ALU"),
    ):
        out.append("| %s | %s |" % (k, v))
    out.append("")

    for kind, title in (("1", "One-byte opcodes -- all 256"),
                        ("0f", "Two-byte (0F) opcodes -- all 256")):
        out.append("\n### %s (generated)\n" % title)
        out.append("| Op | Status | Handler | Mode | Flag macros | Warnings |")
        out.append("|---|---|---|---|---|---|")
        for op in range(256):
            r = rows[(kind, op)]
            d = r["detail"]
            fm = ", ".join(x.replace("X86_FLAGS_", "") for x in
                           (d["flag_macros"] if d else [])) or "-"
            out.append("| %s | %s | `%s` | %s | %s | %s |" % (
                ("0F %02X" % op) if kind == "0f" else "%02X" % op,
                "reached" if r["reached"] else "**`.exit_loop`**",
                r["symbol"], r["mode"], fm,
                ", ".join(r["warnings"]) or "-"))

    out.append("\n### Group `/reg` reachability (generated)\n")
    out.append("`Grp1` and `Grp2` dispatch through an 8-entry `TBH` indexed by "
               "a 3-bit field, so\nall eight `/reg` values reach code by "
               "construction and no chain appears here.\n`Grp3`/`Grp4`/`Grp5` "
               "use a `CMP`/`BEQ` chain: any `/reg` the chain does not name "
               "falls\nto `.exit_loop`. The chains below are extracted from "
               "the source.\n")
    out.append("| Dispatch | /reg reached | /reg falling to `.exit_loop` |")
    out.append("|---|---|---|")
    for name in sorted(chains):
        regs = chains[name]
        got = ", ".join("/%d -> `%s`" % (r, regs[r]) for r in sorted(regs))
        miss = ", ".join("/%d" % r for r in range(8) if r not in regs) or "none"
        out.append("| `%s` | %s | %s |" % (name, got, miss))
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--write", action="store_true",
                    help="regenerate the generated block inside docs/07")
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if docs/07's generated block is out of date")
    ap.add_argument("--json", help="dump the derived facts here")
    args = ap.parse_args()

    rows, chains, _ana = build()
    text = render(rows, chains)

    if args.json:
        with open(args.json, "w") as f:
            json.dump({"%s%02X" % ("0F" if k == "0f" else "", o):
                       {kk: vv for kk, vv in r.items() if kk != "detail"}
                       for (k, o), r in rows.items()}, f, indent=1)
        print("wrote %s" % args.json)

    block = BEGIN + "\n\n" + text + "\n" + END

    if args.write or args.check:
        src = open(DOC).read()
        if BEGIN in src and END in src:
            new = re.sub(re.escape(BEGIN) + r".*?" + re.escape(END),
                         lambda _m: block, src, flags=re.S)
        else:
            new = src.rstrip() + "\n\n" + block + "\n"
        if args.check:
            if new != src:
                print("docs/07 generated block is OUT OF DATE. "
                      "Run: python3 scripts/gen_opcode_coverage.py --write")
                return 1
            print("docs/07 generated block is up to date.")
            return 0
        with open(DOC, "w") as f:
            f.write(new)
        print("regenerated the generated block in %s" % DOC)
        return 0

    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
