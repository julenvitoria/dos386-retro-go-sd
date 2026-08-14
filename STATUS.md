# Status

Last verified 2026-08-14. How to run: [`TOOLING.md`](TOOLING.md). Roadmap and
backlog: [`PLAN.md`](PLAN.md). Design: [`docs/`](docs/).

**This file is state, not history.** Under 100 lines, deliberately — a status
doc nobody reads is where numbers go stale, and three wrong conclusions in one
session came from exactly that. Conclusions live in `docs/`; the run that
produced them does not live anywhere.

## Verification state

| Tier | State |
|---|---|
| 1. gwemu bare-metal | `test_opcodes` **273/273**; `test_video` all arms with Mode Y and EGA 0Dh **64000/64000**; `test_seam`, `test_pit`, `test_dos`, `test_paging`, `test_nor`, `test_sysinfo`, `test_xms` **47/47**, `test_ems` **40/40**, `test_text`, `test_keyboard`, `test_osk`, `test_disk_cache` |
| 2. gwemu + retro-go | `core_dtcm_ok=1`, bonus pool 153,600 B, 1,000,001 insns — matches tier 1. **XMS is live and a shipped title uses it**: WOLF3D detects it, allocates and moves 430,636 B in 278 moves, `xms_opens=1`, 0 errors, at a 4% frame cost ([`docs/12`](docs/12_memory_tiers.md) §6) |
| 3. Hardware | ~**2.8 M insn/s** stock, ~**3.8 M** overclocked, 59 fps, ~1.3 frame overruns/s. **The reworked (direct-pointer) firmware ABI is VERIFIED on silicon**: core registers, `core_ready=1`, `x86_vga13_ok=1`, guest advancing. KEEN4 ~1.37 M insn/s in 0Dh |

`scripts/run_planar_controls.py`: all negative controls caught + the LEARN
positive control. **Golden gate: WOLF3D 0.000%** (which is what proves
`lcd_set_clut_ext` against the reworked ABI), ALLEYCAT 0.874% pre-existing,
KEEN4 known-bad.

**Known-failing, pre-existing, not news:**

- `test_screen.py` — text-mode CLUT assertion, entries 17-19.
- `test_core_retrogo.py` — blitter/CLUT assertions whenever a real `.dsk` is on
  the card; they expect `demo_guest`'s pattern and the demo only runs when no
  disk mounts. The assertions need to be guest-aware.

## Budgets — measure these before and after any core change

| | value | note |
|---|---|---|
| ITCM | ~**17.5 KB** free of 65,536 | headroom roughly doubled by the opcode-threading work |
| RAM_EMU | `arm-none-eabi-nm build/core/dos_core.elf \| grep __CORE_BSS_END__` | **exhaustion is SILENT**: `guest_ram` returns NULL, `core_ready` stays 0, no message. Core TEXT is in this region too |
| arena | 143,360 B | check `x86_arena_largest`, not just `_free` — claims fragment |

## What is broken right now

| | where |
|---|---|
| Wolf3D's cached pictures render as noise (off-screen VRAM cache too small) | [`docs/13`](docs/13_title_status.md) |
| Wolf3D paints mid-draw frames in gameplay (`x86_modey_mid_draw`) | [`docs/08`](docs/08_vga_mode13h.md) §4 |
| Retro-go's `/CONFIG` magic is cleared by a probe reset (`Expected 0xcafef00d, got 0`) | cosmetic; settings return on the next clean boot |
| The planar store path halves the interpreter for graphics titles | [`docs/09`](docs/09_frame_pacing_and_clocks.md) §7 |
| **Wolf3D's WEAPON SPRITE is corrupt** — a stable band of wrong pixels across the pistol at the level-1 spawn. DETERMINISTIC (byte-identical in successive captures), so it is gradeable, unlike the "rare" stage-select corruption it may share a cause with | Queue B1 |
| **KEEN4 hangs, then the WATCHDOG resets the device** — measured on HARDWARE: `guest_insns_total` reaches 34,707,122 over 944 frames, then stops advancing; `boot_magic=0xbad00005 Watchdog`. It runs first and wedges partway in | [`docs/13`](docs/13_title_status.md) |
| **KEEN4 gives 60 of 160 pager frames to the arena** (`x86_pg_frames_total` 160 → 100 once 0Dh is up), so it runs on 100. EMS pins 16 more when used — a much tighter budget than the 144 the EMS work assumed | [`docs/12`](docs/12_memory_tiers.md) |
| ALLEYCAT renders CYAN where the golden has MAGENTA (worst pixel `rgb(0,170,170)` vs `rgb(170,0,170)`), 0.874% of pixels. A CGA palette-register selection difference, not noise. Pre-existing and stable; the golden itself was blessed on a stuck first frame | Queue B1 |
| Retro-go's Pause overlay is unreadable over 256-colour modes | [`docs/08`](docs/08_vga_mode13h.md) §1 |
| Keen's watchdog reset and half-height intro | [`docs/13`](docs/13_title_status.md) |
| A segment override before `REP` survives only the first element | `rep_resume` records the repeat prefix, not the START of the prefix run |
| `moffs` (`A0`-`A3`) takes its offset at the operand size, not the ADDRESS size | |
| `MOV SS,x` does not inhibit interrupts | can land on a half-switched stack |
| Protected-mode segment loads trap out (`CR0.PE`) | no descriptor walk; sized in [`docs/13`](docs/13_title_status.md) |
| Five handlers call `DECODE_MODRM` by symbol, ignoring a `0x67` prefix | |
| 13 opcode slots reach `.exit_loop`: x87 (`D8`-`DF`), `BOUND`/`ARPL`, `XLAT`, `WAIT`, `INT1` | |

## Retractions worth keeping

One line each, because their value is stopping a wrong answer being re-derived.

- **Keen 4's levels are NOT impossible.** The verdict quoted the prior port's
  budget as if it were this hardware's. [`docs/13`](docs/13_title_status.md).
- **Do not derive the frame period from `guest_fps_achieved`.** It is a runaway:
  overrun lowers fps, which raises the period, which raises the budget.
  [`docs/09`](docs/09_frame_pacing_and_clocks.md) §2.
- **Wolf3D's split/mirrored gameplay was NOT a video bug.** It was `INC r16`/
  `DEC r16` never normalising their operand — SF/ZF/OF came off bit 31 of a
  32-bit add — which corrupted `finetangent[]` and so the raycaster. Three
  sessions were spent in the planar path. `tests/test_alu_fuzz.py` grades this
  against Unicorn now; it is what should have been written first.
- **A pager counter read while no core is loaded is NOISE, not evidence.**
  Overlay addresses alias, so `x86_pg_exhausted` climbing to 527 was read as a
  thrash when the core had never started. Check `core_ready` FIRST.
- **`MPU_TEX_LEVEL1` on the framebuffer measured no gain** and was not the
  corruption it was tried for. Patch kept at
  `build/artifacts/mpu_level1.patch`.

> **An inherited number from another project is not a property of this
> hardware.** It produced a wrong "impossible" and a wrong "38% idle" in one
> day. Re-measure before concluding.

## Where the design lives

| | |
|---|---|
| [`docs/01`](docs/01_memory_architecture.md) | fold table, address translation |
| [`docs/02`](docs/02_cpu_pipeline_optimization.md) | Thumb-2 interpreter, dual-issue |
| [`docs/03`](docs/03_devices_and_hle.md) | device model, DOS HLE |
| [`docs/04`](docs/04_retro_go_and_gwemu.md) | firmware ABI, gwemu's limits |
| [`docs/06`](docs/06_gwemu_determinism.md) | determinism |
| [`docs/07`](docs/07_opcode_coverage.md) | opcode coverage |
| [`docs/08`](docs/08_vga_mode13h.md) | mode 13h, Mode Y, EGA 0Dh, presentation |
| [`docs/09`](docs/09_frame_pacing_and_clocks.md) | **frame pacing and the three clocks** |
| [`docs/10`](docs/10_granule_seam.md) | **the granule seam** |
| [`docs/11`](docs/11_pit_and_interrupt_controller.md) | **PIT, 8259, and what reads them** |
| [`docs/12`](docs/12_memory_tiers.md) | **pager tiers, arena, frame donation** |
| [`docs/13`](docs/13_title_status.md) | **per-title state and settled verdicts** |
