# Device Emulation & FatFs High-Level Emulation (HLE)

## 1. Minimal PC Devices
To support Windows Protected Mode and standard DOS applications, we must implement a minimal subset of PC hardware in Thumb v2 assembly:
- **PIT (Programmable Interval Timer)**: Required for OS scheduling.
- **PIC (Programmable Interrupt Controller)**: Required for hardware interrupt routing.
- **Keyboard (PS/2)**: Interfaced via Retro-Go's input ABI.
- **VGA**: Requires specialized memory mapping.

## 2. VGA Memory Management
The STM32H7B0VB has a ~154KB Framebuffer. 
Standard VGA text and CGA graphics require 32KB of VRAM. This 32KB is permanently allocated from the AXI SRAM pool.

### EGA 256KB Mode
When a guest requests EGA high-resolution graphics (which requires 256KB of VRAM), we cannot satisfy it with the base 32KB allocation.
- We will dynamically borrow the additional **224KB** from the AXI guest RAM pool (~840KB).
- This temporarily reduces the available physical RAM for guest pages, causing more aggressive swapping to the SD card.
- However, games utilizing EGA 256KB typically require less raw guest RAM, making this dynamic borrowing acceptable.
- VGA accesses will be trapped and translated linearly to this memory block, which is periodically blitted to the Retro-Go framebuffer via DMA2D or CPU depending on format.

## 3. Storage & FatFs High-Level Emulation (HLE)
Emulating a raw IDE controller (INT 13h) and forcing the guest OS to run its own FAT driver results in double-caching (the guest caches sectors in its RAM, and Retro-Go/FatFs caches them again), destroying performance.

To circumvent this, we will use **High-Level Emulation (HLE) for disk I/O**.

### Implementation Strategy
1. **Intercept DOS Interrupts**: We will hook INT 21h (DOS API) at the emulator level before the x86 CPU executes the interrupt.
2. **Translate API Calls**: When the guest requests a file open/read/write, we translate the INT 21h registers into direct `f_open`, `f_read`, `f_write` calls to the host's **FatFs** library.
3. **Bypass Guest OS Layers**: The guest OS's block layer and FAT driver are entirely bypassed. Data is DMA'd directly from the SD card into the guest's memory buffer in AXI RAM.

### Backend Pagefile Priority
Because guest disk I/O is routed through our HLE layer, we can strictly control SD card bandwidth. 
We will implement an I/O arbiter that prioritizes reads/writes to our **Backend Pagefile** (used for COW memory swapping) over standard guest file I/O. This ensures that memory page faults are resolved with minimum latency, preventing CPU starvation.

## 4. Speaker and Sound Blaster

### 4a. What our titles ACTUALLY probe — measured, not assumed

`probe_report.py --title …` with `X86_PROBE=1`, gwemu, one 22–28 s startup
window per title. This re-orders the work, so it comes first:

| title | 0x22x (SB DSP) | 0x388/0x389 (AdLib) |
|---|---|---|
| PRINCE_OF_PERSIA | **the full base scan**: `OUT 0x226` ×3, `IN 0x22E` ×32, `IN 0x22A` ×33, then 0x240, 0x280, 0x2C0 in turn | `OUT 0x220/0x221/0x228/0x229`, `IN 0x228` ×94 |
| WOLF3D | **nothing** | `IN 0x388` ×348, `OUT 0x388/0x389` ×6 each |
| KEEN4 | **nothing** | the same, ×348 / ×6 / ×6 |
| ALLEYCAT, SIMCITY, TOPBENCH | nothing | nothing |
| EPIC_PINBALL, JAZZ, TYRIAN, MORTAL_KOMBAT | nothing — they die first | nothing |

Two conclusions worth keeping. **Only one title in the library reaches an SB
probe at all**, so SB is not the blocker for anything except Prince of Persia;
and **the AdLib timer test is the more common ask by title count**. Epic
Pinball's `SOUNDCRD.INF` (`REAL16 MAS ISBSERIES MDRV004R.MUS`) is an
EXPECTATION, not evidence — the title stops at its EXEC and never gets there.

348 status reads and 32 data-available polls are what a detect loop looks like
when the port floats: it runs to its bound every time.

### 4b. What is modelled (x86_io.c)

**Detection**, unchanged from the first pass:

* **OPL2 timers.** 0x388 status / 0x389 data, aliased at 0x220/0x221 and
  0x228/0x229 as on a real SB. Timers 1 and 2 expire in guest time (`x86_pit_ticks`,
  80 us and 320 us per step), the flags are sticky until a register-4 IRQ reset,
  and the status low bits read `0x06` -- the YM3812 answer. **The FM register file
  is deliberately NOT stored**: it is write-only on the real part.
* **SB DSP reset handshake.** 0x226 write-1-then-0 puts `0xAA` in the read
  buffer; 0x22A reads it, 0x22E reports data-available, 0x22C reports
  write-ready. **ONE card at ONE base** (0x220), so a scanning title finds one.

**And digitised PLAYBACK**, which is what turns that into sound:

* **An 8237A pair.** DMA1 at 0x00-0x0F, DMA2 at 0xC0-0xDF (stride 2), page
  latches at 0x80-0x8F with the NON-SEQUENTIAL channel map (`0x87/0x83/0x81/
  0x82`, `0x8F/0x8B/0x89/0x8A`). Address and count latches behind a PER-CHIP
  lo/hi flip-flop, mask/mode/master-clear, auto-init reload, and a status
  register whose TC bits clear on read. **Channel 5 counts WORDS**: the physical
  address is `(page & 0xFE) << 16 | addr << 1`.
* **IRQ 5 through the 8259.** `x86_sb_raise_irq()` is called from the frame loop
  beside `x86_kbd_raise_irq()` and obeys the same one-deep-slot contract. **The
  IMR is enforced for IRQ 5 and only for IRQ 5** -- the boot mask 0xB8 masks it,
  so a title that never installs a handler is never given the interrupt. The
  in-service bit is cleared by the EOI at 0x20; the DSP's own line is cleared by
  a READ of 0x22E (8-bit) or 0x22F (16-bit). **Both acknowledgements are
  required and a guest that does one gets exactly one interrupt.**
* **DSP 4.05 and the SB16 command set.** `E1h` answers 4.05, and that identity is
  a promise the rest of the file keeps: `0x40`/`0x41` rate, `0x14`/`0x15`
  single-cycle, `0x1C`/`0x90` auto-init, `0x48` block size, `0xD0`/`0xD4`/
  `0xD5`/`0xD6` pause-continue, `0xD9`/`0xDA` exit-auto-init, and the whole
  `0xB0`-`0xCF` block (16-/8-bit, auto-init, signed, stereo, length in SAMPLE
  FRAMES minus one, THREE argument bytes). Claiming 4.05 with a 2.x command
  table is the specific way this device desynchronises a driver.
* **The CT1745 mixer** at 0x224/0x225. A register file, plus three that are not:
  0x80 IRQ select, 0x81 DMA select (**honoured** -- a driver may move the
  channel), and 0x82 IRQ status, read-only and derived from the live line.
* **`BLASTER=A220 I5 D1 H5 T6`** in the environment. Every field is checkable
  against `x86_io.c`.

**THE BLOCK BOUNDARY IS WHERE THE INTERRUPT COMES FROM, NOT THE DMA'S TC.**
They are separate on the hardware and the difference is not academic: a
double-buffering driver programs the 8237 for the whole buffer and the DSP for
half of it, and takes an interrupt at each half. Driving the IRQ off terminal
count gives it one interrupt per two blocks and it plays the same stale half
for ever.

**ADPCM RUNS BUT IS SILENT.** `0x16`/`0x17`/`0x74`-`0x77`/`0x1F`/`0x2C`/`0x7D`/
`0x7F` drain the DMA, complete the block and raise the interrupt, and emit
silence, counted in `x86_sb_adpcm_silenced`. Refusing them mid-transfer would
reinstate the hang; decoding them is separate, gradeable work.

### 4b-2. The engine has ONE clock, and it is the frame loop

`x86_sb_render_mix()` is called once per rendered frame from the audio phase in
`src/main.c`, and **pulling the bytes it needs for the samples it emits IS the
transfer**. There is no per-instruction DMA tick anywhere. That is both the
cheap arrangement -- a VGA `OUT` already measures 1,242 cycles on this hardware
and an engine that ran per instruction would dwarf it -- and the physically
honest one, because on real hardware the transfer advances at the sample clock.

The consequence to know: **a transfer only advances while audio is being
rendered.** A harness that never renders sees a transfer that never moves. That
is a property of the design, not a defect, and `tests/main.c`'s playback arm
reproduces the frame loop in miniature for exactly that reason.

**Resampling** is a zero-order hold on a 16.16 accumulator: one add and one
compare per output sample, a source fetch only when it crosses, and the fraction
carried ACROSS frame boundaries so the pitch is the guest's rate and not that
rate rounded to a frame. Linear interpolation is one multiply-add per output
sample (800 of them per frame, in host C) and is a one-line change if the
aliasing is judged audible on silicon.

**The source walk respects the granule seam.** A host pointer is good only for
the 4 KB page it was resolved in, so the walk caches one granule and re-resolves
at each boundary -- measured at **one `x86_guest_ptr()` call per render call**
(`x86_sb_dma_maps`), not one per byte. The window is dropped at the end of every
render, because between two calls the pager may move any page.

### 4c. The counters, and what each one separates

| counter | the question it answers |
|---|---|
| `x86_sb_starts` vs `x86_sb_samples` | "programmed a transfer" vs "played one" |
| `x86_sb_irqs` vs `_delivered` | "the card asked" vs "the guest was given it" |
| `_irqs_masked` / `_irqs_lost` | refused by the IMR / by the one-deep slot |
| `_irqs_acked` / `x86_sb_eois` | the DSP ack / the 8259 ack -- BOTH are needed |
| `x86_sb_dma_bytes` | bytes actually pulled out of guest RAM |
| `x86_sb_underruns` | the source ran dry mid-render |
| `x86_sb_adpcm_silenced` | a block that ran but could not be decoded |
| `x86_sb_dma_refused` | **narrowed**: only what this model CANNOT run (record, direct DAC). It is no longer "asked for audio and got silence" |
| `x86_dos_env_seg` | whether `BLASTER` was reachable at all |
| `x86_opl_t1_gap` vs 96 | **the AdLib probe's delay, in ticks.** Under 96 means the guest's wait was too short for our clock, not that the card refused -- 4e |
| `x86_io_isa_ticks` | ISA bus time charged to FM-port accesses (0 = the negative control is built) |

All of them are in `probe_report.py`'s health table.

### 4d. Measured at tier 2 (gwemu), and one claim in 4c above was DISPROVED

| title | before | after |
|---|---|---|
| **EPIC_PINBALL** | reached mode 13h and **spun on `IN 0x22A` 64,000 times** | **the spin is GONE: 6 reads.** `sb_resets=2`, `sb_cmds=5`, **`cmds_unknown=0`**, `dma_refused=0`, and it programs the MIXER (10 writes / 5 reads of 0x224/0x225) and a rate of **22,050 Hz**. It now dies EARLIER in instruction count (483,354 vs 620,904) on `guest_exit_reason=4` `exit_info=0x0F` -- **an unimplemented opcode, not a sound defect** |
| **PRINCE_OF_PERSIA** | one base found, no transfer | unchanged and healthy: `sb_resets=2`, `sb_cmds=4`, `cmds_unknown=1`, still running at 233 M instructions. Its digitised effects are in-game and the timeline stops at the title screen |
| **WOLF3D** | golden 0.000% | golden **0.000%**, and see the retraction below |

**RETRACTED: "Wolf3D reads BLASTER to find the card and does not probe 0x22x
without it."** That was a hypothesis and it is now disproved. `BLASTER=A220 I5
D1 H5 T6` is present, well-formed and reachable (`x86_dos_env_seg=113`, block
dumped out of guest memory), and **WOLF3D still touches no 0x22x port at all**.
What it does instead is the AdLib timer probe at 0x388 (348 reads), which
concludes "no card" under gwemu for the clock reason in 4e -- so it never
reaches its Sound Blaster path. The prediction that follows is a **tier-3**
one: on silicon, where the OPL timer does elapse, it should get there.

The safe side of that result is that setting `BLASTER` left WOLF3D bit-identical
-- the golden is 0.000% with it set.

### 4d-2. What it cost

* **RAM_EMU +2,816 B**, `__CORE_BSS_END__` 0x2405FC30 -> 0x24060718, of which
  only ~280 B is data; the rest is CODE. **This costs ONE guest page frame**:
  `guest_ram_pages` 159 -> **158**, `guest_ram_pages_short` 1 -> **2**
  (measured at tier 2 on a clean `X86_PROBE=0` build). That is the honest price
  and it is the RAM_EMU cliff in STATUS.md, paid deliberately rather than
  tripped over.
* **ITCM unchanged** at `__CORE_ITCM_CODE_END__` = **0xD2D8**. Nothing here is
  on the interpreter's hot path.
* **No per-instruction cost at all.** The only new work on the guest's path is
  the port decode, and the 8237 and page latches are decoded in the FALL-THROUGH
  arm of the switch so the hot ports (0x3DA, 0x60, 0x40) pay nothing for them.
  The render early-outs on `sbp.active` when nothing is playing.
* **NOT MEASURED: cycles on silicon.** gwemu must not be trusted for timing
  (CLAUDE.md), so the per-frame cost of the render is a tier-3 question and is
  stated here as unmeasured rather than estimated.

### 4e. SOLVED: the delay loop is calibrated in BUS CYCLES, not instructions

**This was "our CPU is too fast for an 80 us timer" and the arithmetic offered
for it was wrong.** The prediction was that silicon's ~2.8 M insn/s would
elapse ~130 of the 96 ticks the timer needs where gwemu's ~11.9 M elapsed only
30-70. It was finally MEASURED ON THE DEVICE and it read
`x86_opl_timer_expiry=0` there too -- same 348 status reads, same 6 FM writes,
same "no card". gwemu was never the variable.

**What the guest is actually doing.** Wolfenstein 3D's `SDL_DetectAdLib()`
starts timer 1 at preset FFh (80 us, 96 PIT ticks) and then waits by READING
THE FM STATUS PORT: id's `alOut()` spends 6 reads after the address byte and 35
after the data byte, and the detect adds a loop of 100 on top -- ~135 accesses,
which is exactly the 348 status reads and 6 register writes the counters show.
On a real machine an 8-bit ISA I/O cycle is ~1 us **whatever the CPU is**, so
that loop is ~135 us and the flag is up. That is why the code is written as
port reads and not as a counted NOP loop: it is calibrated in BUS cycles.

Our clock converted those 135 accesses at the INSTRUCTION rate, so they bought
24 us. The instrument that says so is `x86_opl_t1_gap`, the longest wait ever
observed for timer 1: **56 ticks against the 96 needed**, on hardware.

**The fix is to charge the bus.** `x86_io.c` gives the FM card its own clock --
machine ticks plus one PIT tick per access to an FM port (`X86_ISA_IO_TICKS`,
`x86_io_isa_ticks`). Measured on silicon, WOLF3D:

| | `X86_ISA_IO_TICKS=0` | `=1` (shipping) |
|---|---|---|
| `x86_opl_t1_gap` / needed | 56 / 96 | **200** / 96 |
| `x86_opl_timer_expiry` | 0 | **1** |
| `x86_opl_writes` | 6 | **253** -- the detect's 245-register zero pass |
| `x86_sb_resets` / `_cmds` | 0 / 0 | **1 / 2**, `cmds_unknown=0` |

The last row is the owner's complaint. **WOLF3D's Sound Blaster probe is
DOWNSTREAM of the AdLib conclusion**, so "it never touches 0x22x" was never an
SB defect at all; the card, the `BLASTER` string and the PSP link were all
correct and all irrelevant while the OPL timer said "no card".

**Why the OPL's clock and not the machine's.** Charging every port access into
`pit_now` is the more general model and it was built and measured first: it is
physically right and costs nothing off the I/O path, but it moves the machine's
clock by **+11%** (1.16 M bus ticks against 10.4 M instruction ticks over 48 M
instructions on WOLF3D) because a retrace spin on 0x3DA really does cost bus
time. An 11% faster clock retimes every title's intro -- the WOLF3D golden
landed on the title screen instead of the credits -- so it is a machine-pacing
change to be graded as one, not a rider on a sound fix. It is left as the
obvious next step, not as this change.

**The golden moved anyway, for a different and correct reason.** With sound
detected, WOLF3D takes its sound-present startup path and its intro sequence is
further along at the golden's fixed 250 M-instruction capture point. `WOLF3D.png`
was re-blessed to the title screen with that stated, after `repeat --runs 3`
proved three identical frames. **Do NOT read the re-bless as "the picture
changed": the picture is the same machine one screen later.**

**Two candidates were tested first and both are innocent**, which is worth
keeping because they are the cheap ones anyone would reach for:

* **PSP:002C.** Read out of guest memory ON HARDWARE, not inferred: the root
  PSP is at linear 0x770, `[0x79C]` = 0x0071, and 0x71 is `x86_dos_env_seg`;
  the block at 0x710 holds `PATH=`, `COMSPEC=` and `BLASTER=A220 I5 D1 H5 T6`
  followed by the `01 00` count and `C:\WOLF3D.EXE`. The link is sound, and the
  guest's own Borland startup at 0x870 is visibly `8B 1E 2C 00`, `mov bx,[2Ch]`.
* **The title's stored configuration.** The shipped image is Wolf3D SHAREWARE
  (`WL1`, not `WL6`), and `CONFIG.WL1` is present, 522 B. Its three mode words
  after the seven high-score records read `sd=2` (`sdm_AdLib`), `sm=1`
  (`smm_AdLib`) and **`sds=3` (`sds_SoundBlaster`)** -- the title ASKS for
  Sound Blaster digitised sound. `ReadConfig()` downgrades `sds` to `sds_Off`
  when `SoundBlasterPresent` is false, which is what was happening.

**WOLF3D still does not start a transfer, and the reason is now NAMED rather
than suspected.** `x86_sb_cmd_hist_lo`/`_hi` record the last eight DSP command
bytes, and across a full gameplay run WOLF3D's ENTIRE conversation with the
card is **`D1 40`** -- speaker on, then a time constant that comes back out as
`x86_sb_rate` = **7042 Hz**, i.e. `256 - 1000000/7000`, which is `SDL_StartSB()`
and its 7,000 Hz `SBSampleRate` exactly. And `x86_dma_writes` is **0**: the
8237 is never touched. So the driver is fully initialised and armed, and
`SDL_SBPlaySeg()` -- which programs channel 1 and issues `14h` -- is never
called. Nothing between the port and the speaker is implicated; the title never
asks. The remaining question is upstream of the card entirely (`DigiMap` /
`NumDigi`, i.e. `SDL_SetupDigi()` and the VSWAP sound pages), and it is a
WOLF3D question, not a Sound Blaster one.

### 4e-bis. `F2h` -- THE IRQ A DRIVER ASKS FOR, AND HOW LATE IT ARRIVED

**PRINCE OF PERSIA now plays digitised sound on silicon**, and the whole fix
was one command and its timing.

Its entire DSP conversation used to be `E0 E0 E1 F2` and then nothing, ever
again. `F2h` is "assert the 8-bit interrupt, with no transfer behind it" --
how a driver FINDS which vector the card is wired to. We answered `unknown`
(`x86_sb_cmds_unknown` = 1), so the line never rose, so PoP concluded the
card's interrupt was dead and fell back to the PC speaker.

Implementing it was not enough, and the second half is the interesting one.
`x86_sb_irqs_delivered` stayed **0** while `x86_sb_irqs_masked` climbed past
68,000: the assert was queued for the frame loop's retry at the next sub-slice
boundary, thousands of guest instructions away, and PoP had unmasked IRQ 5,
sent `F2h`, spun a short delay and RE-MASKED long before that. `x86_pic_imr_ever`
(every value written to 0x21, ANDed) reads **0x10**, which proves the unmask
happened -- without it, "never delivered" and "never unmasked" are the same
number.

So `F2h` is delivered AT THE PORT WRITE. One catch, and it is the reserved-
register contract: `x86_sb_raise_irq()` drops `pit_counter` to arm the next
instruction boundary only `if (cpu->eflags & 0x200)`, and **`cpu->eflags` is
stale inside a slice** -- IF lives in `R11`. The check that matters is
`.handle_pit_or_irq`'s own `TST R11, #(1<<9)`, so the boundary is armed
unconditionally and the interpreter decides; being wrong costs one early timer
exit, once per `F2h`.

MEASURED ON SILICON, PRINCE OF PERSIA, 36 s: `x86_sb_starts` **0 -> 18**,
`x86_sb_cmds` 4 -> 42 (`D1 D1 40 14`, then `40 14` repeating -- set rate,
single-cycle 8-bit playback), `x86_sb_samples` **92,981**, `x86_sb_underruns`
**0**, rates 8,264 and 11,111 Hz, and `x86_audio_peak_max` **991** where it had
been 0 for the card. `tests/test_sound.py` grades both halves and
`--negative-f2` / `--negative-f2-now` are the controls; the second leaves every
counter green and is caught only by the guest's ten-iteration window.

### 4e-ter. THE HOST CHAIN WAS NEVER THE BUG, AND IT WAS 12 dB TOO QUIET

`x86_audio_submits` / `_peak_last` / `_peak_max` / `_volume` (src/main.c) close
the half of this that had never been checked: one buffer submitted per frame,
the loudest sample in it, and the launcher's own volume. Measured at tier 2 on
stock master with WOLF3D: `submits` climbed in step with `frames_rendered`,
the firmware's `dma_counter` climbed with it -- and `peak_max` was **0**. The
chain runs; it was being handed silence. `X86_AUDIO_TONE=1` is the standalone
partition (a 440 Hz square straight into the half-buffer, every emulated source
bypassed).

What that instrument then caught is an amplitude bug worth 12 dB:

* `common_emu_sound_get_volume()` returns `volume_tbl[level]`, which is
  ALREADY a fraction of full scale, and the launcher's default level is 4 of 9
  = **31**. Scaling it again by `>> 11` gave 31/2048 = **1.5% of full scale**.
  tama's `main_tama.c` uses `>> 10` with the comment "adjust max volume to 25%
  as 100% is REALLY, REALLY loud" and is the core that documented the choice.
  Now `>> 10`.
* The Sound Blaster was then halved AGAIN, to keep a beep audible under the
  music. **There is no music** -- FM is not built (4f) -- so that was 6 dB paid
  for a collision that cannot happen. Gone.

Peak on silicon for the same PoP workload: **248 -> 991**.

### 4f. What is STILL not here: FM synthesis, and what it would take

Digitised playback is built (4b). **OPL FM tone generation is not**, and the
gap is visible: the OPL timers answer a detection probe and the register file is
deliberately not stored, so a title whose MUSIC is AdLib is silent while its
digitised effects play.

Sizing it, so nobody re-derives it: an OPL2 is nine channels of two operators,
and each operator is a phase accumulator, an exponential envelope generator in
four stages, a sine/waveform table lookup and a level multiply -- **per output
sample**, plus feedback and FM modulation routing between the pair. At the
48,000 Hz this core renders at, that is 18 operators x 48,000 = 864,000 operator
updates per second of audio, against ~2.8 M insn/s of TOTAL hardware budget of
which 29.4% is already ordinary guest opcodes. It does not fit at full rate.
What might: rendering FM at a lower internal rate and upsampling through the
same accumulator the DMA path already uses, and only while a channel's key is
on. That is a project with its own measurement, not an increment on this one.

The register file would also have to start being STORED, which 4b explains is
currently and correctly not the case.

**Also unmodelled and larger than everything sound-related combined:** WOLF3D
issues **2.3 M reads of the game port `IN 0x201`** (measured, tier 2), which
floats 0xFF -- read as "present but stuck". It is trivially answerable in
`x86_io.c` (buttons up, axis timers never firing) and is deliberately NOT done
here: it changes a 2.3-million-times-per-run path and belongs in a change that
grades it on its own.

Reference for the SB16 register set: the Linux driver,
https://raw.githubusercontent.com/torvalds/linux/refs/heads/master/sound/isa/sb/sb16.c
