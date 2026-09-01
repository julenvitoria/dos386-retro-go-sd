# The granule seam

**An access that spans a 4 KB boundary is NOT one access.** This is the defect
class that corrupts a guest silently, at roughly 1 address in 4,096, with
nothing faulting and nothing counted.

Code: `x86_seam.c`, `SEAM_CHECK` in `cpu_exec.S`, slack in `x86_pager.h`.

## 1. Why granules are not contiguous

`host = lin + k` with **one `k` per 4 KB granule**. The pager hands frames out in
**fault order** from a single pool (`x86_pager.c`, `take_free_frame()`), copy-on-
write moves a granule into a pool page, and a fixed mapping's neighbour is a
different device entirely. So guest page N+1 is almost never the host memory
after guest page N.

A guest 16- or 32-bit access whose bytes span the boundary resolves **one**
pointer from the LOW granule's `k` and then reads or writes straight past the
top of that host frame, into unrelated memory.

The prior DOS port named this "the seam"
(`references/dos_port_prior/CLAUDE.md`, "The memory map"), where a straddling
16-bit **data** read is what made KEEN4 and TOPBENCH diverge.

## 2. The mechanism: pull, then push

Ported from the prior port. Every pager frame carries **8 bytes of slack** past
its top belonging to nobody (stride 4,104 for 4,096 of guest space).

- **Pull** — before a straddling access, the neighbour's leading bytes are
  copied into that slack, so the READ is correct.
- **Push** — at the next instruction boundary, any slack byte that CHANGED is
  written into the neighbour, so the WRITE lands. Byte-wise and conditional, so
  a pure read never dirties the neighbour.

The lifetime rule that makes the instruction boundary the correct flush point:
*no operand pointer the decoder produces outlives the instruction that decoded
it.*

**A bounce buffer cannot serve data, and this is proven, not assumed.** Folded
operand pointers have arithmetic done on them (string helpers, `R_M_PUSH`, the
second word of a far pointer). The prior port tried a gather buffer for data and
it turned their suite red — `8086tiny.c:11360-11366`, *"Do not re-try it."*
Instruction **fetch** is different: it is provably read-only and its pointer is
never written through, so a gather buffer is legal there and that is what fetch
uses.

Our trigger is `LSLS #20` / `CMN`, two instructions, and catches offsets
`0xFFC`-`0xFFF` — the reference's `(a & 0xFFF) == 0xFFF` test would miss
`0xFFD`/`0xFFE`, which a 386's 4-byte operands need.

**A new fold site that loads or stores more than one byte needs `SEAM_CHECK`, or
it is silently wrong at one address in 2,048.** `x86_seam_overflow` is the one
number that says a straddle was refused.

## 3. The acceptance test, by picture

`X86_DOS_PSP_SHIFT=15` puts the PSP 256 bytes off a 4 KB boundary — the
placement that used to corrupt the guest:

| build | WOLF3D |
|---|---|
| PSP shift 0, seam fix ON | title screen, **57,874** px |
| PSP shift **256 B**, seam fix ON | title screen, **57,874** px |
| PSP shift **256 B**, seam fix **OFF** | shredded, streaked garbage, 57,457 px |

The third row is the negative control and it is the proof: same tree, same
shift, one flag.

**This is why it survived for months.** WOLF3D over 25 s: `x86_seam_syncs=52,216`,
`x86_seam_writes=1`, `x86_seam_overflow=2`. One wrong byte in 52,000 straddles.

## 4. What it explains

A program's load address could only move by **whole 4 KB granules** without
corrupting the guest. Bisected: master's PSP is linear `0x710`; `0x1710` works,
`0x810` and `0x820` do not. Shift a program by a granule and every structure
keeps its offset within its page, so the same accesses straddle and the same
luck holds; shift it 256 bytes and a different set lands on a boundary.

That made **any** change to low-memory layout — an environment block, a bigger
arena base, a device driver stub — a loaded gun whose failure was silent and
looked like a game bug. It was found while bisecting an environment block that
was innocent.

**Tier 1 could not see this class**: `tests/main.c` maps guest memory with an
identity fold, under which `ptr+1` is always correct. `tests/test_seam.py` is
the one harness with a deliberately non-contiguous fold (two frames from extents
48 KB apart, low frame's slack painted `0xEE`) and is the template for anything
testing this class.

## 4b. The 1 MB line, now that the HMA is backed

The sixteen pages at 1 MB are ordinary pager pages whenever A20 is open
(`x86_mem_wrap_1mb()`, `docs/13`). They are handed frames in FAULT order like
everything else, so a 16-bit access straddling a boundary INSIDE the window is
this defect exactly, and it is served by the ordinary slack mechanism —
`tests/test_paging.py` grades both halves of the value landing in the right
frames.

**The line ITSELF is the refusal case, and a title reads it.** The low granule
of the `0x0FFFFF` boundary is page 255, a FIXED mapping with no eight bytes of
its own, so the pull cannot happen: `x86_seam_sync()` refuses and counts
`x86_seam_overflow`. Both Dark Forces and Mortal Kombat read **1** there — one
refused straddle each, in code that is testing the A20 gate. That is the
designed degradation, not a defect to chase, and the arm in `test_paging.py`
exists so "counted" stays a fact.

## 5. Cost

`tests/test_benchmark.py`, a pure `DEC`/`JNZ` loop — the most flush-check-
dominated workload possible, no memory operand at all:

| | |
|---|---|
| cpi | 9.96 → 11.36 (**+14.1%**) |
| WOLF3D `guest_ips_achieved` | **unchanged** (2,982,107 with and without) |

The synthetic figure is the worst case and the real figure is zero, because a
title is frame-paced rather than interpreter-bound.

## 6. The two sibling hazards from the same table

Both were silent in the prior port, and both come from the fold being an offset
table rather than a base:

1. **A folded pointer used as a NUMBER.** Their `LEA` computed the guest address
   as `rm_addr - mem`, correct only while the fold is the identity. Once a
   granule is COW'd, FreeDOS died **~877,000 instructions later**. Checked here
   and clean: `DECODE_MODRM` returns a *linear* address in `R7` and
   `.op_lea_8D` subtracts the segment base.
2. **Pool exhaustion.** Their seam re-asked an exhausted pool once per straddling
   access and turned `dos_cow_lost` into 16,392. Ours is sticky.

## 7. THE STACK IS A HOLE. Demonstrated, not argued.

`PUSH_V` and `POP_V` (`cpu_exec_handlers.S:288`, `:302`) are multi-byte fold
sites and carry **NO `SEAM_CHECK`** — zero occurrences in the tree. Nobody
owned the question; the answer is that it is real.

**The demonstration** is `tests/test_seam.py`'s arm 1b, on the same
deliberately non-contiguous fold as arm 1: `SS = 0xC000`, `SP = 0x1001`, so
`PUSH AX` writes the low byte at the last byte of one granule and the high byte
at the first byte of the next. On today's tree the neighbour reads **0xBB**
(untouched) and the low frame's **slack** holds the pushed **0x56** — the high
byte went off the top of the low frame. The contiguous control passes, so it is
the seam and not the program.

**Nothing prevents it.** SP is not aligned on x86 and nothing in the
interpreter aligns it: a 16-bit push at offset 0xFFF straddles, a 32-bit one at
0xFFD-0xFFF straddles. One stack address in 2,048, exactly as CLAUDE.md's rule
predicts for a fold site without the check.

**Why it has never shown, and why that is the dangerous part.** The matching
`POP` walks off the top of the *same* frame in the *same* direction and reads
back precisely what the `PUSH` wrote, so the guest's own round trip is intact —
the test grades that too, and it passes either way. What is lost is the byte at
the *neighbour's* first address: a later non-straddling access to it reads stale
data, and the slack the push corrupted is never pushed back because nothing
called `x86_seam_sync()`. The failure is a wrong byte somewhere else, later.

**It is REPORTED and not FAILED by default** (`--stack-seam` grades it). The
fix is a `SEAM_CHECK` on every push and every pop, which is a **hot-path** cost
on a machine where 29.4% of wall is ordinary opcodes and where `SEAM_CHECK`'s
measured cost elsewhere was 0.04% — cheap, but per-push rather than per-fold-site
and therefore a number somebody should take before the check lands, not a
decision a red test makes by itself. `x86_seam_overflow` does not fire here
either: the site never asks.
