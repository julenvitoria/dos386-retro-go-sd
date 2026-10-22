# Keys for the buttons (the `.cfg`'s `[gamepad]`)

*[Leer en español](06-gamepad-keys.md)*

*A guide for whoever writes a game's `.cfg`. The format and the parser are in
[`x86_pad.h`](../../x86_pad.h) / `x86_pad.c`; this page is the list you look up
while writing it.*

A title's `.cfg` sits next to its `.dsk` with the same name
(`MONKEY.dsk` → `MONKEY.cfg`) and the buttons go in a `[gamepad]` section:

```ini
[gamepad]
a      = shift          ; one button = one PC key
b      = ctrl+alt       ; or up to 3 with "+": pressed in order, released in reverse
start  = f5
select = none           ; none / nada = the button does nothing
repeat = on             ; repeat while held (default on)
mouse  = on             ; mouse: the d-pad moves the pointer, A/B are the buttons
```

**Buttons**: `up` `down` `left` `right` (or `arriba` `abajo` `izquierda`
`derecha`), `a`, `b`, `time`, `start` (or `x`), `select` (or `y`). `start` and
`select` are the physical buttons only some consoles have (the Zelda one).
`game` **cannot be changed**: it is the on-screen keyboard's door.

**Default**, with no `[gamepad]`: arrows on the d-pad, A = Space, B = Enter,
TIME = Esc, START = Y, SELECT = N.

## Key names

Upper or lower case does not matter.

| PC key | Accepted names |
|---|---|
| Letters and digits | `a` … `z`, `0` … `9` |
| F1 … F12 | `f1` … `f12` |
| Enter | `enter`, `intro` |
| Space | `space`, `espacio` |
| Esc | `esc` |
| Tab | `tab` |
| Backspace | `backspace`, `retroceso` |
| Shift | `shift`, `mayus` · the right one: `rshift` |
| Control | `ctrl` |
| Alt | `alt` |
| Caps Lock | `capslock` |
| Arrows | `up`/`arriba`, `down`/`abajo`, `left`/`izquierda`, `right`/`derecha` |
| Home / End | `home`/`inicio`, `end`/`fin` |
| Page Up / Page Down | `pgup`/`repag`, `pgdn`/`avpag` |
| Insert / Delete | `ins`, `del`/`supr` |
| Numeric keypad | `kp0` … `kp9`, `kpplus`, `kpminus`, `kpmul` (`kp8` is the same key as the up arrow, as on an XT keyboard) |
| Symbols | `-` `=` `[` `]` `'` `` ` `` `\` `,` `.` `/` |
| Semicolon | **only `semicolon`**: a `;` after a space starts a comment |
| Mouse (with `mouse = on`) | `click`/`clic` (left), `click2`/`clic2` (right) |

A name that is not in the table counts as an error and that button keeps its
default key; the rest of the section still applies.

**Differences from the XTulator core (`dos.bin`)**: there the long English
aliases also work (`escape`, `return`, `control`, `insert`, `delete`, `minus`,
`comma`…), plus `tabulador`, `numlock`, `scrolllock`, `keyboard`/`teclado` and
`mup`/`mdown`/`mleft`/`mright`. They are not here because every byte of the core
is memory taken from the game (RAM_EMU = guest pages); with the short names in
the table a `.cfg` works for both. To move the mouse here, use `mouse = on` and
the d-pad.

## How do I find out which keys a game uses?

There is no automatic way: every DOS game chose its own. In order:

1. **What the game itself says**: many show it at start-up or on a help screen
   (Alley Cat asks "Y/N", Prince of Persia shows its keys in its demo,
   LucasArts games have their menu on F5…).
2. **The manual**: almost all of them have a table of keys. Scans can be found
   by searching "<game> manual" (archive.org, MobyGames…).
3. **Trying with the on-screen keyboard** (GAME): if a key does something
   useful, put it on a button.
4. **The usual keys of the period**: arrows to move; Space, Ctrl or Alt to jump
   or fire; Enter to accept; Esc for the menu or to skip scenes.

Watch out for games where a key does something unexpected: in Monkey Island
(and the other SCUMM games) **Space is pause**, so with the default A the game
pauses; Enter (B) advances the text and Esc (TIME) skips scenes.
