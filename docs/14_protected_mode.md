# Protected mode

State: [`STATUS.md`](../STATUS.md). Roadmap: [`PLAN.md`](../PLAN.md) Queue C 4.
Code: `x86_pmode.c`, `L(SET_SREG)` in `cpu_exec_handlers.S`, group 6/7 plus
`L(HANDLE_INTERRUPT)` and `.op_iret_CF` in `cpu_exec_handlers_0f.S`. Tests: `tests/test_pmode.py`,
`scripts/run_pmode_controls.py`.

## 1. What is built, and what is not

**Built and graded.** `LGDT`, `LIDT`, `LLDT`, `SLDT`; the GDT/LDT descriptor
fetch and its bound; the S/type/DPL/present rules with the correct vector and
error code for each; the per-segment descriptor cache (base, limit with the G
shift applied, rights); `MOV Sreg,Ev`, `POP Sreg`, `LES`/`LDS`; `CR0.PE` in
both directions; a same-privilege far `JMP` — the transfer that installs CS and
flushes the pipeline — and, since the gate work below, **same-privilege
interrupt DELIVERY through an IDT gate and the `IRET` that returns from it**.

Since the inspection step below, also **`LAR`, `LSL`, `VERR`, `VERW`, `STR`,
`LTR` and `CLTS`** — every remaining member of group 6 plus the two
descriptor-reading opcodes and the TS clear.

**Not built, and not stubbed.** Call gates, privilege-change stack switches,
**task switching**, task gates, `IRET` across privilege, paging, and per-access
**segment limit enforcement**. Each is REFUSED rather than approximated: an
`INT` or `IRET` that would change ring comes back as `#GP` and exits loudly.
V86 is not needed at all: INT 21h is HLE'd. The TASK REGISTER now exists —
`LTR` loads it and `STR` reads it — and **that is not the same thing as a task
switch**; see §9.

**This is now sized against a title, and that is new.** Dark Forces and Mortal
Kombat both reach `CR0.PE=1` through `LMSW` — not `MOV CR0`, because a DOS
extender must also start on a 286, which is why the walk sat unexercised for so
long — and both stop on the same `INT imm8`. The ~1,225-line estimate that used
to sit in `PLAN.md` still has no measured demand behind it; **what the titles
ask for NEXT is a measurement nobody has yet taken** (see §8).

**And it is very likely NOT DPMI.** DOS4GW was told no by DPMI
(`INT 2Fh AX=1687h`), by VCPI (`INT 67h AH=DEh`) and by `INT 15h`, and went
raw-mode protected on its own. Do not build a DPMI server on the strength of the
extender's name.

## 2. Why real mode pays nothing

29.4% of wall is ordinary opcodes at ~57 cyc/insn. Segment loads and memory
accesses are in that path, so a protected-mode check placed carelessly taxes
every real-mode title for a feature none of them use.

It is avoided **structurally, not carefully**:

- Every segment register load in the machine — `MOV Sreg,Ev`, `POP Sreg`,
  `LES`/`LDS`, far `JMP`/`CALL`/`RETF`/`IRET`, interrupt dispatch — funnels
  through **one** routine, `L(SET_SREG)`. It already opened with
  `LDR CR0 / TST #1 / BNE` in order to trap protected mode out loudly. Only the
  branch target changed.
- Nothing in `x86_pmode.c` is reached from a memory **access**. The descriptor
  is decoded once, on load, into the `segs[]` cache the interpreter already
  reads for its base. That is `references/tiny386`'s shape too (i386.c:1076),
  and it is what the `R6` contract in `CLAUDE.md` is for: R6 carries a POINTER
  to the active descriptor, so limit and rights are one load from any site that
  eventually wants them.

**Proven by disassembly diff against master**, per function, mnemonics
normalised for address shifts. The tool is `scripts/disasm_diff.py`
([`TOOLING.md`](../TOOLING.md)) — it resolves jump-table entries to the symbol
they point at and strips alignment padding, so a symbol that merely MOVED
compares equal:

| path | change |
|---|---|
| `.fetch_decode_loop` | byte-identical |
| every `.op_*` handler **except `.op_iret_CF`**, `decode_modrm_addr16/32`, the ALU/SHIFT/string tables, all four 256-entry dispatch tables | byte-identical |
| `L(SET_SREG)` real-mode arm | one `STRH` → `STR` — same instruction, same cost |
| `L(HANDLE_INTERRUPT)` real-mode arm | instruction-for-instruction identical; the one difference is the TARGET of the `CR0.PE` test that already stood there |
| `.op_iret_CF` / `_32` | **+3 instructions** — `LDR` CR0, `TST`, not-taken branch |
| `x86_cpu_interp_asm` | +2 instructions, **per interp CALL** |
| `.save_state_and_exit` | +1 instruction, **per EXIT** |

Cost: **+912 B ITCM** for the walk, **+296 B** for the gate and `IRET` work
(all cold); **+704 B** and **+1,024 B** RAM_EMU respectively.

**`.op_iret_CF`'s three instructions are the only thing protected mode costs
real mode anywhere**, and they are per IRET — per interrupt, not per
instruction. They cannot be moved onto the mode-record axis §6 describes: that
would have to DOUBLE the record block, 8 KB → 16 KB of DTCM against 5,856 B
free. And they cannot go below the pops, because the CS validation and the
16-bit EFLAGS carry-over are both decisions that must be made before anything
commits.

**`tests/test_benchmark.py` cannot decide this and must not be quoted for it.**
Under gwemu its cycle count measured 10,583,983 / 10,302,184 / 11,884,810 for
1,000,000 instructions **on the unmodified baseline** — a 15% spread that
swallows anything this work could have cost. gwemu does not model the pipeline;
a real number is tier 3 only. Since nothing was added to any per-instruction
path, there is nothing per-instruction to measure.

## 3. Two references, for two different things

They do not agree, and taking the wrong one for the wrong job is the trap.

**`references/tiny386/i386.c` is the SHAPE.** `set_seg()` at :1076 is 55 lines:
a real-mode fast path on `cr0 & 1`, the granularity shift at :1115, D/B driving
`code16` and `sp_mask` at :1122/:1125, CPL from the CS selector's RPL at :1121.
All correct, all copied.

**It is NOT the rules.** Its own comment at :1097 reads
`// TODO: various permission checks`, and the single type check it has (:1103)
is annotated `to avoid win95 BSOD...` — a workaround. **There is no DPL/RPL/CPL
validation on a data segment load anywhere in that function.** An
implementation modelled on it inherits that hole, and so does any test suite
written alongside it: every privilege test passes because neither side
implements the rule.

**`references/dosbox-x` is the RULES** — `CPU_SetSegGeneral()` at
`src/cpu/cpu.cpp:2976-3076`, descriptor classes at `include/cpu.h:284/389/470`.
Several of its checks carry the name of the title that forced them
("Earth Siege 1" :3014, "extreme pinball" :3053, "gabriel knight" :3060).
Where the two disagree, DOSBox-X wins, and the comment at the check says so.

### Three places we are deliberately STRICTER than DOSBox-X

Because it is loose there, not because it disagrees:

1. **The descriptor-table bound.** DOSBox-X tests `address >= limit`
   (`include/cpu.h:481`, `:500`), so it accepts a descriptor whose last four
   bytes lie past the table limit. Intel requires all 8 bytes inside; tiny386
   implements that (`off + 7 > limit`, i386.c:1063) and so do we.
2. **A null selector's effect on the cache.** tiny386 infers "null" later from
   `index == 0 && limit == 0` (i386.c:639), which cannot distinguish a null
   selector from a legitimately loaded zero-limit segment; DOSBox-X stores the
   selector but **leaves the previous limit** in `Segs.limit[]`. We clear P, so
   `X86_SEG_USABLE()` is exact and costs no extra field.
3. **Expand-down.** tiny386's is dead code (`#if 0`, i386.c:645). DOSBox-X's is
   live but behind `do_seg_limits`, **off by default**
   (`src/cpu/cpu.cpp:78`), and its arm (`core_normal/table_ea.h:204`) tests only
   the START address and ignores the B bit. Ours checks the last byte too and
   takes the ceiling from B.

## 4. The `flags` packing, and the bug it already caused

`X86CPUSeg.flags` is 16 bits and must carry both the descriptor's granularity
byte (byte 6) and its access byte (byte 5):

```
flags == desc[6] | (desc[5] << 8)
  bits 0-3 limit 19:16   bit 6 D/B   bit 7 G
  bits 8-11 TYPE   bit 12 S   bits 13-14 DPL   bit 15 P
```

**The D bit's position is not a choice.** `X86_DESC_D_FLAG` already shipped as
`1 << 6` and the interpreter reads it as bit 22 of the 32-bit word
`sel | (flags << 16)` at `SEG_CS_SEL`. Descriptor byte 6 carries D at its own
bit 6, so byte 6 must be the low half — and the access byte then has nowhere to
go but the high half.

The obvious-looking `flags = (w2 >> 16) & 0xFFFF` keeps D in the right place and
**silently drops P, S, DPL and TYPE entirely**. It was written here first and
every descriptor came back not-present. Neither reference uses this packing and
neither can (tiny386 uses `w2 >> 8`, DOSBox-X a bitfield struct), so when a line
from either is cited for a rule, the rule is what is cited — never a bit index.

## 5. Faults, and why they EXIT rather than dispatch

There is no protected-mode IDT gate walk yet, so a segmentation fault cannot be
delivered. It is recorded — vector in `cpu->pmode_excp`, error code in
`cpu->pmode_err` — and the interpreter exits with `X86_EXIT_GPF` carrying the
error code in `exit_info`. That makes an undelivered fault loud and readable
rather than lost, and delivery becomes a later step rather than a silent gap.

Error codes are the selector with RPL cleared (`sel & 0xFFFC`), uniform across
both references, except the null-SS and privileged-instruction cases which are
a literal 0 (`src/cpu/cpu.cpp:2997`).

### The stack-pointer bug this uncovered

An exit taken from inside a handler's subroutine call leaves that handler's own
pushes on the stack: every caller of `L(SET_SREG)` does
`PUSH {LR} / BL / POP {LR}`, and a fault leaves without reaching the `POP`. The
final `POP {R4-R11, PC}` then reads one word too high and branches to whatever
the handler saved — a wild jump that presents as *the interpreter never
returning*, which is exactly how it was found.

**This was already latent**: the old `TST CR0,#1 / BNE .exit_loop` at the top of
`L(SET_SREG)` had the identical imbalance and simply never fired, because no
guest in the tree ever set PE. The fix is general rather than local — the
prologue records SP in `cpu->host_sp` and `.save_state_and_exit` restores it, so
every exit path is stack-safe however deep the handler was. One store per
interp call, one load per exit.

## 6. The route for per-access LIMIT enforcement

`x86_pmode_limit_ok()` is written and tested (expand-up, expand-down, the
offset wrap, the B-bit ceiling) but **is not on the access path**. Putting it
there naively is precisely the tax §2 exists to avoid.

The route is the **mode record** (`x86_build_mode_records`, `x86_cpu.c`). The
record already selects the ModRM decoder by address size at zero cost — one
`EOR` on `R2` for `66`/`67`. Protected mode is another bit on the same
selector: a fifth and sixth record whose `REC_MODRM_FN` points at a
limit-checking decoder, with real mode dispatching to the decoders it uses today
and never executing the check.

Cost of that step, which is why it is a step and not part of this one: the
record block doubles (8 KB → 16 KB of DTCM, against 63,268 B free) and a second
`DECODE_MODRM` must fit in ITCM (~12.7 KB free). Both are affordable; neither is
free enough to do without measuring.

## 7. Negative controls

`scripts/run_pmode_controls.py`. Sixteen checks, each compiled out individually by
`X86_PMODE_NEGCTL=n`, each required to make `tests/test_pmode.py` FAIL. It runs
a baseline arm first — with everything on the suite must PASS, or every arm
below "fails" for an unrelated reason.

**Three arms survived on the first run**, which is the argument for the whole
exercise:

- **Table bound.** The test used a selector past the limit with *nothing* at
  that address, so removing the bound merely moved the fault to the S-bit check
  and the arm still passed. Fixed by placing a well-formed present descriptor
  one entry past the limit.
- **SS privilege.** Every SS arm used a selector that also failed the *type*
  rule, so removing the privilege rule changed nothing. Fixed by loading SS with
  a writable, present data selector whose only defect is its RPL.
- **CS type.** No arm loaded CS in protected mode at all. Fixed by adding the
  far `JMP` arms — and the data selector it jumps to deliberately shares the
  code selector's base, so a survivor would find the same `HLT` and could not be
  mistaken for a pass.

A fourth, granularity, was caught only because the test uses a limit **field of
zero** with G set: with a non-zero field an ignored G still yields a distinct
number and the check looks live.

Sixteen now, not ten: the interrupt path adds 11 IDT bound, 12 gate type,
13 gate present, 14 gate DPL and 15 frame width, and `IRET` adds 16. They are
numbered ON from the walk's rather than renumbered into it, so an arm's number
keeps meaning the same check across commits.

**A fifth survivor-shaped trap, caught in the gate work**: the gate-DPL arm's
positive twin cannot end in a `HLT`. `HLT` is a **privileged** instruction, so
at CPL 3 it is itself a `#GP` — the twin delivered its interrupt perfectly and
then exited 13 on its own halt, which reads exactly like a refusal. There is no
ring-3-legal way to stop the interpreter, so both arms are graded on
`x86_pmode_gates` instead. That is the sharper check anyway: with the DPL rule
compiled out, the REFUSED arm delivers and its counter moves off zero.

## 8. Interrupt delivery, and the two things that decide the frame

`x86_pmode_int_gate()` and `x86_pmode_iret()`. The authority is DOSBox-X's
`CPU_Interrupt()` (`src/cpu/cpu.cpp:1200`) and `CPU_IRET()` (`:1509`).

**The frame WIDTH on the way in is the GATE's, not the operand size** —
`gate.Type() & 0x8` (`cpu.cpp:1443`). A 16-bit code segment may perfectly well
have a 386 trap gate, which is why the dual-emitted `PUSH_V` could not have
built this frame and why the whole path is C. On the way OUT it is the
INSTRUCTION's operand size (`cpu.cpp:1509`). The two need not agree, and a guest
that mismatches them is wrong on silicon too.

**The STACK width is neither: it is SS's B bit** (`cpu.cpp:1401-1411`). ESP is
wrapped to that width *before* SS.base is added, so a 16-bit stack cannot walk
out of its segment. The pushes go through `x86_guest_copy_in/out` and not a
hand-rolled fold walk, because a push at the top of a granule straddles the seam
and those two are the only functions that split it (`docs/10`).

**The IDT error code is NOT a selector.** It is `vec*8 + 2 + EXT`
(`cpu.cpp:1312`) — the `+2` IS the IDT bit, and the `+1` EXT separates an
external interrupt from a software one. Reusing `sel & 0xFFFC`, which is what
the segment path does and what the eye expects, produces a number that means a
GDT selector and sends whoever reads it to the wrong table.

**`IRET`'s CS rules are STRICTER than `L(SET_SREG)`'s CS arm.** That arm checks
only "is it code" and "is it present", which is right for a far `JMP` and too
loose for a return: a non-conforming target must have DPL == RPL and a
conforming one DPL <= RPL (`cpu.cpp:1710-1721`). Nothing else in the suite can
grade that, which is what negative control 16 is for.

**`IRET`'s pops are SPECULATIVE.** A CS that fails must leave ESP reading
exactly as it did before the instruction, so the pops happen inside the C
function rather than being handed to it; DOSBox-X restores `orig_esp` on its
fault path (`cpu.cpp:1520`) for the same reason.

**Two things the assembly was getting wrong, both invisible in real mode.**
`POP_V R11` at 16 bits ZEROED the high half of EFLAGS — nothing lives up there
in real mode, VM and AC do in protected mode, and a 16-bit `IRET` carries the
high half over (`cpu.cpp:1683`). And which popped bits may take effect is
CPL-dependent: at CPL 0 all of them, above it IOPL is protected and IF is
protected once CPL exceeds IOPL. The mask is DERIVED even though this path is
same-privilege, because "it is always 0 here" is the assumption that later hands
a ring-3 `IRET` IOPL 3.

**A hardware IRQ arrives through the same label and is treated as a SOFTWARE
interrupt.** The only thing that distinguishes them is the gate-DPL check, which
cannot fire at CPL 0. This is a known, bounded divergence and it is one line to
fix — but fixing it before measuring whether any title ever leaves CPL 0 would
be building for a case nobody has shown exists.

## 9. The descriptor-inspection instructions, and the lane MEASUREMENT DELETED

`x86_pmode_lar/lsl/verrw/ltr` in `x86_pmode.c`; group 6 and `0F 02`/`0F 03`/
`0F 06` in `cpu_exec_handlers_0f.S`.

**The demand is static and it was counted, not guessed.**
`scripts/opcode_demand.py` over DOS4GW.EXE: **58 `LAR` sites, 28 `LSL`,
5 `CLTS`, 5 `STR`, 2 `LTR`** — one of the `LTR`s at `+0x476e`, in the
PE/paging re-entry path. Every one of them reached `.exit_loop`.

**The same scan deleted roughly 1,225 planned lines.** The extender's IDT
template is 256 entries and **every one is DPL 0**, and **no DPL-3 CODE
descriptor is built anywhere in the image** — ring-3 code could not issue
INT 21h at all. **DOS/4GW RUNS ENTIRELY AT CPL 0.** Call gates, TSS stack
switches, task switching and `IRET`-across-privilege are therefore not deferred,
they are **not wanted**, and building them would have been building for a case
nobody has shown exists.

**An `LTR` is NOT evidence of a ring change**, and that is the trap this step
walks past: a TSS at DPL 0 is wanted for SS0:ESP0 and the I/O permission bitmap
whether or not a ring ever changes. So the task register is loaded, its
descriptor is validated as an **available** TSS (a busy one is `#GP`, which is
what stops a task being entered twice) and marked busy in guest memory — and no
task switch is implemented.

### The two rules that are easy to get wrong

**The accessible-type table is not "any present descriptor",** and it is wrong
in *both* directions. An interrupt gate is present and well-formed and is NOT
accessible to `LAR` (`cpu.cpp:2824`); a not-present descriptor IS accessible to
it, because reporting P is the whole point of reading an access-rights byte.
`LAR` and `LSL` differ over exactly one descriptor class — a **call gate**,
which `LAR` reads and `LSL` refuses because a gate has no limit — and negative
control 20 is that one descriptor.

**`LAR`'s result is not the raw access byte and not our `flags` word.** It is
the second descriptor dword with its low byte masked: the access byte at bits
8-15, the granularity byte at 16-23 (`cpu.cpp:2856`). Our packing (§4) puts the
access byte at bits 8-15 of a **16-bit** word and the granularity byte at 0-7,
so the value is **derived from the packing** rather than lifted from `w2` —
citing a reference for a bit index is the mistake §3 names and this is the site
it names it for.

**`LAR`/`LSL`/`VERR`/`VERW` NEVER FAULT.** An inaccessible selector is `ZF=0`
and nothing else, so they go through a non-faulting `read_desc_raw()`; routing
them through `read_desc()` would record a phantom vector in `cpu->pmode_excp`
and move `x86_pmode_faults`, which is the counter the walk arms are graded on.
And all four are **`#UD` in real mode** — the handlers test `CR0.PE` and take
`.exit_loop`, so a real-mode title that lands on `0F 02` exits loudly with the
opcode named instead of walking a GDT it never installed.

### Cost

`tests/test_pmode.py` **164 checks**; `run_pmode_controls.py` **22/22 seen to
fail**. Real mode again pays **NOTHING**, and again by disassembly diff rather
than by argument: `--hot` reports **0 symbols changed**, the only entries being
the three ADDED handlers and six RELOCATED-ONLY. ITCM
**0xD978 → 0xDE60 (+1,256 B)**, `__CORE_BSS_END__`
**0x24065510 → 0x24065748 (+568 B)** — under one 4,104-byte pager frame, but
read `guest_ram_pages_short` before adding more.

### What has NOT been measured, and it is the next thing to do

**Where Dark Forces stops now is unknown.** The tier-2 run
(`probe_report.py --title darkforces`) does not reach the guest in a fresh
worktree: retro-go boots, registers the core and lists the ROM, and then sits in
its launcher with `core_magic` never set. **Reproduced identically on stock
master (`d2b82d1`), so it is not a protected-mode regression** — the retro-go
firmware and its SD content live in `references/game-and-watch-retro-go-sd`,
which a worktree reaches by symlink and must not rebuild while other agents are
working in the shared tree.

Until that is unblocked, **do not start call gates, stack switches, the TSS or
paging.** `docs/13` sizes them at ~1,225 lines and nothing above has measured
demand. The question worth answering first is the cheap one: *does the extender
ever change ring at all?* If DOS4GW runs everything at CPL 0 — which its refusal
of DPMI, VCPI and `INT 15h` makes likely — the entire privilege-change lane is
deleted rather than built.
