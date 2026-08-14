# VGA mode 13h, the firmware ABI change it needed, and where memory now lives

320x200x256, linear, one byte per pixel at guest `A000:0000`, 64,000 bytes.
Row `y` at `y * 320`. No banks, no interleave, no planes, no CRTC start
address. It is byte-for-byte the format of our LUT8 framebuffer, which is why
it is the 256-colour mode worth building first and why the blit is 200
`memcpy`s.

Companion reading: `references/8086tiny/docs/video/10-mode-13h.md` (the mode),
`15-mode-y.md` (unchained 13h, NOT built here — see the last section),
`12-vga13-direct-scanout.md` (the cache arithmetic this design leans on).

## 1. Why this needed a firmware change

`STATUS.md` used to list mode 13h as **undisplayable, not merely tight**, and
that was correct. `lcd_set_clut(clut, count)` clamps `count` to
`LCD_CLUT_CACHE_MAX = 32` and then writes a 40%-darkened **twin** of entry `i`
at `count + i`, because retro-go's overlay dims the screen by OR-ing
`LCD_DARKEN_BIT = 0x20` — a fixed +32 — into every framebuffer byte. The
staging array is 68 entries (32 cart + 32 twins + 4 overlay theme).

256 entries cannot be expressed in that model at all: the twins would need
**512 hardware slots against the LTDC's 256**. Raising the cap is not a bigger
buffer, it is an impossibility. So a 256-colour mode has to own the whole
table, and the only question is where the copy lives.

The firmware change is therefore a second entry point, not a bigger cache:

```c
void lcd_set_clut_ext(const uint32_t *clut, uint16_t count);   /* count <= 256 */
```

It takes a **caller-owned** array and costs the firmware 4 bytes of state
(`ext_clut`, `ext_clut_count`) instead of the ~1.8 KB of DTCM a 256-entry
`active_clut[]` would need — and DTCM is firmware's and tight. `clut_push()`
re-pushes the external palette instead of the cache, so an overlay theme change
cannot stamp `active_clut[0..67]` over the guest's first 68 colours.
`lcd_pack_color()` searches the external palette too, so menu colours still
resolve to a real nearest match rather than to a stale CGA index.

Exposed to external cores as **one appended ABI op**:

```c
GW_LCD_SET_CLUT_EXT = 11,   /* a=(uintptr_t)clut*, b=count */
```

Appended after `GW_LCD_BACKLIGHT_SET`, so cores built against an earlier v2
header are unaffected. **No version bump**: v2 is unpublished and already
appends freely.

The `gw_lcd.c` / `gw_lcd.h` half is a hand-application of the user's own commit
`ba73090b6` "video: VGA mode 13h -- firmware-side palette and blit"; the
`dos_video.c` and 8086tiny-submodule halves of that commit belong to a
different core and were deliberately left behind.

### The accepted caveat: `LCD_DARKEN_BIT` stops working

`odroid_overlay_darken_all()` ORs `0x20` into every pixel. Under a full-range
256-colour palette that maps a pixel to an **unrelated guest colour** rather
than to a dimmed one, so **retro-go's menu backdrop shifts colour instead of
dimming** while a 256-colour mode is up. The menu box and its text are drawn
*after* the darken pass and are unaffected, so the menu stays readable.

This is agreed and documented rather than solved. `gnw-doom` tried to fix it
with a bit-5-invariant palette; that collapses the palette to 128 entries and
mangles about half the colours. **Do not repeat it.**

The core hands the table back automatically: leaving 13h pushes the 32-entry
`GW_LCD_SET_CLUT` path again, and `tests/test_video.py` asserts exactly that —
`x86_video_clut256()` must return NULL outside a 256-colour mode, or the
firmware would keep a core-owned table that the overlay then draws through.

## 2. What the core does

| Piece | Where |
|---|---|
| Mode table entry `{ 0x13, VM_VGA8, 40 }`, refusal when unbacked | `x86_video.c`, `vmodes[]` / `vmode_find()` |
| The blit — 200 rows of `memcpy` into the letterbox | `x86_video.c`, `vga13_blit()` |
| DAC (ports `0x3C6`-`0x3C9`), 6-bit storage, 6→8 expansion | `x86_video.c`, `x86_vga_dac_*` |
| Port dispatch | `x86_io.c` |
| `INT 10h AH=10h` AL=`10h`/`12h`/`15h`/`17h` | `x86_bios.c` — **calls the same DAC**, so there is one palette model, not two |
| 256-entry CLUT push / withdraw | `src/main.c`, the frame loop |

Three things are worth stating because each is a failure that hides:

- **The DAC cannot be a port latch.** `0x3C9` is a *stream*: a game writes the
  index once to `0x3C8` and then 768 consecutive bytes, three per entry,
  against a counter that auto-increments after every third. Modelled as "the
  last value written is the state", 767 of those 768 are lost and the entire
  screen comes out one colour — while the pixel *indices* stay perfect, so a
  blit test cannot see it. `tests/test_video.py` streams the palette through
  the real ports for this reason.
- **Values are stored as the guest wrote them**, 6 bits per channel, because
  read-modify-write fade loops read the DAC back. The consumer expands with
  `(v << 2) | (v >> 4)`, so 63 maps to 255 rather than 252 — a plain `<< 2`
  leaves every white 1.2% grey and every fade-to-white short of it.
- **Nothing is added to the store path.** The aperture is linear, so the blit
  reads it once per frame. That is the whole reason this mode is cheap and the
  planar modes are not, and it is why Mode Y is not in this commit (§4).

`x86_vga13_ok` is the witness: mode 13h with no aperture is **refused and
logged** like any other unrenderable mode (event tag 5), rather than adopted so
the guest can draw into the open-bus filler.

## 3. Memory — what moved, and the belief that was wrong

Three allocations changed homes. Two of them were justified by a claim about
the MPU that **is not true of this firmware**.

`src/main.c` used to say the LCD bonus pool is uncached, and kept VRAM out of
it on that basis. Read out of the firmware source instead:
`mpu_set_lcd_pool_uncached_range()` covers **exactly the framebuffer
footprint** — 154 KB in LUT8 — and its own comment says everything past it
"defaults to Normal cacheable, which is what we want for the LCD bonus area".
The uncached window is the two framebuffers. **The bonus area above them is
cached AXI, the same memory class as RAM_EMU.**

That matters because
`references/8086tiny/docs/video/12-vga13-direct-scanout.md` prices emulated
VRAM behind a strongly-ordered region at ~5.4-6.5 cycles per guest **byte
store** — 4x the blit it would save. Had the old belief been true, putting the
13h aperture there would have been actively wrong rather than a trade.

### The bonus pool, now one map in one place

153,600 bytes, carved by fixed offsets (`BONUS_*_OFF` in `src/main.c`). It is
not an allocator and does not need to be: every consumer is known at build time
and none is ever freed. The offsets used to be open-coded `4096`/`8192` at four
call sites.

| Offset | Bytes | What |
|---|---|---|
| 0 | 4,096 | open-bus filler, aliased over 0xA0000-0xFFFFF |
| 4 K | 4,096 | BIOS identification page at 0xFF000 |
| 8 K | 65,536 | **VGA mode 13h aperture at 0xA0000** |
| 72 K | 2,048 | mode 13h host palette (768 B DAC + 1 KB ARGB CLUT) |
| 74 K | 32,768 | **CGA VRAM at 0xB8000** (moved out of RAM_EMU) |
| | **108,544 used, 45,056 free** | |

Free space is deliberately left **contiguous at the top**: the next thing that
wants it is a VRAM arena that can balloon for an EGA mode and shrink again.

64 KB and not 64,000: `0xA0000-0xAFFFF` is sixteen whole 4 KB fold pages, so the
mapping needs no partial page and no special case against the filler aliases.
The aperture is mapped **always**, not on the mode set, because a guest may
write pixels before it switches.

### The sector cache moved to the AHB heap

The `.dsk` window cache (4 x 16 KB) is now `GW_MEM_AHB`, not the bonus pool.
This is **not** primarily about making room:

- It is a **disk staging buffer**, and AHB is D2 SRAM — the only SRAM DMA1/DMA2
  can reach (they cannot touch DTCM). That is where it has to live the day
  SPI1's polled `HAL_SPI_TransmitReceive` becomes a DMA transfer, which is the
  asynchronous-completion fix `STATUS.md` names as the deeper answer to the
  stutter.
- `GW_MEM_AHB` is the **only ABI pool with a real `free()`**. ITC/RAM/DTC are
  bump allocators that can never hand anything back.

Measured on the target: it lands at `0x30005DF8`, inside AHB SRAM
(`_heap_start = 0x30005DC0`, `_heap_end = 0x3001E000`, so ~98 KB of heap
exists), `x86_disk_cache_ok = 1`, and TOPBENCH's **115 guest reads → 7 backend
calls** is unchanged by the move.

Its free space is the one number that **cannot be read off a link map**,
because the AHB heap is shared with the firmware's own newlib allocations.
`disk_cache_mem` publishes the pointer for that reason.

### RAM_EMU

Mode 13h cost 2,896 bytes of core **text** (its data is all caller-owned), which
took headroom from +872 to **-2,024 bytes** — i.e. `guest_ram` would have
returned NULL, `core_ready` would have stayed 0 and the emulator would simply
never have started, with no message. Moving the 32 KB of CGA VRAM to the bonus
pool took it to **+30,728 bytes**.

Measure it after any change to the core:
`arm-none-eabi-nm build/core/dos_core.elf | grep __CORE_BSS_END__`.

## 4. Mode Y (unchained 13h) and EGA 0Dh — BUILT

Wolfenstein 3-D runs in Mode Y, not chained 13h: Sequencer index 4 bit 3
(Chain-4) cleared, four planes at 80 bytes per scanline, page flips through the
CRTC start address at `0x0C`/`0x0D`. It is now implemented, together with the
0Dh planar path it shares a write pipeline with.

This section previously listed three missing subsystems. All three exist:

| was missing | is now |
|---|---|
| a store-side hook | `VGA_ST`/`VGA_LATCH` in `cpu_exec.S` (landed separately) driving `x86_vga_planar_store()` |
| a Sequencer/CRTC register file with effects | `x86_io.c`'s register file, which was already a faithful store, now feeds `x86_video_planar.c` through `x86_video_planar.h`. Nothing is duplicated |
| page storage that does not exist | it still does not exist. Pages are FOLDED. See "what that costs" below |

The de-interleaving blit is nearly free, as this section always said:
`4*(y*80 + (x>>2)) + (x&3)` is exactly `320*y + x`, so at the standard geometry
the correct de-interleave *is* the linear read. `vga13_blit()` gained only a
per-row source offset; the chained path is byte-identical and
`tests/test_video.py` asserts that.

### Where the code is

| what | where |
|---|---|
| the adapter: cell lookup, latches, four write modes, the page fold | `x86_video_planar.c` |
| the interface, and the rule that no state is held twice | `x86_video_planar.h` |
| register writes with an effect; the word-`OUT` split | `x86_io.c` |
| the blits, the arena claims, the mode table | `x86_video.c` |
| present-on-page-flip | `src/main.c`, `x86_video_page_flip()` |
| the tier-1 evidence | `tests/test_video.py`, `scripts/run_planar_controls.py` |

### Presentation: WHEN the screen is painted, and the two bugs that were in it

Because pages alias, the one page of storage holds **whatever the guest is
drawing right now**. So the screen is presented on the guest's page flip — the
only instant it is a complete picture — and the frame loop owes a Mode Y title
nothing at its own frame boundary. That much shipped and is right. Two things
around it were wrong, and both were reported from hardware:

> "moving the selection between menu items, the selection appears DOUBLED and
> pops back and forth before settling"; "in gameplay the viewport is SPLIT IN
> HALF, house of mirrors while rotating".

**Bug 1 — the escape hatch was a TIMER.** A title that stops flipping must not
be a frozen screen, so the loop painted anyway every 8 frames without a flip. A
timer has no idea whether the guest has finished drawing, and WOLF3D flips once
per *guest* frame while running far below the panel rate — so the hatch fired
**between essentially every pair of flips**, blitting the page mid-redraw. That
is the split viewport (the raycaster partway across) and the doubled menu
highlight (id's menu code erases the old item and draws the new one as two
operations). Measured with the timer compiled back in, on the modelled gameplay
case: **20 of 200 frames painted mid-draw**.

The hatch now asks a better question. A guest that is drawing is **storing**,
and `x86_vga_planar_stores` is already maintained on the hot path. The frame
loop may paint only when the aperture has been **quiet for two whole slices**
and something has been drawn since the last present — quiet is a completed
picture by construction, and it is exactly the case the hatch exists for (a menu
that draws once and waits for a key). A staleness bound remains so that a guest
which stores forever and never flips cannot freeze the screen — it was 60 frames
flat, which is **bug 3 below**.

**Bug 2 — the fallback painted ONE buffer.** The frame loop calls
`GW_LCD_SWAP` **unconditionally every iteration**, because the swap is also its
pacing; the panel therefore alternates between the two buffers. The flip path
always painted both. The fallback painted only the active one, which left the
two holding different pictures — and the swap then showed each in turn. That, on
its own, is "pops back and forth before settling": not a compositing artefact
at all, two buffers disagreeing. **Every Mode Y paint now paints both buffers**
(`modey_paint_both()` in `src/main.c`). Every other mode paints the active one
and lets the swap carry it, which is correct because it repaints every frame.

The decision lives in the **core** (`x86_video_modey_paint_due()` in
`x86_video_planar.c`), not in the frame loop, because "which frames are
painted" is not a pixel question: `tests/test_video.py` was at 64000/64000 with
this defect shipping. The core function is drivable with no LCD and no CPU, and
`tests/test_video.py --present-only` grades it against four modelled guests —
gameplay (flips, stores every frame: **0 of 200** loop paints), a menu (draws,
then waits: exactly **1** paint, and not one while it is drawing), and a
pathological non-flipping guest (only the staleness bound may fire, and must).
`-DX86_MODEY_TIMER_NEGCTL=1` compiles the timer back in and is the fifth arm of
`scripts/run_planar_controls.py`.

Counters, on WOLF3D under gwemu through the signon and title screens, 30 s:
`flips=2 presents=2 fallbacks=3 stale_paints=2 flip_gap_max=14`, and all of them
**stop growing** once the guest goes quiet — a static screen is painted once and
then not repainted at all, where the timer repainted it 8 times a second.

**Bug 3 — the staleness bound WAS the gameplay bug.** Bugs 1 and 2 fixed the
menus, which go quiet, and hardware still reported the split viewport and the
house of mirrors. The reason is in the paragraph above, written as if it were a
detail: *"every flip resets it, so it cannot fire inside a title that is
flipping"* — true only while the guest flips faster than 60 frame-loop
iterations. **A raycaster never goes quiet**, so the quiet rule can never fire
during gameplay and the 60-frame bound was the *only* rule left; and a WOLF3D
gameplay frame on the device is far longer than 60 loop iterations, so the bound
fired **inside every guest frame, at an arbitrary column of the redraw**. It was
the 8-frame timer again, at 60 frames.

Note what this was NOT: the flip path was never suppressed. `x86_video_page_flip()`
is called unconditionally from `x86_vga_planar_start_write()`
(`x86_video_planar.c:718`) and paints both buffers immediately. Flips were always
the primary trigger; the fallback was firing *as well*.

The bound is no longer a constant. Its only job is anti-freeze, and **a title
that is flipping is not frozen however slowly it flips**, so it is scaled to what
this guest has been observed to do: flat 60 frames until the title flips at all,
and afterwards `max(floor, 8 × the worst flip gap seen)`. Because the worst gap
seen so far is a lower bound and never the gap — WOLF3D's menus flip fast and its
gameplay flips slowly — the estimate is necessarily wrong across a menu-to-level
transition, and **nothing distinguishes "drawing a slow frame" from "stopped
flipping and still drawing"**; they are the same observation. So the bound
**backs off**: every stale paint taken with stores banked since the last flip
doubles the floor. A wrong estimate costs a couple of frames at the transition
and then converges above the guest's real frame time and stays silent.

`x86_modey_mid_draw` / `x86_modey_mid_draw_max` count exactly that failure — a
loop paint with a non-zero store count since the last flip — and the store count
says how far into the redraw the page was caught. It is the defect, counted.

The tier-1 model gained the case that was missing, and the reason the old one
missed it is one number: it flipped every **20** frames against a **60**-frame
bound, so the bound could not fire and the arm only ever proved that the quiet
rule stays out of the way. The new arm (`tests/main.c`, `which == 4`) plays
100 menu frames flipping every 20, then 1500 gameplay frames drawing in every
frame and flipping every 300, twice: the transition may cost ≤ 2 paints, and the
settled run must be **0 loop paints, 0 mid-draw, 0 quiet fallbacks**. It then
stops flipping while still drawing and must be rescued, which pins the
anti-freeze property against the fix itself.

**Bug 4 — "a quiet page is a finished page" was the LAST gameplay bug.** Bug 3
silenced the staleness bound and hardware still showed the repeated scene, with
`x86_modey_mid_draw` reading **zero** — because that witness was only ever
incremented on the stale path. The counter that was moving is
`x86_modey_fallbacks`: **62** of them in a hardware capture
(`build/artifacts/hw_wolf3d.png`) whose flip count only reached **94**. Two
thirds as many half-drawn pages on the panel as complete ones.

Those are QUIET-page paints, authorised by a rule whose comment asserted that
"a quiet page is a finished page whatever else is true of it". **That assertion
is false for a raycaster.** A guest frame is not one unbroken run of stores:
WOLF3D casts rays, sorts its vislist, pulls texture pages through its page
manager and waits on the tic counter, all with the aperture untouched. Any of
those gaps that spans two frame-loop slices is *indistinguishable* from a menu
that has finished drawing, so the loop painted the one aliased page with the
redraw partway across it. Caught on successive gaps at successive rotations,
that is the scene repeated across the viewport — one door per fragment.

Note what this is NOT: it is not a spatial addressing defect. The status bar,
the HUD and the weapon in that capture are pixel-correct, and they travel the
same Map Mask / plane-offset / page-fold path as the viewport. What separates
them is not address, it is **time**: the furniture is drawn once and never
redrawn, the viewport is redrawn every frame. A Map Mask, stride, interleave or
fold error would mangle both.

The fix has bug 3's shape, because it is bug 3's problem. For a title that has
flipped and has stored since that flip — demonstrably partway through a redraw —
the quiet run required before the loop may paint starts at the flat two frames
and **doubles every time such a paint is taken**, clamped at 32 frames so a
genuinely static screen is never more than ~0.5 s late. A title that has never
flipped is untouched and keeps the flat two frames; it is the only kind the
quiet rule now serves without argument. Every such paint is counted in
`x86_modey_mid_draw`, so "the loop put a half-drawn page on the panel" has one
number however the loop got there.

**Why the 64000/64000 Mode Y arm never saw any of this** — it cannot. It grades
one still frame of a program that draws once and halts; "which frames are
painted" is not a pixel question. And the *presentation* arm missed it for a
sharper reason: `which == 1` and `which == 4` both store in **every** modelled
frame, so `modey_quiet` can never reach 2 and the quiet rule is structurally
unable to fire. `which == 4`'s own assertion is written as `must be 0: never
quiet`. The model assumed the conclusion. `which == 5` draws in **bursts** — 22
frames of stores, 3 with none, through a 300-frame guest frame — which is how
software actually drives the adapter, the same lesson as
`references/8086tiny/docs/video/11-ega-planar.md` §"Why the first test passed
while a real game rendered garbage". Measured: **60** mid-draw paints per 1500
frames with `-DX86_MODEY_QUIET_NEGCTL=1` (the shipped rule compiled back in),
**0** with the fix, and arms 1–4 stay green under that control.

**What is left, and it is accepted.** Two things. First, the ≤ 2 frames the
back-off costs at a menu-to-gameplay transition, which is unavoidable without a
"the guest has finished" signal that does not exist. Second, the aliasing itself:
one page of storage means the panel shows page N while the guest draws N+1 over
it, and `references/8086tiny/docs/video/15-mode-y.md` measured a real 256 KB card
holding pages 0 and 2 finished with page 1 a few columns into the next frame.
Presenting on the flip is what keeps that to the measured **0.58 %** of the
reference card on the same frame, and it is not fixed by getting the fold right.
Nothing here is fixed by more memory either — a second real page is 66,560 bytes
that do not exist.

### The arena arithmetic that actually held

Modes are mutually exclusive, so the peak demand is the largest mode and not the
sum. Against a 143,360-byte arena with CGA (32,768) and the 13h aperture
(65,536) held, 45,056 bytes are free, and one 32,768-byte block serves either
planar mode — 12,288 to spare, whichever is up:

* **Mode Y**: the 64 KB aperture *is* the plane storage (one aliased page,
  16,384 plane offsets x 4 = 65,536). The arena block is the OFF-SCREEN cache at
  `0xC300`, 8,192 plane offsets, claimed under `X86_ARENA_ID_SPARE`.
* **0Dh**: the aperture's bottom 16 KB is the CPU read shadow, and the arena
  block is the planes — 8,192 offsets, a 32 KB card, under `X86_ARENA_ID_EGA`.

`vmode_find()` gates 0Dh on `x86_arena_would_fit()`, so the mode is REFUSED
before it is adopted rather than accepted and not rendered, and the refusal is
loud: `x86_video_mode_log` tag 5 plus `x86_vga_ega_refused`, with
`x86_arena_deficit` holding the byte count.

### Two improvements over the reference, both from the hook's shape

1. **Latches are exact for PLAIN reads**, not only for read-modify-writes. The
   reference is a C interpreter whose read path never surfaced a plain load, so
   it could only latch from a store's destination. `VGA_LATCH` sits on 47 real
   load sites and records the window offset of the last guest read in
   `cpu->vga_latch`, so `mov al,[si] / mov [di],al` — the write-mode-1 block copy
   that is most of the reason to unchain at all — latches from SI, which is where
   the hardware latches from. Measured on Wolf3D: 107,721 latch reloads, **zero**
   misses through the setup and title screens.
2. **No save/restore dance.** `15-mode-y.md` §5 describes saving the aperture
   bytes a raw store is about to destroy and putting them back, because in Mode Y
   the shadow address IS plane storage. Our hook REPLACES the store, so the raw
   byte is never written and `dos_ega_wr_save[]` has no analogue here. Do not
   reintroduce it.

### What the fold costs — carried forward verbatim, because these are the record

A Mode Y page is still 64,000 bytes. Unchaining does not shrink a page, it makes
three more of them addressable. Two real pages is 195,072 B; a full card is
262,144 B. So every plane offset is folded into one page's worth of storage, and:

* **A page flip is a no-op; pages alias. The artefact is TEARING** — against the
  four-copy stacked-frame garbage a chained blit produces. Every Mode Y title
  redraws the whole frame each pass, which is why it double-buffers.
* **Data reads return the wrong plane in Mode Y.** There is no room for a read
  shadow, because the planes *are* the aperture, so a guest read of `A000:a`
  returns `aperture[a]`, i.e. plane `a&3` at plane offset `a>>2`. **Latch loads
  stay exact**, and latch copies are the idiom that matters; a title that reads
  pixels back for collision detection would not be served.
* **`0xE300` upward is discarded.** Measured on the reference: 2,688 of 59,616
  cache latch loads, all in one cold one-off region, and rebuilding with a 48 KB
  cache produced a **byte-identical frame**. 8,192 offsets is sufficient, not a
  compromise.
* **Do not fix the aliasing without bringing ~192 KB with you.** The sums above
  are the record that stops that being re-proposed.
* **Commander Keen 4's level backgrounds need a 256 KB card and are NOT fixable
  in this device's memory.** Its write high-water is plane offset 65,536, 97.2%
  of its latch copies source from beyond an 8 KB plane, and nothing between 8 KB
  and 64 KB per plane buys a partial picture — at 16 KB the frame is *worse* than
  at 8 KB. **0Dh gets Keen's banner and its menus, not its levels.**

Two further limits are ours rather than the reference's:

* **The default stride is a guess.** 16,640 (80 x 208) is what the one Mode Y
  title we have uses, and learning is too late for the furniture a title paints
  before its first flip. A title spacing pages 16,384 apart is displaced until it
  flips. The durable fix is a per-title `.dosmeta` hint.
* **The off-screen base assumes THREE pages.** A title that double-buffers and
  caches two pages up has its cache aliased onto page 3's storage. Nothing
  observable states the page count.

## 5. What WOLF3D actually does today — measured

It reaches video, it unchains, and it renders. This section used to say the
guest never got past a DOS-layer seek defect and that the screen was "black
before this change and black after it"; that is no longer the state.

Under gwemu, `scripts/probe.py --retrogo`, with the ROM preselected:

| counter | value | what it settles |
|---|---:|---|
| `x86_vga_modey_armed` | 1 | the guest cleared Chain-4 and we followed it |
| `x86_vga_modey_page` | **16,640** | learned from the guest's OWN flips, not assumed |
| `x86_vga_modey_flips` | 2 | CRTC display-start changes seen |
| `x86_modey_presents` | 2 | frames painted on a page flip rather than at a frame boundary |
| `x86_vga_planar_stores` | 136,168 | stores that took the adapter |
| `x86_vga_latch_loads` / `_miss` | 136,168 / 256 | latch reloads, and how many found no cell |
| `x86_vga_planar_lost` | 17,920 | stores past the fitted card, discarded rather than aliased |
| `x86_modey_fallbacks` | 3 | frames the LOOP painted, each with a quiet (hence complete) page |
| `x86_modey_stale_paints` | 2 | the 60-frame liveness bound firing — the only path that can still show a half-drawn page |
| `x86_modey_flip_gap_max` | 14 | loop frames between two flips; anything above 8 is a frame the old TIMER hatch would have painted mid-redraw |

**16,640 is the whole design confirming itself against the real title.** It is
the id Galaxy engine's `SCREENSIZE`, it is what the reference measured, and our
core arrives at it from nothing but the distance between two display starts the
guest programmed.

`scripts/screenshot.py` through the menus shows the setup screen (the one whose
off-screen tile cache at `0xC300` produced a band of garbage across the top two
thirds of the reference's screen before that region got its own storage), the
title screen, the Options menu and the difficulty screens, all rendering
correctly. Gameplay itself is past the 60 s timeline cap at gwemu's ~3.0 M
insn/s — the level load does not finish inside it. That is a speed observation,
not a rendering one, and the counters above are the evidence that the pipeline
is live.

**gwemu is still not a witness for what the picture looks like on silicon**
(`15-mode-y.md` says the same, from the other direction — the reference saw a
black Wolf3D under gwemu with unmodified baseline sources). The pixel evidence
is `tests/test_video.py`, which compares all 64,000 pixels for chained 13h, for
Mode Y and for 0Dh against expectations computed host-side from the pattern, and
`scripts/run_planar_controls.py`, which requires four negative controls to fail
and the stride learning to work from a deliberately wrong default.
