# TinyEMU x86 for the Game & Watch (retro-go SD)

*[Leer en español](README.md)*

An MS-DOS PC for the Game & Watch, as an external core for the **retro-go SD**
firmware (`external_cores` branch, ABI v2). It started from
[Fabrice Bellard's TinyEMU](docs/tinyemu-original.md), with the x86 CPU
rewritten in assembly for the STM32H7B0.

- **No need to install MS-DOS**: the core carries its own DOS (it answers the
  game's DOS calls itself), so every game boots straight away.
- A 386 CPU in real mode (some protected mode boots too), CGA, EGA and VGA
  (mode 13h and Mode Y), AdLib, Sound Blaster, mouse, EMS and XMS.
- The game sees 640 KB, but the console has far less RAM: what does not fit is
  paged to a file on the SD card.

For PC XT games (8088, CGA) there is the other core,
[dos-retro-go-sd](https://github.com/julenvitoria/dos-retro-go-sd) (`dos.bin`).
Both live on the same card.

## Installing on the SD card

1. Copy the core to `/cores/`. It is built as `dos.bin`; if you also use
   XTulator (which has the same name), rename it, for example to `dos386.bin`.
2. Copy the games to `/roms/dos/` as **`.dsk`** images.
3. Each game can have a `.cfg` with the same name in `/roms/dos/dos386cfg/`
   (`MONKEY.dsk` → `dos386cfg/MONKEY.cfg`). See below.

### `.dsk` images

A `.dsk` is an ordinary FAT disk image: a floppy (360 KB … 2.88 MB) or a hard
disk with its partition table. A floppy `.img` only needs renaming. The core
mounts that image as the game's drive (`A:` and `C:` are the same disk). A game
that asks which drive it runs from is told `A:` for a floppy image and `C:` for
a hard-disk one: some games (Risky Woods) refuse to run from a floppy. To build
a `.dsk` from a folder: `scripts/make_dsk.py` (see `TOOLING.md`).

Which program starts, in order:

1. The one the `.cfg` names (the `[autoexec]` section, see below).
2. The one the `AUTOEXEC.BAT` inside the `.dsk` names (`cd` and one line with
   the program are understood; `CAT` looks for `CAT.COM`, then `CAT.EXE`).

If the game has no `AUTOEXEC.BAT`, the simplest fix is to give it a one-line one
with the executable's name (for example `MONKEY`).

## Per-game configuration (`.cfg`)

Each game can have a `.cfg` with the same name in `/roms/dos/dos386cfg/`
(`MONKEY.dsk` → `dos386cfg/MONKEY.cfg`), in DOSBox format. An example:

```ini
[cpu]
cycles = 2500        ; speed (as in DOSBox); max = whatever the console gives

[autoexec]
cd \MONKEY
monkey a

[gamepad]
a      = enter       ; which PC key each button sends
mouse  = on          ; mouse on the d-pad
```

- **Every option** (`[cpu]`, `[dos]`, `[autoexec]`, `[gamepad]`), the CPU
  profiles, the games' own sound options (like the `a` in `monkey a`, which
  picks AdLib) and which cards are emulated:
  [`docs/cfg.en.md`](docs/cfg.en.md).
- **Key names** for the buttons, and how to find out which keys a game uses:
  [`docs/input/06-gamepad-keys.en.md`](docs/input/06-gamepad-keys.en.md).

## Controls

**Default buttons** (no `[gamepad]`): d-pad = arrows, A = Space, B = Enter,
TIME = Esc, START = Y, SELECT = N (START and SELECT only on the consoles that
have them).

**GAME** opens the on-screen keyboard and steps through keyboard → mouse →
closed.

- **Keyboard** (two rows at the top and two at the bottom, the game does not
  move): d-pad to move (holding it keeps going), A presses, B is Backspace, TIME
  is Shift.
- **Shift, Ctrl and Alt** stay pressed for the next key (green). **Shift pressed
  twice locks** (red, like Caps Lock); a third press releases it.
- The **`:`** key sends a colon in one go, without going through Shift.
- The green **`A:`** key opens the list of `.dsk` files in the folder: ↑/↓
  choose, A loads it in place of the current disk, B goes back. It is for
  multi-disk games. The green **`X`** closes the keyboard.
- **Mouse**: the d-pad moves the pointer, B/A are the left and right buttons,
  TIME goes back to the keyboard.

More detail: [`docs/input/03-onscreen-keyboard.md`](docs/input/03-onscreen-keyboard.md).

## Branches

- **`main`**: the version to use, the one built and shipped as `dos386.bin`. Since
  2026-10-30 it is the former `overlays`: the AdLib chip, the mouse, the NOR snapshot and the
  pagefile live in the memory the screen leaves free in palette (LUT8) mode, and the game gets
  158 pages of 160. It needs a retro-go firmware that loads that part of the core (this
  project's does).
- **`normal`**: the previous build, without that extra memory (152 pages), for firmwares that
  do not load it. No longer developed.
- Any other branch is a test: it moves to `main` once it is tested on the console.

## For developers

- [`STATUS.md`](STATUS.md): current state. [`PLAN.md`](PLAN.md): roadmap.
- [`TOOLING.md`](TOOLING.md): building (`make -f Makefile.core pack`), the
  scripts and the three test tiers (gwemu, gwemu + retro-go, console).
- [`docs/`](docs/): design of each part (memory, CPU, video, timer…).
- [`CLAUDE.md`](CLAUDE.md): the project's rules.

## Licence

MIT, like the original TinyEMU ([`MIT-LICENSE.txt`](MIT-LICENSE.txt)).
