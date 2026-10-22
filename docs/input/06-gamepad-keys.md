# Teclas para los botones (`[gamepad]` del `.cfg`)

*[Read in English](06-gamepad-keys.en.md)*

*Guía para quien escribe el `.cfg` de un juego. El formato y el parser están en
[`x86_pad.h`](../../x86_pad.h) / `x86_pad.c`; esta página es la lista que se
consulta al escribirlo.*

El `.cfg` de un título va junto a su `.dsk` con el mismo nombre
(`MONKEY.dsk` → `MONKEY.cfg`) y los botones se ponen en una sección `[gamepad]`:

```ini
[gamepad]
a      = shift          ; un botón = una tecla del PC
b      = ctrl+alt       ; o hasta 3 con "+": se pulsan en orden y se sueltan al revés
start  = f5
select = none           ; none / nada = el botón no hace nada
repeat = on             ; repetición al mantener pulsado (por defecto on)
mouse  = on             ; ratón: la cruceta mueve el puntero, A/B son los botones
```

**Botones**: `up` `down` `left` `right` (o `arriba` `abajo` `izquierda`
`derecha`), `a`, `b`, `time`, `start` (o `x`), `select` (o `y`). `start` y
`select` son los botones físicos que solo tienen algunas consolas (la de
Zelda). `game` **no se puede cambiar**: es la puerta del teclado en pantalla.

**Por defecto**, sin `[gamepad]`: flechas en la cruceta, A = Espacio, B = Intro,
TIME = Esc, START = Y, SELECT = N.

## Nombres de teclas

Da igual mayúsculas o minúsculas.

| Tecla del PC | Nombres que acepta |
|---|---|
| Letras y números | `a` … `z`, `0` … `9` |
| F1 … F12 | `f1` … `f12` |
| Intro | `enter`, `intro` |
| Espacio | `space`, `espacio` |
| Esc | `esc` |
| Tabulador | `tab` |
| Retroceso | `backspace`, `retroceso` |
| Mayúsculas (Shift) | `shift`, `mayus` · la derecha: `rshift` |
| Control | `ctrl` |
| Alt | `alt` |
| Bloq Mayús | `capslock` |
| Flechas | `up`/`arriba`, `down`/`abajo`, `left`/`izquierda`, `right`/`derecha` |
| Inicio / Fin | `home`/`inicio`, `end`/`fin` |
| Re Pág / Av Pág | `pgup`/`repag`, `pgdn`/`avpag` |
| Insert / Supr | `ins`, `del`/`supr` |
| Teclado numérico | `kp0` … `kp9`, `kpplus`, `kpminus`, `kpmul` (`kp8` es la misma tecla que la flecha arriba, como en un teclado XT) |
| Signos | `-` `=` `[` `]` `'` `` ` `` `\` `,` `.` `/` |
| Punto y coma | **solo `semicolon`**: un `;` detrás de un espacio empieza un comentario |
| Ratón (con `mouse = on`) | `click`/`clic` (izquierdo), `click2`/`clic2` (derecho) |

Un nombre que no esté en la tabla cuenta como error y ese botón se queda con su
tecla por defecto; el resto de la sección se aplica igual.

**Diferencias con el núcleo XTulator (`dos.bin`)**: allí valen además los
alias en inglés largos (`escape`, `return`, `control`, `insert`, `delete`,
`minus`, `comma`…), `tabulador`, `numlock`, `scrolllock`, `keyboard`/`teclado`
y `mup`/`mdown`/`mleft`/`mright`. Aquí no están porque cada byte del núcleo es
memoria que se le quita al juego (RAM_EMU = páginas del invitado); con los
nombres cortos de la tabla un `.cfg` vale para los dos. Para mover el ratón
aquí se usa `mouse = on` y la cruceta.

## ¿Cómo saber qué tecla usa cada juego?

No hay forma automática: cada juego de DOS eligió las suyas. Por orden:

1. **Lo que dice el propio juego**: muchos lo muestran al arrancar o en una
   pantalla de ayuda (Alley Cat pregunta "Y/N", Prince of Persia enseña las
   teclas en su demo, los de LucasArts tienen el menú en F5…).
2. **El manual**: casi todos traen una tabla de teclas. Se encuentran
   escaneados buscando "<juego> manual" (archive.org, MobyGames…).
3. **Probar con el teclado en pantalla** (GAME): si una tecla hace algo útil,
   ponla en un botón.
4. **Lo típico de la época**: flechas para moverse; Espacio, Ctrl o Alt para
   saltar o disparar; Intro para aceptar; Esc para menú o saltar escenas.

Cuidado con los juegos en los que una tecla tiene un efecto inesperado: en
Monkey Island (y los demás SCUMM) **Espacio es pausa**, así que con la A por
defecto el juego se pausa; Intro (B) avanza los textos y Esc (TIME) salta
escenas.
