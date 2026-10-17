/*
 * x86_pad.h -- PER-TITLE GAMEPAD MAPPING and TYPEMATIC REPEAT.
 *
 * Ported from the gw-dos (XTulator) frontend, where it is what made most titles
 * playable without the on-screen keyboard: every DOS game picked its own keys,
 * and one fixed table (pad_scancode[] in src/main.c) cannot serve them all.
 * Alley Cat wants N at its first prompt, Prince of Persia Shift, Monkey Island
 * F5 for its menu, Rick Dangerous a digit for the video card.
 *
 * THE FORMAT is one more section in the title's existing DOSBox-syntax .cfg,
 * so a title still has exactly one human-authored file:
 *
 *     [gamepad]
 *     up     = up          ; arrows by default
 *     a      = shift       ; any key name below
 *     b      = ctrl+alt    ; up to three keys, pressed in order, released in
 *                          ;   reverse -- Ctrl+C, Alt+X, Shift+F1 ...
 *     time   = n
 *     start  = f5          ; Zelda START  (ODROID_INPUT_X)
 *     select = none        ; Zelda SELECT (ODROID_INPUT_Y); none = unbound
 *     repeat = on          ; typematic repeat for held buttons (default on)
 *     mouse  = on          ; INT 33h mouse driver; the d-pad moves the pointer
 *                          ;   and A/B are left/right click (x86_mouse.h)
 *
 * Buttons: up down left right a b time start select (Spanish: arriba abajo
 * izquierda derecha). `game` is REFUSED: GAME is the on-screen keyboard's only
 * door on every unit (x86_osk.c), and a .cfg must not be able to lock a user
 * out of it.
 *
 * THE USER-FACING LIST (Spanish, every accepted name, the differences from
 * dos.bin, and how to find which keys a game uses): docs/input/06-gamepad-keys.md.
 * Keep it in step with key_names[] in x86_pad.c.
 *
 * Keys: letters, digits, f1..f12, esc, enter, space, tab, backspace, shift,
 * ctrl, alt, up, down, left, right, home, end, pgup, pgdn, ins, del, kp0..kp9,
 * the punctuation keys (- = [ ] ; ' ` \ , . /) and Spanish aliases (intro,
 * espacio, mayus, retroceso, supr ...). `click` / `click2` (clic / clic2) are
 * the left / right MOUSE buttons, meaningful with `mouse = on`. Anything else is a bad value: counted,
 * the button keeps its default, the rest of the section still applies.
 *
 * TYPEMATIC REPEAT. A real keyboard re-sends the make code of a held key after
 * ~0.5 s, ~20 times a second. Without it, games that move a cursor one step per
 * make code (SCUMM's keyboard cursor in Loom and Monkey Island, menus, text
 * prompts) need one button press per pixel. Modifiers are never repeated, and
 * for a combo only the LAST key repeats -- as on a keyboard.
 *
 * Freestanding C, no I/O, no allocation: src/main.c hands over the bytes it
 * already read for x86_cfg_parse(), and hands key events to its own emitter.
 */
#ifndef X86_PAD_H
#define X86_PAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define X86_PAD_BUTTONS 10      /* bit numbering of src/main.c poll_input() */
#define X86_PAD_KEYS    3       /* keys per button (a combo)                */
#define X86_PAD_GAME    6       /* reserved: the on-screen keyboard's door  */

/* Pseudo make codes for the mouse buttons. Unused in scancode set 1, and their
   break codes (| 0x80) stay distinct from every make code, so they travel the
   same emit path as keys and src/main.c's emitter routes them to
   x86_mouse_button() instead of the keyboard controller. */
#define X86_PAD_CLICK_L 0x71
#define X86_PAD_CLICK_R 0x72

#define X86_PAD_REPEAT_DELAY_MS 500
#define X86_PAD_REPEAT_RATE_MS  50

typedef struct {
    unsigned applied;           /* bindings taken from [gamepad]            */
    unsigned bad;               /* unknown button / key names, refused game */
} x86_pad_result_t;

/* make codes (set 1) per button, 0-terminated; [i][0] == 0 means unbound */
extern uint8_t x86_pad_keys[X86_PAD_BUTTONS][X86_PAD_KEYS];
extern uint8_t x86_pad_repeat_on;
/* `mouse = on`: directions are not keys (x86_pad_button / x86_pad_repeat skip
   buttons 0..3) -- src/main.c steers the pointer with them instead */
extern uint8_t x86_pad_mouse_on;

/* the compiled-in table: one make code per button (0 = unbound) */
void x86_pad_defaults(const uint8_t single[X86_PAD_BUTTONS]);

/* apply a .cfg's [gamepad] section over the defaults. Other sections are
   ignored here (x86_cfg.c owns them, and skips this one). */
void x86_pad_parse(const unsigned char *buf, unsigned long len, x86_pad_result_t *out);

/* emit the make codes (down) or break codes in reverse (up) of button i */
void x86_pad_button(unsigned i, int down, void (*emit)(uint8_t sc));

/* call once per frame with the held-button mask and a millisecond clock;
   emits repeated make codes for buttons held long enough and returns how many.
   held = 0 forgets every hold (call it while the on-screen keyboard owns the
   buttons, so a button held across it is not read as held for seconds) */
unsigned x86_pad_repeat(uint32_t held, uint32_t now_ms, void (*emit)(uint8_t sc));

#ifdef __cplusplus
}
#endif

#endif
