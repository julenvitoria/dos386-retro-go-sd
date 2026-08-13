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

## 4. Mode Y (unchained 13h) — scoped, NOT built, and why

Wolfenstein 3-D runs in Mode Y, not chained 13h: Sequencer index 4 bit 3
(Chain-4) cleared, four planes at 80 bytes per scanline, page flips through the
CRTC start address at `0x0C`/`0x0D`.

The de-interleaving blit would be nearly free — `4*(y*80 + (x>>2)) + (x&3)`
is exactly `320*y + x`, so at the standard geometry the correct de-interleave
*is* the linear read this commit already does. **The blit is not the problem.**

What Mode Y actually needs, and what this core does not have:

1. **A store-side hook.** Every guest write to `A000:*` must run through the
   Map Mask, the latches and the write mode. Our VRAM is deliberately plain
   mapped RAM with **no MMIO path at all** — that is the design property that
   makes the interpreter fast. There is now a mechanism that could serve
   (the NOR COW fold traps: an entry with bit 31 set traps to a slow path), but
   arming it over the aperture puts ~20 instructions of adapter logic on every
   VRAM store, and the NOR work measured that shape at **6-8x slower** for
   code pages.
2. **A Sequencer and CRTC register file.** We model `cga_mode`, `cga_color_select`
   and a synthetic beam, and nothing else. CRTC `0x0C`/`0x0D` in particular is
   listed in `STATUS.md` as an open gap: without it a Mode Y title renders a
   static page 0.
3. **Page storage that does not exist.** A Mode Y page is still 64,000 bytes;
   unchaining does not shrink a page, it makes three more addressable. Two real
   pages are 195,072 B and a full card is 262,144 B, against 45,056 B free in
   the bonus pool. The reference port folds every plane offset into one page's
   worth of storage and accepts that a page flip is a no-op — and needed a
   *learned* page stride (WOLF3D's is 16,640, not 16,384) plus separate
   off-screen storage at `0xC300` to make even that look right.

That is three subsystems, not a small addition, and item 3 has no answer here
at all. **It is correctly a separate piece of work.**

## 5. What WOLF3D actually does today — measured

It does not reach video at all, so mode 13h is neither confirmed nor refuted by
it. Under gwemu, 30 s, `x86_video_mode_log` holds two events for the whole run:
mode 3 adopted at boot, and the renderer following it into text.
`x86_vga_dac_writes = 0` — the guest never touches the palette.

Where it stops is in **INT 21h**: `x86_dos_path_log` shows a FindFirst for
`*.WL1`, and `x86_dos_file_log` is then all 32 slots of **AH=42h (seek)
returning CF=1, AX=0x0006 "invalid handle"**, repeating forever at 3.0 M
insn/s. That is a DOS-layer defect, in front of everything this commit touched,
and it is the same shape as the TOPBENCH `AH=43h` failure: an unserviced or
mis-serviced call in the *middle* of a sequence is indistinguishable, to the
guest, from broken hardware.

**The screen is black before this change and black after it, for that reason.**
Mode 13h's evidence is `tests/test_video.py`, which is pixel-exact over all
64,000 pixels plus all 256 CLUT entries — not a screenshot of a guest that
never gets there.
