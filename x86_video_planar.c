/*
 * VGA aperture store hook.
 *
 * Every guest STORE that lands in the 64 KB aperture at 0xA0000 arrives here
 * instead of being written by the interpreter -- see the VGA_ST macro in
 * cpu_exec.S. The hook REPLACES the store; it is never a notification after
 * one. That is load-bearing for the planar modes this file will grow: in an
 * unchained ("Mode Y") write the byte the guest presents is not the byte that
 * reaches any plane, so letting the raw store happen first would destroy a
 * pixel written four stores earlier.
 *
 * THIS FILE IS THE MECHANISM ONLY. The body below does the plain store, i.e.
 * byte-for-byte what the interpreter did inline before the conversion, so the
 * emulator's behaviour is unchanged. It is also unreachable today: nothing
 * ever writes cpu->vga_base, so the aperture check at every site is always
 * false. The planar pipeline replaces this body and nothing else.
 */

#include <stdint.h>
#include <stddef.h>
#include "cutils.h"
#include "x86_cpu.h"

/* Host base of the guest 0xA0000 aperture, or NULL. Owned here rather than in
   x86_video.c so the hook has no dependency on the renderer, but SET there --
   x86_video_set_aperture() is the single place the aperture is decided. */
uint8_t *x86_vga_planar_aperture;

/* Host-visible witness: how many stores took the hook. Zero until a planar
   mode arms cpu->vga_base, which is what a probe checks for. */
volatile uint32_t x86_vga_planar_stores;

void x86_vga_planar_store(uint32_t off, uint32_t val, uint32_t size)
{
    uint8_t *p = x86_vga_planar_aperture;

    x86_vga_planar_stores++;
    if (!p)
        return;                     /* armed with no storage: drop the write */
    p += (off & (X86_VGA_APERTURE_BYTES - 1u));

    switch (size) {
    case 1:
        p[0] = (uint8_t)val;
        break;
    case 2:
        p[0] = (uint8_t)val;
        p[1] = (uint8_t)(val >> 8);
        break;
    default:
        p[0] = (uint8_t)val;
        p[1] = (uint8_t)(val >> 8);
        p[2] = (uint8_t)(val >> 16);
        p[3] = (uint8_t)(val >> 24);
        break;
    }
}
