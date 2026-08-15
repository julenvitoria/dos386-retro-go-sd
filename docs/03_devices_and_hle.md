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

### 4e. THE OPEN PROBLEM: our CPU is too fast for an 80 µs timer

**WOLF3D and KEEN4's AdLib probe still concludes "no card" under gwemu**
(`x86_opl_timer_expiry=0` across a whole run, measured on the shipping core).
The card is answering; the guest's *delay* is too short. Timer 1 at preset FFh
needs 96 PIT ticks, and guest time is instructions ÷ `x86_pit_kips`: under gwemu
the guest runs at ~11.9 M insn/s, so the few hundred instructions of Wolf3D's
delay loop — a loop calibrated for a 286 — elapse only ~30–70 ticks. At silicon's
~2.8 M insn/s the same loop elapses ~130 and the flag *should* come up. **That is
arithmetic, not a measurement: it is a tier-3 check nobody has run.** Do not
"fix" it by shortening the OPL period — the period is right and the clock model
is the subject. This is the same three-clock question as `docs/09`.

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
