# Library-wide static opcode demand — findings

Lane D, branch `agent/demand-scan`. Pure static analysis; no tier 2, no
hardware. Generated data: [`build/artifacts/opcode_demand_library.md`] and
`.json`, reproduced by

```bash
python3 scripts/opcode_demand.py --library \
    --report build/artifacts/opcode_demand_library.md \
    --json   build/artifacts/opcode_demand_library.json
```

The scanner's mechanics and every flag are in [`TOOLING.md`](../TOOLING.md);
this file is what the scan *found* and what should be built next.

## The headline

**No title in the library shows strong evidence of needing a one-byte or
two-byte opcode the dispatch tables do not implement.** Every `missing` row in
the cross-title ranking is graded `weak`, across all 17 images.

`63` **ARPL** is the only one-byte slot still exiting. Its aggregate count is
3,412, and its enrichment over the shuffled control is **0.9–1.0 in every
title** — it appears no more often than the byte `0x63` does in data with the
same byte frequencies. It is noise. This matches the one time it was seen for
real: [`docs/13`](13_title_status.md) records Dark Forces halting on `ARPL` at
`0393:005E` **while executing DOS/4GW's own message strings**. That was a
guest that had already jumped into data; the ARPL was the symptom, not the
cause. ARPL is `#UD` in real mode and no protected-mode title here issues one.
Leave the slot exiting.

The rest of the ranking is `MOVUPS`, `FEMMS`, `PI2FD`, `PSUBSB`, `CMOVLE` and
similar — MMX, SSE and 3DNow! mnemonics that cannot exist in a 1990-1995 DOS
binary and are residual mis-decode. `F6 /1` and `F7 /1` (the undefined Grp3
`/1` alias of `TEST`) score 0.3–0.8 enrichment and are likewise noise.

The one class of `0F` row that is *architecturally* plausible rather than
absurd is **`0F C8`–`0F CF` BSWAP** (486, and Watcom does emit it). Its counts
are 1–4 per title with no enrichment. That is not evidence *for*; it is the
only missing opcode in the list a 1990s binary could legitimately contain, so
it is the one to keep in mind if a title later exits on an unknown `0F`.

## Ranked table — what each title demands that we lack

| # | Title | Strongly-evidenced missing opcodes | Notes |
|---|---|---|---|
| — | ALLEYCAT | **none** | |
| — | BATTLECHESS | **none** | |
| — | EPIC_PINBALL | **none** | confirms the earlier lane's finding by a second, independent method |
| — | JAZZ / JAZZ_2 | **none** | JAZZ_2 is Win32 PE; nothing DOS to scan |
| — | KEEN4 | **none** | LZEXE-packed, only 16 KB of 105 KB readable |
| — | MORTAL_KOMBAT | **none** | LE payload now scanned (see below) |
| — | PRINCE_OF_PERSIA | **none** | |
| — | SIMCITY | **none** | |
| — | STAR_WARS_DARKFORCES | **none** *in the integer ISA* | **but see x87 below — this is the real gap** |
| — | TANKWARS | **none** | |
| — | TOPBENCH | **none** | packed, 20 KB of 87 KB readable |
| — | TYRIAN | **none** | |
| — | WOLF3D | **none** | LZEXE-packed, 24 KB of 110 KB readable |
| — | WIN311, freedos, msdos622 | **none** | not game targets |

There is no ranking to give because there is nothing to rank. That is the
result.

## Shortest path

**The shortest path is not an opcode.** No small set of missing instructions
unlocks any title, because no title is blocked on a missing instruction. Two
consequences for the roadmap:

1. **Stop spending sprints on one-byte and two-byte coverage.** Tonight's
   landings (PUSH/POP FS/GS, LSS/LFS/LGS, BT/BTS/BTR/BTC, BSF/BSR, SHLD/SHRD,
   group 6, LAR/LSL/CLTS, BOUND/WAIT/INT1) appear to have closed the integer
   ISA against this library. The remaining 192 exiting `0F` slots are MMX,
   SSE, 3DNow!, VMX and 64-bit; none is reachable from a DOS binary.
2. **The next blocker is not in the dispatch tables**, so it will be found by
   running titles, not by scanning them. Static demand has answered its
   question and cannot answer the next one — see the limits section.

The one exception, and the only build item this scan generates, is x87 for
Dark Forces.

## The four open questions

### x87 — one title needs it, and only one

The premise that x87 is 13 exiting slots is **out of date**: `D8`–`DF` reach
`.op_esc_x87` in `cpu_exec_handlers.S`, the no-coprocessor stub that counts
`x86_x87_escapes` and advances EIP correctly. Nothing exits.

Neither statistical control can settle whether a title *computes* in floating
point, and they fail in opposite directions:

* the shuffle model says **no** title's `D8`–`DF` clears the noise floor —
  every enrichment in the library is between 0.02 and 1.22;
* `ctx` says `WOLF3D.EXE` is **full** of high-diversity x87 — but WOLF3D.EXE
  is LZEXE-packed, so its byte diversity is 22 everywhere for the same reason
  a JPEG's would be.

So the question is settled by **idiom search** (`x87_idioms()`): fixed byte
sequences that are x87 and nothing else, found without decoding. The
discriminating one is `DF E0 9E` — `FNSTSW AX; SAHF` — whose chance rate is
about 0.015 occurrences in a 250 KB file.

| binary | `FNSTSW AX; SAHF` | verdict |
|---|---|---|
| `DF/DF.EXE` | **32** | real floating-point code |
| `DF/KEYCONFI.EXE` | **12** | real floating-point code |
| `WOLF3D.EXE` | 1 | chance |
| every other binary in the library | 0 | — |

Confirmed by reading the disassembly of the LE executable object, where the
format itself certifies the bytes as code. Two sites, both in `DF/DF.EXE`:

```
  +0x86260  dbe3        fninit                  <- Watcom's FPU probe. Harmless:
  +0x86262  50          push eax                   under the stub it concludes
  +0x86263  d93c24      fnstcw word ptr [esp]      "no coprocessor", as intended.
  +0x86267  80fc03      cmp ah, 3

  +0xa7608  d9ee        fldz                    <- NOT a probe. A double-precision
  +0xa760a  dc5d10      fcomp qword ptr [ebp+0x10]  compare in a computational
  +0xa760d  dfe0        fnstsw ax                   routine, and the branch below
  +0xa760f  9e          sahf                        is taken on its result.
  +0xa7610  730a        jae 0xa761c
```

**This is the finding that sizes the sprint.** Under `.op_esc_x87` that second
sequence does not fault and does not stop — `FLDZ` and `FCOMP` do nothing,
`FNSTSW` leaves AX unchanged, and `SAHF` then sets the flags from whatever was
in AH. Dark Forces will not crash; it will *silently take the wrong branch*.
`x86_x87_escapes` is the number that distinguishes a probe (a handful) from
computation (millions), and it should be read before any Dark Forces screenshot
is believed.

Answer: **the "our targets are fixed-point" reasoning holds for the whole
library except Dark Forces.** Wolf3D, Keen 4, Prince of Persia, Epic Pinball,
Tyrian, SimCity, Alley Cat, Tank Wars, Battle Chess and Mortal Kombat show
nothing above the chance floor. Size an x87 sprint against Dark Forces alone,
and close the question for everything else.

### `0F 0D` PREFETCHW — explained, and it was a methodology bug

Not 3DNow!. Not present in Epic Pinball's code at all: `EP1`–`EP4`, `EP13` and
`PINBALL.EXE` contain **zero** `0F 0D` byte pairs anywhere. The 37 hits came
from `EP9` and `EP11`, every one of them at the edge of a long run of `0D`
bytes inside an asset blob — `0d0d0b78575453525152f77a0f0d0d0d0d0d0d0c`. In
`EP9`'s case byte `0x0D` is 16% of the surrounding 17 KB.

The enrichment of 5.3 was real arithmetic and a false signal, because **the
shuffle control models byte FREQUENCY but not byte CLUSTERING**. Shuffling
breaks runs, so a run-structured region is under-represented in the control and
any opcode sitting on a run edge is spuriously enriched.

Two fixes landed in the scanner, and both were needed:

* `ctx_diversity()` — the median number of distinct byte values in the 24
  bytes around each hit. Runs and tables score under 14; real code scores
  18–24.
* `code_windows()` — keep only 4 KB windows that decode like code at all. This
  alone dropped Epic Pinball's `0F 0F` PI2FD from **217 occurrences to 4** and
  `0F 0D` from 109 to 26/weak.

A third bias was found on the way and matters more than either. **Raw
enrichment is not comparable across opcode lengths.** Shuffling preserves
single bytes but destroys ordered pairs, so a one-byte opcode's enrichment
tends to 1 however real it is — `8B` MOV, the most common instruction in x86,
scores **1.27** — while any surviving two-byte `0F xx` explodes: `0F 0F` scored
**217** in a 1993 pinball game. Every cross-length comparison of raw `enrich`
in the earlier scan was therefore meaningless. The report now prints `rel`,
enrichment normalised by the median of the same opcode length, and `rel` is the
number to read.

### `63` ARPL

Answered above: no protected-mode title in the library issues it, its
enrichment is ~1.0 everywhere, and the one historical sighting was a guest
executing DOS/4GW's message strings after a bad jump. Leave it exiting — as a
trap it is more useful than as an instruction.

### Titles already fully satisfied

By this scan, **all of them** — every title's demand is met by the current
tables. Ordered by how much of the title the scan could actually read, since
that is what the claim is worth:

| confidence | titles | why |
|---|---|---|
| high | STAR_WARS_DARKFORCES (`DF.EXE` 190/191 windows), MORTAL_KOMBAT, SIMCITY, PRINCE_OF_PERSIA, BATTLECHESS, TANKWARS, TYRIAN | LE objects or unpacked MZ; most of the image was read |
| medium | ALLEYCAT, EPIC_PINBALL, JAZZ | large data blobs, but 30–200 KB of real code read per binary |
| **low** | WOLF3D (24 KB of 110 KB), KEEN4 (16 KB of 105 KB), TOPBENCH (20 KB of 87 KB) | LZEXE-packed; most of the file is compressed and holds no x86 to count |

The three low-confidence titles are the ones where "no missing opcode" is a
weaker claim than it looks — see below.

## What is strong evidence here and what is not

**Strong.** These do not depend on the statistics:

* `63` ARPL is not demanded — enrichment ~1.0 in all 17 images, independently
  corroborated by `docs/13`'s account of the only real sighting.
* Dark Forces contains real x87 — a byte-exact idiom count, plus disassembly
  read by hand inside a region the LE header certifies as executable.
* Epic Pinball contains no `0F 0D` — a byte search, no decode, no phase.
* `SCRIPTIN.EXE` is a Win32 PE, `JAZZ2.EXE`/`JCS.EXE` are PE, all of WIN311 is
  NE/W3 — file-format facts.
* Mortal Kombat's real game image is a 32-bit LE at file offset 168,676, not
  the DOS/4GW stub. The first library scan missed it entirely.

**Weak, and must not be actioned alone.** Every `missing` row in the ranking.
All are `weak`; the SSE/MMX/3DNow! mnemonics are certainly mis-decode.

**Not established at all.** Three limits, stated plainly:

1. **Packed binaries cannot be scanned.** Wolf3D, Keen 4 and TOPBENCH are
   LZEXE-packed; the scan reads only the unpacker stub and whatever is stored
   uncompressed. "No missing opcode" for those three is a claim about a
   quarter of the file. The way to settle them is to dump the *unpacked* image
   from a running guest and scan that — a tier-2 task for when tier 2 is back,
   and the obvious follow-up to this lane.
2. **A count is an occurrence in the image, never an execution count.** This
   ranks what a title *could* need. `profile_opcodes.py` ranks what it *does*.
3. **This says nothing about correctness.** A `REVIEW` row means a handler
   exists and a mechanical marker fired on its body; 48 slots carry one. An
   opcode that is present but wrong is invisible to a demand scan, and after
   tonight's landings that is the more likely failure mode than a missing slot.
