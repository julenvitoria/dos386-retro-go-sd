# Status

Last verified 2026-08-15. How to run: [`TOOLING.md`](TOOLING.md). Roadmap and
backlog: [`PLAN.md`](PLAN.md). Design: [`docs/`](docs/).

**This file is state, not history.** Under 100 lines — a status doc nobody reads is
where numbers go stale.

## Verification state

| Tier | State |
|---|---|
| 1. gwemu bare-metal | `test_opcodes` **273/273**; `test_video` **25/25 arms** at the shipping `EXTRA_PAGES=1`, Mode Y and EGA 0Dh **64000/64000**; `test_seam`, `test_pit`, `test_dos`, `test_paging`, `test_nor`, `test_sysinfo`, `test_xms` **47/47**, `test_ems` **40/40**, `test_text`, `test_keyboard`, `test_osk`, `test_disk_cache`, **`test_pmode` 80/80** (protected mode; 10/10 negative controls caught by `scripts/run_pmode_controls.py`) |
| 2. gwemu + retro-go | `core_dtcm_ok=1`, bonus pool 153,600 B, 1,000,001 insns — matches tier 1. **XMS is live and a shipped title uses it**: WOLF3D detects it, allocates and moves 430,636 B in 278 moves, `xms_opens=1`, 0 errors, at a 4% frame cost ([`docs/12`](docs/12_memory_tiers.md) §6) |
| 3. Hardware | ~**2.8 M insn/s** stock, ~**3.8 M** overclocked, 59 fps. ABI VERIFIED on silicon. **Disk reads are NOT a live cost — they freeze at t+36 s and never move again; SylverB's CMD18/DMA work is already in the flashed firmware.** The live cost is the pagefile WRITE |

`scripts/run_planar_controls.py`: all 6 negative controls caught + the LEARN positive
control. **Golden gate: WOLF3D 0.000%**, ALLEYCAT 0.874% = the moving cat, KEEN4
known-bad.

**Known-failing, pre-existing, not news:**

- `test_nor.py` — the arm never runs. PRE-EXISTING: reproduced on stock master
  with no local changes, so it is not news and not a protected-mode regression.
- `test_screen.py` — text-mode CLUT assertion, entries 17-19.
- `test_core_retrogo.py` — blitter/CLUT assertions whenever a real `.dsk` is on the
  card: they expect `demo_guest`'s pattern, which only runs with no disk.

## WHERE THE CYCLES GO — read this before optimising anything

Wolf3D, Mode Y, **hardware**, gated gameplay window, **% of WALL** throughout.
Method and menu control: [`docs/09`](docs/09_frame_pacing_and_clocks.md) §8.

| block | % wall |
|---|---|
| idle waiting for the panel (vsync-quantised, not freely convertible) | 27.0 |
| **planar store hook** | **25.0** |
| **interpreter excluding the hook** — of which: | **47.1** |
| — ordinary opcodes, the guest's own work, **~57 cyc/insn** ← LARGEST BLOCK | 29.4 |
| — `88` aperture byte store 10.3, `AB` REP STOSW 3.3, `89` word store 2.3 | 15.9 |
| — VGA `OUT`/`IN`, **1,242 cyc each** (`ega_shadow_rebuild()`) | 1.5 |
| Mode Y blit + paint | 0.84 |
| residual (root-zone self-time, measured not derived) | 0.05 |

An aperture byte store costs **171 cyc**; the same store to ordinary memory
costs **55**. ~1.67 M insn/s, ~16 presents/s.

**Open and sized:** `REP STOSW`'s per-element hook (~3%; the page cannot change
mid-`REP`, so the decision is resolvable once), aperture reads (~4.5%), the
1,242-cyc `OUT`.

### Measured and REJECTED — do not re-propose

| lever | measured |
|---|---|
| DMA2D for the blit | blit is 0.84% — the whole ceiling |
| `planar_store` in assembly | AAPCS is ~25 of 171 cyc, ≤2% of wall |
| Planes in DTCM vs AXI | 121.8 vs 120.8 cyc — identical; ITCM likewise nothing |
| `x86_code_reloc` | 0.79% (rate 14.8 k/s, not the 88 k/s first claimed) |
| `SEAM_CHECK` | 0.04% |
| `IDIV`/`IMUL` ("division-heavy raycaster") | `F7` is 4.2% at 55.2 cyc — below average |
| Disk reads / CMD18 / SD DMA | reads freeze at t+36 s; already in shipped firmware |

## Budgets — measure these before and after any core change

| | value | note |
|---|---|---|
| ITCM | ~**12.7 KB** free of 65,536 | `__CORE_ITCM_CODE_END__` = 0xCD08 (52,488 B used). The protected-mode walk cost **+912 B**, all of it cold: the out-of-line `SET_SREG_PMODE`, group 6/7 (`LLDT`/`SLDT`/`LGDT`/`LIDT`) and the `MOV CRn` transition |
| DTCM | **63,268 B** free above a bump at `0x2000A8DC` | the pool ends at `__dtc_padding_end__` = `0x2001A000`, NOT `0x20020000` — the 24 KB above it is the main stack. `core_dtcm_headroom_max` over-reported by exactly that for the whole project until now ([`docs/12`](docs/12_memory_tiers.md) §8) |
| RAM_EMU | `arm-none-eabi-nm build/core/dos_core.elf \| grep __CORE_BSS_END__` = **0x2405E960** (+704 B for `x86_pmode.c`) | **exhaustion is SILENT**: `guest_ram` returns NULL, `core_ready` stays 0, no message. Core TEXT is in this region too |
| arena | 143,360 B | check `x86_arena_largest`, not just `_free` — claims fragment |

## What is broken right now

| | where |
|---|---|
| Wolf3D's cached pictures render as noise (off-screen VRAM cache too small) | [`docs/13`](docs/13_title_status.md) |
| Wolf3D paints mid-draw frames in gameplay (`x86_modey_mid_draw`) | [`docs/08`](docs/08_vga_mode13h.md) §4 |
| **Wolf3D's WEAPON SPRITE is corrupt** — a stable band of wrong pixels across the pistol at the spawn. DETERMINISTIC, so gradeable; may share a cause with the "rare" stage-select corruption | Queue B1 |
| **KEEN4 hangs, then the WATCHDOG resets** — HARDWARE, every run, freeze point VARIES (14.9 M/347 … 31.5 M/638 frames); both counters freeze, `boot_magic=0xbad00005`, reboot, repeat. **NOT the pagefile, and NOT `ega_shadow_rebuild()`** (both below). The guest keeps executing across the freeze, so **one SLICE never returns**. Reaches the MAIN MENU and runs indefinitely under gwemu | [`docs/13`](docs/13_title_status.md) |
| **ONE 4 KB PAGEFILE WRITEBACK COSTS UP TO 433 ms ON SILICON** (`x86_pg_wb_us_worst`) vs 23 ms for a sector read, and GROWS over a session. The per-page `fopen`/`fclose` is fixed; `pgf_us_seek` vs `_us_write` says what the rest is, `FF_USE_FASTSEEK 0` the suspect. Gates Keen and anything EMS/XMS-backed | `src/main.c` |
| ~~KEEN4 gives 60 of 160 frames to the arena~~ **FIXED**: `X86_PG_DONATE_RESERVE` 100 -> 134 (its measured peak). Pool stays 160, evictions 84 -> **0**, pagefile writes 82 -> **0**, on hardware too. Cost: the card is the arena's 35,840 offsets, not the donated 61,440 — and donation is now **inert for this title by construction** (a claim cannot span regions, so beating the arena needs ~35 frames; Keen spares 26) | [`docs/12`](docs/12_memory_tiers.md) |
| Retro-go's Pause overlay is unreadable over 256-colour modes | [`docs/08`](docs/08_vga_mode13h.md) §1 |
| **INT 21h AH=4Bh (EXEC) is built but OFF in the shipping core** (`X86_DOS_EXEC`). WOLF3D calls it with a loadable image and honouring it kills the title: `guest_exit_reason`=5 (MEM_FAULT), `guest_insns_total`=0. The load, not the transfer — the pager has nowhere to put ~30 untouched guest pages mid-run | `PLAN.md` Queue C3 |
| **`X86_VPROF=1` DOES NOT FIT and fails SILENTLY** — the flag adds ~1,080 B to RAM_EMU, `guest_ram` comes back NULL, `core_ready` stays 0 and the frame loop runs painting nothing. Reproduced on STOCK master, KEEN4, tier 2 (`core_ready`=0 at 1, `core_ready`=1 and 157 M insns at 0). The same shape as the `X86_PROBE` trap below | `Makefile.core` |
| **`scripts/probe_report.py` is VOID on every title tried** — `core_ready`=0, `guest_insns_total`=0 under `X86_PROBE=1`, on MASTER as well as this branch. The project's "what did this title ask for" instrument does not currently work | `CLAUDE.md`, the X86_PROBE trap |
| **Interpreter gaps**: seg override before `REP` survives one element; `moffs` uses operand not ADDRESS size; `MOV SS,x` does not inhibit interrupts; 5 handlers ignore `0x67`; 13 slots exit (x87, `BOUND`/`ARPL`, `XLAT`, `WAIT`, `INT1`) | [`docs/07`](docs/07_opcode_coverage.md) |

## Protected mode — the descriptor walk is IN, real mode pays NOTHING

`CR0.PE` no longer traps out: `LGDT`/`LIDT`/`LLDT`, GDT/LDT fetch, type/DPL/
present with the right vector and error code, cached per-segment descriptor,
`CR0.PE` both ways, same-privilege far JMP. `test_pmode.py` **80 checks**;
`run_pmode_controls.py` **10/10 seen to fail** when compiled out. Zero
real-mode cost proven by DISASSEMBLY DIFF, not argued — `.fetch_decode_loop`
and every `.op_*` handler byte-identical. **`test_benchmark` cannot decide
this**: under gwemu it spans 10.30–11.88 M cycles on the BASELINE alone. NOT
built: gates, `IRET` across privilege, stack switches, paging, per-access limit
enforcement. [`docs/14`](docs/14_protected_mode.md).

## Retractions worth keeping

One line each; their value is stopping a wrong answer being re-derived.

- **`references/tiny386` HAS NO PROTECTED-MODE PERMISSION CHECKS.** `set_seg()` (i386.c:1076) carries `// TODO: various permission checks` and does no DPL/RPL/CPL validation. Right SHAPE, wrong RULES; DOSBox-X's `CPU_SetSegGeneral` (src/cpu/cpu.cpp:2976) is the authority on the rules.
- **ALLEYCAT's golden 0.874% is NOT a palette bug.** Owner-confirmed: the cat moves and the capture never lands twice on one frame; its mask is too narrow for that travel.
- **Keen 4's levels are NOT impossible.** The verdict quoted the prior port's budget as if it were this hardware's. [`docs/13`](docs/13_title_status.md).
- **Do not derive the frame period from `guest_fps_achieved`.** It is a runaway: overrun lowers fps, raising the period, raising the budget. [`docs/09`](docs/09_frame_pacing_and_clocks.md) §2.
- **Wolf3D's split/mirrored gameplay was NOT a video bug.** `INC`/`DEC r16` never normalised their operand, corrupting `finetangent[]`. `test_alu_fuzz.py` grades it.
- **A pager counter read while no core is loaded is NOISE.** Check `core_ready`. **And KEEN4's wedge is NOT pagefile thrash.** Tested on SILICON with the donation refused and the pagefile provably untouched (`donated=0 frames_total=160 evictions=0 wb_file=0 wb_us_worst=0`) -- it still wedges. `x86_pg_exhausted=0` fits thrash AND an unrelated hang; only the writeback counters separate them. **And it is NOT `ega_shadow_rebuild()` either** -- measured on silicon with a counter and a DWT bracket, not sampled: one rebuild is 846,000 cycles (3.0 ms), a whole pre-wedge run does **10** of them, the burst coincident with the wedge is ~230 ms against a ~472 ms WWDG window, and fixing the guard (steady state 10 -> 2) leaves the burst and the wedge untouched. **The instrument replaced the conclusion it was built to test**: across the wedge the rebuild counter KEEPS ADVANCING while both frozen counters do not, so the guest is still executing and it is ONE SLICE THAT NEVER RETURNS, not a stopped frame loop. [`docs/13`](docs/13_title_status.md).
- **`MPU_TEX_LEVEL1` on the framebuffer measured no gain.** Patch at `build/artifacts/mpu_level1.patch`.
- **Mode Y's cost is NOT plane-storage placement.** The plane RMW cost the SAME in DTCM as in AXI (121.8 vs 120.8 cyc, hardware) — because the cost was the READ, now gone.
- **The Mode Y BLIT and PRESENTATION are 0.84% of wall** -- DMA2D's ceiling on this path is 0.84%. The blit was never the cost.
- **FIXED: the planar store read 4 bytes to write 1.** Nothing was computed from the cell's old contents. Store 286.9 -> **171.1 cyc**, WOLF3D **13.5 -> 16.3 fps (+21%)**.
- **Wolf3D's interpreter is NOT slower in gameplay, it runs a different MIX** (90% of 66 -> 125 cyc/insn): aperture STORES, not `IDIV`; `x86_code_reloc` **0.79%** of wall, `SEAM_CHECK` **0.04%**. Both §8-9 of [`docs/09`](docs/09_frame_pacing_and_clocks.md). > **An inherited number from another project is not a property of this hardware** —
> and neither is a bound that was never checked against the map.

## Where the design lives

[`docs/01`](docs/01_memory_architecture.md) fold table, address translation; [`docs/02`](docs/02_cpu_pipeline_optimization.md) Thumb-2 interpreter, dual-issue; [`docs/03`](docs/03_devices_and_hle.md) device model, DOS HLE; [`docs/04`](docs/04_retro_go_and_gwemu.md) firmware ABI, gwemu's limits; [`docs/06`](docs/06_gwemu_determinism.md) determinism; [`docs/07`](docs/07_opcode_coverage.md) opcode coverage; [`docs/08`](docs/08_vga_mode13h.md) mode 13h, Mode Y, EGA 0Dh, presentation; [`docs/09`](docs/09_frame_pacing_and_clocks.md) frame pacing and the three clocks; [`docs/10`](docs/10_granule_seam.md) the granule seam; [`docs/11`](docs/11_pit_and_interrupt_controller.md) PIT, 8259, and what reads them; [`docs/12`](docs/12_memory_tiers.md) pager tiers, arena, frame donation; [`docs/13`](docs/13_title_status.md) per-title state and settled verdicts; [`docs/14`](docs/14_protected_mode.md) protected mode, the descriptor walk
