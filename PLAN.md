# Plan

**Goal**: rewrite TinyEMU's 32-bit x86 core as ARM Thumb-2 assembly for the
STM32H7B0VB running Retro-Go.

State: [`STATUS.md`](STATUS.md). Running: [`TOOLING.md`](TOOLING.md). Design:
[`docs/`](docs/). **Roadmap and backlog — a list, not a narrative.** Rationale
and measured history belong in `docs/`, not here.

## Phases

1. **Memory** ([`docs/01`](docs/01_memory_architecture.md),
   [`docs/10`](docs/10_granule_seam.md),
   [`docs/12`](docs/12_memory_tiers.md)) — DONE: checked translation
   (`host = lin + k`, `FOLD_INVALID` costs one `ADDS`+`BMI`), the granule seam,
   eviction, the SD pagefile, frame donation. **Dead, measured**: NOR backing.
   Eviction *policy* is not the lever — LRU caps at 2.1x over FIFO while the
   pool-size cliff is 350x.
2. **Interpreter** ([`docs/02`](docs/02_cpu_pipeline_optimization.md)) — hot
   loop in ITCM, `R8`-`R11` reserved (`CLAUDE.md`, binding). `LEA` is the
   decode floor to watch.
3. **Devices and HLE** ([`docs/03`](docs/03_devices_and_hle.md)) — DOS file I/O
   straight to FatFs. 13h, Mode Y and EGA 0Dh are pixel-exact
   ([`docs/08`](docs/08_vga_mode13h.md)); timing in
   [`docs/11`](docs/11_pit_and_interrupt_controller.md).

## Validation hierarchy

Strictly in this order — `TOOLING.md`, and `CLAUDE.md` on never skipping a tier.
**1.** gwemu bare-metal (Thumb-2 logic, ITCM/DTCM/AXI placement) · **2.** gwemu
+ Retro-Go (ABI, DTCM, SD, video, input, audio) · **3.** hardware — the only
tier that sees the pipeline, peripheral init or real memory ordering.

## Backlog

### Queue A — make the shipped titles good

**Owner's definition (this supersedes the old A/B split).** Everything that is
not Dark Forces lives here.

1. **Per-game `.cfg`** — bring the prior port's implementation over; the owner
   wants to KEEP that design. It is already a clean two-layer split and both
   halves are in-tree:
   - `references/8086tiny/dos_settings.{c,h}` owns the FORMAT and the policy —
     defaults, ranges, what a corrupt file means, what is written and omitted —
     and deliberately does NO I/O, which is what let it be tested without a
     machine (`test286/runsettings.sh`).
   - `references/dos_port_prior/dos_settings_rg.{c,h}` is the retro-go binding:
     work out the path, read the bytes, hand them over; write them back on a
     menu change. Cold path only.
   Scope is **every Options entry AND launch parameters** per title, not just
   the CPU profile. We already have `odroid_dialog_choice_t` options
   (`src/main.c:2138`, the CPU profile callback) to feed it.
   **The trap is recorded in that header and must be honoured: `MAX_OPEN_FILES`
   is 8 across the WHOLE firmware, the DOS core already holds three for its
   entire run plus one for the pagefile, and exhaustion is SILENT — every other
   `fopen()` starts returning NULL and the UI degrades with nothing logged. The
   launcher once rendered as diamonds because of it.** Open, read, close; never
   hold a handle. Load BEFORE `dos_cpu_init()`'s equivalent here, because a
   setting read on the segment-load path applied late looks intermittent.
2. ~~**Guest RAM -> NOR backup path**~~ — DIAGNOSED and FIXED IN CODE, **not yet
   confirmed on silicon**. It is a watchdog, and the flash is innocent: the
   firmware's transport kicks throughout (`OSPI_Program()` per 256-byte page,
   and its comment names the **~472 ms WWDG window**; `OSPI_Erase()` per
   command; the SD driver in its busy-waits). The two un-kicked stretches are
   ours: (a) the snapshot is a **menu callback**, so the frame loop's
   once-per-frame refresh is absent for all 1,595–1,614 ms of it, and
   `x86_nor_attach()`'s 640 KB XIP readback touches neither driver; (b) **the
   first slice after the attach** runs the AXI-tuned `max_slice` at NOR prices,
   with every access and every instruction fetch now trapping to OCTOSPI — and
   the slice regulator corrects by 1/8 per frame, so it cannot save a frame it
   does not survive. (b) is what matches "seems to work, then bluescreens".
   Fixed by kicking across all three of ours and resetting the regulator to
   `MAX_SLICE_FLOOR` (debt cleared) at attach.
   **What is left is one tier-3 run**: read `nor_snap_wdog_gap_ms` (worst gap
   between kicks) against 472 ms, and `guest_exec_ms` on the frames after the
   attach. The device was held by another agent all session.
   Also settled: `test_nor` fails because its opening store to `0x90000000`
   **bus-faults** — the harness never sets OCTOSPI up, so the arm's premise is
   false. Separate fix, in `tests/main.c`. `tests/test_nor_wdog.py` covers the
   heartbeat without that window (12 checks; negative control 5/12 seen to fail).
3. **Sound Blaster** — not implemented at all. Ports 0x220/0x388 are unanswered.
   Size it against what real titles actually probe rather than against the
   spec; Epic Pinball and Mortal Kombat diagnoses are being measured now and
   will say what is really asked for.
4. **Epic Pinball** — if it is not running by the time 1-3 are done, making it
   run is the LAST task in Queue A.

Also live here, measured and sized, unblocked whenever a slot frees:

- **The 433 ms pagefile writeback** — `x86_pg_wb_us_worst` on silicon, against
  23 ms for an ordinary sector read, and it GROWS over a session. The per-page
  `fopen`/`fclose` is already fixed; `pgf_us_seek` vs `pgf_us_write` says what
  the rest is and `FF_USE_FASTSEEK 0` is the suspect (`f_lseek` walks the
  cluster chain from byte zero every call). Gates Keen and everything
  EMS/XMS-backed.
- **`REP STOSW`'s per-element planar hook** — ~3% of wall. The page cannot
  change mid-`REP`, so the decision is resolvable ONCE instead of ~38 times.
- **Keen 4's watchdog wedge** — 34.7 M instructions over 944 frames, then it
  stops; `boot_magic=0xbad00005`. Being measured now.
- **Wolf3D** is visually done (owner's verdict). What remains is FRAME RATE and
  sound. Remaining measured levers: the `REP STOSW` hook, aperture reads
  (~4.5%), and the 1,242-cycle `OUT` (`ega_shadow_rebuild()` on GC writes).
- Retro-go's Pause overlay is unreadable over 256-colour modes.

### Queue B — Dark Forces. NOT DONE UNTIL DARK FORCES RUNS.

**Nothing here starts before the step above it is measured.**

1. ~~**XMS**~~ — built. A BLOCK-COPY interface, so its memory is never folded
   and costs the interpreter nothing; it lives in `x86page.swp` above every
   pager slot. Advertises 4 MB, **0 with no pagefile**. WOLF3D uses it
   unprompted at a 4% frame cost. [`docs/12`](docs/12_memory_tiers.md) §6.
2. ~~**EMS**~~ — built (LIM 3.2). The OPPOSITE shape: a MAPPED interface, so the
   four 16 KB windows at `E000:0000` must be folded and pinned on map / released
   on unmap. Logical pages are pager pages at linear 4 MB. Advertises 1024 KB,
   **0 with no pagefile**. `tests/test_ems.py`, `x86_ems.h`.
3. ~~**INT 21h AH=4Bh EXEC**~~ — BUILT (AL=00; AL=01/03 refused loudly), and
   **OFF in the shipping core** (`X86_DOS_EXEC`, `Makefile.core`). Shares
   `dos_load_mz()` with the first program; the parent resumes through its OWN
   stacked INT 21h frame, so the stub's IRET is the only transfer. Real parent
   and child in `tests/test_dos.py`, three negative controls seen to fail.
   **WHY IT IS OFF: WOLF3D CALLS AH=4Bh AL=00 WITH A LOADABLE IMAGE** and
   honouring it kills it — `guest_insns_total`=0, `guest_exit_reason`=**5
   (MEM_FAULT)**, golden gate: no picture. Bisected at tier 2: refusing first
   PASSES, doing the LOAD then undoing it FAILS, and the old AX=0001 on a
   failed load still FAILS — so the load SUCCEEDS and it is the load that
   kills. **EXEC's blocker is MEMORY, not DOS semantics**: ~30 untouched guest
   pages, mid-run, with the pool already full. Queue A, and it gates the flag.

4. **DPMI** (INT 2Fh AX=1687h, INT 31h) — needs 1 and the descriptor walk from
   5. This is what DOS/4GW actually asks for; VCPI is the fallback it tries
   first. **INT 2Fh answers only AX=43xx today**, so 1687h and DE00h both
   return with the guest's registers untouched.
5. **Protected mode** — **the DESCRIPTOR WALK is BUILT and PROVEN**
   (`x86_pmode.c`, `tests/test_pmode.py` 80 checks,
   `scripts/run_pmode_controls.py` 10/10 negative controls SEEN TO FAIL).
   `LGDT`/`LIDT`/`LLDT`, the GDT/LDT fetch and its bound, the type/DPL/present
   rules with the right vector and error code, the cached per-segment
   descriptor, `CR0.PE` both ways, a same-privilege far JMP.
   **It costs real mode NOTHING, proven by disassembly diff**: `L(SET_SREG)`
   was already the single funnel for every segment load and already tested
   `CR0.PE`, so only the branch target changed; the fetch loop and every opcode
   handler are byte-identical. +912 B ITCM, +704 B RAM_EMU.
   REMAINING, each its own step, none started: call gates, `IRET` across
   privilege, stack switches, paging, and per-access SEGMENT LIMIT enforcement
   (the predicate is written and tested; putting it on the access path for free
   needs a protected-mode MODE-RECORD axis — the same trick `66`/`67` already
   use — which doubles the record block and needs a second `DECODE_MODRM` in
   ITCM).
   **V86 is not needed** — INT 21h is HLE'd. **Not sized against Dark Forces**:
   the title has never executed one protected-mode instruction here, so nothing
   remaining in this item is measured demand. Re-size after 3 lets DOS4GW.EXE
   start and 4 says what DPMI actually asks for.
   *Two references, for two different things:* `references/tiny386`
   (i386.c:1076) for the compact SHAPE, `references/dosbox-x`
   (src/cpu/cpu.cpp:2976) for the RULES — tiny386 has NO permission checks, its
   own `// TODO` says so, so copying it wholesale produces checks that can never
   fire. [`docs/14`](docs/14_protected_mode.md).
6. **Dark Forces** — RE-MEASURED 2026-08-15, tier 2, XMS and EMS live with a
   pagefile (`x86_pg_frames_total`=160, `x86_xms_total_kb`=4096,
   `x86_ems_total_pages`=64). **The pre-XMS verdict is wrong and must stop being
   quoted.** The screen does read `Not enough memory on exec` / `Bad format on
   exec`, but those are DOS/4GW STUB strings and the cause is not memory:
   `x86_hle_unhandled_vector`=0x21, `x86_hle_unhandled_func`=**0x4B**. DARK.EXE
   opens `DF/dos4gw.exe` and EXECs it, we answer "invalid function", the stub
   prints its error and the guest derails into the IVT (`guest_exit_reason`=4,
   EIP 0x5E, opcode 0x63). **It never asks for extended memory at all**:
   `x86_xms_detects`=0, `x86_ems_device_opens`=0, and no INT 2Fh 1687h/DE00h.
   Its only memory refusal is `AH=4Ah` grow-past-a-neighbour returning error 8,
   which is real DOS semantics (A1).

**The Dark Forces re-measure after EXEC is NOT DONE, and the reason is
tooling**: `probe_report.py` reports `core_ready`=0 / `guest_insns_total`=0 for
`darkforces` AND `wolf3d` — the `X86_PROBE=1` RAM_EMU trap in `CLAUDE.md`,
VERIFIED ON MASTER with this branch stashed. There is no working "what did it
ask for" instrument until that is fixed.

**Measure at each step, not at the end.** Twice now this chain has been judged
on an artefact: first a 640 KB denominator that existed only because no
pagefile was wired, then a message that named the wrong subsystem.

### Queued — SylverB's FatFs DMA + SPI perf work

`references/FatFs-DMA` (branch `DMA`, `6d6f6fb`). **BLOCKED on upstream** — the
driver file is in his FatFs repo but the retro-go-sd plumbing (DMA stream setup,
a transfer buffer in D2/AHB SRAM, cache maintenance) is not. Do not start half
of it. The read-side commit (`fd1ff62`, CMD18 multi-block) is **already in the
firmware we flash** — baseline, not a pending win — and it would not have
helped: both commits are READS and the pagefile's cost is a WRITE.

### Not scheduled, but live

- **VBE 320x240** — the only mode that fills the panel and reduces the blit to
  one `memcpy`. Custom mode number, so little period software asks for it.
- An **adapter-advertisement Options entry** (CGA / EGA+VGA / VGA-only).
- **`x86_vga_planar_store` in assembly** — gated on re-profiling first; the case
  rested on pre-optimisation data. **If the path is a single-digit share of
  frame time, the answer is no.**
