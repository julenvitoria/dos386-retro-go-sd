# Tooling & Testing Hierarchy

Three tiers, each isolating a different class of failure. **Never skip a tier** —
new tooling pointed straight at silicon turns one unknown into two.

| Tier | What it proves | Boots |
|---|---|---|
| 1. gwemu, bare-metal | CPU core in isolation | `test_core.elf` |
| 2. gwemu + retro-go | Core inside the real firmware | retro-go + `dos.bin` |
| 3. Real hardware | Timing, pipeline, SD behaviour | same image as tier 2 |

gwemu is the basis throughout; plain QEMU adds nothing it does not already do.

Guest titles are **`.dsk` floppy images** (FAT12), not bare executables.

## Scripts (`scripts/`)

Run these directly. All take `--help`.

### `build_retrogo.py` — build the retro-go firmware

```bash
python3 scripts/build_retrogo.py                      # defaults
python3 scripts/build_retrogo.py --dry-run            # print the make command
python3 scripts/build_retrogo.py --clean              # required if flags change
python3 scripts/build_retrogo.py --extflash-offset-mb 4
python3 scripts/build_retrogo.py --bank 2             # productive dual-boot
```

Defaults to retro-go's release parameters with `INTFLASH_BANK=1` (boots
directly) and a 64 MB part. Give `--extflash-part-mb` and `--extflash-offset-mb`;
the size is derived. Builds `gwemu_release`, so emulator and device media match.

### `asm_metrics.py` — per-handler assembly metrics

```bash
python3 scripts/asm_metrics.py                        # compare to baseline
python3 scripts/asm_metrics.py --update               # re-baseline (commit it)
python3 scripts/asm_metrics.py --top 20
python3 scripts/asm_metrics.py --cycles --target hardware
```

### `probe.py` — see what a running target is doing

`--watch` reads globals. `--deref SYM+OFF` follows a pointer into a struct.
`--dump SYM:N[xG]` reads a symbol's OWN storage as N words grouped G per line,
which is what an array needs — `--deref` would treat the first word as a
pointer. A literal `0xADDR` works in place of a symbol, for memory with no name
(guest RAM, a stack frame, a peripheral register). Dumped words that land
inside a known function are annotated with it, so a saved LR reads as
`HANDLE_INTERRUPT_16+0xd9` rather than a bare address. Dumps happen AFTER
sampling: read at t=0 and you see the state before the target has done
anything, which is how a full log reads as empty.

`--bytes SYM[:N]` is the third shape and the one for BYTE buffers:
`--dump` reads words of a symbol's own storage and `--deref` reads exactly one
uint32, so **neither can hexdump the memory behind a `uint8_t *`** — pointed at
`vga_dac` (a `static uint8_t *`) `--dump` prints the four bytes of the pointer
itself. `--bytes` takes the same three address forms as `--dump` (`name`,
`0xADDR`, `*name+OFF`) and reads N bytes. `--bytes-group G` inserts a gap every
G bytes: at `G=3` the VGA DAC's r,g,b triples line up in columns, so a PHASE
error in a strided stream is visible instead of being counted by hand — which
is exactly how the palette stream was cleared of a channel rotation.
`--bytes-save SYM[:N]:FILE` writes the range out for a byte compare against a
reference. **For GUEST memory use `--guest`, never `--bytes`.**

```bash
# the 768 DAC bytes behind the pointer, as r,g,b triples
python3 scripts/probe.py --retrogo --wait-magic \
    --bytes '*vga_dac:768=DAC' --bytes-group 3
python3 scripts/probe.py --retrogo --wait-magic \
    --bytes-save '*vga_dac:768:build/artifacts/ours_dac.bin'
python3 scripts/probe.py --retrogo --wait-magic \
    --watch core_ready,core_dtcm_ok,benchmark_insns --deref cpu_state_ptr+8=EAX
python3 scripts/probe.py --dump x86_cs_log:16x4=CSLOG  # needs X86_CS_LOG=1
python3 scripts/probe.py --dump 0x2408bf48:16x4=STACK  # or a raw address
python3 scripts/probe.py --target hardware --watch run_flag,benchmark_cycles
python3 scripts/probe.py --retrogo --set run_benchmark_flag=1 --watch benchmark_insns
```

Samples PC (labelled ITCM / firmware symbol / region), watched globals, and
pointer dereferences. Reach for this when a harness reports a value that makes
no sense — it is faster than lengthening a timeline, which is capped at 60s.

#### Reading GUEST memory — `--guest` and `--guest-save`

```bash
python3 scripts/probe.py --retrogo --wait-magic --guest 0x10e80:96=CSIP
python3 scripts/probe.py --retrogo --wait-magic --guest 0x400:64=BDA
python3 scripts/probe.py --retrogo --wait-magic \
    --guest-save 0x0:0xA0000:build/artifacts/ours.bin
```

`--guest LIN[:BYTES][=LABEL]` hexdumps guest LINEAR memory; `--guest-save
LIN:BYTES:FILE` writes it to a file so it can be byte-compared against a
reference emulator's image.

**This is the only correct way to read guest RAM, and `*guest_ram+OFF` is not.**
Since demand paging landed, `guest_ram` is a frame POOL, not a flat image of the
low megabyte: `*guest_ram+0x400` reads the pool's first page, so the BDA comes
back as zeros and a code dump comes back as an unrelated page — a wrong answer
that looks exactly like a correct one. Both options walk `guest_page_table_ptr`
(host = lin + k, one entry per 4 KB page) and render a page whose entry is
`FOLD_INVALID` as `0xFF`, which is what the guest would fetch there, so a dump
never silently invents zeros for memory that is not mapped.

#### Driving the reference PC (`scripts/ref_dos.py`)

```bash
python3 scripts/ref_dos.py --dsk .../WOLF3D.dsk --no-hooks --launch WOLF3D.EXE \
    --settle 30 --samples 5 --interval 8 --text \
    --save build/artifacts/ref.bin --screen build/artifacts/ref.ppm
```

Four traps are handled in the script because each one produced a WRONG ANSWER
rather than an error:

1. **CHS geometry** comes from the volume's own BPB (1 head / 63 sectors here).
   QEMU's default for a 3 MB disk is not that, and SeaBIOS then sits at 0x7C04
   forever showing "Booting from Hard Disk...".
2. **`--no-hooks`** rewrites CONFIG.SYS in a COPY: `XMSHOOK.SYS`/`EMSHOOK.SYS`
   are written for our emulator and hang a reference machine before
   AUTOEXEC.BAT runs, which reads exactly like the title crashing.
3. **The monitor is read to its PROMPT, never slept on.** It echoes every
   character, so a fixed sleep leaves part of one command's echo in the buffer
   and the next command arrives with that garbage prepended.
4. **Paths passed to `pmemsave`/`screendump` are QUOTED.** The monitor parses
   the size argument as an arithmetic EXPRESSION and `/` is DIVISION, so
   `pmemsave 0 0x100000 /home/...` becomes a division by `home` and dies with
   `invalid char 'h' in expression` — having written nothing. Relative paths
   happen to work, which is exactly what hid it.

`--display` defaults to VNC and **must not be `none`**: QEMU derives the VGA
status bits at port 0x3DA from its display refresh, so under `-display none` a
title polling for vertical retrace spins forever. Measured — WOLF3D sat in
`IN AL,0x3DA / TEST AL,1 / JNZ` indefinitely.

**A reference that is RUNNING is not a reference that has reached the same
place.** Check `--samples`, the mode byte and the screendump before trusting any
comparison, or the two images are snapshots of different moments and every
difference between them is noise.

##### `--engine dosbox` — because QEMU **cannot run Wolfenstein 3D at all**

`--cmd 'CMD'` runs any extra monitor command at every sample and `--qemu-arg=X`
passes X straight to QEMU, so a hypothesis about the machine model can be tested
without editing the script. Both were added to find the following, and the
finding is the reason the second engine exists.

**QEMU's port 0x3DA flips display-enable and vertical-retrace TOGETHER**, so the
state Wolf3D's `VL_SetScreen` waits for never occurs:

```
   mov cx,[TimeCount] / add cx,2         ; the only escape from the loop
L1: in al,3DA / test al,1 / jnz L1       ; wait for DISPLAY ENABLE
L2: cmp [TimeCount],cx / jae done
    in al,3DA / test al,8 / jnz L1       ; a VERTICAL RETRACE restarts at L1
               test al,1 / jz  L2
```

`vga_dumb_retrace()` returns `st01 ^ (ST01_V_RETRACE|ST01_DISP_ENABLE)`, so
0x3DA alternates **0x00 ↔ 0x09** and is never 0x01 or 0x08. Measured by
single-stepping the guest over QEMU's gdbstub (`--qemu-arg=-s`, then
`gdb -ex 'set architecture i8086' -ex 'target remote :1234'` and `stepi`):
`eip=56 al=0 … eip=65 al=9 → back to 56`, forever. The TimeCount escape cannot
fire either — TimeCount is advanced only by the game's own INT 08h handler,
which `SD_Startup()` installs *later*. `vga_precise_retrace` is dead code in
QEMU 10.1: the `retrace` property is exposed on **no** VGA model (`VGA`,
`isa-vga`, `cirrus-vga`, …), so `-global VGA.retrace=on` does nothing. **This
stall is QEMU's. It is not the guest's and it is not ours** — and every reading
of "the reference is stuck in the same place we are" that was taken from it is
a coincidence of two different causes.

DOSBox-X models the two bits independently and runs the title:

```bash
python3 scripts/ref_dos.py --engine dosbox --no-hooks --launch WOLF3D.EXE \
    --dsk references/.../WOLF3D.dsk --settle 38 --samples 10 --interval 3 \
    --keys space,space,space,space --key-pace 5 \
    --screen build/artifacts/w.png --save build/artifacts/w.bin
```

That exact line reaches the **episode select** in every slot; drop one press
(`--keys space,space,space`) and every slot is the **main menu** instead. Which
screen a given slot holds is a race between `--key-pace` and the title's own
timing, so read the PNGs — do not cite a slot number. It boots the image's OWN MS-DOS
(`imgmount 2 … -fs none` + `boot -l c`), not DOSBox-X's built-in DOS, so the
software stack matches the QEMU engine and matches ours.

State comes out of DOSBox-X's `autosave=` **save states**, which are ZIPs
holding the whole `Memory` and `Vga` components: `--save` writes the low
megabyte byte-comparably against `probe.py --guest-save`, `--screen` writes one
PNG per sample of the mode 13h / **Mode Y** framebuffer, and each line prints
the BIOS video mode, the INT 08h/09h vectors and the BIOS tick. **INT 08h ≠ the
DOS default is the single number that says `SD_Startup()` completed** — on
DOSBox-X it reads `2aee:0095` and INT 09h `26b7:0009`, i.e. `SDL_SetTimer0()`
and `IN_Startup()` both ran. Under QEMU both stay at DOS's `0de7:…` forever.

Four DOSBox-X traps, each of which produced silence rather than an error:

1. **`[sdl] priority`'s second field is the INACTIVE priority and can be
   `pause`.** Under `SDL_VIDEODRIVER=dummy` the window is never focused, so the
   emulator boots, prints its banner and then STOPS — an `[autoexec]` that
   simply never runs. `priority=normal,normal`.
2. **DOSBox-X's working directory is not yours.** Under the flatpak it is
   `~/.var/app/com.dosbox_x.DOSBox-X/config/dosbox-x`, so every relative path in
   `[autoexec]` resolves somewhere else and `imgmount` fails with nothing
   host-visible. Every path the script writes is ABSOLUTE.
3. **`mount d <hostdir>` is not writable from the guest here**, so the DOS shell
   cannot be used as an output channel. Everything observable comes from the
   save state and the `[log]` file.
4. **The signon screen waits for a key**, and a reference sitting on it looks
   exactly like one that hung. `--keys` is delivered with `autotype -w`, which
   is asynchronous and survives the `boot`.

Three decoding rules in `state_screen()` that a naive reader gets wrong, all
noted in the code. Wolf3D is **unchained**, so a pixel is
`plane = x & 3, offset = y*80 + (x >> 2)` over DOSBox-X's four-byte-interleaved
VRAM — read linearly it comes out quarter-width and repeated, which looks like a
rendering defect rather than a decoding one. The 6-bit DAC is widened to 8 by
**replication**, `(c<<2)|(c>>4)`, as DOSBox-X does — `c*255//63` is off by one
on most values. And **neither the DAC nor the VRAM base is searched for by
shape** (see below).

##### The oracle was miscolouring every frame, and could not tell

`state_screen()` used to take the first 768-byte window that was all-6-bit and
varied as the palette, and take the last 2 MB of the component as VRAM. Both
were plausible and both were wrong:

* The scan matched `vga.dac + 6` — the true table shifted by ten bytes of
  `combine[]`, i.e. three entries and one channel — so every index rendered as
  its neighbour's colour with the channels rotated and Wolf3D's menus came out
  **saturated blue** instead of dark red. Rotation alone leaves greys grey; it
  was the index shift that made it visible, and nothing in the scan could tell
  "found the palette" from "found something palette-shaped".
* VRAM is *not* the tail: `vgapages`, `memsize`, `vga.lfb` and the
  Paradise/Tseng/XGA blocks follow it, so every frame was read 1296 bytes late
  — rolled up by four scanlines.

Both are now read where DOSBox-X actually writes them, and **proven** there.
`SerializeVga::getBytes()` (`references/dosbox-x/src/hardware/vga.cpp:2050`)
writes one POD of `VGA_Dac` (`include/vga.h:573`), which carries the host-pixel
cache `xlat32[256]` right after `rgb[256]` and rebuilds it from `rgb[]` on load
(`vga_dac.cpp:501`) — so `xlat32[i] == pack(rgb[i] widened)` holds for all 256
entries of the live DAC and for no shift of it, no fade buffer and no second
palette the game keeps around. VRAM is located by arithmetic from the
`vgapages` + `vga.mem.memsize` pair written after it (`vga_memory.cpp:3018`).
If either check fails the script **raises**; there is no greyscale fallback and
no "closest match", because a reference that renders the wrong colours without
complaining silently invalidates every colour judgement made against it.

**The image itself is healthy and that was checked host-side, not assumed.** All
eight `.WL1` files plus `WOLF3D.EXE` are present on the MBR-partitioned FAT16
volume with an allocated cluster chain at least as long as the directory size —
nothing truncated, `VSWAP.WL1` exactly 742,912 B.

`scripts/watch_memory.py --addr guest:0xLIN` resolves the same way, for catching
the write that corrupted a guest address rather than reading it after the fact.

#### Catching a derailment: the invariant traps in `x86_trace.inc`

`X86_TRACE_EIP` SAMPLES. These CATCH: they arm a predicate, fire on the first
instruction that violates it, record the state, and **freeze a 64-deep ring of
the linear EIPs that led there**. Read the whole block with
`probe.py --dump x86_oddsp_log:73x1`; the ring is `[8..71]`, oldest at `[7]&63`.

| define | fires on |
|---|---|
| `X86_TRACE_ODDSP` | turns the trap on at all (required by the others) |
| `X86_TRACE_ODDREG R` | first boundary where guest register `R` is ODD; default `REG_ESP`. A 16-bit DOS guest moves SP by two for its whole life, so an odd SP is a defect at the instant it happens |
| `X86_TRACE_LOWEIP N` | first instruction with linear EIP below `N`. "The guest is running the IVT as code" — fires EARLIER in a derailment than any register predicate, so reach for it first |
| `X86_TRACE_LOWSP N` | first boundary with SP below `N`, for a push/pop imbalance whose single leak is invisible and whose drift is not |
| `X86_TRACE_MEMEQ A` + `X86_TRACE_MEMVAL V` | first boundary at which the guest 16-bit word at linear `A` equals `V` — a store watch. A hardware watchpoint cannot do this job here: the host address of a guest page is only known once its fold entry is live, and a guest that halts a second after it starts leaves no window in which a host-side tool can resolve and arm one (`watch_memory.py` armed on a stale entry and pointed at 0x02255706, which is not a host address at all). This fires on the instruction AFTER the store, and the ring names the writer |
| `X86_TRACE_ATEIP A` (+ `X86_TRACE_ATREG R`) | changes what the RING holds: register `R` (default `REG_ESP`) sampled at exactly one guest EIP, one entry per pass. The ring INDEX at `[7]` is then a hit count, which answers "was this instruction ever executed at all" — a 0 there is how the only writer of a variable was shown never to run. A slow stack leak cannot be found with a threshold — the drift is smaller than normal frame churn, so any bound low enough to miss a legitimate `sub sp,N` prologue is already past the start. Sampling the same instruction every iteration makes the per-iteration delta the reading |
| `X86_TRACE_EIPREG A` + `X86_TRACE_REGMASK 0` + `X86_TRACE_MEMVAL 0` | **"who called this function?"** — with a zero mask the register test is a tautology, so the predicate collapses to "the guest reached EIP `A`", and because `ATEIP` is NOT defined the ring keeps plain EIPs. It therefore freezes the last 64 instructions *before the first entry*, which is the CALLER and its whole lead-in. `ATEIP` cannot do this (it records one register per pass at that EIP and nothing else), and a hit COUNT cannot either. This is how Wolf3D's `CA_CacheGrChunk` was shown to be entered from `Quit()` rather than from `InitGame`. `[1]` is the instruction number, `[73]` the CS base, `[74..81]` eight stack words — enough to read a `char *` argument off the frame and name the error string |
| `X86_TRACE_SKIP N` | arms the trap only after `N` instructions. Not optional for a register predicate: BP is a general register during startup (the unpacker loads it with a segment value at instruction 28), so an ungated "first odd BP" fires on a non-defect and hides the real one. The ring keeps filling while disarmed |

**Catch, do not bisect, when the event is rare.** A bisection needs the run to be
reproducible at the granularity you are bisecting; a trap needs nothing. That
mattered here: `X86_TRACE_EIP`'s index used to count DISPATCHES, and a batched
`REP` element re-enters the string handler without passing the sampler, so where
a batch got cut moved with the wall-clock-driven slice and the index differed by
~1,500 between identical builds. `.str_exit` bumps the counter now, and the index
is bit-reproducible (measured: 2,647,648 on three consecutive WOLF3D runs, from
2,453,372/2,454,873 before). **The guest itself was never the problem** —
`guest_insns_total` was 2,723,483 on every run throughout.

**A zero in `x86_eip_log` means NO SAMPLE, not a value of zero.** The index is
bumped for a batched `REP` element (that is what makes it deterministic) but the
sampler only stores from `.resume_fetch`, so a sample whose index falls inside a
batch leaves its slot at 0. Reading those zeros as data is how a healthy stack
sitting at 0xFFB8 was misread as collapsing to 0, and it cost a bisection step.
Filter empty slots out before interpreting a sweep.

**The old `*guest_ram+OFF` spelling now FAILS with a message** in `--dump`,
`--deref` and `watch_memory.py --addr`, rather than serving a wrong page. It is
still in older harnesses and in this file's history, and a reader that returns
plausible bytes for the wrong address is the exact failure class this project
keeps losing hours to.

#### The three clocks, and how to watch them

The emulator loop has three independent clocks (see the block comment at the
top of `src/main.c`). All of them are host-readable, and all of them are
host-**writable** where that is a knob, so the trade is measured on a running
target instead of rebuilt for:

```bash
# what the guest is actually getting
python3 scripts/probe.py --retrogo --wait-magic \
    --watch guest_kips,guest_slice_now,guest_ips_achieved,guest_fps_achieved,\
guest_insn_deficit,guest_ticks,guest_uptime_ms,guest_ticks_dropped,\
frame_ms_last,guest_exec_ms,panel_ms_min

python3 scripts/probe.py --retrogo --wait-magic --set guest_kips=0       # max
python3 scripts/probe.py --retrogo --wait-magic --set guest_kips=330     # 8088
```

| Symbol | Meaning |
|---|---|
| `guest_kips` | emulated CPU speed in thousands of insns/s. **0 = unthrottled, and the default.** `KIPS_*` in `src/main.c` has the profile rungs; the PAUSE/SET menu's "CPU Speed" entry sets it too |
| `cpu_profile_persisted` / `cpu_profile_events` | what the settings store kept when the menu entry was changed, and how many dialog callbacks reached us. The menu path has no other witness — a malformed options array never calls us at all |
| `frame_10us_avg` | mean iteration period, 8x oversampled, in 10 us units. What the firmware's `common_emu_frame_loop()` observer is fed |
| `upper_filler_ok` | is 0xA0000-0xFFFFF backed? It comes from the bonus pool now, but a 0 here explains an option-ROM scan faulting |
| `guest_insn_deficit` | instructions the emulated CPU owed and never ran. The answer to "I asked for a 386 — am I getting one?". Meaningful only in paced mode |
| `guest_ips_achieved` / `guest_fps_achieved` | measured over a 1 s window. The number that matters in unthrottled mode |
| `guest_ticks` / `guest_ticks_dropped` | IRQ 0s handed to the guest, and ones refused by the catch-up cap or by the one-deep pending slot. **The rate is the GUEST's, not 18.2065 Hz**: it is the PIT input divided by whatever channel 0 holds |
| `guest_tick_divisor` / `x86_pit0_writes` | the PIT channel 0 divisor in force, and how many times a guest has programmed port 0x40. 65536 is the boot value (18.2065 Hz); Wolfenstein 3D writes 0x214B (8523) for 140 Hz and paces its turning, animation and menu key-repeat off the resulting ISR. **A `guest_tick_divisor` of 65536 on a title whose `x86_pit0_writes` is non-zero means the program was dropped** -- which is what made Wolf3D's whole sense of time run at 13% of real. `guest_ticks / guest_uptime_ms` should read back as `1193182 / guest_tick_divisor` |
| `panel_ms_min` | the display period, **learned** from the shortest iteration ever completed. Nothing assumes 60 fps |
| `x86_kbd_read_log` / `x86_spk_log` | the last 32 bytes off port 0x60, and the last 32 writes that can move the speaker tone. `--dump x86_kbd_read_log:32x8` |
| `x86_kb_pushes` / `x86_kb_pops` / `x86_kb_peek_hits` | the BDA ring's producer and its two consumers. "The guest polls INT 16h forever" has two opposite causes -- nothing was queued, or something dequeued it first -- and only these three separate them. `pushes == pops == peek_hits` means every key was queued, seen and taken; `pushes > 0` with `pops == 0` is the reported failure |
| `x86_dos_file_log` / `x86_dos_path_log` | The **first 96** file-family INT 21h calls, four words each -- `(func << 8) \| AL`, `(CF << 16) \| AX`, `BX`, `CX:DX` -- plus the last sixteen paths. Read it with **`probe.py --dos-log`**, which decodes it to one line per call. It **SATURATES rather than wrapping**, and that is the point: a guest stuck in a loop overwrote a 32-slot ring with the loop body in milliseconds, so the call that ISSUED the handle the loop was failing on was always gone -- WOLF3D's 32 identical AH=42h slots said nothing, and with 96 saturating slots the answer was in entry 2. The old two-word form also could not say WHICH handle, which is the only question a failing seek raises. `x86_dos_file_calls` counts every call whether recorded or not; write 0 to `x86_dos_file_log_idx` to re-arm for a later phase. `x86_hle_log` cannot answer a file question at all: a guest polling the keyboard fills all 32 of its slots between one open and the next |
| `x86_io_log` | Every port access, **coalesced by (direction, port)** into 32 runs, decoded by **`probe.py --io-log`**. Coalescing is what makes it useful: a mode-13h palette upload is 768 consecutive writes to 0x3C9 and would flush every other port out of a raw ring. What it shows is the SHAPE of a guest's hardware conversation -- which register files it programs, in what order -- and that named WOLF3D's wall in one run: `OUT 3D4,0F / OUT 3D5,66 / IN 3D5` reading back 0xFF, i.e. a CRTC write-then-read-back probe against a register file that did not exist. It also caught the FIX overshooting -- answering the 0x3B4/0x3B5 mono alias too made the guest conclude MONO and poll 0x3BA 32,000 times |
| `x86_disk_reads` / `x86_disk_backend_reads` | guest-side `disk_read()` calls and how many of them reached the SD card. The RATIO is the sector cache's whole point, it is exact under gwemu, and it is the number to quote — TOPBENCH boots on 115 / 7 |
| `vga_aperture_ok` / `x86_vga13_ok` | is the mode 13h aperture (64 KB, bonus pool) and its palette block backed? A 0 in `x86_vga13_ok` means a guest asking for 13h is REFUSED and logged (event tag 5) rather than drawing into the open-bus filler -- which is the right behaviour, but it is a whole mode silently missing |
| `x86_vga_dac_writes` | DAC bytes the guest has streamed through 0x3C9. **0 while a screen is black says the guest never got as far as the palette**, which separates "13h is broken" from "the guest never reached video" -- the distinction WOLF3D turned on |
| `disk_cache_mem` | the AHB pointer the sector cache got. The AHB heap is shared with the firmware's own newlib allocations, so unlike a bump pool its free space is NOT knowable from the link map; this is the only witness |
| `x86_disk_cache_ok` / `x86_disk_cache_bytes` | is the cache BACKED? A 0 here is a silent-allocation failure and the only symptom is the stutter the cache exists to remove |
| `x86_disk_cache_hits` / `_misses` / `_fills` / `_evictions` / `_bypass` | sectors served from a resident window, sectors that forced a fetch, window fetches, and requests that went straight through. Rising `_evictions` with a poor hit rate means the working set no longer fits four windows |
| `x86_disk_us_total` / `x86_disk_us_worst` / `x86_disk_errors` | time in the backend, and the worst single call. **Meaningless under gwemu** (`docs/04`: SD busy state is not modelled) and the reason the timing claim needs silicon |
| `frame_hist` / `frame_ms_worst` / `frame_overruns` | the frame-time TAIL, which is what "stutter" means. Buckets are multiples of the learned panel period, so `frame_hist[0]` is on-time and anything else is a missed deadline. A mean or a minimum cannot show a hitch; only this can. Write `frame_stats_reset=1` to open a window around a specific event. `--dump frame_hist:8x4` |
| `x86_video_mode_log` / `_idx` | the last 16 events that can change what the renderer draws, as `(tag << 24) \| (aux << 8) \| value`: **1** mode adopted (value = mode, aux = columns), **2** a port 0x3D8 write, **3** the renderer following a change (aux = is_text), **5** a mode REFUSED because nothing renders it, **6** an INT 10h AH=00 set-mode REQUEST, logged before the decision (a set to the mode already up adopts nothing and so has no tag 1, yet it still CLEARS the display -- which is how Commander Keen 4's banner disappears, and without tag 6 that wipe has no cause in the ring at all). A sampled mode byte cannot tell "never switched" from "switched and the renderer did not follow" — both read as a constant — and this can. `--dump x86_video_mode_log:16x4` |
| `x86_int_vec_log` | **needs `X86_CS_LOG=1`** (see below). The last 16 interrupts dispatched through the real-mode IVT, as `(vector << 16) \| the CS installed`. `x86_cs_log` says only "LR was `HANDLE_INTERRUPT_16`" -- it cannot name the vector, and a guest that derails after its own code has touched low memory is exactly the case where that is the whole question. `0x00080000` (INT 08h into CS=0x0000) is what named Commander Keen 4's failure in one dump. `--dump x86_int_vec_log:16x4` |
| `x86_hle_log` | the last 32 services as `(vector << 8) \| AH`, bit 16 set if nobody answered. This is what says whether a guest is polling INT 16h AH=00 or AH=01 -- a distinction the two paths do not share a symptom for. `--dump x86_hle_log:32x8` |

Guest time must not move with render rate. The measurement that proves it is
`guest_ticks / guest_uptime_ms` at two different frame rates — force a low one
with a large `--set guest_kips=…` or by loading a heavy guest.

#### Cold guest RAM in NOR, and what to read off it

```bash
# the whole story in one word; the timeline opens the menu and presses A on
# "Cold RAM->NOR" after letting the guest settle for 20 s
python3 scripts/probe.py --retrogo --wait-magic \
    --timeline timelines/nor-snapshot.tl --samples 34 --interval 1.5 \
    --watch nor,guest_ips_achieved,frames_rendered
```

`--watch` takes GROUP names as well as symbols; `nor` is the set below. Groups
live in `WATCH_GROUPS` at the top of `scripts/probe.py` — add to that rather
than pasting seventeen symbols that go stale the moment a counter is added.

| Symbol | Meaning |
|---|---|
| `nor_snap_result` | 0 = never run, 1 = ok, else the reason. **8 is the one to fear**: the firmware's ROM cache served a HIT, programmed nothing and never called our relocate callback, so the address it returned points at an OLD snapshot. The core refuses to remap on it. See `NOR_SNAP_*` in `src/main.c` |
| `nor_snap_cb_bytes` | bytes the relocate callback actually filled. Anything other than 655,360 means the snapshot in flash is not the guest's memory, whatever the returned address looks like |
| `nor_snap_addr` / `nor_snap_ms` | where the firmware placed it, and what the whole operation cost (placeholder file to SD, erase, program, readback verify). **1,595–1,614 ms under gwemu, which is NOT a timing claim** — `docs/04`, and OCTOSPI erase/program latency is not modelled |
| `x86_nor_attached` / `x86_nor_attach_err` | is the guest running out of NOR at all, and if not, which check refused it. `7` means the flash readback did not match guest RAM — the fold table was never touched and the guest is still on AXI, which is the invariant the design turns on |
| `x86_nor_pages_nor` / `_axi` / `_total` | **the measurement.** Pages still served from NOR versus pulled back by copy-on-write. `pages_nor / pages_total` is the answer to "what fraction of this guest's RAM is genuinely read-only once it has settled", and it is measured rather than guessed because the policy is demand-driven with no heuristic in it |
| `x86_nor_axi_free` | AXI bytes reclaimed — what is left unclaimed in the COW pool. The pool IS the old guest RAM block; `ram_malloc()` has no `free()`, so this is the honest number rather than a claim about the allocator |
| `x86_nor_reads` / `x86_nor_writes` | slow-path reads and writes. Reads dominate by orders of magnitude and include every INSTRUCTION FETCH from a NOR-backed code page — that is the cost of the trade, and it is why `guest_ips_achieved` is the number to read next to these |
| `x86_nor_cow_pulls` / `x86_nor_host_cow` | pages copied NOR → AXI, and how many of those were forced by a HOST write rather than a guest one. The host side (BDA tick, INT 21h filling a DTA, a disk read landing in a guest buffer) goes through `x86_guest_ptr()` and never touches the trap stubs; a `pages_axi` that is implausibly small is what exposes a host path storing straight at the XIP window, which gwemu makes look like it works |
| `x86_nor_cow_failures` / `x86_nor_unmapped` | the loud failures: the COW pool empty (structurally impossible — it is exactly as large as the snapshot) and traps that were a genuine unmapped access. Both should be 0 |
| `x86_nor_slow_cycles` | DWT cycles inside the slow path. Ranking only under gwemu |

#### The VRAM arena, A20, and the pin state

```bash
python3 scripts/probe.py --retrogo --wait-magic \
    --timeline timelines/start-preselected-game.tl --samples 2 \
    --watch x86_arena_bytes,x86_arena_used,x86_arena_free,x86_arena_largest,\
x86_arena_refusals,x86_arena_deficit,x86_pinned_pages,x86_a20_state
```

| Symbol | Meaning |
|---|---|
| `x86_arena_bytes` / `_used` / `_free` / `_largest` | the display's AXI, as one pool instead of three permanent blocks. **`_largest` is the number that decides whether a mode fits**, not `_free`: a claim's block must be contiguous, so a fragmented arena can have room for an EGA plane and no run to put it in |
| `x86_vga_ega_card_offsets` | the EGA 0Dh card actually fitted, in PLANE OFFSETS (one offset is four interleaved bytes, so the card is 4x this). Sized from the arena at the mode set, not fixed: 0Dh lends the CGA claim out and pulls the 13h aperture down to the bottom, which is 19,456 offsets on the shipping layout against the 8,192 that cut Keen 4's intro off at row 128. Read it with `x86_vga_planar_lost` and `x86_vga_latch_miss` -- a small card shows up there first |
| `x86_arena_deficit` | bytes the LAST refused claim could not place. This is what a refusal message quotes -- "EGA needs 118,784 bytes more than exist" is actionable and "EGA failed" is not |
| `x86_arena_refusals` / `_claims_ok` | claims refused outright and claims satisfied. A claim is **all or nothing**: the previous port measured a partial EGA card as producing worse output than no EGA (8 KB/plane truncates cleanly; 16 KB and 32 KB alias the floating pages onto each other and fill the screen with noise that reads as a rendering defect). A rising `_refusals` with a black screen is the system working |
| `x86_pinned_pages` | guest pages the pager may never evict. 24 with CGA VRAM and the 13h aperture mapped (8 + 16); a 0 here while a mode is up means the display's storage is evictable, which is a defect the picture would show a frame later |
| `x86_livemap_hits` | evictions vetoed because a host pointer into the page might still be live. `cow_lost == 0` does NOT cover this -- the previous project measured 5 such evictions producing 26,329 wrong bytes with that invariant green throughout |
| `x86_a20_state` / `x86_a20_changes` | the A20 gate, and how many times a guest has actually MOVED it (a probe that writes an already-open gate is not a transition). While it is masked the HMA aliases the bottom of memory; while it is open there is memory above 1 MB at all |
| `x86_pg_frames_total` / `_frames_free` / `_resident` | the pager's AXI, in 4 KB frames, and how many guest pages are resident. **`_frames_total` is the number to check first after any change to the core**: it read 159 instead of 160 purely because `ram_malloc()` returned a 4-byte-aligned block, losing a frame to a partial page at each end |
| `x86_pg_faults_r` / `_faults_w` / `_faults_host` | fold misses serviced, split by cause. `_faults_host` are from `x86_guest_ptr()` and carry **every DOS program load** -- loading is a copy into memory the guest has never touched, so under demand paging all of it faults from the host side. A title whose `_faults_r` is 0 is not broken: any page the host wrote is already resident |
| `x86_pg_fill_zero` / `_fill_nor` / `_fill_file` | where each faulted page's contents came from. The ratio is the answer to "how much of this guest's memory has ever been anywhere but ZERO" |
| `x86_pg_exhausted` | **the loud one.** No frame was free and no victim could be taken, so the guest got a clean `X86_EXIT_MEM_FAULT`. A pool that silently ate the store would be indistinguishable from one that works |
| `x86_seam_syncs` / `_writes` / `_overflow` / `_flushes` | THE GRANULE SEAM (`x86_seam.c`). `_syncs` is how many straddling accesses were SERVED -- the neighbour's leading bytes pulled into the low frame's eight slack bytes; `_writes` is how many bytes a straddling STORE pushed back at the instruction boundary; `_flushes` how many instruction boundaries actually ran a push. **`_overflow` is the loud one**: a straddle that could NOT be served -- a boundary whose low granule has no frame (a fixed mapping, or a NOR-backed page: neither has slack and neither's neighbour is ours to write), or five live boundaries in one instruction against four slots. A non-zero `_overflow` means some access fell back to the old, wrong behaviour, and it is a number to read rather than a silence to infer from. WOLF3D measured 52,216 / 1 / 2 / 52,188 over 25 s: the seam is RARE, and one wrong byte per 52,000 is exactly the shape of a defect that survives for months |
| `x86_pg_unmapped` | faults for addresses OUTSIDE the pager's window. Not a page fault -- a genuinely unmapped address, i.e. the guest's own wild pointer. Kept separate on purpose: conflating the two turns a guest bug into a silent allocation |
| `core_dtcm_next` / `core_dtcm_headroom_max` | where the firmware's DTCM bump pool got to after the core's allocations, and an UPPER BOUND on what is left. `GW_MEM_OP_FREE_SIZE` is "RAM only today", so the ABI cannot be asked; this is the only witness. Measured 96,068 B free above a bump at `0x200088BC` |

### `probe_report.py` — what did this title ask for, and how did we answer

```bash
python3 scripts/probe_report.py --title keen4
python3 scripts/probe_report.py --title wolf3d --settle 30
python3 scripts/probe_report.py --rom .../GAME.dsk --name mygame
python3 scripts/probe_report.py --title keen4 --skip-build --skip-cost   # fast
python3 scripts/probe_report.py --compare before.json after.json
```

Boots a named title, lets it run **from its first instruction** (no settle-then-
reset — the startup probes *are* the subject), and writes one report to
`build/artifacts/probe_<title>_<target>.md` plus the same content as JSON.
Presets: `keen4`, `wolf3d`, `topbench`, `alleycat`.

**The report leads with what we did NOT answer**, then with the hardware we
answered *absent*, and only then lists everything that worked. The column that
matters is `status`:

| status | means |
|---|---|
| `unanswered` | nobody claimed it. The guest kept whatever was in AX |
| `default-arm` | a catch-all produced a shaped, **invented** answer |
| `unmodelled-port` | the access fell off the end of the switch in `x86_io.c` |
| `unclassified` | answered, but nobody has vetted the answer — **ranked high** |
| `stub` | returns success and does nothing |
| `partial` | right for some inputs, falls through for the rest |
| `absent-by-design` | we say "this hardware is not here", and that is TRUE of the machine we claim to be. Not a defect — but it *is* what a title may be dying on, so it gets its own section |
| `implemented` | modelled |

**Runtime evidence outranks the table.** `unanswered` and `unmodelled-port` are
facts the target observed (`rc==0`, `x86_hle_unhandled_*` moving, the switch
fall-through) and cannot drift. The curated table in
`scripts/common/probe_expect.py` only classifies calls the target says *were*
answered — which is exactly the case the target cannot see the difference in.
Anything the table does not cover is `unclassified`, never quietly
`implemented`: a new probe appearing in a title is the thing this tool exists to
surface, and a table that swallowed it would defeat the tool.

That distinction is the whole point. `INT 21h AH=44h` with `AL>=2` returns
`CF=1 AX=0001` from a catch-all; a real handler returning the same thing is
indistinguishable in every other trace in this tree. Here it is a column.

It found, on its first three runs: Keen 4 and Wolf3D both stopping at the
`INT 10h AH=12h BL=10h` EGA gate, and **Alley Cat reading PIT channel 0 (port
`0x40`) 2,013,208 times in 28 s against a port nothing models** — a clock that,
from the guest's side, never moves.

`--compare A.json B.json` diffs two runs and reports answers that were **FIXED**
or **REGRESSED**, probes that appeared or vanished, and counts that moved — so a
fix can be shown to have changed an answer. Poll counts jitter run to run under
a frame-paced emulator; the *classifications* are stable, which is why the
compare leads with those.

#### Cost, and why it is off by default

`X86_PROBE=1`, off by default like every other instrument. **Every report prints
the delta it measured by building the core both ways in that same run** — not an
estimate, because RAM_EMU headroom in this tree has gone negative silently more
than once. On this tree:

| | |
|---|---|
| RAM_EMU | **+3,568 B** (the 2,848 B accumulator plus code) |
| `dos.bin` | **+720 B** — code only. The block is deliberately uninitialised `.bss` so it does not *also* occupy the packed image |
| hot path | **nothing.** One hash probe per INT service and per port access; never per guest instruction |

`--skip-cost` skips the second clean build when the number is not wanted.

#### Why an accumulator and not a tenth ring

There are nine trace rings in this tree and all of them **wrap**, so the probe
issued once at startup is gone by the time anyone looks — that is what cost an
earlier investigation hours when entry 2 was overwritten by 81 identical later
ones. `x86_probe` is keyed by `(kind, id, AH, sub)` and **never evicts**: a
title making 400 calls to twelve services occupies twelve slots, so a longer run
only ever adds. When the 128 entries are all claimed it **saturates and counts
what it refused**, and the report says so rather than presenting a subset as the
whole conversation.

The existing rings are untouched and still the right tool for **order**, which
an accumulator throws away. Reach for `probe.py --dos-log` / `--io-log` when the
question is "in what sequence"; reach for this when it is "what, and did we
answer it".

Two caveats the report states itself: **INT 15h is not dispatched at all**, so a
guest calling it never reaches the accumulator and its absence from a report is
not evidence nothing called it (check `x86_int_vec_log`). And the per-port
"distinct values" figure is a fold of the low 5 bits — enough to separate a
polled status register from a programmed register file, which is the question a
port row raises, and not more than that.

### `watch_memory.py` — catch whatever is writing to an address

```bash
# who is scribbling on the relocated stub at guest linear 0x2E920?
python3 scripts/watch_memory.py --retrogo --wait-magic \
    --addr guest:0x2E920 --length 4
python3 scripts/watch_memory.py --retrogo --wait-magic --addr core_ready --hits 3 \
    --peek 0x24058cd0:224
```

`probe.py` samples; this catches. A corrupting write is one instruction among
millions and polling never sees it, so this sets a real data watchpoint and
reports the ARM PC at the stop resolved against the ELF — inside the
interpreter that PC is the handler, which names the guest instruction class.
`--addr` takes a raw `0x…`, a symbol, `*SYM+OFF` to dereference a pointer, or
`guest:0xLIN` for a guest LINEAR address — which is what guest RAM needs.
`*guest_ram+OFF` is **refused**, not served: see below. `--peek` hexdumps on every hit, which turns a
watchpoint on a completion flag into a clean snapshot of what a load produced.

Caveat: a stop can repeat on the same access without advancing. Treat repeated
identical hits as one event, not as several.

### `trace_diff.py` — bisect our trace against a reference, automatically

```bash
python3 scripts/trace_diff.py --dsk references/.../TOPBENCH.dsk --args=-l --bisect
python3 scripts/trace_diff.py --dsk ... --args=-l --shift 0 --skip 43648
python3 scripts/trace_diff.py --dsk ... --args=-l --shift 0 --skip 1032768 --reg CX
```

Rebuilds the core with the sampler in `cpu_exec.S` switched on (parameters in
`x86_trace.inc`, rewritten by this script and restored on exit), runs it under
gwemu, runs the identical image under Unicorn, and reports the first sample
where the two disagree. `--bisect` walks 16384 → 256 → 1 by itself.

It exists as a script because every part of it is a trap, and each one cost
real time here:

- A failed build followed by `&&`-chained steps leaves a **stale image** on the
  SD card and the probe reports the previous build's trace as the new one.
- The sampler must **saturate, not wrap**. A ring spans only its own length and
  silently shows the end of the run wherever you point it.
- A long `REP` makes thousands of consecutive EIP samples identical, so an
  index-by-index diff reports a match where there is only coincidence. Confirm
  with `--reg CX`.
- `--reg FL` samples R11, not `cpu_state.eflags`: EFLAGS is cached in a register
  and only written back on exit. AF is excluded from the comparison because x86
  leaves it undefined after shifts, rotates, logic and MUL/DIV.
- Unicorn fires its code hook once per `REP` element **and once more** with CX
  already zero. `refx86` drops that phantom firing; without it every index after
  a REP is off by one and it reads as "the target left the loop early".
- Anything we execute that the reference cannot know about — HLE stub
  instructions after an interrupt — shifts our index against theirs.
  `x86_cs_log_idx` says whether any interrupt was taken at all -- but only in
  a build with `X86_CS_LOG=1`; see below.

### `ref_trace.py` — run the same guest under a reference x86

```bash
python3 scripts/ref_trace.py --dsk references/.../TOPBENCH.dsk --args=-l --summary
python3 scripts/ref_trace.py --dsk ... --args=-l --every 16384 --count 64
python3 scripts/ref_trace.py --dsk ... --args=-l --skip 43520 --every 1 --count 64
```

When a guest walks off into memory it does not own, the question is always
"which instruction first behaved differently from a real 8086". This runs the
identical image under Unicorn (`pip install unicorn`) and prints a trace in the
same shape, so the two can be diffed. The image is built by
`scripts/common/dosimage.py`, which mirrors `x86_dos_exec` — same PSP, same
load segment, same relocations, same entry registers; starting anywhere else
would make the comparison meaningless. It stops at the first software
interrupt, which for a self-unpacking program is the finish line.

**Aligning the two traces takes care.** Our interpreter re-enters its fetch loop
once per `REP` element, and so does Unicorn's hook, but instructions executed in
the HLE stub page are ours alone and desynchronise the counters. A long `REP`
also makes thousands of consecutive samples identical, so a naive index-by-index
diff reports a match where there is only coincidence — confirm a suspected
divergence by comparing a register (CX, DX) over the same window, not just EIP.

### `screenshot.py` — capture what the panel is being sent

```bash
python3 scripts/screenshot.py --wait 20 --out build/artifacts/topbench.png
python3 scripts/screenshot.py --timeline timelines/run-to-completion.tl \
    --wait 48 --pan 16 --text --out build/artifacts/topbench.png
python3 scripts/screenshot.py --target hardware --wait 20
```

**It follows `ext_clut` when a 256-colour mode is up, and that is not
cosmetic.** `lcd_set_clut_ext()` parks a CALLER-OWNED palette in `ext_clut` and
`clut_push()` programs the LTDC from it, bypassing the darkened-twin
`active_clut` cache entirely — so reading `active_clut` (which is what this tool
did, and only 32 entries of it) recovers the PREVIOUS 32-entry CGA table and
repaints every mode-13h frame in unrelated colours over perfectly correct
geometry. That is indistinguishable from a palette defect in the core, and
`build/artifacts/wolf3d_after_1ah.png` is a capture of it: the DAC, the
256-entry CLUT and the LTDC were all correct in that run.
**The firmware's own readback has the same gap and is NOT fixed here:**
`lcd_get_clut_rgb565()` (`gw_lcd.c:433`) and `lcd_convert_lut8_to_rgb565()`'s
`clut == NULL` arm both read `active_clut`, so retro-go's on-device screenshots
and save-state thumbnails of a 256-colour guest are miscoloured on silicon even
though the panel is right.

`--text` dumps the 80x25 character buffer alongside the image, decoded as
**CP437**; judging small glyphs by eye is how a `Runtime error 203` went unread
through two captures, and printing only 0x20-0x7E hid every box-drawing and
shading code behind a `.`, which made a correctly drawn full-screen UI read as
four rows of stray letters on an empty screen.
`--text-aa 0|1` picks the renderer before capturing, so the SAME screen can be
grabbed both ways from one run rather than from two builds that also differ in
where the guest got to. `--pairs` counts the DISTINCT (fg,bg) attribute pairs a
text screen uses — the number that decides whether anti-aliasing fits, since
each pair costs a contiguous block of four CLUT entries.

`--attr` dumps the ATTRIBUTE byte of every cell (two hex nibbles, bg then fg)
with a census of the distinct attributes, and it is not optional on any screen
where the SHADING carries the meaning. Commander Keen's startup screen lists
every video mode, input device and sound card it knows about and marks the ones
it DETECTED purely by brightening the attribute — so a `--text` dump of a
machine that detected nothing is byte-identical to one that detected
everything, and a whole session was spent believing the first was the second.

`--pan N` pins the text window's leftmost column. It is now a no-op: the text
cell is 4 px wide, so all 80 columns land on the 320 px panel and there is
nothing off-screen to pan to. It is kept for a mode wider than the panel.

gwemu's own headless screendump comes back black, so it proves nothing. The
LTDC registers ARE modelled: this reads Layer 1's framebuffer address, pitch
and line count, reads that memory and the firmware's CLUT, and rebuilds the
frame. It also prints a coarse text map of the result, so a black or garbage
capture is obvious without opening the file. `tests/test_screen.py` does this
for one expected image; this is the general form.

### `profile_opcodes.py` — which x86 opcodes burn the cycles

```bash
make clean && make sdcard X86_PROFILE=1                   # instrumented core
python3 scripts/profile_opcodes.py --retrogo --settle 20 --window 25
python3 scripts/profile_opcodes.py --retrogo --target hardware \
    --settle 20 --window 25 --json build/artifacts/profile_topbench.json
python3 scripts/profile_opcodes.py --retrogo --sort mean   # expensive handlers
python3 scripts/profile_opcodes.py --retrogo --sort count  # hot but cheap ones
python3 scripts/profile_opcodes.py --bench --window 60     # tier 1 self-check
```

Reads DWT->CYCCNT at every dispatch and charges the delta to the PREVIOUS
opcode, into a 513-slot DTCM table (256 one-byte + 256 `0F` + one HOST slot for
time outside the interpreter). Prints count, total cycles, mean, overhead-
corrected mean and % of all cycles; `--json`/`--csv` make a profile diffable
across builds the way `tests/asm_baseline.json` already is for static counts.
Handler names come from the LIVE dispatch tables, resolved with `probe.py`'s
symbol machinery, so they cannot drift from what ran.

`--settle N` discards the first N seconds and then resets the table, so the
window is a steady state and not DOS startup. `--bench` (bare-metal only) runs
the synthetic benchmark, whose answer is known exactly — that is how the
plumbing is checked, not by trusting the cycles.

**The instrumentation is behind `X86_PROFILE=1` and OFF by default.** It sits in
the fetch/dispatch tail, the hottest and most register-starved code in the
project, and it costs ~65 cycles per dispatch on top of a ~50-cycle instruction.
Flags are not dependencies: build clean or you are profiling the previous core.

Three things the tool says in its own output, because none of them is optional:

- **A gwemu profile is not a profile.** gwemu does not model the pipeline
  (`docs/06`), so only the COUNTS mean anything there. Run it under gwemu first
  anyway — the point of that pass is to prove an instrumented dispatch tail does
  not fault, before it goes anywhere near silicon.
- **Attribution is approximate.** A delta measured between two dispatches on a
  superscalar M7 carries part of the previous instruction and of the cache/AXI
  state this one inherited. Rank hogs with it; do not quote a per-opcode cycle
  count as truth.
- **Measurement overhead is calibrated on the target** by `x86_profile_calibrate`
  (an empty loop against the same macro), stored in the table, and printed as a
  separate corrected column. Nothing on the target subtracts it, and there is no
  fudge factor. It is a lower bound — the calibration loop predicts perfectly
  and stays in cache.

A slot is closed by the NEXT dispatch, so the last instruction before each exit
from the interpreter is uncounted (one per interp call). Profiling also slows
the emulator, so anything timing-dependent in the guest is perturbed; that is
noted, not corrected.

### `profile_all.py` — ONE command, one workload, one full report

```bash
python3 scripts/profile_all.py --workload alleycat
python3 scripts/profile_all.py --workload topbench-menu --window 15
python3 scripts/profile_all.py --workload alleycat --passes zones   # quick
python3 scripts/profile_all.py --rom .../GAME.dsk --name mygame
python3 scripts/profile_all.py --compare before.json after.json
```

Builds the cores, boots a named workload, settles it, opens a window, collects
**everything**, and writes ONE report to `build/artifacts/profile_<workload>_
<target>.md` plus the same content as JSON so runs diff across builds. The
report LEADS WITH THE ANSWER — a "where the time went" paragraph — and puts the
tables under it.

It does not reimplement any instrument. It drives the ones that exist
(`x86_zones.h`, `x86_profile.h`, `probe.py`'s counter reads, the frame
histogram, the link's headroom numbers) and adds the thing that was missing:
**how one frame's wall time divides**.

**Several passes, not one, and that is deliberate.** `X86_PROFILE` costs ~65
cycles at every dispatch — it more than doubles guest execution, so a zone
breakdown taken from that build would say "guest execution is everything",
which is true of the instrumented core and false of the real one. So each
instrument gets its own clean build and the report says which pass every number
came from. Take the wall-clock SHARES from the zone pass and the opcode RANKING
from the profile pass. `--passes zones` skips the second build when only the
breakdown is wanted.

Presets pair a `.dsk` with the timeline and settle time that agree with it:
`alleycat` (graphics + speaker steady state), `alleycat-boot` (DOS startup and
the unpacker — disk-heavy, not a steady state), `topbench-menu` (CGA text, a
database read, an INT 16h poll loop), `topbench-score` (`-l`, `--settle 0`
because it halts on `INSB` a few seconds in).

#### The zones, and what each one contains

`X86_ZONES=1`, off by default like every other instrument. The design, the
nesting scheme and the arithmetic are in `x86_zones.h`; the short version:

| zone | contains |
|---|---|
| `guest` | `x86_cpu_interp_asm()` — emulated instructions |
| `hle` | INT 10h/16h/1Ah/21h service, **minus** the disk I/O nested under it |
| `disk` | the SD backend round trip only. FatFs's own cluster-chain walk is CPU work and stays in `hle` |
| `video` | CGA render + OSK draw + CLUT push |
| `audio` | PC-speaker render into the DMA half-buffer |
| `input` | gamepad poll, retro-go's PAUSE menu, the on-screen keyboard |
| `fwloop` | the firmware's `common_emu_frame_loop()` bookkeeping |
| `swap` | **idle**, waiting for the panel. A big number here is HEADROOM, not cost |
| `residual` | **UNACCOUNTED** — see below |

**Nesting is an explicit push/pop stack and every zone accumulates EXCLUSIVE
SELF-TIME.** While a child is open its parent accumulates nothing, so nothing
is ever counted twice and a parent's *total* cost is its own row plus its
children's. The frame is the ROOT zone, which is what makes the residual honest:
`residual` is the root's own self-time — measured frame-loop time that fell
through every named zone — not a subtraction of one total from another. It is
printed even when small, and flagged above 2%. **A large residual is the report
saying a phase exists that nothing brackets**, which is the finding this
instrument exists to produce.

Per zone the report gives total cycles, occurrences, occurrences per frame,
mean, and **the worst single occurrence** — worst-case is what stutter is made
of and a mean cannot contain one. The frame-time histogram and `frame_overruns`
come from the counters that already existed.

Measured under gwemu (see STATUS.md for the numbers): the instrument costs
**~91 cycles per enter/exit pair**, calibrated on the target by
`x86_zones_calibrate` and printed in the report as both a share of frame time
and a share of WORK. It is a lower bound and says so. It is per-FRAME and
per-SERVICE-CALL, never per guest instruction — that is what keeps it two
orders of magnitude cheaper than `X86_PROFILE`.

#### What is trustworthy, per tier — and the report says so itself

| tier | counts | cycles |
|---|---|---|
| gwemu | **exact and reproducible** | **not meaningful.** No M7 pipeline model (`docs/06`) and no SD busy state (`docs/04`), so `disk` reads as nearly free and *cannot* read as anything else |
| hardware | exact | real, including stalls |

That table is printed **in the report**, not only here, so a gwemu report can
never be mistaken for a silicon one.

### `make_font.py` — regenerate `font8x8.h`, `font4x8.h`, `font4x8aa.h`

```bash
python3 scripts/make_font.py                  # rewrite all three headers
python3 scripts/make_font.py --check          # fail if any is out of date
python3 scripts/make_font.py --show 0xC9      # one glyph, all three tables
```

Assembles a full 256-glyph **CP437** table from Daniel Hepper's public-domain
`font8x8` blocks, which are indexed by Unicode, plus 42 glyphs (card suits,
arrows, the maths operators at 0xEC-0xFE) drawn in the generator. Run it only
when the font changes; the results are committed.

It then emits the two **4-pixel-wide tables the renderer draws from**. 80
columns of 8 px is 640 and the panel is 320, so an 8x8 cell could only ever
show 40 of the 80 columns — and a DOS UI is centred on the 80, so the window
sliced it in half wherever it sat. At 4 px the whole 80x25 screen is exactly
320x200.

`font4x8.h` (1 bit per pixel) is the shipping face. Its letters are
**Bm437_EverexME_5x8** from VileR's Int10h Oldschool PC Font Pack, squeezed 5→4.
That replaced an OR-condensation of the 8x8 face, which looked poor for a
reason no tuning fixes: an 8 px face encodes stem placement and counters chosen
for eight columns, and halving the width destroys all of them at once. Everex
Micro Enhancer is a real character generator that showed 80 columns in a
five-pixel cell, so it was designed narrow. Measured over the 256 glyphs, only
77 put ink in the fifth column and 48 of those are line art that is hand-drawn
anyway, so the squeeze is exact almost everywhere; where it is not, columns 3
and 4 are OR-ed, touching only a right edge.

**0xB0–0xDF is hand-drawn in the generator, not converted** (`LINEART`,
`SHADES`, `SOLIDS`). This is what decides whether a narrow font is usable: a
horizontal rule must reach both cell edges or every frame comes out dashed, and
the vertical rule must sit in the same column in every glyph or corners miss. So
the grid is fixed — vertical at column 1, single horizontal at row 4, double
horizontals at rows 3 and 5. **Double VERTICAL rules deliberately alias to
single**: two strokes with a one-pixel gap in a four-pixel cell is mush and makes
every corner ambiguous. Double horizontals survive, so a `╔═╗` frame still reads
differently from a `┌─┐` one. `tests/test_text.py` asserts both properties over
the whole range, not just samples.

`font4x8aa.h` (2 bits per pixel) is the anti-aliased table: the authentic 8x8
glyph box-filtered 2:1 keeping the intermediate level, so a stem falling between
two output pixels becomes two half-lit pixels instead of vanishing. Line art is
promoted from the hand-drawn bilevel art at full level rather than filtered —
filtering a one-pixel rule would draw every frame at half brightness. See
"Anti-aliased text" below for why it is not the default.

DOS text is CP437, so the renderer indexes the font with the whole character
byte. The tree used to carry Hepper's 128 ASCII glyphs only, which forced a
`& 0x7F` in `text_render` and turned every box-drawing rule and shading block of
a full-screen UI into an unrelated letter.

### `sdcard.py` — compose the SD card

```bash
python3 scripts/sdcard.py --image sdcard_retrogo.img   # gwemu image
python3 scripts/sdcard.py --mount /media/you/RETROGO   # card in a reader
python3 scripts/sdcard.py --probe                      # device, over the probe
```

Same content, three destinations. Lays down `sd_content` (fonts/lang/bios),
`/cores/<name>.bin`, `/roms/<dir>/<rom>.dsk` + sidecars, and a `/CONFIG` that
preselects the ROM and disables idle standby.

### `verify_placement.py` — check the target matches the build

```bash
python3 scripts/verify_placement.py                    # symbol regions only
python3 scripts/verify_placement.py --retrogo --wait-magic --section .core_itcm
```

Confirms symbols landed in the memory they were designed for, and byte-compares
a section against the ELF. Catches a hot loop linked into flash, or a core
segment that loaded stale or partially — which otherwise surfaces only as an
UNDEFINSTR HardFault far from the cause.

### `make_retrogo_config.py` — generate `/CONFIG`

```bash
python3 scripts/make_retrogo_config.py --out build/CONFIG --rom dos ALLEYCAT.dsk
```

Preselects a ROM (so no launcher navigation is needed) and disables idle
standby. Called automatically by `make retrogo_media`.

### Vendored

`make_sdcard_image.py` (partitioned SD image), `sync_from_firmware.sh`
(re-vendor `sdk/` from a firmware checkout, then update `SDK_VERSION`).

## Source layout (emulator side)

| File | Role |
|---|---|
| `cpu_exec*.S`, `opcode*.S` | the Thumb-2 interpreter and its dispatch tables |
| `x86_profile.c/.h` | per-opcode DWT cycle profiler storage + calibration (`X86_PROFILE=1` only) |
| `x86_disk.c` | the block device under our FatFs, plus the .dsk sector cache: 4 x 16 KB LRU windows from the **AHB heap** (D2 SRAM -- DMA-reachable, and the only ABI pool with a real `free()`), which are the read-ahead AND the cache. Geometry is a build-time tunable (`X86_DISK_WINDOW_SECTORS` / `_COUNT`) |
| `x86_zones.c/.h` | frame-loop PHASE accounting (`X86_ZONES=1` only): a push/pop zone stack over the emulator loop, exclusive self-time per zone, and the root zone as an honest unaccounted residual. The instrumentation points are in `src/main.c` |
| `x86_disk.c` | the block device under our FatFs, plus the .dsk sector cache: 4 x 16 KB LRU windows from the bonus pool, which are the read-ahead AND the cache |
| `x86_probe.c/.h` | the probe ACCUMULATOR (`X86_PROBE=1` only): every service and port the guest touched, keyed and never evicted, with the flags that say how each was answered. Hooked at exactly two funnels — `x86_hle_service()` and `x86_port_read`/`x86_port_write` — so no handler needs an annotation macro. Read it with `scripts/probe_report.py` |
| `x86_mem.c` | guest page pool, fold mapping, guest↔host copies, the A20 gate and the HMA-scoped real-mode wrap |
| `x86_arena.c/.h` | the VRAM arena -- AXI claimed at top priority and never evicted -- plus the pin state the pager's victim picker consults. Claims are `count` blocks of `block_bytes`, each block contiguous, **all or nothing**; an EGA claim is four independent 64 KB planes, which is why a claim is blocks and not bytes. Also the live-pointer ring: `x86_guest_ptr()` hands out raw host pointers and no caller releases them, so a refcount cannot work, and the ring is cleared at guest re-entry -- the one moment nothing can still hold one |
| `x86_io.c` | port dispatch, PIT ch2 + speaker, CGA registers |
| `x86_audio_wave.h` | PC-speaker square-wave generator: PIT divisor -> phase increment -> samples. `<stdint.h>` only, deliberately — that is what lets `tests/host_audio.c` build it natively. Vendored from the author's earlier DOS core |
| `x86_video.c`, `cga_blit.S` | the mode table and everything it selects: text 0/1 (8x8 cells) and 2/3/7 (4x8, all 80 columns), CGA 4/5 through `cga_blit.S`, CGA 6 decimated 2:1, **VGA 13h (320x200x256 linear) with its 256-entry DAC**, the BDA-driven per-frame mode detection, the graphics-mode glyph rasteriser, and the 32-entry CLUT layout the CGA modes share. The 13h aperture, its DAC and its 256-entry CLUT are all **caller-owned** (LCD bonus pool) -- 66 KB of new `.bss` would break the guest allocation silently. See [`docs/08_vga_mode13h.md`](docs/08_vga_mode13h.md) |
| `x86_osk.c/.h` | on-screen keyboard + the mouse UI shell: layout, navigation, sticky modifiers, echo, the GAME cycle, and the overlay painted into the letterbox bars. Ported UI (`docs/input/03`); the injection under it is our own scancode path |
| `x86_hle.c` | `0F 0B` stubs, IVT planting, service dispatch |
| `x86_bios.c` | INT 10h / 16h / 1Ah and the BIOS Data Area, including INT 09h's shift decode (the OSK's shifted cells depend on it) |
| `x86_dos.c` | INT 21h, PSP, MCB arena, MZ loader |
| `font8x8.h` | 8x8 CP437 font, public domain (Daniel Hepper); the BIOS font handed to the guest |
| `font4x8.h` | 4x8 1-bit CP437, generated — Everex letters + hand-drawn line art; what `text_render` draws |
| `font4x8aa.h` | 4x8 2-bit CP437, generated — the anti-aliased table, linked only when `X86_TEXT_AA=1` |

## Harnesses (`tests/`)

```bash
python3 tests/test_opcodes.py            # 162 cases: EFLAGS, segmentation,
                                         # 16-bit, 66/67, interrupts, speaker
python3 tests/test_benchmark.py
python3 tests/test_game.py
python3 tests/test_dos.py                # MCB arena, walked from the host, plus
python3 tests/test_dos.py --negative     # 4 controls: rollback, largest,
                                         # coalesce, owner -- each rebuilds
                                         # x86_dos.c with ONE fix mutated back
                                         # to its defective form and requires a
                                         # FAIL; source restored in a finally.
                                         # --negative rollback runs just one.
                                         # The arena half now drives STAGES
                                         # (run_dos_flag 2..7) so the host can
                                         # photograph the whole chain either
                                         # side of a single failed AH=4Ah and
                                         # compare it BYTE for byte, and stage
                                         # 7 runs the DOS/4GW probe loop and
                                         # reports MCB header reads per call
                                         # (x86_mcb_reads).
                                         # FILE I/O from guest code against a
                                         # FAT12 volume the test builds itself:
                                         # 3Dh/3Fh/42h/3Eh/43h/4Eh/4Fh, the
                                         # not-found path, a drive-letter
                                         # prefix and a lower-case name.
                                         # ALSO the MZ LOAD arm: a real .EXE
                                         # the test builds is run through
                                         # x86_dos_exec() and the IMAGE IS
                                         # COMPARED BYTE FOR BYTE at load_seg,
                                         # with the PSP (mem_top, env segment,
                                         # command tail) and the entry state
                                         # read out of guest memory. A one-byte
                                         # poke is the negative control. This
                                         # is the check whose absence let a
                                         # change that loaded WOLF3D.EXE as
                                         # garbage pass every tier-1 test --
                                         # "still running" is not "working".
                                         # NOTE tier 1 maps guest memory FLAT
                                         # (tests/main.c identity fold), so it
                                         # cannot see corruption that depends
                                         # on 4 KB granule alignment; that
                                         # class still needs a picture.
python3 tests/test_pit.py                # PIT channel 0 -- the SYSTEM TIMER's
                                         # divisor -- programmed by GUEST OUTs:
                                         # the real Wolf3D sequence (0x214B =
                                         # 140 Hz), lo-byte-only access keeping
                                         # the high byte, a channel 2 program
                                         # not disturbing it, and reload 0
                                         # meaning 65536. --negative rebuilds
                                         # with the port 0x40 case DELETED and
                                         # requires a FAIL, restoring the source
                                         # in a finally
python3 tests/test_keyboard.py           # BDA ring -> INT 16h, consumed by
                                         # GUEST code: AH=01 peek + its ZF on
                                         # the IRET frame, AH=00 dequeue, the
                                         # empty case and a wrap at the end of
                                         # the ring
python3 tests/test_audio.py              # PC-speaker wave generator, NATIVE --
                                         # no target at all. Compiles
                                         # tests/host_audio.c against
                                         # x86_audio_wave.h with the host cc and
                                         # MEASURES divisor -> output frequency,
                                         # the Nyquist guard and the anti-click
                                         # reset, in under a second. It also
                                         # carries the implementation it
                                         # replaced and prints both errors side
                                         # by side, because a wrong pitch is
                                         # invisible in a screenshot and
                                         # unjudgeable under gwemu
python3 tests/test_osk.py                # the on-screen keyboard, driven by
                                         # button MASK exactly as src/main.c
                                         # drives it: the grid press, the
                                         # navigation, B as Backspace, a
                                         # SHIFTED cell (which only works if a
                                         # real Left-Shift make reaches INT 09h
                                         # first), the three-state GAME cycle,
                                         # the row-width check, and that
                                         # drawing never touches guest VRAM
python3 tests/test_video.py              # CGA mode 4 blitter, pixel-exact, plus
                                         # MODE SWITCHING driven by GUEST code
                                         # through INT 10h: 3 -> 4 -> 3 rendered
                                         # and compared at every step; AH=0Eh
                                         # rasterising an 8x8 glyph into BOTH
                                         # interleave banks in mode 4 (the Alley
                                         # Cat defect); mode 6 at 640x200x1;
                                         # mode 0 at 40 columns and the 8x8
                                         # cell; and a mode nothing renders
                                         # being REFUSED, not adopted.
                                         # It also runs the two PLANAR arms LAST
                                         # (they remap guest 0xA0000/0xB8000
                                         # onto the real aperture, which the
                                         # other arms do not expect): unchained
                                         # 13h "Mode Y" and EGA 0Dh, each 64,000
                                         # pixels exact against a host-computed
                                         # pattern, driven by GUEST code so the
                                         # store hook and the word `out dx,ax`
                                         # register writes are on the path.
                                         # --modey-only runs just the Mode Y arm.
                                         # An EGA-FIT arm runs after those, on
                                         # the layout the REAL core claims (35
                                         # pages, CGA then the 13h aperture):
                                         # mode 0Dh must lend the CGA claim out,
                                         # pull the aperture down and come back
                                         # with 19,456 plane offsets instead of
                                         # 8,192; returning to text must give the
                                         # CGA claim back BLANK; and against an
                                         # arena with no spare page 0Dh must be
                                         # refused and counted, never quietly
                                         # given a card of zero.
                                         # A THIRD Mode Y arm asks a question no
                                         # pixel test can: WHICH FRAMES ARE
                                         # PAINTED. Mode Y aliases every page
                                         # into one page of storage, so a blit
                                         # on a timer lands mid-redraw -- a
                                         # split raycast viewport, a menu
                                         # highlight showing the old item and
                                         # the new one. It drives
                                         # x86_video_modey_paint_due() directly
                                         # against four modelled guests, with
                                         # no LCD and no CPU -- including a
                                         # SLOW gameplay guest that draws in
                                         # every frame and flips every 300,
                                         # which is the case the device hits
                                         # and the 20-frame model missed.
                                         # --present-only runs just that arm
python3 scripts/run_planar_controls.py   # grades those arms: FIVE negative
                                         # controls that must FAIL (chain-4
                                         # inert, a 40-offset blit stride, four
                                         # FLAT planes, the fixed 16 KB page
                                         # mask, and the presentation escape
                                         # hatch back on a plain TIMER) and one
                                         # POSITIVE control --
                                         # -DX86_MODEY_PAGE0=16384 must still
                                         # produce the same 64,000 pixels,
                                         # because the shipped default equals
                                         # the right answer and without it
                                         # nothing tests the stride learning.
                                         # One CLEAN build per arm (flags are
                                         # not dependencies), so it is slow
python3 tests/test_nor.py                # NOR cold-page backing and COW, driven
                                         # by GUEST code. The fake NOR is the
                                         # extflash window ITSELF, written with
                                         # plain stores -- ordinary memory under
                                         # gwemu, but a genuine 0x9xxxxxxx
                                         # address, so bit 31 is set and the
                                         # fold check traps for the same reason
                                         # it will on silicon. A .bss buffer
                                         # would never trap and would prove
                                         # nothing. 23 checks: a mismatched
                                         # snapshot REFUSED with the fold table
                                         # verifiably untouched, a read served
                                         # from NOR, a write that COWs exactly
                                         # one page, a second write to that page
                                         # that does not trap at all, and the
                                         # NOR image byte-unchanged after both
python3 tests/test_video.py              # CGA mode 4 blitter, pixel-exact
python3 tests/test_text.py               # CGA text mode 3, pixel-exact:
                                         # CP437 at 4x8 and 8x8, attributes,
                                         # all 80 columns, scroll, line-art
                                         # continuity, the 32-entry CLUT, and
                                         # BOTH renderers with their per-frame
                                         # palette allocation
python3 tests/test_screen.py             # what the LTDC actually scans out,
                                         # reconstructed to build/artifacts/screen.png
                                         # (checks whichever mode the core is
                                         # in -- text if a guest is running)
python3 tests/test_disk_cache.py         # .dsk sector cache + read-ahead:
python3 tests/test_disk_cache.py --sweep # backend calls + hit rate across
                                         # several cache geometries. The stream
                                         # win is bought by window SIZE (4x16K
                                         # and 2x16K both do 248 reads in 9
                                         # calls; 8K needs 16, 4K needs 32);
                                         # the COUNT buys residency, which the
                                         # metadata-vs-stream arm shows by
                                         # FAILING at --window-count 2
                                         # window alignment, LRU residency
                                         # under a stream, a short window at
                                         # the end of the image, invalidation
                                         # on write, a failed fetch, and 3,000
                                         # random reads byte-compared against
                                         # the UNCACHED path
python3 tests/test_sysinfo.py            # the machine DESCRIPTION services,
                                         # from guest code: INT 11h equipment,
                                         # INT 12h conventional KB, INT 15h
                                         # AH=88h extended KB and AH=C0h, the
                                         # BDA fields that must agree with
                                         # them (0040:0010/0013/0063/0080/
                                         # 0082), and the PC-speaker feedback
                                         # bits on port 0x61. Every call
                                         # POISONS AX first and sets CF the
                                         # wrong way round, because these three
                                         # vectors used to reach the bare IRET
                                         # stub -- which does not clear a
                                         # register, so the guest read back its
                                         # own value and believed it. Measured
                                         # pre-fix: INT 12h answered 65,535 KB
                                         # of conventional memory. There is no
                                         # host-side symptom for that at all
python3 tests/test_timeline.py --offline # the .tl parser and event schedule,
                                         # no target at all
python3 tests/test_paging.py             # the guest memory ARCHITECTURE, as
                                         # opposed to one backing tier:
                                         # A20 through all three doors (the
                                         # 8042 output port, 0xDD/0xDF, and
                                         # port 0x92) with the wrap arms driven
                                         # by GUEST code through a segment
                                         # override -- the only way an address
                                         # above 0xFFFF is formed in real mode;
                                         # and the VRAM arena, whose load-
                                         # bearing arm is that a claim which
                                         # cannot be fully placed changes
                                         # NOTHING and reports its exact
                                         # deficit. Two arms (0x110000 and
                                         # 0x200000 not aliased) fail on the
                                         # pre-fix whole-space wrap
python3 tests/test_seam.py               # THE GRANULE SEAM. host = lin + k with
                                         # one k per 4 KB page, so two adjacent
                                         # GUEST granules are adjacent in HOST
                                         # memory only by luck -- the pager hands
                                         # frames out in FAULT order. A 16- or
                                         # 32-bit access spanning the boundary
                                         # resolves one pointer from the LOW
                                         # granule and runs off the top of that
                                         # frame, silently.
                                         # This is the ONE tier-1 harness whose
                                         # fold is deliberately NOT contiguous:
                                         # every other arm runs identity over
                                         # test_ram or a pager whose frames come
                                         # out of one extent in order, and the
                                         # defect cannot occur in either, which
                                         # is why it went unseen. The arm gives
                                         # the two granules one frame each from
                                         # extents 48 KB apart and paints the low
                                         # frame's slack 0xEE, so a broken access
                                         # returns something recognisable rather
                                         # than a plausible zero.
python3 tests/test_seam.py --negative    # the negative control, and it MUST
                                         # fail without --negative: builds
                                         # X86_SEAM_FIX=0 (no prologue, no
                                         # flush) and asserts the WRONG answers
                                         # -- reads returning 0xEE slack and a
                                         # straddling store landing in it. The
                                         # contiguous arm is asserted positively
                                         # in BOTH builds, because a fixed
                                         # mapping's fold entries compare equal
                                         # and x86_seam_sync() early-outs.
python3 tests/test_core_retrogo.py       # tier 2
python3 tests/test_timeline.py           # tier 2: replays a timeline through
                                         # the HARDWARE player (shadow word at
                                         # 0x30001FF4, which gwemu maps) and
                                         # times each button's arrival in
                                         # input_pad_seen. The one way to prove
                                         # the hardware input path without
                                         # silicon
```

`test_disk_cache.py` is the one harness that does NOT drive a target. It
compiles the shipping `x86_disk.c` for the HOST (`-DX86_DISK_HOST_TEST`, which
only swaps the DWT read for a variable) against a small shim that supplies a
backend reader, and drives `disk_read`/`disk_write` through ctypes. The cache
is arithmetic over a callback — alignment, replacement, boundaries — and those
bugs are found in milliseconds off-target and in ten minutes on one. It caught
two the design had not: a straddling multi-sector read cost MORE backend calls
than no cache at all, and an undersized buffer must be refused outright rather
than half-used.

All accept `--target gwemu|hardware`, `--gui`, `--audio`, `--icount`,
`--timeline`, `--physical-sdcard`, `--keep-temp`.

Each gwemu run copies the SD image and extflash (~66 MB) into `build/run/`,
which is deleted on exit. `--keep-temp` retains it for analysis. Nothing is
written to `/tmp` — it is RAM-backed on this machine.

## Tier 2 build order

```bash
python3 scripts/build_retrogo.py --clean   # firmware (separate tree)
make sdcard                                # core -> pack -> SD image
python3 tests/test_core_retrogo.py
```

`make sdcard` chains the core build, so the image can never be assembled around
a stale core.

**`gwemu_release` does NOT rebuild the emulator bank images.** Its prep block is
guarded by `if [ ! -f build/sdcard.img ]` and prints "QEMU images already exist
(skipping prep)" — so once any gwemu media exists, `qemu_bank1.bin` keeps
whatever the FIRST build put there while every later build updates
`gw_retro_go_intflash.bin` beside it. Tier 2 then boots hours-old firmware with
a successful build log above it; that is how a `REMOTE_INPUT=1` firmware came up
with no remote input in it, and it cost most of an evening. `retrogo_build.py`
now re-cuts the bank the firmware was linked for after every build (bank 1 on a
bank-2 build is the stock-firmware backup and is left alone). Same failure shape
as the stale SD image in docs/04, one layer down: **when tier 2 behaves like
code you did not write, check the mtime of `qemu_bank1.bin` against
`gw_retro_go_intflash.bin` before anything else.**

**Where the line sits:** make compiles (`Makefile` for the bare-metal harness,
`Makefile.core` for the packed core) — it owns source→binary dependency graphs.
Python composes, deploys, orchestrates and talks to targets, because those need
validation, multiple destinations and target awareness.

Launch is driven by `/CONFIG` preselection plus
`timelines/start-preselected-game.tl` (presses A twice). Do not navigate menus.

**Button names are not what they look like.** gwemu's `press start` drives
`B_START` — a button the stock Game & Watch does not have, which retro-go maps
to `ODROID_INPUT_X`, a bit the core does not read. The physical **GAME** button
is `ODROID_INPUT_START`, so `press game` is what tests the core's Return
mapping. A timeline that says `press start` exercises nothing, which is how
"START sends Return" stayed believed-working while the device disagreed.
`timelines/press-game.tl` presses TIME (N, to answer Alley Cat's joystick
prompt) and then GAME, and `tests/test_core_retrogo.py` asserts the byte
0x1C reaches the guest through `x86_kbd_read_log`.
`timelines/set-cpu-option.tl` walks the PAUSE/SET menu to the core's own
"CPU Speed" entry, changes it, then Save & Quits and relaunches — so one run
covers "the menu opens", "the change takes effect" and "it survives a restart".
It navigates by pressing UP once in the settings menu: the dialog's selection
wraps and our entry is appended last, so this keeps working if the firmware
grows an option.
`timelines/press-menu-keys.tl` and `timelines/press-game-menu.tl` drive a
guest's own MENU -- DOWN (scancode 0x50, an extended key with no ASCII) and
then GAME (Return) -- with a screenshot between each press. A keyboard chain is
only proven when the guest's PICTURE changes: counters can all look right while
the key goes to a program that ignores it. Capture with
`screenshot.py --timeline ... --wait N --text` at two different `N`.
`timelines/profile-alleycat.tl` gets Alley Cat actually RUNNING and then stops
touching it, which is what a profile window needs and what no other timeline
did. Alley Cat has **two** gates: the joystick prompt (answer N, which is
gwemu's `press select` -- `press time` sends Y and lands on "Game Control
Adapter is not present") and then a skill-level menu wanting K/H/T/A, a LETTER
**no button can send**. The on-screen keyboard is the only door: GAME opens it
on layer 0 cell 0, which is `a`, and two more GAME presses close it again.
Verified with `screenshot.py --text` at each stage. This matters because
profiling the unanswered prompt returns 56% `CMP Gv,Ev` + 44% `JZ Jb` -- an
exact measurement of a game that never started, and one that looks like a
result.
`timelines/keen4-keypress.tl` presses keys AT the guest, on several different
buttons, for a title whose startup ends in a "press any key" wait. Commander
Keen 4 sits in `IN_Ack` at guest linear `0x15C70` forever under
`start-preselected-game.tl`, which presses nothing after launching -- so every
Keen measurement before this one stopped there by construction and could say
nothing about what follows. It is proven by `x86_disk_reads` moving (618 -> 734)
and the planar store counters resuming, not by the screen, which stays black.
`timelines/run-to-completion.tl` is the same launch with a 58 s window, for a
guest doing real work whose result only appears at the end -- the one case where
the wait IS the result rather than a symptom to chase with `probe.py`.

**GAME NO LONGER SENDS RETURN**, and a timeline written against that is testing
something that was deliberately removed. It is the on-screen keyboard's only
door (`docs/input/02-button-mapping.md`), so `pad_scancode[6]` is 0 and Return
moved to the physical START button -- which IS gwemu's `press start`, the key
this document used to warn was inert. Three timelines drive the keyboard:

- `timelines/osk-open.tl` -- open with GAME, move the cursor, press a key,
  cycle the layer, step into mouse mode, move the pointer, close. One
  screenshot per state, so the keyboard's own picture is the evidence.
- `timelines/osk-over-game.tl` -- the same over a GRAPHICS-mode guest. It
  answers Alley Cat's joystick prompt with `press select` first, because the
  game stays in text mode until it is answered and the capture would otherwise
  be of the other case.
- `timelines/osk-with-pause.tl` -- open the keyboard, then retro-go's PAUSE
  overlay on top of it. This is the one that exercises `core_repaint()` with
  the OSK live; run it under `probe.py --faults` rather than only capturing,
  because "the callback faulted" and "the menu is drawn over it" look identical
  in a screenshot.

Two more drive the keyboard's DELIVERY rather than its picture, and both were
written for the "keys only arrive once the OSK is closed" report:

- `timelines/osk-live-keys.tl` -- answer Alley Cat's prompt, open the OSK and
  press the same cell three times several seconds apart, leaving the keyboard
  UP throughout. Watch `x86_osk_codes` (codes injected) against
  `x86_kbd_port60_reads` (codes the guest took) and `input_kbd_ht` (the 8042
  head/tail): reads that lag the injections and then catch up all at once when
  the OSK closes ARE the defect, and nothing else in the tree shows it.
  `input_irqs` is the second witness -- 60/s means IRQ 1 is being re-armed
  every frame at a guest that cannot take it.
- `timelines/osk-held-across.tl` -- LEFT held for 12 s while GAME cycles the
  OSK all the way round underneath it, then ordinary taps after the release.
  This is the "a button held across the transition" case; the release code goes
  out when the OSK takes the frame and the taps afterwards must still emit.

Which key each captures is worth knowing: the OSK opens on layer 0 cell 0, so
`press a` sends the letter `a` (scancode 0x1E) and `x86_kbd_read_log` is where
it is confirmed to have reached the guest.

### Recording an input timeline

`--record FILE.tl` on any target script captures an interactive gwemu session.
Needs the window (`--gui`); **quit gwemu normally** -- the file is written on
exit, so a killed process leaves nothing.

```bash
python3 scripts/probe.py --retrogo --gui --audio --timeline "" \
    --record timelines/alleycat-ingame.tl --samples 150 --interval 5
```

**The `.tl` grammar, read out of gwemu's own parser**
(`references/gwemu/hw/misc/gnw_timeline.c`):

```
# comment
[MM:]SS[.fff][s] | @FRAME    press   <btn>[+<btn>...]      # down, +100ms, up
                             hold    <btn>[+...] <secs>    # down, +secs, up
                             down    <btn>[+...]           # and stays down
                             release <btn>[+...]
                             screenshot <file.png>
                             quit
```

Buttons are case-insensitive: `up down left right a b game time start select
pause pwr` (gwemu spells it `pwr`; `power` is accepted too). `@N` addresses a
vblank FRAME rather than a time.

**A recording uses only half of this language.** `--record` emits `down` /
`release` pairs and nothing else, and every hand-written timeline in
`timelines/` is written in `press` / `screenshot` — so a parser built from a
recorded file rejects almost the whole directory. `scripts/common/timeline.py`
implements the grammar above, from the source, and `tests/test_timeline.py
--offline` parses every checked-in `.tl` as a smoke test.

`timelines/alleycat-ingame.tl` is a real session driving Alley Cat from launch
to gameplay: TIME answers the joystick prompt with N, then the skill menu needs
a LETTER, which is only reachable through the on-screen keyboard.

### Replaying a timeline on REAL HARDWARE

`--timeline FILE.tl` means the same thing on every tier: the tests do not know
which one they are on.

```bash
# gwemu (unchanged): the emulator replays the file itself, against emulated
# time -- exact.
python3 scripts/probe.py --retrogo --timeline timelines/press-game.tl

# hardware: the same file, injected over the debug probe.
python3 scripts/probe.py --retrogo --target hardware \
    --timeline timelines/press-game.tl --watch input_pad_seen,input_scancodes
```

How it works: a `REMOTE_INPUT` firmware build ORs a 32-bit shadow word at
`0x30001FF4` into the live button state on every input poll, so a probe write is
indistinguishable from a physical press. `HardwareTarget` folds the timeline
into a schedule of button masks and writes them from a background thread while
the harness keeps polling — the mechanism is vendored from the firmware's proven
`scripts/remote_input.py` (`references/` is gitignored, so nothing may depend on
it at runtime).

**The firmware must be built with `REMOTE_INPUT=1`.** Ours always is —
`retrogo_build.py`'s `RELEASE_DEFAULTS` — deliberately not a switch, because a
firmware without it turns every hardware timeline into a test that presses
nothing and passes. **Do not check for it by grepping the binary for
`30001FF4`: the compiler puts `.word 0x30001000` in the literal pool and reaches
the cell with `ldr.w rX,[rY,#4084]`, so the address never appears as four
consecutive bytes.** `retrogo_build.verify_remote_input(elf)` disassembles
`buttons_get` and checks the arithmetic; the build fails if the define did not
take.

**`quit` on hardware stops injecting** — buttons released, shadow word zeroed,
the player finishes. There is no process to kill, and the device keeps running;
harnesses still derive their wait from `timeline_quit_time()`, so a run ends
where it would under gwemu.

**Accuracy.** Every state change is one 32-bit write; nothing compensates for
its latency, and the file is replayed as written. Measured under gwemu over
`GDBBackend` on `timelines/alleycat-ingame.tl` (78 state changes): the write
itself costs **0.11 ms mean / 0.28 ms max**, and events left **0.1–0.2 ms** late
against the file. End to end — press in the file to the bit appearing in the
firmware's `input_pad_seen` — was **+13 to +60 ms**, nearly all of it the
harness's own 50 ms observation poll. **OpenOCD over SWD has not been measured;
expect it to be slower than GDB-to-gwemu.** A press held for less than one input
poll (~16 ms) may be delivered and never observed; the player prints a warning
naming each such press rather than letting it silently do nothing. The shortest
press in any checked-in timeline is 40 ms.

## Modules (`scripts/common/`)

Importable support only — no CLIs.

| Module | Purpose |
|---|---|
| `target.py` | `Image` (what boots) + `GwemuTarget`/`HardwareTarget` (where). One launch path for every tier. Also `timeline_quit_time(path)`: a test that polls longer than the timeline's `quit` outlives gwemu, and the dropped GDB link looks like a target fault. Derive waits from it; never hardcode. |
| `elf.py` | `get_symbols`, `nearest_symbol` |
| `sdcard.py` | Three SD destinations (image / probe / mounted) plus `compose()`, which lays out what the firmware needs |
| `timeline.py` | The `.tl` grammar (gwemu's, in full) plus the two players: gwemu replays a file itself, hardware gets `ShadowWordPlayer` writing `0x30001FF4` over the probe. `--timeline` therefore means the same thing on both tiers. Reports what was ACTUALLY delivered — lateness per state change and the hold each press got — instead of assuming the file's schedule |
| `retrogo_build.py` | Firmware build + artifact staging. Builds with `REMOTE_INPUT=1` and CHECKS the built image for it; also re-cuts `qemu_bank{1,2}.bin`, which `gwemu_release` silently does not (see below) |
| `retrogo_config.py` | `/CONFIG` blob generation |
| `core_abi.py` | Preflight: will this firmware accept this core? |
| `rglog.py` | Read retro-go's in-memory log ring |
| `faults.py` | Decode latched CPU faults |
| `asm_metrics.py` | Instruction counts, DWT capture |
| `zones.py` | Host side of the frame-loop zone accounting: locate the block, ask the TARGET to reset it at its own frame boundary (a host-side memset would race the stack marks), read it back, and derive the shares, the residual and the instrument's own overhead. Layout mirrors `x86_zones.h` |
| `opcode_profile.py` | Host side of the per-opcode DWT profile: locate/reset/read the DTCM table, resolve slots to handler symbols off the live dispatch tables, render table/JSON/CSV. Layout and the on-target half live in `x86_profile.h` / `cpu_exec.S` |
| `probe.py` | Sample PC, globals, pointer derefs and array dumps on a running target; `read_faults` decodes CFSR/HFSR/BFAR |
| `probe_acc.py` | Host side of the probe accumulator: locate `x86_probe`, ask the target to reset it, read it back, and JOIN runtime evidence with the expectations table into one status per row. The precedence rule is the module's whole correctness argument — **runtime evidence wins**, and the table only classifies calls the target says WERE answered. Layout mirrors `x86_probe.h` |
| `probe_expect.py` | The curated "what SHOULD we have returned" table — RBIL-shaped (vector, AH, subfunction, what a real BIOS/DOS does) plus what OUR handler does about it. This is the oracle, and the module docstring records why it is a table rather than a port-capable `refdos.py` or a DOSBox capture |
| `watchpoint.py` | Z2/Z3 data watchpoints over the GDB remote protocol, and the unsolicited stop reply that follows a resume |
| `fat12.py` | Builds a 64 KB FAT12 volume in memory with named files and known contents. A test that asserts on a shipped `.dsk` is asserting on someone else's data; this lets `test_dos.py` write the bytes it then expects to read back |
| `dosimage.py` | FAT12 reader + MZ loader that mirrors `x86_dos_exec`, so a reference emulator starts from OUR entry state |
| `refx86.py` | Unicorn run of that image, sampled like the target's own sampler |
| `refdos.py` | A minimal DOS/BIOS for the reference, so it can run PAST the first INT 21h and say what the program should do. Deliberately not modelled on our x86_dos.c -- mirroring it would make the two agree on the same mistake. It has no port I/O, so it cannot follow a program that times against the CGA retrace |
| `textmode.py` | Host-side model of the CGA text renderer: CP437 font load (4x8 by default, the table the renderer uses), the 80x25 -> palette-index render at 4x8 cells, AH=06 scrolling, and a readable CP437 dump. Shared by `test_text.py` (seeded pattern) and `test_screen.py` (the live guest) |
| `verify.py` | Symbol-region checks and ELF-vs-target section comparison |
| `builder.py`, `runner.py` | Tier-1 build convenience |

## Non-negotiables

- Build flags are **not** dependencies — change one, pass `--clean`. **This
  includes `lib/FatFs/ffconf.h`**: editing it and rebuilding incrementally links
  objects compiled against two different configs, and `FF_FS_EXFAT` alone
  changes the width of `LBA_t`. That produced a hang and a mounted-but-empty
  directory, both of which read as "the change broke FatFs" and neither of
  which was real. `rm -f lib/FatFs/*.o build/core/*.o` first.
- **Our FatFs is not the firmware's.** Ours mounts the FAT12 inside a guest
  `.dsk` and is built with LFN and exFAT OFF (a floppy has neither, and it buys
  8 KB of RAM_EMU). The firmware's separate instance reads the `.dsk` off the
  card and keeps LFN. Code that walks a `FILINFO` must use `DOS_SHORT_NAME()`,
  because `altname` does not exist without LFN.
- **The CS and interrupt-vector trace rings are OFF by default: `X86_CS_LOG=1`.**
  `x86_cs_log`, `x86_cs_log_idx` and `x86_int_vec_log` are written from inside
  `L(SET_SREG)` in `cpu_exec_handlers.S`, which is the single path every far
  `CALL`, `RETF`, `IRET` and interrupt takes -- fourteen instructions and five
  stores per far transfer, plus a four-instruction IVT re-read in the interrupt
  path that exists only because the logging needed R0. TOPBENCH takes 1.22 M far
  transfers per 25 s window, so that was ~17 M instructions of logging against
  56.6 M dispatches. `--gc-sections` drops the storage in a default build, so
  `probe.py --dump x86_cs_log:16x4` and `trace_diff.py` report the symbol as
  MISSING rather than reading zeros. Rebuild (clean -- flags are not
  dependencies) with `X86_CS_LOG=1` when you need either ring.

- **Measure `__CORE_BSS_END__` after any change to the core.** The core's TEXT
  lives in RAM_EMU too, so code growth counts. Overshooting is SILENT: the
  guest RAM allocation returns NULL, `core_ready` stays 0 and the emulator
  never starts.
- Green under gwemu ≠ hardware ready; gwemu does not model peripheral setup.
- Never use raw GDB subprocesses or `gnwmanager monitor`; use the backends.
- Never run concurrent builds or tests from one directory.
- All scripts are Python with `argparse`. No `.gdb` scripts.

Detail lives with the code: each module's docstring, plus
[`docs/04`](docs/04_retro_go_and_gwemu.md) (gwemu/retro-go traps) and
[`docs/06`](docs/06_gwemu_determinism.md) (determinism data).

## Anti-aliased text, and the CLUT budget it lives in

`lcd_set_clut(clut, count)` clamps `count` to 32 and then writes a 40%-darkened
twin of entry `i` at `count + i`. Retro-go's overlay reaches those twins by
OR-ing `LCD_DARKEN_BIT = 0x20` — a FIXED +32 — into every framebuffer byte, so
the two only agree when `count` is exactly 32. Indices 64-67 are the overlay's
own theme (`LCD_OVERLAY_CLUT_BASE`), and the firmware cache is 68 entries.

**So the budget is 32 entries, not 64.** 32-63 are not free; they are the twins.
(One exception, and it is a different firmware path rather than a bigger count:
`GW_LCD_SET_CLUT_EXT` takes a caller-owned 256-entry palette and bypasses the
twin cache entirely, which is what VGA mode 13h uses. The price is that
`LCD_DARKEN_BIT` stops working while that mode is up --
[`docs/08_vga_mode13h.md`](docs/08_vga_mode13h.md).)
The core published 16, which put the twins at 16-31 while the overlay read
32-47 — zeros — so opening retro-go's menu dimmed the game to solid black. It
publishes 32 now, in every mode.

Anti-aliasing spends that budget as eight four-entry blocks, allocated **per
frame** to the (fg,bg) pairs the 2,000 cells actually use, ranked by cell count;
pairs that miss out degrade to bilevel in the nearest published colour.
Measured with `screenshot.py --pairs`, TOPBENCH's menu uses exactly 8 — a fit
with nothing to spare — and its boot console uses 1.

```bash
make sdcard X86_TEXT_AA=1      # capture-only core: AA on, bilevel table dropped
```

It is **off by default**, and the reason is size, not quality:

> RAM_EMU spans `0x2404B000`-`0x24100000` (741,376 B). The core ends at
> `__CORE_BSS_END__` (measure it: `arm-none-eabi-nm build/core/dos_core.elf`),
> and the guest asks for 32 KB VRAM + 640 KB RAM = 688,128 B from that region.
> **Measured 2026-08-13, after mode 13h and moving CGA VRAM to the bonus pool:
> BSS end `0x240587f8`, headroom 30,728 bytes** -- the guest now asks RAM_EMU
> for 640 KB rather than 672 KB, because its 32 KB of VRAM comes from the bonus
> pool. Mid-change the same measurement read **-2,024**, which is the state in
> which `guest_ram` returns NULL and nothing says so. It was
> 424 bytes before the 4 KB open-bus filler was moved to the bonus pool; the
> "2,528 bytes" this document used to quote was two commits of growth out of
> date, which is why the number is now a measurement instruction and not a
> constant.

Each narrow table is 2 KB, so an AA core must drop the bilevel one to pay for
it (`X86_TEXT_BILEVEL_FONT=0`, set automatically) and degrades by thresholding
the 2-bit glyph instead. The bare-metal harness has no such limit and links
both, which is how `test_text.py` checks each renderer against its own font.

**The trap this hid behind:** an over-allocation is quiet. `ram_malloc()` does
bound-check and returns NULL (`Core/Src/gw_malloc.c`) — so nothing is
overwritten — but a NULL that a caller tolerates is a feature silently missing,
not an error. That is exactly what happened to the 4 KB open-bus filler: it was
the LAST allocation, `if (upper_filler)` skipped the mapping, and the symptom
was an option-ROM scan faulting in the guest. It comes from the bonus pool now,
and `upper_filler_ok` reports it. If a change to the core makes a previously
working guest fault early, check `__CORE_BSS_END__` before anything else.
