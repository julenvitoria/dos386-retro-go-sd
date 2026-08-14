CC = arm-none-eabi-gcc
AS = arm-none-eabi-as
LD = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE = arm-none-eabi-size

CFLAGS = -mcpu=cortex-m7 -mthumb -O2 -Wall -g -ffunction-sections -fdata-sections
CFLAGS += -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_GNU_SOURCE
CFLAGS += -DCONFIG_X86EMU -DCONFIG_VERSION=\"1.0\"

# The Retro-Go SD external-core SDK is VENDORED into this repo at sdk/, per the
# template's own model ("headers, bridge trampolines, linker scripts and packers
# are vendored under sdk/; you do not need a firmware checkout to compile").
#
# NOTHING may link against references/ -- that tree is temporary and strictly
# for reading. To pick up upstream retro-go changes, re-vendor:
#     scripts/sync_from_firmware.sh <path-to-game-and-watch-retro-go-sd>
# and update SDK_VERSION. Staying current is a core owner's responsibility.
SDK = sdk
SDK_INC = $(SDK)/include

# Tier-2 flags. Deliberately free of any retro-go/SDK include path: the
# bare-metal CPU harness (test_core.elf) depends only on lib/FatFs (our own
# submodule) and our own headers, which is what keeps it a clean, isolated
# measurement tier.
CFLAGS += -DIS_LITTLE_ENDIAN -Ilib/FatFs
# The bare-metal harness is NOT constrained by RAM_EMU -- it is its own image --
# so it always builds the anti-aliased text path, and tests/test_text.py checks
# both renderers. The packed core defaults it OFF; see Makefile.core.
CFLAGS += -DX86_TEXT_AA=1

# Per-opcode DWT cycle profiling. OFF by default: the instrumentation sits in
# the fetch/dispatch tail, the hottest code in the project, and perturbs every
# timing-dependent thing in the emulator. Flags are not dependencies -- rebuild
# from clean when this changes. See x86_profile.h and scripts/profile_opcodes.py.
X86_PROFILE ?= 0
CFLAGS += -DX86_PROFILE=$(X86_PROFILE)

# Frame-loop zone accounting (x86_zones.h). The bare-metal harness has no frame
# loop, so this only ever compiles the storage; it exists here so the two builds
# agree on the flag. See Makefile.core and scripts/profile_all.py.
X86_ZONES ?= 0
CFLAGS += -DX86_ZONES=$(X86_ZONES)

# Probe accumulator (x86_probe.h): what the guest asked for and how we answered.
# OFF by default -- it emits ~2.8 KB of RAM_EMU storage, and RAM_EMU headroom in
# this tree is small enough to have gone negative unnoticed. Not on the guest
# dispatch path; see x86_probe.h for the cost and scripts/probe_report.py for
# the measured link delta.
X86_PROBE ?= 0
CFLAGS += -DX86_PROBE=$(X86_PROBE)

LDFLAGS = -mcpu=cortex-m7 -mthumb -T STM32H7B0VBTx_SDCARD.ld -Wl,--gc-sections --specs=nosys.specs

PROGS = test_core.elf

TEST_OBJS = x86_cpu.o x86_nor.o x86_profile.o x86_probe.o x86_io.o x86_mem.o x86_arena.o x86_pager.o x86_video.o x86_video_planar.o x86_osk.o x86_hle.o x86_bios.o x86_dos.o cga_blit.o cpu_exec.o tests/main.o hw_accel.o x86_disk.o lib/FatFs/ff.o lib/FatFs/ffunicode.o

all: $(PROGS)

test_core.elf: $(TEST_OBJS) TEST.ld
	$(CC) -mcpu=cortex-m7 -mthumb -T TEST.ld -Wl,--gc-sections --specs=nosys.specs -o $@ $(TEST_OBJS)
	$(SIZE) $@

# X86CPUState field offsets, generated on the host so the Thumb-2 core and the
# C struct cannot drift apart. Built with the host compiler, not $(CC).
x86_offsets.inc: offset.c x86_cpu.h cutils.h
	cc -I. -o offset_gen offset.c
	./offset_gen > $@
	rm -f offset_gen

cpu_exec.o: x86_offsets.inc cpu_exec_handlers.S cpu_exec_handlers_0f.S \
            opcode_table.S opcode_0f_table.S

# Header dependencies. Without these, editing a header leaves every .o that
# included it stale and the harness measures the PREVIOUS build -- which is how
# a rewritten font8x8.h produced a "fixed" renderer that still drew the old
# glyphs. sdk/Makefile has done this all along; this build had not.
%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(TEST_OBJS:.o=.d)

qemu_bank1.bin: test_core.elf
	$(OBJCOPY) -O binary $< test_core_intflash.bin
	dd if=test_core_intflash.bin of=qemu_bank1.bin bs=256k count=1 conv=sync 2>/dev/null

qemu_bank2.bin: qemu_bank1.bin
	dd if=/dev/zero of=qemu_bank2.bin bs=256k count=1 2>/dev/null

# The guest DISK IMAGE. DOS titles ship as .dsk floppy images, not bare
# executables -- a .dsk is a FAT12 filesystem, which is exactly what vdisk.c
# already mounts, and it carries the game's own files rather than one .EXE
# lifted out of context. The sibling .cfg/.dosmeta describe it to the launcher.
GUEST_DSK ?= references/game-and-watch-retro-go-sd/roms/dos/ALLEYCAT.dsk
GUEST_DSK_NAME := $(notdir $(GUEST_DSK))

# Tier-2 extflash: the .dsk IS a FAT12 image, so it is used directly as the
# backing store that vdisk_init() maps -- no repackaging.
extflash.bin: $(GUEST_DSK)
	dd if=/dev/zero of=extflash.bin bs=1M count=64 2>/dev/null
	dd if=$(GUEST_DSK) of=extflash.bin conv=notrunc 2>/dev/null
# Bare 1 MB unformatted card, enough for the tier-2 bare-metal harness which
# never touches it.
sdcard.img:
	dd if=/dev/zero of=sdcard.img bs=1M count=1 2>/dev/null

# ---------------------------------------------------------------------------
# SD card content is NOT built here.
#
# make compiles; Python composes and deploys. The card has to reach three
# destinations -- a gwemu image, a mounted card, and the device over the probe
# -- and that is one recipe with three backends, not an mtools pipeline.
#
#   python3 scripts/sdcard.py --image sdcard_retrogo.img
#   python3 scripts/sdcard.py --mount /media/.../RETROGO
#   python3 scripts/sdcard.py --probe
#
# dos.bin is built by Makefile.core. It is declared PHONY-ish here so the SD
# image can never be assembled around a stale core -- the previous split let
# `make retrogo_media` happily package a core you had not rebuilt.
# ---------------------------------------------------------------------------
RETROGO_SD ?= sdcard_retrogo.img

# Ordered steps, not a dependency graph. `dos.bin: core` looked right but was
# a trap: the dos.bin target had no recipe of its own, so make ran `core`
# (which rewrites dos.bin) yet never re-stat'd it -- and happily packaged a
# stale core into the SD image. sdcard.py rebuilds the image unconditionally,
# so there is nothing for timestamps to save here anyway.
.PHONY: core sdcard
core:
	$(MAKE) -f Makefile.core pack

sdcard: core
	python3 scripts/sdcard.py --image $(RETROGO_SD)

gwemu_media: qemu_bank1.bin qemu_bank2.bin extflash.bin sdcard.img

clean:
	rm -f *.o *.d *~ tests/*.o $(PROGS) *.bin *.img
