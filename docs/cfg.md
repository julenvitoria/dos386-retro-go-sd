# El `.cfg` de cada juego

*[Read in English](cfg.en.md)*

*Todo lo que se puede poner en el `.cfg` de un juego para el núcleo TinyEMU
(`dos386.bin`). Vuelve al [README](../README.md#configuración-por-juego-cfg).*

## Dónde va

Junto a la imagen y con el mismo nombre: `/roms/dos/MONKEY.dsk` →
`/roms/dos/MONKEY.cfg`. Todo es opcional: sin `.cfg` el juego arranca con lo
que diga el `AUTOEXEC.BAT` de su `.dsk` y con los botones por defecto.

## Formato

Es el de DOSBox, así que se puede copiar casi tal cual un `dosbox.conf`:
secciones entre corchetes y `clave = valor` dentro. Mayúsculas o minúsculas, da
igual.

- `;` o `#` al principio de una línea, o detrás de un espacio, empiezan un
  comentario. En `[autoexec]` solo al principio de la línea, porque una orden
  de DOS puede llevar esos caracteres.
- Una línea que no se entiende se cuenta como error y esa opción se queda como
  estaba; el resto del archivo se aplica igual.

## Opciones

### `[cpu]`

| Clave | Valores | Qué hace |
|---|---|---|
| `cycles` | `max`, o un número | Velocidad de la CPU en miles de instrucciones por segundo, como en DOSBox. Se redondea al perfil más cercano de la tabla de abajo. |
| `cpu_profile` | `0` … `5` | El perfil por su número, directamente. |

Perfiles (los mismos que el menú de retro-go, PAUSE/SET → CPU Speed):

| Nº | Perfil | `cycles` |
|---|---|---|
| 0 | Max (lo que dé la consola) | `max` |
| 1 | 8088 a 4,77 MHz (PC XT) | 330 |
| 2 | 286 a 8 MHz | 800 |
| 3 | 286 a 12 MHz | 1200 |
| 4 | 386SX a 16 MHz | 2500 |
| 5 | 386DX a 25 MHz | 4500 |

La consola da unas 5.500 k, así que no hay perfiles más rápidos que el 386DX
25: un 386DX 33 o un 486 irían igual que Max. Un `cycles` de 5500 o más es Max;
un `cpu_profile` antiguo de 6 a 8 también pasa a Max.

Algunos juegos de la época fallan o van demasiado rápidos con una CPU rápida;
para esos, un perfil lento.

### `[dos]`

| Clave | Valores | Qué hace |
|---|---|---|
| `xms` | `true` / `false` | Memoria XMS (4 MB con `true`). |
| `ems` | `true` / `false` | Memoria EMS (1 MB con `true`). |
| `xms_kb` | KB | XMS con un tamaño concreto. |
| `ems_kb` | KB, múltiplo de 16 | EMS con un tamaño concreto. |

### `[autoexec]`

Qué programa se arranca. No es un archivo `.bat` completo: el núcleo trae su
propio DOS y lanza **un** programa, así que de esta sección toma dos cosas:

- `cd <carpeta>` (o `cd\carpeta`): el directorio de trabajo.
- Cualquier otra línea es el programa a ejecutar, con sus parámetros. Si hay
  varias, cuenta la última.
- `mount`, `imgmount`, `echo`, `cls`, `pause`, `exit`, `set`, `keyb`, `c:`…
  se ignoran sin dar error, para poder pegar un `[autoexec]` de DOSBox.

Sin `[autoexec]` manda el `AUTOEXEC.BAT` del `.dsk`.

Lo mismo se puede escribir con claves sueltas, en cualquier sección:
`entry` (el programa), `args` (sus parámetros), `cwd` (la carpeta) y
`use_cmdtail` (`true`/`false`: si se pasan los parámetros que traiga el
`AUTOEXEC.BAT` del juego cuando el `.cfg` no pone ninguno).

### `[gamepad]`

| Clave | Valores | Qué hace |
|---|---|---|
| `up` `down` `left` `right` (o `arriba` `abajo` `izquierda` `derecha`), `a`, `b`, `time`, `start` (o `x`), `select` (o `y`) | nombre de tecla, o hasta 3 con `+` | Qué tecla del PC manda cada botón. `game` no se puede cambiar: abre el teclado en pantalla. Lista de nombres: [`input/06-gamepad-keys.md`](input/06-gamepad-keys.md). |
| `repeat` | `on` / `off` | Repetición al mantener un botón pulsado (por defecto `on`). |
| `mouse` | `on` / `off` | Ratón: la cruceta mueve el puntero y A/B son los botones izquierdo y derecho (salvo que los cambies). |

## Opciones de los propios juegos (sonido, vídeo…)

Esto ya no es del emulador sino de cada juego: muchos se configuran con una
letra o una palabra detrás del nombre del ejecutable, o con un programa de
instalación. Aquí se ponen en `[autoexec]` (por ejemplo `monkey a`).

- **Juegos de LucasArts de la época (Monkey Island 1, Loom, Indiana Jones…)**:
  una letra detrás del ejecutable elige la tarjeta de sonido: `a` = AdLib,
  `r` = Roland MT-32 (no está emulada: sin música), `i` = altavoz del PC. Sin
  letra, el juego busca la tarjeta solo. Con una Sound Blaster se usa `a`: la
  música de estos juegos es FM, y la Sound Blaster lleva el mismo chip que la
  AdLib.
- **Juegos con programa de configuración** (`SETUP.EXE`, `INSTALL.EXE`,
  `SOUND.EXE`…): pon ese programa en `[autoexec]`, arráncalo una vez, elige la
  tarjeta (AdLib, o Sound Blaster en 220h / IRQ 5 / DMA 1), y luego vuelve a
  poner el programa del juego: la elección queda guardada en su carpeta.
- **Para saber qué opciones tiene un juego**: el manual o el `README`/`LEEME`
  de su carpeta.

Qué emula este núcleo: AdLib, Sound Blaster (220h, IRQ 5, DMA 1, DMA alto 5:
`BLASTER=A220 I5 D1 H5 T6`, que el juego ya encuentra puesto) y altavoz del
PC. No emula Roland MT-32, Gravis Ultrasound ni Tandy/PCjr.
