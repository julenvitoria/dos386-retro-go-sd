# Status

Last verified 2026-08-13. How to run: [`TOOLING.md`](TOOLING.md). Roadmap: [`PLAN.md`](PLAN.md).

## Verification state

| Tier | State |
|---|---|
| 1. gwemu bare-metal | 268/268 `test_opcodes.py`, plus `test_video`/`test_text`/`test_dos`/`test_keyboard`/`test_osk`/`test_screen`/`test_disk_cache`; `test_audio` runs natively, no target |
| 2. gwemu + retro-go | `core_dtcm_ok=1`, bonus pool 153,600 B, 1,000,001 insns — matches tier 1 |
| 3. Hardware | 50.50 cyc/insn, 5.54 MIPS |

Cycles/insn: 57.50 pre-fold → 50.00 fold + absolute dispatch → 50.50 with
segmentation, unchanged through the 66/67 axes, segment loads and REP.

## Phase 0/1 — baseline and CPU foundations

Complete except below.

- [ ] `XLAT` (no segment base), `WAIT`, `BOUND`/`ARPL`  (`INS`/`OUTS` done)
- [ ] Five handlers call `DECODE_MODRM` by symbol, so they ignore a `0x67` prefix
- [ ] Limit checks off the R6 descriptor; lazy PF/AF; software TLB above 16 MB
- [ ] Size-aware `MEM_R*`/`MEM_W*` macros + CI grep banning raw `[R9,`
- [ ] Assertions in `test_game.py`/`test_benchmark.py`; realign `docs/01`–`03`, `05`


## Phase 2 — Alley Cat

13 opcode slots still reach `.exit_loop`: x87 (`D8`-`DF`), `BOUND`/`ARPL`,
`XLAT`, `WAIT`, `INT1`. `INS`/`OUTS` (6C-6F) are implemented.

Done: real-mode interrupts and `IRET`; the `0F 0B` hypercall and its HLE service
table; BIOS INT 10h/16h/1Ah over a real BDA; CGA text mode 3 and a mode 4 LUT8
blitter, both pixel-exact; CGA registers with a moving retrace bit; a 8042
keyboard with IRQ 1; PC speaker on PIT ch2; port I/O; the guest page pool and
fold mapping; the frame-paced loop; and the DOS layer.

- [ ] DWT per-opcode profile: tool done (`profile_opcodes.py`, `X86_PROFILE=1`),
      clean under gwemu, silicon pending; gate NOR backing on code-vs-data

## "It just becomes a bunch of pixels" — which was not a mode switch at all

The device report was that Alley Cat does not switch from CGA back to text.
**Measured under gwemu with a new event ring (`x86_video_mode_log`), and the
report's diagnosis does not survive the measurement.** The ring holds three
events for the whole run: the BIOS setting mode 3 at boot, the guest setting
**mode 4 through INT 10h**, and the renderer following it into graphics. Zero
writes to port 0x3D8, ever. Our mode byte, the BDA at 0040:0049 and the renderer
agreed at every sample — **the mode switch was never wrong**.

What was wrong: the guest prints "Do you want to use a joystick (Y/N)?" **while
in mode 4**, and every character went through `x86_video_teletype()`, which
stored a char/attr PAIR into 0xB8000. In mode 4 those two bytes are eight
pixels. 36 characters became 288 pixels of confetti on guest scanline 0 and
nothing else — a screen of pixels, exactly as reported. A real BIOS rasterises
the glyph as an 8x8 bitmap in a graphics mode; the previous DOS core hit this on
the same title and its notes name the same prompt.

`gfx_putchar()` now rasterises into the two-bank interleave (2 bpp in mode 4/5,
1 bpp in mode 6), and **every** character path is routed through it —
teletype, AH=09h/0Ah, and the scroll, which in a graphics mode moves eight
SCANLINES per text row rather than 160 bytes of cells. The previous port scoped
its fix to AH=0Eh and carries the other three as an open defect; ours are three
C functions over one cursor, so covering them cost nothing. Colour 0 means
*visible* (DOS's console never sets one), which is the second half of that fix.

Ported with it, from `dos_video.c` and `references/8086tiny/docs/video/`:

- **The BDA is authoritative and re-read every frame** — 0040:0049 mode,
  0040:004A columns — so a guest that sets its mode by any route is followed.
  0x3D8 is never consulted for graphics-vs-text; that rule was tried in the
  previous port and latched the renderer into decoding text as 2 bpp.
  Two guards make it safe here: the page may be unbacked on silicon, and a blank
  page reads mode 0 / cols 0, so the COLUMNS must also be 40 or 80 or the byte
  is ignored and the last good mode stays.
- **The column count selects the font** — 80 → 4x8, 40 → 8x8, both exactly
  320 px — which is what makes **modes 0/1 (40x25)** cost nothing but code.
- **Mode 6** (640x200x1), decimated 2:1 by OR-ing pixel pairs so one-pixel rules
  survive. **Mode 7** folds to 3. Mode 5 takes the colour-burst palette from its
  MODE NUMBER, not from a 0x3D8 bit a guest need never write.
- **One CLUT layout for every mode**: 0-15 the fixed CGA palette, 16-19 the four
  selected graphics colours, 20-31 a mirror. A mode change now moves four
  entries instead of rewriting the table in both directions, and index 0 is
  black in every mode rather than the guest's chosen background.
- A mode nothing renders is **refused** (logged, previous mode stays up) rather
  than adopted, so "the guest draws into memory nothing reads" is never silent.

Cost: **+1,120 bytes** of core (`__CORE_BSS_END__` 0x240574a0 → 0x24057900),
leaving **1,792 bytes** of RAM_EMU headroom. New data is 64 bytes (the mode 6
nibble table) plus the 68-byte event ring; everything else is code. The 8x8 font
was already linked for the guest's F000:F000 copy, so the rasteriser and the
40-column renderer add no font data.

**Left out, each for a measured reason, not for effort:** VBE (76,800 B); EGA 0Dh/0Eh/10h/12h (planar writes
need a store-side hook, and our VRAM is deliberately plain mapped RAM with no
MMIO path, plus 32-153 KB); Hercules (720x348 needs real resampling). The CRTC
start address at 0040:004D is also not honoured — we do not model CRTC 12/13 —
so a title that hardware-scrolls by moving the start register will look static.

Tier 1: `tests/test_video.py` now drives 3 → 4 → 3 from GUEST code through
INT 10h and compares the rendering at each step, checks the graphics-mode glyph
against a host-side raster of the same font, and covers mode 6, mode 0 and the
refusal path. Tier 2: the prompt and Alley Cat's skill-level menu are readable
(`build/artifacts/ac_after_t12.png`, `ac_after_game.png`) where they were one
coloured line before (`ac_t12.png`); no faults, 1,904 frames, no refused modes.

## VGA mode 13h — and the firmware ABI change that made it possible

Mode 13h was listed above as **undisplayable**, and that was right: 256 CLUT
entries against a `GW_LCD_SET_CLUT` that clamps at 32. The clamp is not
timidity — `lcd_set_clut()` writes a 40%-darkened TWIN of entry `i` at
`count+i` so retro-go's overlay can dim by OR-ing `LCD_DARKEN_BIT` (0x20), and
256 entries would need **512 hardware slots against the LTDC's 256**. Raising
the cap is impossible, not merely expensive.

So the FIRMWARE changed (branch `external_cores`): `lcd_set_clut_ext()` takes a
**caller-owned** palette of up to 256 entries and bypasses the twin cache, at a
cost of 4 bytes of firmware state instead of ~1.8 KB of its DTCM. Exposed as
one appended ABI op, **`GW_LCD_SET_CLUT_EXT = 11`** — append-only, so existing
cores are unaffected, and no version bump (v2 is unpublished and already
appends freely). The `gw_lcd.c`/`gw_lcd.h` half is the user's own commit
`ba73090b6`, hand-applied; its DOS-core halves were left behind.

**Accepted and not solved: `LCD_DARKEN_BIT` stops working while a 256-colour
mode is up.** OR-ing 0x20 under a full-range palette picks an unrelated colour,
so retro-go's menu backdrop SHIFTS COLOUR instead of dimming. The menu box and
its text are drawn after the darken pass and stay readable, and
`lcd_pack_color()` searches the 256 entries so menu colours still resolve to a
real nearest match. `gnw-doom` tried to fix this with a bit-5-invariant
palette; it collapses the palette to 128 and mangles half the colours. Do not
repeat it. Full detail: [`docs/08_vga_mode13h.md`](docs/08_vga_mode13h.md).

The core side is small because the mode is: 320x200x256 LINEAR at 0xA0000 is
byte-for-byte our LUT8 framebuffer, so the blit is 200 `memcpy`s and
**nothing is added to the store path**. What is not small is the DAC — ports
0x3C6-0x3C9, with `0x3C9` modelled as a STREAM and not a latch. A guest writes
the index once and then 768 consecutive bytes; as a latch, 767 are lost and the
screen comes out one colour **while every pixel index stays perfect**, which is
a failure a blit test cannot see. `INT 10h AH=10h` routes into the same DAC, so
there is one palette model rather than two that can disagree.

Tier 1, `tests/test_video.py`: 13h adopted with the BDA agreeing (0x449=0x13,
0x44A=40); the palette streamed through the real ports and read back 6-bit
exact through 0x3C7/0x3C9; **all 256 CLUT entries** exact against a host
expansion (`(v<<2)|(v>>4)`, so 63→255 and not 252); **all 64,000 pixels** exact
against a host image of bars over diagonals — a stride error slants the bars
and re-slopes the diagonals, and one wrong stride cannot satisfy both; the
letterbox untouched; and the 256-entry table WITHDRAWN on leaving 13h, without
which the firmware keeps a core-owned palette the overlay then draws through.
The old "13h is refused" arm now uses mode 0x11, or it would test nothing.

**WOLF3D now reaches video: mode 13h adopted, two full 768-byte palettes
streamed through the DAC. It does not yet render a picture -- it still walks
off later -- but the four walls in front of it were measured and removed, and
none of them was a video defect.**

The previous reading was that `x86_dos_file_log` held 32 slots of `AH=42h`
returning CF=1 "invalid handle" after a FindFirst for `*.WL1`. **That log was
the wrong shape for the question.** A 32-slot WRAPPING ring is overwritten by a
loop body in milliseconds, so the call that ISSUED the handle was always gone,
and two words per slot could not say WHICH handle. It is now **96 slots,
SATURATING, four words** (adding `BX` and `CX:DX`) with a decoder in
`probe.py --dos-log`. The whole prologue then fits, and the answer was in
**entry 2, not in the 81 identical entries after it**:

```
[0] 2Fh get DTA    ok        [2] 4Eh findfirst "*.WL1"  -> CF=1 AX=0x12 no more files
[1] 1Ah set DTA    ok        [3] 1Ah set DTA (restore)  -> ok
```

Every one of Wolf3D's eight `.WL1` files IS on the image (checked host-side --
and the image is an **MBR-partitioned FAT16**, not a bare FAT12 floppy, which
FatFs mounts correctly). Four defects, each with a failing tier-1 test written
first:

1. **A DTA that straddles a 4 KB granule made the disk look EMPTY.**
   `x86_guest_ptr()` refuses a span that crosses a granule and returns NULL;
   `dos_find_report()` asked it for all 43 bytes of the result record at once
   and treated the refusal as "no match", which the caller cannot tell from
   "the directory ended". Wolf3D's DTA is at **DS:0xFFBA**. It goes through
   `x86_guest_copy_in()` now, which walks granules. **The same defect class was
   live in two more places** and is fixed in both: `psp_build()` wrote 256
   bytes at an arbitrary PARAGRAPH, and INT 10h AH=10h AL=12h/17h moved up to
   **768** bytes of palette, which can never fit in one granule -- so a
   256-colour game installing its whole palette in one BIOS call had it
   silently dropped.
2. **`INS`/`OUTS` (6C-6F) did not exist.** Wolf3D's `VL_SetPalette` is
   literally `mov cx,768 / rep outsb` to 0x3C9, so an entire 256-colour palette
   went through one missing opcode: `guest_halted=1`, `exit_reason=4`,
   `x86_vga_dac_writes=0`, black screen -- which reads exactly like a video bug
   and is not one. Implemented for both widths with REP support (the prefix's
   opcode range now covers 6C-6F with no ZF test; without that arm `REP OUTSB`
   would have run ONE byte and dropped 767, which is strictly worse than the
   missing opcode). They are the same opcodes TOPBENCH halts on.
   R2 is the mode record and `x86_port_*` are C calls that clobber it, so R4
   carries it across and every index advance happens before the call.
3. **`AH=42h` was the one file call that never consulted `dos_handle_device()`.**
   A seek on handle 0/1/2 is a seek on a character device; DOS SUCCEEDS with
   DX:AX = 0. We returned "invalid handle", and a program that treats that as
   retryable never leaves the retry.
4. **There was no VGA register FILE at all** -- CRTC, Sequencer, Graphics
   Controller, Attribute. Writes vanished, reads returned the 0xFF no-device
   default. That is not a missing feature a guest degrades around: **the way a
   DOS program finds its display is to write a register and read it back**, and
   `x86_io_log` (new; see `TOOLING.md`) showed Wolf3D doing exactly
   `OUT 3D4,0F / OUT 3D5,66 / IN 3D5` and then repeating it against the mono
   alias. Both read 0xFF. It is a faithful STORE now, deliberately not an
   emulation -- unchained addressing, the start address at CRTC 0x0C/0x0D and
   split-screen still do nothing, but they do nothing VISIBLY, with values a
   probe can read.

   **The first version of that fix overshot, and the port log caught it.**
   Answering the 0x3B4/0x3B5 mono alias as well made Wolf3D conclude it had an
   MDA and poll 0x3BA **32,000 times** waiting for a retrace on a card that is
   not there. Misc Output bit 0 is the I/O address select and a card decodes
   ONE pair; the silence of the other pair is information. Gated, port accesses
   went 35,165 -> 2,370.

**Where it stops now, measured:** `guest_halted=1`, `exit_reason=4`,
`exit_info=0xFF` at CS=SS=DS=0x3804 -- executing 0xFF filler inside its own
heap, i.e. a wild far transfer. Immediately before it there are **81 identical
`AH=42h` calls with identical registers** (`BX=0`, `CX:DX=0x0000FC00`), which
is a loop of already-corrupted code, not a service we are answering wrongly --
all 81 now succeed and the outcome is unchanged. The port log ends cleanly with
the CRTC probe completing correctly, so the divergence is in pure computation
after it. Naming it needs a reference trace, and `refdos.py` has no port I/O so
it cannot follow Wolf3D past its first VGA access. That is the next piece of
tooling, not the next guess.

RAM_EMU headroom **30,728 -> 28,288 B** across all of it. Tier 1 all green:
`test_opcodes` **273/273** (+5 INS/OUTS cases), `test_dos` (+ a split-granule
DTA arm and a device-handle seek), `test_video` (+ the register-file probe,
including the mono alias staying silent), `test_text`, `test_keyboard`,
`test_osk`, `test_audio`, `test_disk_cache`, `test_nor`.

**Mode Y (unchained 13h), which is what Wolf3D actually uses, is scoped and NOT
built.** The blit would be nearly free — at the standard geometry the correct
de-interleave IS the linear read. Three other things are not: a store-side hook
on every VRAM write (our VRAM is deliberately plain mapped RAM with no MMIO
path), a Sequencer + CRTC register file (we model neither, and CRTC 0x0C/0x0D
is an existing open gap), and page storage that does not exist — a Mode Y page
is still 64,000 B and two real pages are 195,072 B against 45,056 B free.
Three subsystems, correctly separate work.

## Where memory lives now — and a belief that was wrong

`src/main.c` kept VRAM out of the LCD bonus pool because the pool "is
uncached". **Read out of the firmware source, it is not.**
`mpu_set_lcd_pool_uncached_range()` covers exactly the framebuffer footprint
(154 KB in LUT8) and its own comment says everything past it "defaults to
Normal cacheable, which is what we want for the LCD bonus area". The bonus area
is cached AXI, the same class as RAM_EMU. That mattered: emulated VRAM behind a
strongly-ordered region costs ~5.4-6.5 cycles per guest BYTE store, 4x the blit
it saves.

Three things moved.

1. **The .dsk sector cache → the AHB heap (`GW_MEM_AHB`)**, not for room but on
   principle: it is a disk staging buffer, AHB is D2 SRAM and D2 is the only
   SRAM DMA1/DMA2 can reach, so it is where it must live if SPI1's polled
   transfer ever becomes a DMA one. It is also the **only ABI pool with a real
   `free()`**. Measured on target: `0x30005DF8` (heap is
   `0x30005DC0`-`0x3001E000`), `x86_disk_cache_ok=1`, and TOPBENCH's
   **115 guest reads → 7 backend calls is unchanged by the move**. Its free
   space is the one number that cannot be read off a link map — the heap is
   shared with the firmware's own allocations — so `disk_cache_mem` publishes
   the pointer.
2. **The 13h aperture → the 64 KB the cache vacated**, at bonus+8 K.
3. **CGA VRAM (32 KB) → the bonus pool**, which is what saved the build:
   13h cost 2,896 B of core TEXT and took RAM_EMU headroom from +872 to
   **-2,024 B** — `guest_ram` returns NULL, `core_ready` stays 0, no message.
   After the move, **+30,728 B**.

The pool map is now `BONUS_*_OFF` in one place instead of open-coded
`4096`/`8192` at four call sites: filler 4 K, BIOS page 4 K, aperture 64 K,
13h palette 2 K, VRAM 32 K = **108,544 used of 153,600**. The 45,056 B free is
deliberately left contiguous at the TOP, for a VRAM arena that can balloon for
an EGA mode and shrink again.

**The cache geometry is a build-time tunable now** (`X86_DISK_WINDOW_SECTORS`,
`X86_DISK_WINDOW_COUNT`), and `tests/test_disk_cache.py --sweep` measures it.
The stream win is bought **entirely by window SIZE, not by count** — 4x16 KB
and 2x16 KB both serve 248 single-sector reads in 9 backend calls, while 8 KB
windows need 16 and 4 KB need 32. The COUNT buys residency: at 2 windows the
test's own metadata-vs-stream arm fails, because the stream evicts the metadata
window — which is the property the set exists for, failing exactly as designed.
Nothing forces a size trade any more, so it stays at 4 x 16 KB.

Tier 2 after all of it: Alley Cat renders its mode 4 screen in full
(`build/artifacts/alleycat_after_vram_move.png`), `core_ready=1`,
`vga_aperture_ok=1`, `x86_vga13_ok=1`, bonus pool 153,600, 1,000,001 insns,
~60 fps.

## On-screen keyboard — shipped; mouse UI — shell only

`x86_osk.c`, wired from `poll_input()`/`core_repaint()` in `src/main.c`. The UI
is a port of the author's previous DOS port; the injection under it is ours —
XT make/break codes through `x86_kbd_scancode()` plus IRQ 1. **GAME cycles
OFF -> keyboard -> mouse -> OFF**, so `pad_scancode[6]` is deliberately 0 and
GAME no longer sends Return (the physical START button does).

Measured under gwemu: `tests/test_osk.py` 12/12 (including a shifted cell
reaching the BDA ring as `!`), tier 2's `check_game_opens_osk` (A puts 0x1E on
port 0x60 inside the guest), no fault with retro-go's PAUSE menu over it, and
screenshots over both a text-mode and a graphics-mode screen. Cost 3,331 B
text; RAM_EMU headroom after it 4,960 B.

**Mouse mode is a UI shell with nothing listening: there is no INT 33h driver.**
What one would take, and why half-building it would be worse than none, is in
[`docs/input/05-osk-port.md`](docs/input/05-osk-port.md) with the rest of the
detail.

## String ops: the batched fast path, and what it measured

The silicon profile of TOPBENCH said string ops **plus their prefixes were ~59%
of all cycles**, and `F3` alone was **17% purely as re-dispatch overhead** — 7 M
dispatches doing no guest work. That is the price of the rewind design: one
element per pass through fetch/decode/dispatch, twice (prefix + string opcode).

`.str_exit` now re-enters the string handler **directly** for the next element
(`rep_handler`, resolved off the live dispatch table by the prefix) instead of
rewinding EIP. Semantics are untouched — the handler still re-walks the fold per
element, so granule straddles, DF=1, 16/32-bit index wrap and the ZF early exit
all behave exactly as on the slow path, which is why CMPS/SCAS batch as safely
as MOVS/STOS/LODS. The batch is bounded by `rep_batch` (256, reloaded by the
prefix) and by `pit_counter`, which IS the slice budget: a batched element
decrements it exactly as a loop trip would, so `guest_kips` pacing and
`guest_insn_debt` stay honest, and a raised IRQ — which the host signals by
dropping `pit_counter` to 1 — still lands on the same element boundary.

Measured under gwemu, TOPBENCH, `--settle 0 --window 8` (the whole guest life,
so both runs execute the identical instruction stream; control opcodes agree to
a few %). Only the COUNTS mean anything under gwemu:

| | F3 | F2 | A4 | A5 | A6 | AE | AB | AD | string+prefix | all dispatches |
|---|---|---|---|---|---|---|---|---|---|---|
| before | 343,238 | 40,980 | 177,391 | 116,043 | 41,276 | 41,300 | 25,080 | 37,108 | 847,492 | 2,534,356 |
| after | 23,075 | 269 | 55,852 | 1,860 | 802 | 803 | 2,768 | 17,751 | 130,235 | 1,968,543 |
| | **14.9x** | 152x | 3.2x | 62x | 51x | 51x | 9.1x | 2.1x | **6.5x** | **−22%** |

`AC` LODSB is unchanged (23.0 k → 23.9 k): TOPBENCH runs it unprefixed, so
there is nothing to batch — the fast path is not silently skipping it.
`asm_metrics`: `.str_exit` 17 → 37 (+20), `REP_ONE_16/32` +4 each; no string
handler changed. Silicon numbers not yet taken.
## The PC speaker, and the four things that made it sound tinny

The device report was "the beep is lower quality than the old DOS port". The
generator from that port (`dos_audio_wave.h`) is **vendored** here as
`x86_audio_wave.h` — `<stdint.h>` only, which is what makes it host-testable —
and `tests/host_audio.c` measures both implementations side by side. All four
defects are measurements, not opinions:

1. **Pitch.** The old path computed an integer `1193182 / divisor` and then a
   16-bit step from it. Two floors compound and the error grows as the tone
   falls: **middle C came out 4.7 cents flat, C3 12.9 and C2 13.5**. The
   32-bit accumulator takes the divisor directly — no integer frequency exists
   in the signal path — and the worst case over the same divisors is
   **0.23 cents**.
2. **No Nyquist guard.** At 16 kHz every divisor below 149 was above Nyquist
   and folded down as a loud, tuneless alias; divisor 10 (119 kHz) came out as
   a **7,318 Hz** tone. Those are silence now.
3. **The half-buffer was four frames long.** `GW_AUDIO_GET_LENGTH` only reports
   what `START` last set, so asking first and starting with the answer left the
   firmware default of 1077 samples — 67 ms at 16 kHz, refilled every ~17 ms.
   The writer lapped the DMA reader three times over and a correct tone reached
   the SAI as a stitch of restarts. Now 48 kHz with an 800-sample half-buffer:
   one panel period exactly, inside the 1077 ceiling.
4. **48 kHz, and the volume knob.** A speaker square is all edge; at 16 kHz
   every edge quantised to one of ~48 samples per period. Amplitude follows
   `common_emu_sound_get_volume()` (0..255, `>> 11`) instead of a fixed 6000.

Kept: the timestamped-event renderer and the 8253 latch fixes below. The ring
carries the **divisor** now, not an integer Hz, and the anti-click phase reset
fires on the rising edge the events already locate.

Measured under gwemu, Alley Cat: 26 tone changes, **0 dropped**, 59 fps, 3.0 M
insn/s. `x86_spk_log` shows the real sequence — `43h=B6` (ch2, lo/hi, mode 3),
`42h=FB`, `42h=11`, `61h=03` — i.e. divisor **0x11FB = 4603 = 259.2 Hz**.
gwemu cannot judge audio quality; the listening test is the user's.

## Where the frame time goes — one command, one report

`python3 scripts/profile_all.py --workload <name>` builds, boots, settles,
measures and writes ONE report (markdown + JSON) to `build/artifacts/`. How to
run it and what each zone means: [`TOOLING.md`](TOOLING.md).

The gap it closes: the project could already say which OPCODE burns cycles,
which HANDLER is long and what a dozen counters read — and could not say **how
one frame's wall time divides**. `x86_zones.h` is that instrument: a push/pop
zone stack over the emulator loop, exclusive self-time per zone, and **the
frame as the ROOT zone, so the unaccounted residual is measured rather than
inferred**. `X86_ZONES=1`, off by default, +752 B of core text.

Measured under gwemu, 10 s windows, 595 frames each. **Only the COUNTS mean
anything here** — no pipeline model, and the SD is free, so `disk` cannot read
as anything but ~0:

| zone | Alley Cat (playing) | TOPBENCH menu |
|---|---|---|
| `swap` (idle headroom) | 86.38% | 80.12% |
| `guest` | 13.14% | 16.57% |
| **`residual` (unaccounted)** | **0.06%** | **1.29%** |
| `hle` | 0.00%, 0.31 calls/frame | 1.40%, **359 calls/frame** |
| `video` | 0.32% | 0.51% |
| `input` / `audio` / `fwloop` | 0.06 / 0.03 / 0.01% | 0.06 / 0.03 / 0.02% |
| `disk` | 0 (nothing read in the window) | 0 (9 reads, all cache hits) |

Four things it measured that were not known before:

1. **86% of the frame is idle**, on both workloads, at 59 fps and ~3.0 M
   insn/s. There is a lot of headroom on this workload and it is in the panel
   wait — which is also why `swap` had to be its own zone rather than part of
   the residual: idle time is not a cost and must not read as one.
2. **TOPBENCH's menu makes ~359 HLE service calls per frame** — its INT 16h
   AH=01 poll — and each is a full exit from and re-entry into the interpreter.
   `hle` self-time is only 1.34% of the frame, so the calls are individually
   cheap; the cost is the 362 exec-loop trips around them.
3. **That is exactly what the residual found.** 0.06% on Alley Cat against
   1.29% on TOPBENCH (1.36% on a repeat run — the shape is stable), and the
   difference is not noise: it is the
   `while (!guest_halted)` loop body — the exit-reason test and the keyboard
   re-raise, between two bracketed zones — running 362 times a frame instead of
   1.3. Roughly 163 cycles a trip, which reconciles to the measured figure. **A
   residual that tracks a workload is the accounting working**: it named a
   phase nobody had bracketed, which is the whole reason the root zone is the
   residual rather than a subtraction.
4. **The instrument's own cost, calibrated on the target: ~89-91 cycles per
   enter/exit pair.** 0.11% of the WORK on Alley Cat (4.5 k transitions) and
   7.1% on TOPBENCH (431 k transitions). Reported against work, not against a
   mostly-idle frame, because measuring against idle flatters it. Compare
   `X86_PROFILE`'s ~67 cycles at *every dispatch* — which is why the driver
   runs the two in SEPARATE passes with separate clean builds and quotes the
   shares from one and the ranking from the other.

The opcode pass on a RUNNING Alley Cat says something worth acting on: **18%
`NOP`, 16% `IN AL,Ib` and 10% `OUT Ib,AL`** — a quarter of guest cycles are the
game's own busy-wait on the CGA status register, not work it wants done.

Two traps the work walked into, both now closed:

- **Profiling Alley Cat before it starts is profiling a spin loop.** The game
  has TWO gates: the joystick prompt (answer N = gwemu `press select`) and a
  skill-level menu wanting a LETTER, which **only the on-screen keyboard can
  send**. Unanswered, the opcode pass reads 56% `CMP Gv,Ev` + 44% `JZ Jb` — an
  exact measurement of a game that never started, and one that looks like a
  result. `timelines/profile-alleycat.tl` drives both gates and was verified
  with `screenshot.py --text` at each stage, not assumed.
- `frame_overruns` counts 456 of 596 nominally on-time frames, because
  `panel_ms_min` learns downward to 11 ms while the real period is ~16.7 ms.
  Pre-existing, and it makes the overrun count read alarmist; the histogram is
  the honest view. Not fixed here.

RAM_EMU headroom with `X86_ZONES=1` in: **2,352 B** (`__CORE_BSS_END__`
0x240576d0). The flag is off by default, so a shipping core is unaffected.

Silicon is where the CYCLES become real. Nothing above claims otherwise.

## "I had to turn off the keyboard before it would send the keys"

The device report was that Alley Cat goes deaf to input, and the suggested
cause was a host/guest desync wedging `changed = keys ^ x86_io.input_keys` to
zero. **Measured under gwemu, that is not it, and the desync is not reachable:**
`x86_io.input_keys` has exactly one writer and `poll_input()` updates it
*before* the OSK branch, so `prev` and `keys` cannot disagree; a run with LEFT
held for 12 s across a full off→keyboard→mouse→off cycle
(`timelines/osk-held-across.tl`) kept emitting, and the release-all on OSK open
delivered the held key's break exactly as the previous port's
`dos_input_release_all()` does. Both directions of the hypothesis were already
handled.

**What is real is the user's own second sentence.** With the OSK up
(`timelines/osk-live-keys.tl`, three cell presses seconds apart):

| | before | after |
|---|---|---|
| codes the guest read, of 6 injected | **1** | **6**, each within the same second as its press |
| 8042 queue at the last press | head 7 / tail 12 — **5 stuck** | head 12 / tail 12 — empty |
| `input_irqs` over the window | 6 → **1,066** (60/s) | 6 → **12**, one per code |
| `guest_tick_irq_lost` | 0 → **329** (every 18.2 Hz tick refused) | 2 → 4 |
| guest EFLAGS.IF | **0 for 18 seconds** | 1 |

All five queued codes arrived at once the moment the OSK closed — which is
precisely the reported experience.

**The mechanism is `pit_counter`, not the keyboard.** `pit_counter = 1` pulls
the guest's next instruction boundary in so an IRQ lands now instead of at the
end of the slice. It only *delivers* anything if IF is set — and a guest running
its own INT 09h handler has IF CLEAR. `poll_input()` set it once a frame,
unguarded, for as long as anything sat in the queue, so the boundary could not
deliver and merely **ended the slice: the guest ran one instruction per frame**,
inching through its own interrupt handler for eighteen seconds while the OSK
kept the queue full. `x86_io_log` shows the handler's shape
(`IN 60 / IN 61 / OUT 61 x2 / OUT 20`) creeping forward a few accesses per
sample. The same store also overwrote the **one-deep** pending slot, so the
18.2 Hz tick was discarded 18 times a second.

The decision now lives in one place, `x86_kbd_raise_irq()` in `x86_io.c`, with
both guards: refuse when the pending slot is taken, and pull the boundary in
only when IF allows delivery. The slice loop's copy already had the first guard
and `poll_input()` had neither — two call sites disagreeing about one rule is
the whole defect.

Tier 1: `tests/test_osk.py` grew four checks over that function (the grid checks
could not see this — they call `x86_bios_kbd_irq()` themselves and never go
through the arming rule). Two of them fail on the pre-fix behaviour and pass
after. `test_opcodes` 273/273, `test_dos`, `test_video`, `test_text`,
`test_keyboard`, `test_audio`, `test_disk_cache`, `test_nor`, `test_sysinfo`
all green. RAM_EMU headroom **27,232 B** (`__CORE_BSS_END__` 0x240595a0); the
change is net-negative in code.

Still open, and NOT this bug: a button held across an OSK open/close is released
in the guest and never re-made while it stays held, so the user must let go and
press again. The previous port has the same property (`dos_prev_down[]` is
resynced from raw state on the transition), so it is a shared UX defect rather
than a regression.

## Phase 3 — Wolf3D

- [ ] COW/pool/eviction with dirty tracking, loud exhaustion; pool sizing
- [ ] Gate: measure DOS FAT/sector overhead → decide the INT 21h fast path
- [ ] Protected mode: descriptors, far transfers, IRET privilege change, paging
- [ ] Profile-directed tuning against a Wolf3D DWT profile
## TOPBENCH — running, scoring, screenshotted

The 4 px cell is an **Everex Micro Enhancer 5x8** face squeezed to 4, not a
condensation of the 8x8 one, with 0xB0-0xDF hand-drawn on a fixed grid (double
verticals alias to single; double horizontals do not). Three captures of the
same menu are in `build/artifacts/`: `font_a_current.png` (the old
OR-condensation), `font_b_everex.png` (shipping) and
`font_c_antialiased.png`. Text renders through a row-expansion blit now — one
32-bit store per glyph row instead of four byte writes.

`TOPBENCH.EXE -l` scores **236** (`build/artifacts/topbench.png`); without `-l`
it draws its full-screen menu (`topbench_ui.png`). Both with **no pan** — the
4 px cell puts all 80 columns on the panel. Six defects were in the way: a
128-glyph ASCII font; 4 of 16 CLUT entries reaching the panel; INT 21h AH=3Ch
and a read-only floppy (`Runtime error 001`); and INT 10h AH=12h/1Ah unanswered,
which left TOPBENCH's detection unit reading through a pointer it never set.

`trace_diff.py` agrees with a reference 8086 (Unicorn) through startup and the
unpacker — 1,532,786 instructions to the first INT 21h. Sixteen defect classes
closed, one commit each; one shape accounts for four — reading a register
operand BEFORE `DECODE_MODRM`, which uses R4/R5 as scratch. `refdos.py` still
has no port I/O, so the oracle stops at the first CGA-retrace timing loop.

## TOPBENCH's INI, and the INT 21h calls that were not there

The device report was "the menu runs but it cannot read its files". **Measured**,
with a new file-only trace ring (`x86_dos_file_log`, because `x86_hle_log` is
100% 0x1601 keyboard polls between one file call and the next): TOPBENCH does
`3Ch`/`40h`/`3Eh` on its own `topbench.log`, then **`43h AL=00` on
`database.ini`, which the default arm answered CF=1, AX=0x0001 "invalid
function"**. It never issued the `3Dh` that would have opened the file. The open
and read paths were never reached, let alone broken -- an unimplemented call in
the MIDDLE of the sequence is indistinguishable, to the guest, from a missing
file.

Implemented: `43h` get/set attributes (`f_stat`; SET is accepted and changes
nothing, FatFs is `FF_USE_CHMOD=0`), `4Eh`/`4Fh` FindFirst/FindNext with our own
DOS 8.3 wildcard matcher over `f_opendir`/`f_readdir` (`FF_USE_FIND=0`) and a
serial stamped into the DTA, `41h` delete, `56h` rename, `19h`/`0Eh`/`47h`. Two
more defects the tier-1 test caught on its own: **a drive-letter prefix was
fatal** -- FatFs is `FF_STR_VOLUME_ID=0`, so `"A:TEST.INI"` returns
FR_INVALID_DRIVE and every program that names its own drive got "file not
found"; and `2Fh` get-DTA returned the bottom NIBBLE of the linear address with
no segment, so a program that saved and restored its DTA pointed into the IVT.

Tier 1: `tests/test_dos.py` now drives all of it from GUEST code against a
64 KB FAT12 volume the test builds (`scripts/common/fat12.py`), so the expected
bytes are the test's own -- open, a short read at EOF (partial count, CF CLEAR),
the zero read after it, seek from start and from end, close, the not-found path
(CF set, AX=02), attributes, FindFirst/FindNext through the DTA, and both
`A:TEST.INI` and lower-case `test.ini`. It fails on the pre-fix core on exactly
three of those and passes after.

Tier 2, TOPBENCH under gwemu: the file log now reads `43h` OK, `3Dh` -> handle
5 on `database.ini`, thirteen `3Fh` reads of 0x2400 bytes each (the file is
124,054 B) and `3Eh`; `x86_dos_last_error` is 0. The screen says it too --
before, rows 0-2 were empty backdrop; after, they are database records read out
of the INI:

```
   0 |....Celeron.Tualatin.clone....
   1 |....Athlon.4400+.@.2.2GHz.clone
   2 |....Toshiba.Portege.7140CT....
```

## The .dsk sector cache, and what the stutter actually was

A device report said TOPBENCH's database listing loads but STUTTERS as entries
appear (124 KB of `database.ini` in 13 reads of 0x2400). The suggested fix was
"a sector buffer in AHB". **Measured first, and the measurement changed the
design.**

What one guest read costs, end to end, measured under gwemu (tier 2) and read
out of the firmware source:

1. `x86_disk_run_hist` is **115 in slot 0 and zero everywhere else** — every
   `disk_read()` is for exactly ONE sector. FatFs never asks for a run, because
   INT 21h AH=3Fh serves the guest through a 512-byte bounce buffer, so
   `f_read` is handed one sector at a time and its multi-sector direct path is
   never taken. `FF_FS_TINY 1` is not the cause; the bounce buffer is.
2. Each of those becomes `rg_storage_copy_file_range_to_ram`, which is stdio
   and **re-opens the .dsk every call**: `fopen`→`f_open` (a directory walk),
   `fseek`→`f_lseek` which with `FF_USE_FASTSEEK 0` **walks the cluster chain
   from byte zero reading FAT sectors**, one `f_read`, `fclose`.

So the cost is **per-CALL, not per-byte** — and that is what picks the
mechanism. A plain LRU cache would fix the FAT/directory re-reads and do
nothing for a 124 KB stream; plain read-ahead would fix the stream and thrash
on the metadata. What shipped is both: **4 windows x 16 KB, LRU-replaced**, out
of the LCD bonus pool. The window is the read-ahead; the set is the cache.

Measured, same guest, same timeline, gwemu tier 2 (counts are exact there):

| | disk_read calls | backend calls | sectors fetched | hits/misses |
|---|---|---|---|---|
| before | 115 | 115 | 115 | — |
| after | 115 | **7** | 224 | 108 / 7 |

**16.4x fewer SD round trips, 93.9% hit rate**, `x86_disk_errors=0`,
`x86_disk_cache_ok=1`. 59 fps, `frame_hist` all in bucket 0, `frame_overruns=0`.
`tests/test_disk_cache.py` streams 248 single-sector reads into **9** backend
calls (28x) and 3,000 random reads into 2,901 (vs 3,000 uncached).

**gwemu cannot show the stutter and this does not claim to have fixed it.**
`docs/04` is explicit that SD busy state is not modelled, so the SD is
effectively free there; the call/sector/hit COUNTS above are exact and the
TIMING win is unproven until someone reads `x86_disk_us_total`,
`x86_disk_us_worst` and `frame_hist` off silicon.

Two things the test found that reasoning had not:

- **The cache could be WORSE than no cache.** A multi-sector request that
  straddles a window boundary with neither half resident costs two fills where
  a direct read costs one call — the random-access arm measured 3,024 backend
  calls against 3,000 uncached. That case now goes straight through, so "never
  more backend calls than the uncached path" is an invariant, not a hope.
- **RAM_EMU was oversubscribed by 920 bytes** and the only symptom was
  `core_ready=0`, `frames_rendered=0` — `guest_ram` silently failed to allocate
  and the emulator never started. Baseline headroom was **480 bytes**, not the
  4,016 this file used to quote. Dropping LFN and exFAT from OUR FatFs (it
  mounts FAT12 floppies; the firmware's separate instance keeps LFN) reclaimed
  **8,296 bytes**, so headroom is now **7,680 B with the cache in**.

Not built, and the honest reason: **asynchronous completion across frames** is
the deeper fix and it is **not reachable from the core**. The ABI's storage
entries are all synchronous and blocking, the SPI1 driver uses the polled
`HAL_SPI_TransmitReceive`, and no DMA stream is wired to SPI1. See
`docs/03` and the report in this commit's message for the exact ABI additions
it would take.

## Guest timing — three clocks, measured

The loop used to derive everything from one constant (`PIT_SLICE_INSNS`), and
after the merge it carried **two** BIOS-tick drivers at once: one per rendered
frame (182/600, exact only at 60 fps — at the device's measured 10 fps it fired
at ~3 Hz) and one per instruction slice (`TICKS_PER_SLICE = 10`, ~100 Hz and
moving with our throughput). That is Alley Cat's "pitch right, timing horrible".

Now separated, none of them a hardcoded 60:

1. **Display** — the panel's own swap signal; the period is *learned*
   (`panel_ms_min`), so an overclock needs no constant changed.
2. **Guest wall clock** — the firmware's ms counter. 18.2065 Hz exactly, carry
   in fixed point, catch-up capped at 4 ticks with the excess counted in
   `guest_ticks_dropped`.
3. **Guest CPU rate** — `guest_kips`, paced against wall time. `0` is
   unthrottled; anything else runs at that speed *and no faster*, with the
   shortfall counted in `guest_insn_deficit`.

Measured under gwemu (`probe.py`, tier 2): tick 18.24 Hz **identical** at 60 fps
and at a forced 19 fps, and `0040:006C` tracks it exactly. Paced at 1200 kips
(286-12): 1,200,000 insn/s, 60 fps, deficit 0. Unthrottled: ~10 M insn/s at
60 fps. Asked for 100,000 kips: still 60 fps, deficit climbing ~90 M/s — loud,
not silent. TOPBENCH scores 46 at 1200 kips (was 236 when a "CGA frame" was
200,000 instructions, i.e. a machine 6x faster than we are); no divide-by-zero.

Also fixed here: HLT no longer fabricates an INT 08h on demand (a guest that
HLTs in a wait loop got a timer tick every time it asked); the PC speaker is
rendered from timestamped *events* rather than point-sampled once a frame, so
a beep shorter than one frame is heard; the 8253 lo-only/hi-only writes keep
the other byte; a counter-latch command is no longer stored as an access mode;
and `AUTOEXEC.BAT`'s command tail is no longer dropped (`TOPBENCH.EXE -l`).

`guest_kips` is a single global, and its default is now **KIPS_MAX** —
unthrottled, "as fast as the silicon delivers". The deficit has no meaning with
no target, so it is held at zero in that mode and `guest_ips_achieved` is the
number to read. Per-title selection via `roms/dos/*.cfg` is **not** wired: those
files belong to another project's core and nothing here parses them.

## The CPU is a menu entry, and fps/busy are no longer zero

**CPU Speed** is a Retro-Go game-options entry now (Max / 8088 4.77 / 286 8 /
286 12 / 386SX 16 / 386DX 33), passed where we used to pass `NULL`. The key it
persists under is **"Palette"**, not a name of our own: the firmware's
`odroid_settings_app_int32_get/set` match against a fixed whitelist and ignore
anything else *silently*, and the slot is per-app so the name is ours alone.
`cpu_profile_persisted` reads the value straight back so the round trip is
visible without a reboot.

Measured under gwemu, `timelines/set-cpu-option.tl`: menu opens, our callback
fires (`cpu_profile_events`), RIGHT sets `guest_kips` 0 → 330 and the achieved
rate goes 12.0 M → **330,000 insn/s exactly**, CFSR/HFSR/BFAR clean. Save & Quit
back to the launcher and relaunch: the core comes up at **330** with no menu
touched — persistence across a restart, measured, not argued.

`common_emu_frame_loop()` is now called once per iteration, which is the only
thing that feeds `odroid_system_tick()` — never calling it is why the overlay
read `0.0 (0.0) fps` and `busy: 0.0%`. **Pacing is not shared**: the panel's
swap signal (clock 1) still owns it and the frame loop DEFERS — its draw/skip
verdict is ignored and it is fed our measured mean period so its integrator
agrees with the pacer that is really running. Feeding it `panel_ms_min` instead
(the *shortest* iteration) made it call 768 of 1032 drawn frames "skipped".
`cpumon_sleep()` in the swap wait is what makes `busy_ms` accumulate.

Measured: firmware `counters` = 1024 frames, **0** skipped, 8198 ms busy over
17.4 s → **58.7 fps, 47.0% busy**, cross-checked against an independent
`frames_rendered` rate of 59.0 fps. The 18.2 Hz tick survives both ends: 18.18 Hz
at 59 fps, and 18.2 Hz at ~10 fps (forced with `--set max_slice=4000000`) once
the ticks the catch-up cap deliberately dropped are counted.

The 4 KB open-bus filler for 0xA0000-0xFFFFF now comes from the **bonus pool**,
not RAM_EMU. It was the last allocation in a region with a few hundred bytes
spare, so any growth in the core silently unmapped upper memory;
`upper_filler_ok` says whether it is backed at all.

## The BIOS keyboard path, measured end to end

A device report said keys reached the guest but INT 16h never returned them,
with TOPBENCH's menu unresponsive. **Measured under gwemu, it does not
reproduce, and the reported evidence does not support the diagnosis.**

Tier 1, `tests/test_keyboard.py` -- guest code executing INT 16h, not host C
calls: head/tail initialise to 0x1E; the push writes AT the offset the tail
points to and wraps at 0x3E; AH=01 returns 0x1C0D without dequeueing and puts
ZF on the IRET FRAME with the right sense in both directions; AH=00 dequeues
the same word; the pop wraps. Five mutations (head/tail zeroed, ZF inverted,
tail treated as an index, no wrap, destructive peek) each make it fail.

Tier 2, TOPBENCH's own menu under gwemu: `x86_kb_pushes == x86_kb_pops ==
x86_kb_peek_hits` on every key -- queued, seen and taken -- while
`x86_bios_int16_calls` climbs ~23,000/s, which is simply what TOPBENCH's menu
loop does and is NOT evidence of a stall. `x86_hle_log` says the poll is
**AH=01**, not AH=00. And the picture moves: two DOWN presses walk the
selection from Introduction to Database Operations (the help line at row 24
changes with it) and GAME dismisses the Introduction dialog
(`timelines/press-menu-keys.tl`, `timelines/press-game-menu.tl`).

What the device evidence actually shows is that INT 09h consumed every
scancode. It says nothing about what INT 16h did, because the device had no
counter for it -- `x86_kb_pops` is that counter now, and one probe run on
silicon settles it. The remaining silicon-only candidate is that the BDA page
is not writable there: every `bda_w*` does `if (p) ... ;` and drops the write
in silence, which produces exactly the reported symptom and is invisible under
gwemu, where the whole low megabyte is plain RAM.

## Cold guest RAM in NOR — shipped read-only-first, and what it measured

**Cold RAM->NOR** is a Retro-Go game-options entry next to CPU Speed. Pressing
A freezes the guest, snapshots all 640 KB of guest RAM into memory-mapped NOR,
points every page at it and frees the AXI; pages the guest writes pull
themselves back into AXI by copy-on-write. No selection heuristic, no sampler,
no pagefile, no eviction — demand-driven, which is what makes the fraction of
genuinely read-only memory a MEASUREMENT rather than a guess.

Two facts about the firmware decided the shape, both read out of its source:

1. **There is exactly one flash-write door**,
   `odroid_overlay_cache_file_in_flash_relocate()` — the ROM cache. It takes a
   PATH. No buffer-to-flash call, no `OSPI_Erase`/`OSPI_Program`, no way to
   query flash size or invalidate the cache. So the snapshot goes in through
   the RELOCATE CALLBACK, which gets each 16 KB chunk in RAM before it is
   programmed and may modify it: the SD file only has to be the right SIZE, and
   guest memory is never dumped to the card.
2. **A cache HIT programs nothing and never calls the callback**, while still
   returning a valid-looking address into an older snapshot. Three independent
   guards: a fresh filename per snapshot, the callback's coverage is counted
   (`nor_snap_cb_bytes` must be 655,360), and `x86_nor_attach()` reads the
   flash back and byte-compares it against guest RAM before it moves a single
   fold entry. A half-applied remap is impossible by ordering, not by care.

**gwemu models OCTOSPI erase and page-program, and its XIP window shares the
backing store** (`hw/misc/gnw_h7b0_ospi.c`, `hw/arm/gnw_h7b0_soc.c:491`). So
the FULL chain is exercised at tier 2, not just the read side — this is a
stronger gwemu result than `CLAUDE.md`'s "extflash is plain memory there"
suggests, and that bullet is now too pessimistic about this one peripheral.

Measured, Alley Cat, tier 2, `timelines/nor-snapshot.tl` (counts exact):

| | before snapshot | after |
|---|---|---|
| guest RAM in AXI | 655,360 B | **4,096 B** |
| pages NOR / AXI | 0 / 160 | **159 / 1** |
| guest rate | 2.98 M insn/s | **0.36–0.52 M insn/s** |
| fps | 60 | 60 |

**99.4% of guest RAM was reclaimed and the guest kept running** —
`x86_nor_attach_err=0`, `x86_nor_cow_failures=0`, `x86_nor_unmapped=0`,
`x86_disk_errors=0`. The snapshot itself took 1,614 ms end to end.

And the price, which is the other half of the answer: **~6–8x slower**, with
~1.0 M slow-path reads per second. Every INSTRUCTION FETCH from a NOR-backed
code page traps, and demand-driven means code pages never leave NOR. That is
the accepted trade stated plainly rather than the sentinel being weakened — but
it is why v2 wants code pages kept in AXI, which the DWT per-opcode profile was
always going to gate.

Caveats that are not negotiable: **the 99.4% is an IDLE title.** Alley Cat sat
at its skill menu polling INT 16h, so `x86_nor_writes` was 0 and the one COW
came from the HOST (the BDA tick). A playing guest will write far more. And
**erase/program latency, XIP read latency versus AXI, and the real chip's erase
granularity are silicon-only** — gwemu's flash is free.

One defect the counters found on their own: `x86_nor_pages_axi = 1` after 25 s
was implausibly small, and the reason was that **every host-side write bypassed
the trap**. `x86_guest_ptr()` resolves the fold itself, so the BDA tick, INT
21h filling a DTA and a disk read landing in a guest buffer were all storing
straight at the XIP window. gwemu makes that look like it works, because its
model backs the region with ordinary memory; on silicon the store is dropped
and the guest diverges silently. `x86_guest_ptr()` now COWs first.

Cost: ITCM 36,416 → 48,208 B of 65,536 (~360 trap stubs — handlers are emitted
twice). **RAM_EMU headroom 7,680 → 4,576 B** (`__CORE_BSS_END__` 0x24057078 →
0x24057c98). That is the number to watch before anything else lands.

## Keen 4's detection screen — the shading was the answer, and `--text` cannot see it

The previous section read Keen 4's startup banner off a `--text` dump and
called the BIOS surface under it clean. **That dump was the wrong instrument.**
On that screen the CHARACTERS are a static block copied out of the EXE and the
DETECTION RESULT is carried entirely by the ATTRIBUTE byte — Keen brightens the
line for each device it found. A character dump of a machine that detected
nothing is byte-identical to one that detected everything, so the banner
rendering was never evidence that anything was detected.

`screenshot.py --attr` is that instrument now: the attribute of every cell, plus
a census of the distinct attributes. Measured on the pre-fix core:

```
dominant 0x5d (1115 of 2000 cells); 0x5f x159   <- 0x5d dim, 0x5f bright
row  7 |....5f x11 ("Video Modes")......|  "CGA" and "EGA/VGA" both 0x5d
row 14 |....5f x14 ("Detected Sound")...|  "PC Speaker", "AdLib" both 0x5d
row 15 |....5f x24 ("Memory Available")...|  "Standard xxxxx Kbytes" still xxxxx
```

**Every 0x5f run is a LABEL. Not one VALUE is bright, and the memory box still
holds the EXE's literal `xxxxx` placeholders.** The user's report is exact:
nothing is detected. What it is NOT is four broken probes — it is one program
that never reached its own reporting code.

### What Keen actually asks, in order

`x86_hle_log` decoded (32 slots, oldest first): `21/30` DOS version, four
`21/35` + `21/25` vector hooks, `21/4A` resize, `1A/00`, two `21/44` IOCTLs,
`10/0F`+`10/12` twice, `10/08`, `21/3D` on `EGAGRAPH.CK4`, `21/3E`, **`10/00`
set mode + `10/02` set cursor — this is the banner being drawn** — then `21/4A`
twice and `21/3D` on `EMMXXXX0` (MM_Startup and its EMS probe, correctly
answered CF=1/AX=2), and finally **`10/1A` then `10/12`**. Nothing after that.

So the banner is drawn BEFORE the hardware detection, and the highlight pass
that would brighten "PC Speaker" or fill in "640 Kbytes" runs AFTER it. Keen
dies in the video probe, upstream of all four families. `x86_io_log` confirms it
never gets as far as the input or sound hardware: the last 17 coalesced runs are
`OUT 3D4,0F / OUT 3D5,66 / IN 3D5 -> 0x66` (our CRTC file answering correctly),
the mono alias at 3B4/3B5 correctly staying silent at 0xFF, then `OUT 3CE,0205`
and `OUT 3C4,0F02` — Graphics Controller write mode 2 and Sequencer map mask
0x0F, i.e. **the EGA planar clear** — and then 0x3DA forever. **Port 0x60, 0x64,
0x61, 0x201, 0x388 and 0x220 are never touched at all.**

### The machine-description services were not "absent", they were lying

Four calls a program uses to size itself were **not in the served vector set**,
so each reached the bare IRET stub. An IRET does not clear a register: the guest
gets its OWN value back and believes it. Measured with guest code that poisons
AX first (`tests/test_sysinfo.py`, written failing):

| call | pre-fix answer | now |
|---|---|---|
| INT 11h equipment | **0xFFFF** — every device at once, incl. a coprocessor | 0x0021 |
| INT 12h conventional | **65,535 KB** | 640 |
| INT 15h AH=88h extended | **34,816 KB, CF SET** | 0, CF clear |
| INT 15h AH=C0h | caller's own AH, caller's own CF | AH=86h, CF set |
| 0040:0010 equipment | 0 | 0x0021 |
| 0040:0013 memory size | **0 in the product** | 640 |
| 0040:0063 CRTC base | 0 | 0x3D4 |
| 0040:0080/0082 ring bounds | 0 / 0 | 0x1E / 0x3E |

0040:0013 is the one worth staring at: **`tests/main.c` wrote it and `src/main.c`
never did**, so it read 640 under test and 0 in the shipping core. It is written
by `x86_bios_init()` now, from `x86_bios_conv_kb`, which `src/main.c` sets from
the allocation that actually happened — one place, both tiers, and INT 12h reads
the same word rather than a constant that can drift away from it.

The equipment word describes THIS machine and not a generic PC: one floppy
(bit 0), **no** coprocessor (D8-DF still exit the interpreter), **no** game
adapter (0x201 floats high and there is nothing behind it), and bits 4-5 = 10b,
**initial video mode 80x25 COLOUR**. That field, plus 0040:0063 = 0x3D4, is
where "CGA-class colour adapter" is honestly reported — it had no channel at all
before.

### The PC speaker: two bits of 0x61 that were constants

Port 0x61 returned the byte the guest last wrote, so **bit 5 (PIT channel 2 OUT)
and bit 4 (the DRAM refresh toggle) never moved**. That is not a wrong value, it
is a hang: both bits exist to be polled until they flip, and bit 5 is precisely
how a program CONFIRMS a speaker is there. Both are derived from the slice
counter now, the same source the 0x3DA retrace bit uses and for the same reason
(the frame loop only advances while the guest is stopped, so anything it pokes
reads as a constant from inside the guest). Bit 5 follows the programmed ch2
divisor and idles high when the gate is low, as an 8253 in mode 3 does. The
audible path is untouched — `x86_speaker_divisor()` is unchanged.

Measured, guest code taking 1024 samples of 0x61 and accumulating OR and AND:
pre-fix, bits 4 and 5 were **never high**; after, both are seen high and low.

"Detected Sound Hardware: PC Speaker" is, to answer the question directly, **a
DEFAULT and not a detection** — the ID engine assumes a speaker and probes only
for AdLib (0x388) and Sound Blaster (0x220). Both float 0xFF here, which is the
correct "absent", and both stay dim on a working machine too. Keen never reaches
either probe.

### Where it stops now, and it is a different place

`guest_halted=1, exit_reason=4, exit_info=0xFF` — executing open-bus filler
after a wild far transfer — is **gone**. With the honest answers in, Keen 4 runs:
`guest_halted=0`, `exit_reason=3` (timer), `guest_insns_total` climbing
14.6M → 29.5M → 53.2M across a 12 s window where it previously froze at
**1,730,515**. `x86_int_vec_log`'s `0x00080000` (INT 08h dispatched into CS=0,
which named the derailment) is replaced by `0x00080060` — the timer landing in
the stub segment, as it should. The low 64 KB is no longer wiped.

What it does instead is spin: 15,228 port accesses and rising, essentially all
0x3DA, with occasional `OUT 3CE` — Keen inside its EGA planar clear, waiting on
a retrace it gets, clearing a screen segment it does not have. Disk reads frozen
at 418, no mode ever set beyond the boot mode 3, `x86_vga_dac_writes = 0`.

**A silent wander became a live spin. That is strictly better and it is not
progress toward the picture.**

### EGA is the wall, and this stops here

The screen is byte-identical before and after — same 8 attributes, same 1115
dim cells, same 159 bright label cells, same `xxxxx`. Nothing in the four probe
families was answerable in a way that changes it, because **all four are
downstream of a video gate Keen 4 cannot pass**: it is EGA-only, its detection
concludes correctly that there is no EGA, and it has no CGA fallback to take.

Claiming EGA is the one move that gets past the gate, and it is measured as
worse — the AH=12h comment records it landing straight in mode 0Dh, which
`x86_video` refuses, leaving a black screen forever. **Claiming VGA through
AH=1Ah is the same move wearing a different hat**: AH=1Ah has no return code for
"VGA except the planar modes", and VGA is a strict superset of EGA, so AL=1Ah
licences 0Dh/0Eh/10h/12h. AH=1Ah therefore stays AL=0, with that reasoning at
the call site. Mode 13h stays reachable by simply ASKING for it, which is how
Wolf3D gets there — measured: Wolf3D never calls AH=1Ah at all.

Mode 0Dh needs a store-side hook on every VRAM write, a Sequencer + Graphics
Controller that DECODE rather than store, and 32 KB of page storage. Three
subsystems. **Not started, and not started deliberately.**

Cost of all of the above: **400 bytes** of core (`__CORE_BSS_END__` 0x240593d0
→ 0x24059560), RAM_EMU headroom **27,696 → 27,296 B**. Tier 1 green:
`test_opcodes` 273/273, `test_sysinfo` (new), `test_dos`, `test_video`,
`test_text`, `test_keyboard`, `test_osk`, `test_audio`, `test_disk_cache`,
`test_nor`.

## Commander Keen 4 — reaches its startup screen, dies on the EGA gate

First run of `KEEN4.dsk`, measured under gwemu (tier 2). It gets **further than
any title here has**: its full 80x25 startup banner renders
(`build/artifacts/keen4_startup.png`, `--text`) — title box, "Video Modes
Available", "Detected Sound Hardware: PC Speaker", "Initializing... Please
Wait". 7.2 M instructions, 418 sectors, `x86_dos_last_error=0`.

The DOS and BIOS surface underneath it is **clean, and that is a measurement,
not an assumption**. `x86_dos_file_log` holds three calls for the whole run and
all three are correct: `3Dh` on `EGAGRAPH.CK4` → handle 5, `3Eh` on it, and
`3Dh` on **`EMMXXXX0`** → CF=1, AX=2. That last one is the EMS probe, and
"file not found" IS the right answer — the device name is how a program asks
whether an EMS driver is loaded. INT 2Fh (XMS) reaches the bare-IRET stub and
leaves AL alone, which is likewise "absent". **Nothing in the INT 21h surface
was wrong for this title**, so nothing there was changed.

Then it stops, `guest_halted=1`, `guest_exit_reason=4`, **`exit_info=0xFF` at
CS:EIP `D02A:D18C`** — which is not an unimplemented instruction at all. It is
the **open-bus filler**: 0xD02A0 is upper memory, and 0xFF FF decodes as group 5
`/7`, i.e. the interpreter reporting that the guest is executing nothing.

The chain, read off `x86_int_vec_log` (added for this) and `x86_cs_log`:

1. The last two BIOS calls are the video-card detection pair, INT 10h **AH=1Ah**
   then **AH=12h**. Both answer "absent", which is what this machine is.
2. The guest then clears "video memory" with `REP STOSW`, CX=0x8000, through a
   screen segment of **0x0000** — it never set a graphics mode, so the variable
   was never initialised. `x86_video_mode_log` holds two events for the whole
   run (BIOS mode 3 at boot, the renderer following); no mode set, no refusal.
3. That wipes the low 64 KB. Measured after the halt: guest linear 0x0000
   (**the IVT**), 0x0400 (the BDA) and 0x0600 (**our own HLE stub page**) are all
   zero.
4. The 18.2 Hz tick lands mid-`REP`. `x86_int_vec_log` ends with **`0x00080000`
   — INT 08h dispatched into CS=0x0000**, every earlier entry being 0x21/0x2F/
   0x10 into the stub segment 0x0060. It executes the zeroed memory as `ADD
   [BX+SI],AL` until it reaches the part of the wipe that had not happened yet,
   hits an `EA` far JMP in that data, and lands in the filler.

It is **intermittent** for exactly that reason: the run survives when the tick
misses the window.

**The blocker is EGA, and this was tested rather than argued.** Answering AH=12h
BL=10h with BX=0003/CX=0009 ("EGA present, 256 KB") gets Keen 4 past the gate:
it reads 200 more sectors and asks for **mode 0Dh**, which `x86_video` refuses
(mode log tag 5, value 0x0D), after which it runs forever against a black
screen. That change was **reverted** — a wrong answer converts a clean stop into
a silent one — and the measurement is recorded at the call site in `x86_bios.c`
so nobody repeats the experiment.

Mode 0Dh is 320x200x16 planar through the Sequencer: a store-side hook on every
VRAM write (our VRAM is deliberately plain mapped RAM with no MMIO path), a
Sequencer + Graphics Controller register file (we model neither; the guest was
already writing `OUT 3CE` when it died), and 32 KB of page storage against
45,056 B free in the bonus pool. Three subsystems. **Not started** — same
verdict, and the same three reasons, as Mode Y for Wolf3D.

Fixed here, and it is the reason the numbers above are trustworthy:
`guest_insns_total` was incremented by the whole slice BUDGET once per frame
**unconditionally**, so a latched guest went on "running" and
`guest_ips_achieved` read **2,982,107 insn/s in the same sample as
`guest_halted=1`, `guest_exit_reason=4`**. It now stops when the guest does, and
the rate falls to 0 — measured before (3.0 M/s forever) and after (0).

## Open defects

- **A segment override in front of `REP` survives only the first element.**
  `rep_resume` is the address of the F2/F3 prefix, not of the whole prefix run,
  so `26 F3 A4` (`ES: REP MOVSB`) rewinds to the `F3` and every element after
  the first reads from DS. Pre-existing and independent of the batching, which
  keeps the override for the elements it runs in-handler (R6 is not reset there)
  and so is if anything closer to the hardware. The fix is to record the START
  of the prefix run, not the repeat prefix.
- ~~TOPBENCH halts ~3 s in on `INSB` (`6C`)~~ -- FIXED: 6C-6F are implemented. Original note: — `exit_reason=4`,
  `exit_info=0x6C`, EIP 21695, identical before and after the string work.
  `INS`/`OUTS` are on the Phase 0/1 list above. It is why a TOPBENCH profile
  window has to start at `--settle 0`: past ~3 s there is no guest running and
  the table comes back empty.

- **RAM_EMU headroom is 7,680 bytes (measured 2026-08-13, after the FatFs
  reclaim), and running out is QUIET.** It was **480** before that reclaim, and
  adding the sector cache took it to **-920**, at which point `guest_ram`
  returned NULL, `core_ready` stayed 0 and the emulator simply never started --
  no fault, no message. Measure it (`arm-none-eabi-nm build/core/dos_core.elf |
  grep __CORE_BSS_END__`) before and after any change to the core; the core's
  TEXT is in this region too, so CODE growth counts, not just data. The core ends at `__CORE_BSS_END__ = 0x24057078` in a region ending
  at `0x24100000` — 692,144 B against the 688,128 B the guest asks for (32 KB
  VRAM + 640 KB RAM). `ram_malloc()` bound-checks and returns NULL rather than
  overrunning, so nothing is corrupted, but a caller that tolerates NULL loses a
  feature in silence — which is what happened to the 4 KB open-bus filler, now
  moved to the bonus pool and reported by `upper_filler_ok`. Half the fix
  `TOOLING.md` asked for; the guest POOL itself is still RAM_EMU-only.
- Anti-aliased text is measured VIABLE on palette — TOPBENCH's menu uses exactly
  8 distinct (fg,bg) pairs and 32 CLUT entries buy exactly 8 four-entry blocks
  (`screenshot.py --pairs`) — and blocked only by the above. It is a build-time
  option (`make sdcard X86_TEXT_AA=1`), fits with 40 bytes to spare, and only by
  dropping the bilevel font table to pay for its own.
- The text CLUT published 16 entries, so retro-go's overlay (which ORs a fixed
  +32) found zeros and dimmed the screen to black. Fixed: 32 in every mode.
- `moffs` (`A0`–`A3`) takes its offset at the operand size, not the ADDRESS size.
- `MOV SS,x` does not inhibit interrupts, so one can land on a half-switched stack.
- Protected-mode segment loads trap out (`CR0.PE` set) — no descriptor walk yet.
- `tlb_*` arrays: 2 KB of DTCM declared, unreferenced.
- Text framing, FIXED: an 8x8 cell fits only 40 of 80 columns, so a UI centred
  on the 80 (TOPBENCH's menu is columns 25-52) was sliced at column 40, and no
  40-column window could contain a 28-column box plus its context. 4 px now;
  80 x 4 = 320 exactly. `x86_video_pan` survives as a no-op.
- **GAME does reach the guest as Return — measured.** `input_pad_seen` bit 5,
  0x1C/0x9C queued, IRQ 1 raised, and the guest read `0x1C` off port 0x60
  (`x86_kbd_read_log`). The chain is not the defect. What is: the *test* pressed
  gwemu's `start`, which is `B_START`/`ODROID_INPUT_X` and not the GAME button
  at all; and Alley Cat's skill-level menu wants K/H/T/A, which **no button can
  send** — so Return there is correctly delivered and correctly ignored. The
  eight-button pad cannot reach a letter menu; that needs the on-screen
  keyboard, not a mapping change.
- `tests/test_core_retrogo.py` fails its blitter/CLUT assertions whenever a real
  `.dsk` is on the card: they expect `demo_guest`'s pattern, and the demo only
  runs when no disk mounts. Pre-existing; the assertions need to be
  guest-aware.
- gwemu's headless **screendump** produces nothing (Retro-Go's own menu comes
  back black too), but its **LTDC registers are modelled**, so `test_screen.py`
  rebuilds the scanned-out image. A black timeline screenshot is not a bug.
- No hardware probe this session, so `50.50 cyc/insn` predates the interrupt,
  speaker and video work.
