# Frame pacing, and the three clocks

How fast the guest runs is not a property of the interpreter. It is a property
of this file's subject. Read it before optimising anything, because the first
question — "are we CPU-bound at all?" — is answered here and the answer has
been "no" more often than "yes".

Code: `src/main.c`, the slice controller and the frame loop.

## 1. Throughput is `max_slice x fps`

One loop iteration is **one frame**. The loop calls `GW_LCD_SWAP`
unconditionally every iteration and the swap paces to the panel, so the
iteration period *is* the display period whenever we are keeping up.

Therefore guest throughput is `max_slice` instructions x ~60 iterations/s, and
anything that caps `max_slice` caps the machine regardless of how fast the core
executes.

| knob | role |
|---|---|
| `GUEST_EXEC_PCT` (85) | **the binding constraint.** Share of a display period the guest may execute in; the rest is blit + audio refill + input poll |
| `MAX_SLICE_CEIL` (200,000) | a backstop for a pathological guest, ~36 ms at 280 MHz. **Not** the throughput limit — the regulator settles far below it (measured 29-78 k) |
| `MAX_SLICE_FLOOR` (1,000) | |
| `panel_ms_min` | the display period the budget is a percentage OF |

The regulator works on **measured time**, not instruction count: it grows
`max_slice` by 1/8 when `guest_exec_ms` is under target and shrinks it when
over. That is what makes a large ceiling safe — a guest whose instructions turn
expensive shrinks its own slice.

`last_slice_halted` suppresses growth after a `HLT`, because idle is not
headroom.

## 2. Two measured traps in the reference period

**`panel_ms_min` is a MINIMUM WITH AN UPWARD LEAK, and must be floored.** It
latches the shortest iteration ever completed — which during boot, in menus, and
on any frame the guest spent halted is genuinely 6-9 ms — then leaks back up
`panel_ms_min++` every 512 frames (~8.5 s).

Measured consequences before it was floored at `PANEL_MS_MIN_SANE` (14 ms):

- The budget was 90% of **11 ms** instead of ~16.7 ms. The controller held
  `guest_exec_ms` exactly on its target and the machine sat **~56% busy with 0
  dropped frames** — working perfectly against a target 40% too small.
- The score crept upward for minutes as the leak raised the budget. 6→11 ms over
  42 s is five increments of 512 frames = 2,560 frames ≈ 43 s. That drift is
  what "it gets faster the longer it runs" was.
- `frame_overruns` counts iterations over **1.5x** this value, so at a latched
  7 ms an ordinary 16.7 ms frame was reported as an overrun: **1,733 "overruns"
  in 20 s while `guest_fps_achieved` sat at a steady 59**. `frame_hist`'s
  buckets are in the same units and were skewed identically.

**Deriving the period from `guest_fps_achieved` instead is a RUNAWAY.** Tried
and measured: budget too big → frames dropped → fps falls → the measured
"period" reads longer → budget grows again. fps went 59 → 43 with the derived
period pinned at its 20 ms clamp and `guest_exec_ms` at 20 ms against a 16.9 ms
frame. The clamp bounded it; it did not stop it. **The reference must not be
something the budget can influence.**

## 3. Overrunning the deadline costs a WHOLE frame

The swap paces to the panel, so a slice that finishes in under ~16.7 ms catches
the next vsync and a slice that takes 20 ms misses it and waits for the one
after — losing the frame *and* idling for the remainder. Work either fits in one
panel period or costs two.

This is why the optimum is just **inside** the deadline and why overshooting by
3 ms costs 16.7. It is also why `GUEST_EXEC_PCT` is a cliff-edge tuning knob
rather than a smooth one: 90% produced 4.2 dropped frames, 85% did not.

## 4. The three clocks, none of them a hardcoded 60

1. **Display** — the panel's own swap signal; the period is *learned*, so an
   overclock needs no constant changed.
2. **Guest wall clock** — the firmware's ms counter. 18.2065 Hz exactly, carry
   in fixed point, catch-up capped at `TICK_CATCHUP_MAX` (4) with the excess
   counted in `guest_ticks_dropped`. **This is DOS's time of day and must not
   accelerate because a game retimed the chip** — BDA `0x46C` keeps its own
   accumulator (`bda_acc`) even when the guest programs PIT channel 0 to 140 Hz.
3. **Guest CPU rate** — `guest_kips`, instructions per millisecond, paced
   against wall time. `0` is unthrottled (`KIPS_MAX`, the default); anything
   else runs at that speed *and no faster*, with the shortfall counted in
   `guest_insn_deficit`. A deficit climbing at ~37.5 M/s is a 40k rung asking
   for 40 M ips against ~2.8 M delivered — loud, not silent.

`guest_kips` is DOSBox's `cycles` unit exactly, which is why the CPU-profile
labels carry the number.

## 5. CPU profiles

A Retro-Go game-options entry (`cpu_profile_*` in `src/main.c`), persisted under
the key **`"Palette"`** — the firmware's `odroid_settings_app_int32_get/set`
match against a fixed whitelist and ignore anything else *silently*, and the
slot is per-app so the name is ours alone.

| rung | cycles | rung | cycles |
|---|---:|---|---:|
| Max | unthrottled | 386SX 16 | 2,500 |
| 8088 4.77 | 330 | 386DX 33 | 6,000 |
| 286 8 MHz | 800 | 486SX 25 | 9,000 |
| 286 12 MHz | 1,200 | 486DX2 66 | 24,000 |
| | | 486DX4 100 | 40,000 |

The 486 rungs are **stress targets, not period-correct emulation** — they are
above what the silicon delivers, so they never throttle and are
indistinguishable from Max in score. They earn their place by changing how the
frame is *shaped*, so a workload that keeps up at one rung and stutters at the
next names the subsystem that could not follow.

**A rung below the achievable rate is the only one that throttles.** Before the
pacing fixes the effective rate was ~3,000 cycles, which sits between the 2,500
and 6,000 rungs — that is why 386SX 16, 386DX 33 and Max all scored alike.

## 6. Reading it on a target

`scripts/probe.py --watch` these, and they answer different questions:

| counter | says |
|---|---|
| `guest_ips_achieved` | what we actually got. **The number to read in Max** |
| `guest_max_slice` | where the regulator settled |
| `guest_panel_ms_used` | the period the budget is 90%/85% of |
| `guest_exec_ms` | host ms spent RUNNING the guest (1 ms granularity — coarse) |
| `guest_slice_halted` / `guest_grow_blocked` | the guest is idling, and growth is being suppressed for it |
| `guest_insn_deficit` | a paced rung we cannot satisfy. 0 in Max by definition |
| `frame_overruns` / `frame_hist` / `frame_ms_worst` | the TAIL. A mean cannot show a stutter |
| `x86_hlt_log` | WHERE the guest halted — `(CS<<16)|IP`, 16 deep |

**`guest_slice_halted` alone cannot tell a benchmark's timing loop from a DOS
keyboard wait.** Reading it as the former when it was the latter produced a
"38% of frames idle on the tick" conclusion that does not hold for a running
benchmark, where it is 0. That is what `x86_hlt_log` exists for.

## 7. Measured state

Hardware, TOPBENCH (text mode, so it measures the interpreter and not the blit):

| | |
|---|---|
| stock 280 MHz | ~2.8 M insn/s |
| overclocked | **~3.8 M insn/s**, 59 fps, ~1.3 frame overruns/s |
| interpreter gain | **+12.9%** work across the decode round and opcode threading |

Wolf3D in Mode Y manages **~1.4 M ips** against the same core's ~3.0 M in text,
with PC samples landing in `x86_vga_planar_store`. The planar store path, not
the interpreter, is the limit for graphics titles.
