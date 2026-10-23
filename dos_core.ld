/*
 * Retro-Go SD core linker script for the TinyEMU x86 DOS core.
 *
 * Forked from sdk/ld/core_ram_emu.ld per its own "Extending (multi-segment
 * cores)" instructions, to add a GNW_CORE_REGION_ITCM segment. Read that file
 * first -- it is the authoritative reference for the layout, the symbols and
 * the pitfalls; only the deltas are commented here.
 *
 * Reference implementation: cores/pce/pce_core.ld in the firmware tree.
 *
 * Two segments:
 *   0  RAM_EMU  entry trampoline + C code + data + bss   (mandatory, offset 0)
 *   1  ITCM     the hand-written Thumb-2 interpreter hot loop
 *
 * The interpreter's assembly emits into .itcram_hot, which the bare-metal test
 * harness (TEST.ld) maps to ITCMRAM directly. Gathering that same input section
 * into .core_itcm here means ONE assembly source serves both builds -- no
 * per-target section renaming.
 *
 * NOTE: DTCM is deliberately absent. It is not a core load target; it is a
 * firmware bump pool reached via mem_ctl(GW_MEM_DTC, ...) at runtime
 * ("AHB/DTCM are firmware dynamic pools (malloc / dtc_*), not load targets"
 * -- gnw_core_meta.h). The CPU state and guest page table are allocated there
 * at startup and reached through cpu_state_ptr / guest_page_table_ptr.
 *
 * NOTE: taking ITCM as a load segment consumes the same 64K that the firmware
 * otherwise offers as the GW_MEM_ITC bump pool. That is intentional here -- the
 * hot loop is the whole performance thesis -- but it means this core must not
 * also ask for ITC allocations.
 */

INCLUDE gnw_ram_emu.ld
INCLUDE gnw_itcm_core.ld
INCLUDE gnw_ram_uc_core.ld

MEMORY
{
  RAM_EMU (xrw) : ORIGIN = __RAM_EMU_START__,  LENGTH = __RAM_EMU_LENGTH__
  ITCM    (xrw) : ORIGIN = __ITCM_CORE_START__, LENGTH = __ITCM_CORE_LENGTH__
  RAM_UC  (xrw) : ORIGIN = __RAM_UC_CORE_START__, LENGTH = __RAM_UC_CORE_LENGTH__
}

PHDRS
{
  text PT_LOAD FLAGS(5); /* PF_R | PF_X */
  data PT_LOAD FLAGS(6); /* PF_R | PF_W */
  itcm PT_LOAD FLAGS(5); /* PF_R | PF_X */
  ramuc PT_LOAD FLAGS(5); /* PF_R | PF_X */
  ramucbss PT_LOAD FLAGS(6); /* PF_R | PF_W */
}

SECTIONS
{
  .core_entry __RAM_EMU_START__ :
  {
    KEEP(*(.core_entry))
    KEEP(*(.gw_core_bridge_probe))

    . = ALIGN(4);
    *(EXCLUDE_FILE(*emu8950.o *x86_opl.o *x86_mouse.o *x86_nor.o) .text*)
    *(EXCLUDE_FILE(*emu8950.o *x86_opl.o *x86_mouse.o *x86_nor.o) .rodata*)
    *(.glue_7)
    *(.glue_7t)

    . = ALIGN(4);
    __CORE_INIT_ARRAY_START__ = .;
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array*))
    __CORE_INIT_ARRAY_END__ = .;
  } >RAM_EMU :text

  /* No ALIGN before this -- .data may be empty, and an ADVANCE without
     CONTENTS desynchronises __CORE_CODE_END__ from the flat binary. */
  .data :
  {
    *(.data*)
  } >RAM_EMU :data

  __CORE_CODE_END__ = .;

  .bss (NOLOAD) :
  {
    . = ALIGN(8);
    *(EXCLUDE_FILE(*emu8950.o *x86_opl.o *x86_mouse.o *x86_nor.o) .bss*)
    *(EXCLUDE_FILE(*emu8950.o *x86_opl.o *x86_mouse.o *x86_nor.o) COMMON)
    . = ALIGN(8);

    /* `.dtcm_bss` is a TEST.ld section name: in the bare-metal harness it
       lands in real DTCM. THE SHIPPING CORE HAS NO DTCM LOAD SEGMENT (see the
       note at the top of this file), so anything still carrying that attribute
       is ordinary core RAM here -- and it MUST be named, because an ORPHAN
       `.dtcm_bss` is emitted by GCC as PROGBITS and the linker placed it
       between .data and .bss as a LOADED section. That cost 15,616 bytes of
       dos.bin payload to ship a block of zeros, and it is why the section is
       gathered here, inside NOLOAD, rather than left to orphan placement.

       Gathering the wildcard does NOT defeat --gc-sections: collection is per
       INPUT section, so each variable must also carry its own
       `.dtcm_bss.<name>` suffix or one live variable pins every dead one.
       That is exactly what happened -- `cga_expand` held the three
       1,024-entry `io_port_*_table`s (12,288 B, dead in this build because
       their only writer, x86_machine.c's port-map sync, is not compiled into
       the core) resident in RAM_EMU, at four guest page frames. */
    *(.dtcm_bss)
    *(.dtcm_bss.*)
    . = ALIGN(8);
  } >RAM_EMU :data

  __CORE_BSS_END__ = .;

  ASSERT(__CORE_BSS_END__ <= __RAM_EMU_END__,
         "Error: core code+data+bss overflows RAM_EMU")

  /* ------------------------------------------------------------------ */
  /* Segment 1: the interpreter hot loop, executing from 0-wait ITCM.    */
  /* VMA in ITCM, LMA in RAM_EMU so the payload carries the bytes;       */
  /* pack_core.py auto-detects the __CORE_ITCM_* symbol triple.          */
  /* ------------------------------------------------------------------ */
  .core_itcm __ITCM_CORE_START__ : AT(__CORE_CODE_END__)
  {
    __CORE_ITCM_CODE_START__ = .;
    *(.itcram_hot)
    *(.itcram_hot*)
    . = ALIGN(4);
    __CORE_ITCM_CODE_END__ = .;
  } >ITCM :itcm

  /* ------------------------------------------------------------------ */
  /* Segment 2: the AdLib synthesiser, in the LCD's LUT8 bonus area.     */
  /* RAM_EMU is guest frames; the bonus area is the video arena, which   */
  /* only EGA/Mode Y titles fill. emu8950's code, tables and state and   */
  /* x86_opl.c go here (~23 KB), so every title gets those frames back.  */
  /* The firmware switches to LUT8 before loading a RAM_UC segment and   */
  /* lcd_get_bonus_pool() returns what is left after it; bonus_layout()  */
  /* in src/main.c is computed from that at runtime.                     */
  /* ------------------------------------------------------------------ */
  .core_ram_uc __RAM_UC_CORE_START__ : AT(__CORE_CODE_END__ + SIZEOF(.core_itcm))
  {
    __CORE_RAM_UC_CODE_START__ = .;
    *emu8950.o(.text*)
    *emu8950.o(.rodata*)
    *x86_opl.o(.text*)
    *x86_opl.o(.rodata*)
    *x86_mouse.o(.text*)
    *x86_mouse.o(.rodata*)
    *x86_nor.o(.text*)
    *x86_nor.o(.rodata*)
    . = ALIGN(4);
    __CORE_RAM_UC_CODE_END__ = .;
  } >RAM_UC :ramuc

  .core_ram_uc_bss (NOLOAD) :
  {
    *(.ram_uc_bss*)
    *emu8950.o(.bss* COMMON)
    *x86_opl.o(.bss* COMMON)
    *x86_mouse.o(.bss* COMMON)
    *x86_nor.o(.bss* COMMON)
    /* End the segment 2 KB short of a page boundary. The bonus pool starts
       here, bonus_layout() puts the 2 KB palette first and the arena on the
       next page, so this makes that page the very next byte: the gap is
       spent HERE (the mouse driver and the NOR snapshot code fill most of it) instead of being lost to the
       arena's round-up. */
    . += (ALIGN(ABSOLUTE(.) + 2048, 4096) - 2048) - ABSOLUTE(.);
    __CORE_RAM_UC_BSS_END__ = .;
  } >RAM_UC :ramucbss

  ASSERT(__CORE_RAM_UC_BSS_END__ <= __RAM_UC_CORE_END__,
         "Error: the RAM_UC segment overflows the LUT8 bonus area")

  /* Code-only segment: the interpreter's state lives in DTCM (runtime) and
     RAM_EMU (.bss), never in ITCM. */
  __CORE_ITCM_BSS_END__ = __CORE_ITCM_CODE_END__;

  ASSERT(__CORE_ITCM_CODE_END__ <= __ITCM_CORE_START__ + __ITCM_CORE_LENGTH__,
         "Error: interpreter hot loop overflows the 64K ITCM segment")

  __exidx_start = .;
  __exidx_end = .;

  /DISCARD/ :
  {
    *(.fini_array*)
    *(.preinit_array*)
    *(.ARM.exidx*)
    *(.ARM.extab*)
    *(.comment)
    *(.ARM.attributes)
  }
}
