/*
 * x86_mouse.h -- a Microsoft-compatible MOUSE DRIVER (INT 33h), served by us.
 *
 * Ported from the gw-dos (XTulator) frontend's mouse_hle.c. MS-DOS ships no
 * mouse driver and neither does our HLE DOS, so a mouse-driven title (Loom,
 * Monkey Island, every SCUMM game) sees none and falls back to a keyboard
 * cursor -- or, as Loom's difficulty screen does here, to nothing usable.
 *
 * OPT-IN PER TITLE. The driver answers only when the title's .cfg asks for it
 * (`mouse = on` in [gamepad], x86_pad.h); otherwise INT 33h stays on the bare
 * IRET, AX=0000h comes back unchanged and reads as "no driver" -- exactly the
 * behaviour every title had before this file existed. A driver that is present
 * changes what a game does (most switch input model when one answers), so it
 * must not appear under titles that were already working without one.
 *
 * WHO MOVES IT. Two front ends, one state:
 *   - the on-screen keyboard's MOUSE mode (GAME twice, x86_osk.c), B = left,
 *     A = right, with the acceleration ramp;
 *   - with `mouse = on`, the d-pad directly while the OSK is closed, and any
 *     button bound to the pseudo-keys `click` / `click2` (x86_pad.h).
 *
 * COORDINATES follow the real driver: x in 0..639 virtual for every mode,
 * y in screen lines (200, 350 or 480 by BDA mode), both clamped to the range a
 * guest sets with AX=0007h/0008h. Movement arrives in the OSK's 640x200 units
 * and y is scaled to the mode's height.
 *
 * EVENT HANDLERS: AX=000Ch IS CALLED (x86_hle.c, x86_mouse_events()): the
 * frame loop raises INT 74h on a move or button change the mask wants, and a
 * stub in the HLE block calls the handler with AX/BX/CX/DX set -- Dune II
 * never polls. AX=0014h/0018h are still accepted and never called. The driver never draws a cursor
 * into video memory; the OSK paints its own arrow over the frame while the
 * guest has asked for the driver's cursor (AX=0001h) to be shown.
 */
#ifndef X86_MOUSE_H
#define X86_MOUSE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t x86_mouse_enabled;      /* set from the .cfg, before boot      */
extern volatile uint32_t x86_mouse_calls;   /* INT 33h services answered      */
extern volatile uint32_t x86_mouse_resets;  /* AX=0000h/0021h: a guest found us */

void x86_mouse_reset_state(void);
int  x86_mouse_int33(void);            /* x86_hle.c dispatch; 1 = handled     */
void x86_mouse_move(int dx, int dy);   /* OSK units: x of 640, y of 200       */
void x86_mouse_buttons(unsigned b);    /* bit 0 left, bit 1 right             */
void x86_mouse_button(unsigned bit, int down);
/* Where the driver's cursor is on the 320x200 picture. Returns 1 when a guest
   has found the driver and has it shown (the OSK should paint an arrow), 0
   otherwise. */
int  x86_mouse_cursor(int *x, int *y);
/* 1 once a guest has reset the driver: from then on it, not the OSK's own
   position, says where the pointer is. */
int  x86_mouse_in_use(void);

#ifdef __cplusplus
}
#endif

#endif
