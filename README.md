# TinyEMU x86 para Game & Watch (retro-go SD)

Un PC con MS-DOS para la Game & Watch, como núcleo externo del firmware
**retro-go SD** (rama `external_cores`, ABI v2). Parte del
[TinyEMU de Fabrice Bellard](docs/tinyemu-original.md), con la CPU x86
reescrita en ensamblador para el STM32H7B0.

- **No hace falta instalar MS-DOS**: el núcleo trae un DOS propio (atiende él
  las llamadas de DOS del juego), así que cada juego arranca directamente.
- CPU 386 en modo real (también arranca algo de modo protegido), CGA, EGA y VGA
  (modo 13h y Mode Y), AdLib, Sound Blaster, ratón, EMS y XMS.
- El juego ve 640 KB, pero la consola tiene mucha menos RAM: lo que no cabe se
  pagina en un archivo de la tarjeta SD.

Para juegos de PC XT (8088, CGA) existe el otro núcleo,
[gw-dos-xtulator](https://github.com/julenvitoria/gw-dos-xtulator) (`dos.bin`).
Los dos conviven en la misma tarjeta.

## Instalación en la tarjeta SD

1. Copia el núcleo a `/cores/`. Al compilarse sale como `dos.bin`; si también
   usas XTulator (que se llama igual), renómbralo, por ejemplo a `dos386.bin`.
2. Copia los juegos a `/roms/dos/` como imágenes **`.dsk`**.
3. Al lado de cada juego puede ir su `.cfg` con el mismo nombre
   (`MONKEY.dsk` → `MONKEY.cfg`). Ver más abajo.

### Las imágenes `.dsk`

Un `.dsk` es una imagen de disco FAT normal: un disquete (360 KB … 2,88 MB) o un
disco duro con su tabla de particiones. Una `.img` de disquete basta con
renombrarla. El núcleo monta esa imagen como la unidad del juego (`A:` y `C:`
son el mismo disco).

Qué programa se arranca, por orden:

1. El que diga el `.cfg` (sección `[autoexec]`, ver abajo).
2. El que diga el `AUTOEXEC.BAT` de dentro del `.dsk` (se entienden `cd` y una
   línea con el programa; `CAT` busca `CAT.COM` y luego `CAT.EXE`).

Si el juego no trae `AUTOEXEC.BAT`, lo más sencillo es crearle uno de una línea
con el nombre del ejecutable (por ejemplo `MONKEY`).

## Configuración por juego (`.cfg`)

Al lado de cada juego puede ir un `.cfg` con su mismo nombre (`MONKEY.dsk` →
`MONKEY.cfg`), en el formato de DOSBox. Un ejemplo:

```ini
[cpu]
cycles = 2500        ; velocidad (como en DOSBox); max = lo que dé la consola

[autoexec]
cd \MONKEY
monkey a

[gamepad]
a      = enter       ; qué tecla del PC manda cada botón
mouse  = on          ; ratón con la cruceta
```

- **Todas las opciones** (`[cpu]`, `[dos]`, `[autoexec]`, `[gamepad]`), los
  perfiles de CPU, las opciones de sonido de los propios juegos (como el `a`
  de `monkey a`, que elige AdLib) y qué tarjetas se emulan:
  [`docs/cfg.md`](docs/cfg.md).
- **Nombres de teclas** para los botones, y cómo saber qué teclas usa un juego:
  [`docs/input/06-gamepad-keys.md`](docs/input/06-gamepad-keys.md).

## Controles

**Botones por defecto** (sin `[gamepad]`): cruceta = flechas, A = Espacio,
B = Esc, TIME = Y, START = Intro, SELECT = N (START y SELECT solo en las
consolas que los tienen).

**GAME** abre el teclado en pantalla y va pasando por teclado → ratón → cerrar.

- **Teclado** (dos filas arriba y dos abajo, el juego no se mueve): cruceta para
  moverse (mantenida sigue avanzando), A pulsa, B es Retroceso, TIME es Shift.
- **Shift, Ctrl y Alt** se quedan pulsados para la siguiente tecla (verde).
  **Shift pulsado dos veces queda bloqueado** (rojo, como Bloq Mayús); una
  tercera vez lo suelta.
- La tecla **`:`** manda los dos puntos de una vez, sin pasar por Shift.
- La tecla verde **`A:`** abre la lista de `.dsk` de la carpeta: ↑/↓ elige,
  A lo carga en lugar del disco actual, B vuelve. Sirve para juegos de varios
  discos. La **`X`** verde cierra el teclado.
- **Ratón**: la cruceta mueve el puntero, B/A son los botones izquierdo y
  derecho, TIME vuelve al teclado.

Más detalle: [`docs/input/03-onscreen-keyboard.md`](docs/input/03-onscreen-keyboard.md).

## Para desarrolladores

- [`STATUS.md`](STATUS.md): estado actual. [`PLAN.md`](PLAN.md): hoja de ruta.
- [`TOOLING.md`](TOOLING.md): compilar (`make -f Makefile.core pack`), los
  scripts y los tres niveles de prueba (gwemu, gwemu + retro-go, consola).
- [`docs/`](docs/): diseño de cada parte (memoria, CPU, vídeo, temporizador…).
- [`CLAUDE.md`](CLAUDE.md): las reglas del proyecto.

## Licencia

MIT, como el TinyEMU original ([`MIT-LICENSE.txt`](MIT-LICENSE.txt)).
