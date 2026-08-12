/*
 * CGA video.
 *
 * VRAM is a dedicated 32 KB block, not pool pages. It is mapped once at guest
 * 0xB8000 and never moves, so the guest writes the framebuffer as plain memory
 * and nothing needs an MMIO fault path: a CGA program pokes B800:0 and reads it
 * back, and both work because it really is RAM. The conversion to the LCD
 * happens once per frame instead of once per store, which is the whole reason
 * this is affordable.
 *
 * Budget (AXI, 1024 KB total): 154 KB LUT8 framebuffer, 32 KB VRAM here, and
 * the remaining ~838 KB is the guest RAM pool. A DOS game in conventional
 * memory wants 640 KB of that.
 */
#include <stdint.h>
#include <string.h>

#include "cutils.h"
#include "x86_cpu.h"

/* Mode 4 planes: even scanlines at +0x0000, odd at +0x2000, 80 bytes a row. */
#define CGA_PLANE_ODD   0x2000u
#define CGA_ROW_BYTES   80u
#define CGA_WIDTH       320u
#define CGA_HEIGHT      200u

void cga_mode4_blit(uint8_t *dst, const uint8_t *even, const uint8_t *odd,
                    const uint32_t *tab, uint32_t dst_advance);

static uint8_t *cga_vram;
/* 2bpp -> four palette bytes. In DTCM: the blitter hits it once per source
   byte, and DTCM's second port keeps that off the AXI path the source is on. */
static uint32_t cga_expand[256] __attribute__((section(".dtcm_bss")));

/* The 16 CGA colours in hardware index order, as ARGB8888.
 *
 * The LTDC CLUT is 32-bit, NOT RGB565 -- lcd_set_clut() takes const uint32_t*
 * and caps the count at 32. Handing it a uint16_t array of RGB565 gives a
 * black screen, because every pair of entries is reinterpreted as one ARGB
 * word. */
static const uint32_t cga_argb[16] = {
    0xFF000000, 0xFF0000AA, 0xFF00AA00, 0xFF00AAAA,
    0xFFAA0000, 0xFFAA00AA, 0xFFAA5500, 0xFFAAAAAA,
    0xFF555555, 0xFF5555FF, 0xFF55FF55, 0xFF55FFFF,
    0xFFFF5555, 0xFFFF55FF, 0xFFFFFF55, 0xFFFFFFFF,
};

static uint32_t *cga_clut;      /* the LUT8 CLUT the LCD scans out through */
/* Set whenever the four active colours change. The frame loop re-pushes the
   CLUT on it: the firmware copies our table into its own, so editing ours
   after the fact would never reach the panel. */
uint32_t cga_clut_dirty;

/*
 * Recompute the four active colours from the mode/colour-select registers.
 *
 * Mode 4 takes colour 0 from the background field of 0x3D9 -- it is a real
 * register, not a constant, and games change it -- while colours 1..3 come
 * from the palette pair selected by bit 5, brightened by bit 4. The
 * black-and-white bit in 0x3D8 selects a third set on real hardware.
 */
void x86_video_refresh_palette(void)
{
    if (!cga_clut)
        return;

    unsigned sel = x86_io.cga_color_select;
    unsigned background = sel & 0x0F;
    unsigned bright = (sel & 0x10) ? 8u : 0u;

    unsigned c1, c2, c3;
    if (x86_io.cga_mode & 0x04) {          /* composite/BW: cyan, red, white */
        c1 = 3; c2 = 4; c3 = 7;
    } else if (sel & 0x20) {               /* palette 1: cyan, magenta, white */
        c1 = 3; c2 = 5; c3 = 7;
    } else {                               /* palette 0: green, red, brown */
        c1 = 2; c2 = 4; c3 = 6;
    }

    cga_clut[0] = cga_argb[background];
    cga_clut[1] = cga_argb[(c1 | bright) & 15];
    cga_clut[2] = cga_argb[(c2 | bright) & 15];
    /* Brown is the odd one out: index 6 brightens to 14 (yellow); white (7)
       brightens to 15, which the mask already gives. */
    cga_clut[3] = cga_argb[(c3 | bright) & 15];
    cga_clut_dirty = 1;
}

void x86_video_init(void *vram, void *lut8_palette)
{
    cga_vram = (uint8_t *)vram;
    cga_clut = (uint32_t *)lut8_palette;
    memset(cga_vram, 0, X86_VRAM_BYTES);

    /* CGA packs the leftmost pixel in the HIGH bits of each byte. The table is
       built so a little-endian word store lands them left to right. */
    for (unsigned b = 0; b < 256; b++) {
        cga_expand[b] = ((b >> 6) & 3)
                      | (((b >> 4) & 3) << 8)
                      | (((b >> 2) & 3) << 16)
                      | (((b >> 0) & 3) << 24);
    }

    /* Power-on state: mode 4 graphics with the bright cyan/magenta/white
       palette, which is what the BIOS leaves behind. The guest overrides it
       through 0x3D8/0x3D9 and the CLUT follows. */
    x86_io.cga_mode = 0x0A;
    x86_io.cga_color_select = 0x30;
    x86_video_refresh_palette();
}

/* Map VRAM into the guest at 0xB8000. Fixed, not pooled: it is a device
   aperture that happens to be readable memory, and it must not move. */
int x86_video_map(void)
{
    return x86_map_fixed(0xB8000u, cga_vram, X86_VRAM_BYTES);
}

/* Where the beam is, as a fraction of the frame. The core loop advances this
   so a guest waiting on vertical retrace waits roughly a frame, not forever
   and not zero time. */
void x86_video_set_beam(uint32_t frac)
{
    x86_io.cga_beam = (uint16_t)frac;
}

uint8_t *x86_video_vram(void)
{
    return cga_vram;
}

/*
 * One frame. `dst` is the LUT8 framebuffer, `stride` its pitch in bytes.
 * 320x200 lands inside a 320x240 panel, so the picture is centred with 20
 * blank rows above and below rather than stretched -- CGA pixels are not
 * square, but scaling costs a multiply per pixel and looks worse.
 */
void x86_video_render(uint8_t *dst, uint32_t stride)
{
    uint8_t *top = dst + ((240u - CGA_HEIGHT) / 2u) * stride;
    cga_mode4_blit(top, cga_vram, cga_vram + CGA_PLANE_ODD,
                   cga_expand, stride - CGA_WIDTH);
}
