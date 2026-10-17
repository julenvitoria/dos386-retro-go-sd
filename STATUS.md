# Status

Last truth-pass 2026-09-02 against `f0594d1`. Running: [`TOOLING.md`](TOOLING.md). Roadmap: [`PLAN.md`](PLAN.md). Design: [`docs/`](docs/).

**This file is state, not history.** Under 100 lines — a status doc nobody reads is where numbers go stale.

## TIER 2 IS BACK (2026-10-16) — it was a stale gwemu, not us

- **Cause, now settled**: gwemu `a35a9b9acc` (2026-08-19) modelled TIME on PA2 and held PC5 low as a PGOOD sense; our retro-go reads TIME from PC5 (`main.h:155-156`), saw `B_TIME` held from reset, skipped auto-launch and ate every key. gwemu itself has reverted to **TIME=PC5, PGOOD=PA2** (its `gnw_h7b0_gpio.c`, 2026-10-10 tree: "TIME must never drive PA2"). No pin mapping changed on either side.
- **Re-verified at tier 2**, gwemu 2026-10-10 + the external_cores firmware (`b791f155`): ALLEYCAT launches, `core_magic`='TINY', `core_ready`=1, frames advancing, 0 evictions in 55 s.
- **The frame-count conflict is SETTLED by measurement**: `guest_ram_pages` = **151**, `_pages_short` = **9** (neither 160 nor the static 154). The pool IS short: the NOR snapshot stays disabled (`nor_snap_result=2`) and the pagefile is reached sooner.
- **Guard**: `GwemuTarget` reads `GPIOC_IDR` while halted and refuses a gwemu holding PC5 low (`GWEMU_ALLOW_PC5_LOW=1` overrides), positive and negative control checked.
- Tier-2 items marked UNMEASURED below can now be measured; nothing dated before 2026-08-19 changed.

## 2026-10-17: Monkey, Keen 4 card, audio clock (silicon-checked by the owner)

- **Monkey Island EGA** boots (tick re-plan on a divisor change, `TICK_IRQ_MAX` 24); **VGA** finds its AdLib (INT 15h AH=86h). **Clicks** at 386SX were DMA halves replayed by overrun frames (17-20% of windows at 1598-sample lag on console recordings); `audio_pump()` now refills on `dma_counter` at every sub-slice boundary -- owner: "va genial" in both.
- **Keen 4 full EGA card** (two-piece card + working-set donation, docs/12 §5c) merged from `ega-full-card`; world map whole on silicon. Level entry is black ~7 s on the old core, ~9 s with the card -- OPEN. **Prince of Persia** freezes in room 1 with no prince at tier 2 back to `4776be0` -- OPEN.

## Verification state

| Tier | State |
|---|---|
| 1. gwemu bare-metal | **Re-derived statically this pass**: `tests/test_opcodes.py` **351 cases**, `tests/test_pmode.py` **164 checks**, `run_pmode_controls.py` **22 negative controls**. **NOT re-run this pass, unverified today**: `test_video` 25 arms with Mode Y and EGA 0Dh 64000/64000, `test_paging` 205, `test_xms` 47, `test_ems` 40, `test_seam`, `test_pit`, `test_dos`, `test_sysinfo`, `test_text`, `test_keyboard`, `test_osk`, `test_disk_cache`. `test_alu_fuzz` defaults to 4,000 cases; any larger figure is a run result, not a property of the tree |
| 2. gwemu + retro-go | **UP again (2026-10-16)** with a current gwemu — see above. Previous results (`core_dtcm_ok=1`, bonus pool 153,600 B, 1,000,001 insns; WOLF3D detecting XMS and moving 430,636 B in 278 moves, `xms_opens=1`, 0 errors) are dated 2026-08-15, BEFORE the break, and stand |
| 3. Hardware | ~**2.8 M insn/s** stock, ~**3.8 M** overclocked, 59 fps. ABI verified on silicon. **Disk reads are NOT a live cost** — they freeze at t+36 s. The live cost was the pagefile WRITE, now 5x cheaper ([`docs/12`](docs/12_memory_tiers.md) §1b) |

Golden gate (tier 1): **WOLF3D 0.000%**, ALLEYCAT 0.874% = the moving cat, KEEN4 known-bad. `run_planar_controls.py`: 6 negative controls + the LEARN positive.

**Known-failing, pre-existing, not news:** `test_nor.py` — its opening `memcpy` to `0x90000000` takes a precise bus fault (`CFSR` BFARVALID|PRECISERR, `BFAR` 0x90000000) because nothing in the bare-metal harness sets up OCTOSPI memory-mapped mode; the arm proves nothing until it does, and `test_nor_wdog.py` covers the watchdog heartbeat without that window. `test_screen.py` — text CLUT entries 17-19. `test_core_retrogo.py` — expects `demo_guest`'s pattern, so it fails whenever a real `.dsk` is on the card.

## WHERE THE CYCLES GO — read this before optimising anything

Wolf3D, Mode Y, **hardware**, gated gameplay window, **% of WALL**. Method: [`docs/09`](docs/09_frame_pacing_and_clocks.md) §8.

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

An aperture byte store costs **171 cyc**; the same store to ordinary memory **55**. **Open and sized:** `REP STOSW`'s per-element hook (~3%), aperture reads (~4.5%), the 1,242-cyc `OUT`. **Measured and REJECTED** — the list with its numbers is in [`CLAUDE.md`](CLAUDE.md), which every agent reads first.

## Budgets — `python3 scripts/budget_gate.py` IS the measurement

It forces a real re-link, refuses a stale ELF, and exits non-zero when ITCM, `__CORE_BSS_END__`, the implied frame count or `docs/07` regressed. Thresholds live in `tests/budgets.json` and carry the date they were taken.

| | value — `tests/budgets.json`, recorded 2026-09-02, stock flags, forced re-link |
|---|---|
| ITCM | `__CORE_ITCM_CODE_END__` = **0xF108** — **61,704 B used, 3,832 B free** of 65,536. The "~11.3 KB free / 0xD580" quoted here for several passes is STALE; the 0F bit and double-shift set alone cost +2,008 B |
| RAM_EMU | `__CORE_BSS_END__` = **0x24065748**. It MOVES every merge — measure it (`arm-none-eabi-nm build/core/dos_core.elf \| grep __CORE_BSS_END__`), never quote it. Frame stride is **4,104**, not 4,096 (granule-seam slack); a full 160-frame pool wants **656,640 B** |
| DTCM | **5,856 B** free (`core_dtcm_headroom_max`); the pool ends at `__dtc_padding_end__` = `0x2001A000`, not `0x20020000` — the 24 KB above it is the main stack ([`docs/12`](docs/12_memory_tiers.md) §8). Not an escape hatch for instruments |
| arena | 143,360 B — check `x86_arena_largest`, not `_free`; claims fragment |

**THE FRAME COUNT IS IN CONFLICT AND ONLY TIER 2 SETTLES IT.** The memory lane reclaimed 11,808 B (`8d597aa`) and concluded `guest_ram_pages` = **160**, `_pages_short` = **0** — *delta arithmetic* against a tier-2-measured 158/2-short baseline. The budget gate computes an *upper bound* of **154 frames** from `__RAM_EMU_END__ - __CORE_BSS_END__` (633,016 B headroom, 656,640 B wanted, short by 23,624 B). `scripts/common/budgets.py` states the derived count is an UPPER bound and the live `guest_ram_pages` is LOWER, because other RAM_EMU claims land first — so a live 160 **cannot** coexist with a bound of 154. Both cannot be right, and **tier 2 is the only thing that settles it**. Do not pick the flattering number. Until then assume the pool is short: read `_pages_short` after adding ANY RAM_EMU storage (one frame per 4,104 B comes out of the guest), and note a short pool disables the NOR snapshot (`guest_ram_pages != 160` -> `nor_snap_result=2`).

## What is broken right now

| | where |
|---|---|
| **KEEN4 has no music, and it is NOT the synthesiser** — `x86_opl_writes`=264, `keyons`=**0**: its driver never writes a note. Config right (`CONFIG.CK4`), detection works (`opl_timer_expiry=1`), WOLF3D drives the identical path and sounds. **Lead: `INT 08h` reaches our stub ~16/s = the 18.2 Hz chained BIOS rate, not the 140 Hz music tick.** | [`docs/13`](docs/13_title_status.md) |
| **Sound costs 6 pager frames.** Judged payable only because `x86_pg_evictions` stayed 0 across a full WOLF3D run. `X86_OPL_FM=0` returns every byte with detection intact. Silicon: ~5.9% of wall for Wolf3D's music, 10.4-13.8% worst case | `Makefile.core` |
| **The game port is now modelled as "no joystick present" (`9cb3e90`)** — WOLF3D's 2.35 M `IN 0x201` reads are answered; the frame-cost effect is UNMEASURED (tier 2 is back: measurable now) | `x86_io.c` |
| Wolf3D's cached pictures render as noise (off-screen VRAM cache too small); Wolf3D paints mid-draw frames in gameplay (`x86_modey_mid_draw`); its **WEAPON SPRITE is corrupt** — deterministic, so gradeable | [`docs/13`](docs/13_title_status.md), [`docs/08`](docs/08_vga_mode13h.md) §4 |
| **ONE 4 KB PAGEFILE WRITEBACK COSTS UP TO 433 ms ON SILICON** (`x86_pg_wb_us_worst`) vs 23 ms for a sector read, and grows over a session. The per-page `fopen`/`fclose` is fixed; `pgf_us_seek` vs `_us_write` says what the rest is. **`FF_USE_FASTSEEK` is NOT the suspect — `docs/12` §1b measured and ruled it out on silicon** | `src/main.c` |
| **The NOR snapshot's watchdog BSOD**: both un-kicked stretches were ours — the snapshot is a menu callback outside the frame loop across a 640 KB XIP readback, and the first slice after attach serves every fetch from OCTOSPI while `max_slice` is still tuned for AXI. FIXED in code; `nor_snap_wdog_gap_ms` settles it in ONE tier-3 run against the ~472 ms WWDG window. **NOT YET MEASURED ON SILICON**, and unreachable meanwhile because the pool is short (`nor_snap_result=2`) | `x86_nor.c`, `src/main.c` |
| **FM synthesis is present (emu8950); ADPCM still emits SILENCE** (`x86_sb_adpcm_silenced`) despite running, completing and interrupting. Retro-go's Pause overlay is unreadable over 256-colour modes | [`docs/03`](docs/03_devices_and_hle.md) §4f, [`docs/08`](docs/08_vga_mode13h.md) §1 |
| **`X86_VPROF=1` DOES NOT FIT and fails SILENTLY** — ~1,080 B of RAM_EMU, `guest_ram` returns NULL, `core_ready` stays 0, nothing paints. Third instance of that shape | `Makefile.core` |
| **Interpreter gaps**, from the GENERATED and gated `docs/07`: **one** one-byte slot exits — `63` ARPL; 192 of 256 `0F` slots exit; **x87 `D8`-`DF` are now IMPLEMENTED** to the depth the library demands (64-bit doubles, not 80-bit extended -- the trade and the out-of-scope list are in `docs/07`, "x87 -- what is built"); read `x86_x87_escapes` before believing a Dark Forces picture. Semantic gaps: seg override before `REP` survives one element; `moffs` uses operand not ADDRESS size; `MOV SS,x` does not inhibit interrupts; 5 handlers ignore `0x67` | [`docs/07`](docs/07_opcode_coverage.md) |

## Protected mode — the walk, IDT gate delivery, IRET and descriptor inspection are IN

`CR0.PE` no longer traps out, nor does `INT imm8` with PE set — the instruction Dark Forces and Mortal Kombat both stopped on — nor `LAR`, `LSL`, `VERR`, `VERW`, `STR`, `LTR`, `CLTS`. Built: `LGDT`/`LIDT`/`LLDT`, GDT/LDT fetch, type/DPL/present with the right vector and error code, cached per-segment descriptors, `CR0.PE` both ways, same-privilege far JMP, same-privilege IDT gate dispatch and `IRET`, group 6 complete, task register loaded and stored with NO task switch. Real-mode cost proven by DISASSEMBLY DIFF (`scripts/disasm_diff.py`), not argued: every hot symbol byte-identical, and the whole cost is 3 instructions at the top of `.op_iret_CF`, per IRET. `test_benchmark` cannot decide this — under gwemu it spans 10.30–11.88 M cycles on the baseline alone.

**DELETED, not deferred (~1,225 lines), on a STATIC measurement:** `opcode_demand.py` over DOS4GW.EXE finds all 256 IDT template entries at DPL 0 and NO DPL-3 code descriptor — **DOS/4GW runs entirely at CPL 0**. Call gates, TSS stack switches, task switching and `IRET`-across-privilege are gone. Still refused rather than approximated: paging and per-access limit enforcement. **NEXT IS A MEASUREMENT — where Dark Forces stops now — unblocked since tier 2 is back.** [`docs/14`](docs/14_protected_mode.md).

## Retractions worth keeping

- **The stack-seam hole is CLOSED (`aaafc76`).** `PUSH_V`/`POP_V` were multi-byte fold sites with no `SEAM_CHECK` — demonstrated by `test_seam.py` arm 1b, one stack address in 2,048. The guest's own POP read it back correctly, which is why it never showed. [`docs/10`](docs/10_granule_seam.md) §7.
- **Epic Pinball's `0F A0` PUSH FS wall is FIXED (`e70db05`)** — PUSH/POP FS,GS and LSS/LFS/LGS are in. Where it stops next is UNMEASURED: tier 2.
- **"WOLF3D does not detect the Sound Blaster" was never an SB defect — it was the CLOCK.** Its AdLib probe reads the FM status port ~135 times, an ISA cycle is ~1 us whatever the CPU is, and we charged those at the instruction rate. The FM card now carries `X86_ISA_IO_TICKS`; on SILICON `t1_gap` 56 -> 200, `opl_timer_expiry` 0 -> 1, `opl_writes` 6 -> 253, `x86_sb_resets` 0 -> 1. [`docs/03`](docs/03_devices_and_hle.md) §4e.
- **A general ISA-bus-time model was built, measured and DEFERRED** — physically right, but it moves the machine's clock +11%. A pacing change to grade on its own. **"EXEC AL=00h kills WOLF3D" was wrong**: `x86_dos_execs`=0 over 340 M insns; the reported symptom was the RAM_EMU starvation signature.
- **`references/tiny386` HAS NO PROTECTED-MODE PERMISSION CHECKS** — `set_seg()` (i386.c:1076) carries `// TODO: various permission checks`. Right shape, wrong rules; DOSBox-X's `CPU_SetSegGeneral` is the authority.
- **Keen 4's levels are NOT impossible** — that verdict quoted the prior port's budget as if it were this hardware's. **An inherited number from another project is not a property of this hardware.**
- **Do not derive the frame period from `guest_fps_achieved`** — it is a runaway. **Wolf3D's split/mirrored gameplay was NOT a video bug** — `INC`/`DEC r16` never normalised their operand, corrupting `finetangent[]`.
- **`boot_magic=0xbad00005` is `BOOT_MAGIC_BSOD | BSOD_WATCHDOG` — the device is ALREADY DEAD**, and every PC sampled after it is retro-go's button-wait spin, so `probe.py`'s stacked-frame decode is invalid there.
- **`cpu_state.pit_counter` (offset 2184) is stored once per guest instruction and is readable live** — `probe.py --deref cpu_state_ptr+2184=PITCNT` answers "is the guest executing right now?" with no build and no flag.
- **KEEN4's wedge is NOT pagefile thrash and NOT `ega_shadow_rebuild()`** — both refuted on SILICON with counters. The rebuild counter keeps advancing across the wedge while the frozen counters do not, so it is ONE SLICE THAT NEVER RETURNS.
- **Mode Y's cost is not plane placement** (DTCM 121.8 vs AXI 120.8 cyc); **the blit is 0.84% of wall**, which is DMA2D's whole ceiling; **the planar store read 4 bytes to write 1** — fixed, 286.9 -> 171.1 cyc, WOLF3D 13.5 -> 16.3 fps.

## Where the design lives

[`docs/01`](docs/01_memory_architecture.md) fold table; [`docs/02`](docs/02_cpu_pipeline_optimization.md) interpreter; [`docs/03`](docs/03_devices_and_hle.md) devices, DOS HLE; [`docs/04`](docs/04_retro_go_and_gwemu.md) firmware ABI, gwemu's limits; [`docs/06`](docs/06_gwemu_determinism.md) determinism; [`docs/07`](docs/07_opcode_coverage.md) opcode coverage (GENERATED); [`docs/08`](docs/08_vga_mode13h.md) video modes; [`docs/09`](docs/09_frame_pacing_and_clocks.md) pacing and the three clocks; [`docs/10`](docs/10_granule_seam.md) the granule seam; [`docs/11`](docs/11_pit_and_interrupt_controller.md) PIT and 8259; [`docs/12`](docs/12_memory_tiers.md) pager tiers, arena, donation; [`docs/13`](docs/13_title_status.md) per-title state; [`docs/14`](docs/14_protected_mode.md) protected mode
