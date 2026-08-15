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

### 4b. What is modelled (x86_io.c), and it is DETECTION ONLY

* **OPL2 timers.** 0x388 status / 0x389 data, aliased at 0x220/0x221 and
  0x228/0x229 as on a real SB. Timers 1 and 2 expire in guest time (`x86_pit_ticks`,
  80 µs and 320 µs per step), the flags are sticky until a register-4 IRQ reset,
  and the status low bits read `0x06` — the YM3812 answer, so an OPL2/OPL3 test
  concludes OPL2. **The FM register file is deliberately NOT stored**: it is
  write-only on the real part, so a guest cannot tell, and storing it would only
  imply a synthesiser that does not exist.
* **SB DSP reset handshake.** 0x226 write-1-then-0 puts `0xAA` in the read
  buffer; 0x22A reads it, 0x22E reports data-available in bit 7, 0x22C reports
  write-ready (bit 7 clear). Commands: `E1h` version (**2.01** — a plain SB 2.0,
  which claims no mixer and no SB16), `E0h` identify, `E4h`/`E8h` test register,
  `D1h`/`D3h`/`D8h` speaker, `E3h` answered with a bare NUL terminator so a
  copyright-string reader terminates instead of hanging.
* **ONE card at ONE base.** 0x240/0x280/0x2C0 stay absent, so a scanning title
  finds exactly one. Measured effect on Prince of Persia: the scan collapses to
  0x220 alone and the other three bases disappear from its trace entirely.

### 4c. What is ABSENT on purpose, and the counters that say so

The mixer (0x224/0x225), DMA and the IRQ are **not** modelled. We cannot deliver
a DMA-completion IRQ 5, so a transfer would start and never finish. The DSP
therefore *accepts* a transfer command's argument bytes — refusing them
mid-command desynchronises the command stream, which is worse — and counts them
in **`x86_sb_dma_refused`**. That counter is the honest reading of "a title asked
for digitised audio and got silence"; `x86_sb_cmds_unknown` is the same for a
command we accept and do nothing with.

**`BLASTER` is deliberately NOT in the environment.** `x86_dos.c` builds
`PATH=\0COMSPEC=C:\COMMAND.COM\0` and nothing else. Wolfenstein 3D reads BLASTER
to find the card and does not probe 0x22x without it — which is exactly what the
table above shows. Setting it would make WOLF3D start a DMA transfer and wait for
an IRQ that cannot arrive. **Adding BLASTER is the LAST step of sound support,
not the first.**

### 4d. Measured after the change (gwemu, tier 2)

| title | before | after |
|---|---|---|
| PRINCE_OF_PERSIA | 4 bases scanned, 33 reads of 0x22A, no card found | `x86_sb_resets=2`, `x86_sb_cmds=4`, **`x86_sb_dma_refused=0`**, one base, 296 M insns, no hang |
| WOLF3D | golden 0.000% | golden **0.000%**, 262 M insns |
| ALLEYCAT / KEEN4 | — | unchanged (0.874% pre-existing / known-bad) |

Cost: **+1,096 B of RAM_EMU** (`__CORE_BSS_END__` 0x2405EBF8 → 0x2405F040, both
clean `X86_PROBE=0` builds), ITCM **unchanged** at 0xD2D8, and the pager pool is
159 frames with `guest_ram_pages_short=1` **before and after** — no frame lost.

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

### 4f. Producing actual SAMPLES — what it would take, and it is not small

Not built, deliberately, and sized here so nobody re-derives it. Retro-go's ABI
(`audio_get_buffer_length` / `audio_get_active_buffer` / `audio_start_playing`,
`src/main.c`) is a **once-per-frame** render, not an ISR-fed ring, and
`x86_speaker_render()` already uses it for the PC speaker. Digitised SB output
therefore needs, in order: a DMA controller model (8237 channel 1: page, address
and count registers on ports 0x00-0x0F and 0x81-0x8F, none of which exist today),
an IRQ 5 raised at transfer completion and acknowledged through the 8259 we
currently do not enforce, a sample-rate conversion from the DSP time constant to
the panel's frame-quantised buffer, and — the part that decides the whole thing —
a per-frame budget on a machine where 29.4% of wall is already ordinary opcodes
at ~57 cyc/insn. OPL FM synthesis is a second, larger project again (nine
channels of two operators each, per sample). **Neither belongs on this hardware
before the interpreter's cost comes down.**

Reference for the SB16 register set: the Linux driver,
https://raw.githubusercontent.com/torvalds/linux/refs/heads/master/sound/isa/sb/sb16.c
