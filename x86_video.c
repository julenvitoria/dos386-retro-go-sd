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

void x86_video_init(void *vram, void *lut8_palette)
{
    cga_vram = (uint8_t *)vram;
    memset(cga_vram, 0, X86_VRAM_BYTES);

    /* CGA packs the leftmost pixel in the HIGH bits of each byte. The table is
       built so a little-endian word store lands them left to right. */
    for (unsigned b = 0; b < 256; b++) {
        cga_expand[b] = ((b >> 6) & 3)
                      | (((b >> 4) & 3) << 8)
                      | (((b >> 2) & 3) << 16)
                      | (((b >> 0) & 3) << 24);
    }

    /* Palette 1 high intensity: black, cyan, magenta, white -- what most CGA
       games select, and what Alley Cat uses. Written as RGB565 for the LUT8
       framebuffer. */
    if (lut8_palette) {
        uint16_t *pal = (uint16_t *)lut8_palette;
        pal[0] = 0x0000;    /* black       */
        pal[1] = 0x07FF;    /* light cyan  */
        pal[2] = 0xF81F;    /* light magenta */
        pal[3] = 0xFFFF;    /* white       */
    }
}

/* Map VRAM into the guest at 0xB8000. Fixed, not pooled: it is a device
   aperture that happens to be readable memory, and it must not move. */
int x86_video_map(void)
{
    return x86_map_fixed(0xB8000u, cga_vram, X86_VRAM_BYTES);
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
