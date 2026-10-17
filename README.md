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

Usa el formato de DOSBox, así que se puede copiar casi tal cual un
`dosbox.conf`. Todo es opcional:

```ini
[cpu]
cycles = 2500        ; miles de instrucciones por segundo (como en DOSBox); max = lo que dé

[dos]
xms = true           ; o false, o un tamaño: xms_kb = 2048
ems = true           ; o false, o ems_kb = 1024 (múltiplos de 16)

[autoexec]
; directorio de trabajo y programa a ejecutar (con sus parámetros si los lleva)
cd \MONKEY
monkey

[gamepad]
a      = enter       ; qué tecla del PC manda cada botón
b      = esc
start  = f5
select = none
repeat = on          ; repetición al mantener pulsado
mouse  = on          ; ratón: la cruceta mueve el puntero, A/B son los botones
```

- `;` o `#` empiezan un comentario (al principio de la línea, o detrás de un espacio). En `[autoexec]` solo a principio de línea, porque una orden de DOS puede llevarlos.
- En `[autoexec]` las líneas típicas de DOSBox (`mount`, `imgmount`, `echo`,
  `cls`, `c:`…) se ignoran sin dar error; cuenta el `cd` y el último programa.
- **Teclas de los botones**: la lista completa de nombres (y cómo saber qué
  teclas usa un juego) está en
  [`docs/input/06-gamepad-keys.md`](docs/input/06-gamepad-keys.md).
- **Velocidad de la CPU**: también se cambia desde el menú de retro-go
  (PAUSE/SET → CPU Speed), con perfiles de 8088 a 486. Algunos juegos de la
  época fallan si la CPU va demasiado rápida.

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
