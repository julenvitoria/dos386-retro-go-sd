/*
 * TinyEMU x86 — Retro-Go SD external core.
 *
 * The emulator loop: guest memory, CGA video, PC speaker and input, driven a
 * frame at a time. What it does NOT have yet is the DOS layer (#3), so the
 * guest it runs is a built-in real-mode program rather than a loaded .EXE --
 * see demo_guest[]. That program drives the same paths a real game does
 * (segment loads, REP STOSW into video memory, port writes), so video, audio
 * and input are exercised end to end rather than asserted to work.
 *
 * It also still proves the pieces the architecture rests on:
 *
 *   1. The hand-written Thumb-2 interpreter runs from a GNW_CORE_REGION_ITCM
 *      segment (see dos_core.ld) -- the entire performance thesis.
 *   2. CPU state and the guest page table can live in DTCM, which for a core
 *      means a runtime mem_ctl(GW_MEM_DTC, ...) allocation rather than a
 *      statically-placed section ("AHB/DTCM are firmware dynamic pools, not
 *      load targets" -- gnw_core_meta.h).
 *   3. The benchmark and the opcode suite produce the same numbers here as in
 *      the bare-metal harness, so tier 2 and tier 3 are comparable.
 *
 * Video, input, sound and the DOS layer are deliberately absent.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "common.h"
#include "gw_lcd.h"
#include "rom_manager.h"
#include "odroid_system.h"
#include "gw_malloc.h"

#include "gw_core_bridge.h"

#include "cutils.h"   /* BOOL/TRUE, needed by iomem.h via x86_cpu.h */
#include "x86_cpu.h"

#define APP_ID       100
#define SAMPLE_RATE  16000

/* Guest RAM for the spike.
 *
 * AXI budget, measured against the built firmware rather than assumed:
 *   RAM_EMU      724 KB  (__RAM_EMU_LENGTH__ = 0xb5000) -- code + data + bss
 *   bonus pool  ~150 KB  reclaimed by running the LCD in LUT8
 *   total       ~874 KB  which is docs/01's "~870 KB AXI pool"
 *
 * The LCD pool is statically sized at 300 KB for the largest mode (2x RGB565).
 * Switching to LUT8 halves the framebuffer requirement and the firmware frees
 * the upper half as a bonus pool -- so the reclaimed AXI is NOT extra RAM_EMU,
 * it is a separate region fetched at runtime via GW_LCD_GET_BONUS_POOL.
 *
 * The spike's benchmark needs only a few KB (code at 0, IDT at 0x1000, ISR at
 * 0x2000, stack at 0x3000), so it runs in 64 KB; the bonus pool is claimed and
 * reported here purely to prove the mechanism and the real budget before the
 * paging architecture is built on top of it. */
/* Conventional memory. A DOS game wants 640 KB; VRAM is a separate dedicated
 * block so the video aperture never moves and never competes with the pool. */
#define GUEST_RAM_SIZE   (640 * 1024)
#define FRAME_W          320
#define FRAME_H          240

extern void x86_cpu_interp_asm(void);
static void emulator_loop(const gw_firmware_abi_t *abi);
static int mount_guest_disk(void);

/* The interpreter dereferences these; it never references the symbols
 * directly, which is what lets one assembly source serve both builds. */
extern X86CPUState *cpu_state_ptr;
extern uint32_t *guest_page_table_ptr;

static uint8_t *guest_ram;
static uint8_t *guest_vram;
static char dsk_path[256];
volatile uint32_t dsk_mounted = 0;
volatile uint32_t dsk_exec_rc = 0xFFFFFFFF;
volatile uint32_t dsk_startup_ok = 0;
static uint32_t core_clut[16];
static uint8_t *upper_filler;

/* Host-readable so a failure to place either block is visible rather than a
   black screen. */
volatile uint32_t guest_ram_bytes = 0;
volatile uint32_t guest_vram_ok = 0;
volatile uint32_t frames_rendered = 0;
volatile uint32_t guest_exit_reason = 0;
volatile uint32_t guest_halted = 0;
volatile uint32_t fb_addr = 0;        /* the buffer the blit was handed */
volatile uint32_t fb_sample = 0;      /* first 4 pixels of the picture area */
volatile uint32_t clut_sample = 0;    /* colour 1, to prove the CLUT took */
volatile uint32_t hle_calls_seen = 0;
volatile uint32_t hle_bad_vector = 0;
volatile uint32_t hle_bad_func = 0;
volatile uint32_t guest_ax = 0;

/*
 * Built-in guest, 16-bit real mode, entered at 0000:0100.
 *
 * Sets the CGA colour-select register, fills the video aperture through
 * ES:DI with REP STOSW, programs PIT channel 2 and opens the speaker gate,
 * then spins. Every one of those goes through a path this core implements
 * from scratch, so if the screen or the tone is wrong the failure is here and
 * not in a missing DOS layer.
 */
static const uint8_t demo_guest[] = {
    /* INT 10h AH=0Eh: teletype a character through the BIOS */
    0xB4, 0x0E,                   /* mov ah, 0x0E        */
    0xB0, 0x41,                   /* mov al, 'A'         */
    0xCD, 0x10,                   /* int 0x10            */
    /* INT 21h AH=30h: ask DOS its version */
    0xB4, 0x30,                   /* mov ah, 0x30        */
    0xCD, 0x21,                   /* int 0x21            */
    /* colour select: palette 1, high intensity */
    0xBA, 0xD9, 0x03,             /* mov dx, 0x03D9      */
    0xB0, 0x30,                   /* mov al, 0x30        */
    0xEE,                         /* out dx, al          */
    /* mode: graphics, video enabled */
    0xBA, 0xD8, 0x03,             /* mov dx, 0x03D8      */
    0xB0, 0x0A,                   /* mov al, 0x0A        */
    0xEE,                         /* out dx, al          */
    /* fill 16 KB of VRAM: ES = 0xB800, DI = 0, CX = 8192, AX = pattern */
    0xB8, 0x00, 0xB8,             /* mov ax, 0xB800      */
    0x8E, 0xC0,                   /* mov es, ax          */
    0x31, 0xFF,                   /* xor di, di          */
    0xB9, 0x00, 0x20,             /* mov cx, 8192        */
    0xB8, 0x1B, 0x6C,             /* mov ax, 0x6C1B      */
    0xF3, 0xAB,                   /* rep stosw           */
    /* speaker: 0xB6 to the PIT control port, divisor 0x0497 (~1 kHz) */
    0xB0, 0xB6,                   /* mov al, 0xB6        */
    0xE6, 0x43,                   /* out 0x43, al        */
    0xB0, 0x97,                   /* mov al, 0x97        */
    0xE6, 0x42,                   /* out 0x42, al        */
    0xB0, 0x04,                   /* mov al, 0x04        */
    0xE6, 0x42,                   /* out 0x42, al        */
    0xE4, 0x61,                   /* in  al, 0x61        */
    0x0C, 0x03,                   /* or  al, 3           */
    0xE6, 0x61,                   /* out 0x61, al        */
    0xEB, 0xFE,                   /* jmp $               */
};

/* Host-visible results, read over the debug backend exactly as in the
 * bare-metal harness so the same Python instruments work against both. */
/* Signature, not a flag. Until the firmware loads us, these addresses hold
 * whatever the firmware left in RAM_EMU, so a "non-zero means ready" test
 * latches onto garbage. The host waits for exactly this value. */
#define CORE_MAGIC 0x54494E59u   /* 'TINY' */
volatile uint32_t core_magic = 0;

/* ITCM integrity checkpoints.
 *
 * One byte of our loaded interpreter (offset 0x120, inside MEM_GET_PTR's BFI)
 * differs from the ELF on the target, and the CPU dies on UNDEFINSTR. These
 * bracket the suspects so the answer is measured rather than argued: if the
 * sum changes across init, our own startup did it; across the benchmark, the
 * interpreter did it to itself. */
#define ITCM_BASE   ((const volatile uint32_t *)0x00000000u)
#define ITCM_WORDS  (21832u / 4u)
volatile uint32_t itcm_sum_entry = 0;
volatile uint32_t itcm_sum_pre_bench = 0;
volatile uint32_t itcm_sum_post_bench = 0;
volatile uint32_t itcm_byte_120_entry = 0;
volatile uint32_t itcm_byte_120_pre = 0;
volatile uint32_t itcm_byte_120_post = 0;

static uint32_t itcm_sum(void)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < ITCM_WORDS; i++) {
        sum += ITCM_BASE[i];
    }
    return sum;
}

static uint32_t itcm_byte_120(void)
{
    return *(const volatile uint8_t *)0x00000120u;
}
volatile uint32_t core_ready = 0;
volatile uint32_t core_dtcm_ok = 0;
volatile uint32_t bonus_pool_base = 0;   /* AXI reclaimed by running LUT8 */
volatile uint32_t bonus_pool_size = 0;
volatile uint32_t benchmark_cycles = 0;
volatile uint32_t benchmark_insns = 0;
volatile uint32_t run_benchmark_flag = 0;

#define DWT_CTRL        (*(volatile uint32_t *)0xE0001000)
#define DWT_CYCCNT      (*(volatile uint32_t *)0xE0001004)
#define CoreDebug_DEMCR (*(volatile uint32_t *)0xE000EDFC)

/* Allocate the 0-wait-state state from the firmware's DTCM bump pool. */
static bool dtcm_state_init(void)
{
    const gw_firmware_abi_t *abi = gw_firmware_abi();

    X86CPUState *st = (X86CPUState *)abi->mem_ctl(
        GW_MEM_OP_ALLOC, GW_MEM_DTC, 1, sizeof(X86CPUState));
    /* The full index range, not just the mapped pages: the 12-bit lookup can
     * produce any of FOLD_ENTRIES, and a short table is read past its end. */
    uint32_t *pt = (uint32_t *)abi->mem_ctl(
        GW_MEM_OP_ALLOC, GW_MEM_DTC, FOLD_ENTRIES, sizeof(uint32_t));

    /* Mode records. Over-allocated by one record so the base can be aligned
     * up to 8 KB, which the 66/67 EOR toggles require. */
    uint8_t *mr = (uint8_t *)abi->mem_ctl(
        GW_MEM_OP_ALLOC, GW_MEM_DTC, 1,
        X86_MODE_RECORDS_BYTES + X86_MODE_RECORDS_ALIGN);

    if (!st || !pt || !mr) {
        return false;   /* loud, not silent: core_dtcm_ok stays 0 */
    }

    cpu_state_ptr = st;
    guest_page_table_ptr = pt;
    x86_build_mode_records(mr);
    return true;
}

/* Same synthetic benchmark as tests/main.c: 500,000 iterations of
 * DEC EAX / JNZ, then HLT. Exactly 1,000,001 instructions. */
static void run_benchmark(void)
{
    memset(cpu_state_ptr, 0, sizeof(X86CPUState));
    for (int i = 0; i < 6; i++) {
        cpu_state_ptr->segs[i].limit = 0xFFFFFFFF;
    }
    /* CS.D: this benchmark's guest code is 32-bit. A zeroed struct means a
       16-bit code segment, which is now genuinely different. */
    cpu_state_ptr->segs[1].flags |= X86_DESC_D_FLAG;

    memset(guest_ram, 0, GUEST_RAM_SIZE);
    guest_ram[0] = 0x48;   /* DEC EAX      */
    guest_ram[1] = 0x75;   /* JNZ  -3      */
    guest_ram[2] = 0xFD;
    guest_ram[3] = 0xF4;   /* HLT          */

    /* Unmapped everywhere, then the guest RAM window. Entries are offsets:
     * host = guest_linear + k. Anything outside the window now faults with
     * X86_EXIT_MEM_FAULT instead of writing into our own ITCM code. */
    for (uint32_t i = 0; i < FOLD_ENTRIES; i++) {
        guest_page_table_ptr[i] = FOLD_INVALID;
    }
    for (uint32_t i = 0; i < GUEST_RAM_SIZE / 4096u; i++) {
        guest_page_table_ptr[i] = FOLD_K((uint32_t)guest_ram + i * 4096, i);
    }

    /* Hold the PIT off so the guest loop runs to its own HLT and the
     * instruction count is real rather than a host-side assumption. */
    cpu_state_ptr->pit_reload = 0xFFFFFFFF;
    cpu_state_ptr->pit_counter = 0xFFFFFFFF;
    cpu_state_ptr->regs[0] = 500000;   /* EAX: iterations */
    cpu_state_ptr->regs[4] = 0x3000;   /* ESP             */

    uint32_t start = DWT_CYCCNT;
    x86_cpu_interp_asm();
    uint32_t end = DWT_CYCCNT;

    benchmark_cycles = end - start;
    benchmark_insns = 2u * (500000u - cpu_state_ptr->regs[0]) + 1u;
}

void app_main(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    (void)load_state; (void)start_paused; (void)save_slot;

    ram_start = (uint32_t)&__CORE_BSS_END__;
    core_magic = CORE_MAGIC;      /* we are alive and this is our RAM_EMU */
    itcm_sum_entry = itcm_sum();
    itcm_byte_120_entry = itcm_byte_120();
    odroid_system_init(APP_ID, SAMPLE_RATE);

    CoreDebug_DEMCR |= (1 << 24);
    DWT_CYCCNT = 0;
    DWT_CTRL |= 1;

    core_dtcm_ok = dtcm_state_init() ? 1 : 0;

    const gw_firmware_abi_t *abi = gw_firmware_abi();

    /* Run the LCD in LUT8 so the upper half of the 300 KB framebuffer pool is
     * released as a bonus pool. This is where the AXI that docs/01 budgets as
     * "~870 KB" actually comes from -- RAM_EMU alone is only 724 KB. */
    abi->lcd_ctl(GW_LCD_SETUP_FB, LCD_MODE_LUT8, 0, 0);

    uint8_t *bonus = NULL;
    size_t bonus_sz = 0;
    abi->lcd_ctl(GW_LCD_GET_BONUS_POOL, (uintptr_t)&bonus,
                 (uintptr_t)&bonus_sz, 0);
    bonus_pool_base = (uint32_t)bonus;
    bonus_pool_size = (uint32_t)bonus_sz;

    /* VRAM first and from RAM_EMU, not the bonus pool: the bonus region is
     * uncached, and the blitter reads all 16 KB of VRAM every frame. Both the
     * interpreter's writes and the blitter's reads are this same core, so
     * there is no coherency reason to give that up. */
    guest_vram = (uint8_t *)abi->mem_ctl(GW_MEM_OP_ALLOC, GW_MEM_RAM,
                                         1, X86_VRAM_BYTES);
    guest_vram_ok = guest_vram ? 1 : 0;

    guest_ram = (uint8_t *)abi->mem_ctl(GW_MEM_OP_ALLOC, GW_MEM_RAM,
                                        1, GUEST_RAM_SIZE);
    upper_filler = (uint8_t *)abi->mem_ctl(GW_MEM_OP_ALLOC, GW_MEM_RAM, 1, 4096);
    guest_ram_bytes = guest_ram ? GUEST_RAM_SIZE : 0;

    if (guest_vram) {
        /* The CLUT the LCD scans out through. LUT8 gives 256 entries; CGA
         * mode 4 uses four and the palette registers pick which. */
        /* ARGB8888, and only the four CGA colours: lcd_set_clut() caps the
         * count at 32 and reads 32-bit entries. */
        x86_video_init(guest_vram, core_clut);
        abi->lcd_ctl(GW_LCD_SET_CLUT, (uintptr_t)core_clut, 4, 0);
        cga_clut_dirty = 0;
    }

    core_ready = (core_dtcm_ok && guest_ram && guest_vram) ? 1 : 0;

    itcm_sum_pre_bench = itcm_sum();
    itcm_byte_120_pre = itcm_byte_120();

    /* The benchmark is NOT run automatically. It faults, the device reboots,
     * and the reboot wipes the very globals we need to read -- so polling from
     * the host races a reset. Staying alive here makes the checkpoints
     * readable; the host triggers the run deliberately via run_benchmark_flag. */

    mount_guest_disk();
    emulator_loop(abi);
}

/*
 * Set the guest up in real mode with conventional RAM mapped flat, VRAM at the
 * video aperture, and the demo program at 0000:0100.
 */
/*
 * Sector reads for our FatFs, served straight off the SD card.
 *
 * rg_storage_copy_file_range_to_ram reads a byte range of a file, which is
 * exactly a sector fetch, and it holds no handle open between calls -- so the
 * 1.44 MB image costs no AXI at all and any .dsk in /roms/dos works without a
 * rebuild.
 */
static int sd_sector_read(void *ctx, void *dst, uint32_t offset, uint32_t len)
{
    const gw_firmware_abi_t *abi = gw_firmware_abi();
    size_t got = abi->rg_storage_copy_file_range_to_ram(
        (char *)ctx, (uint8_t *)dst, offset, len, NULL);
    return (got == len) ? 0 : -1;
}

/*
 * Work out what to run. The disk images are built to boot FreeDOS, so the
 * program to launch is named in AUTOEXEC.BAT rather than being guessable from
 * the .dsk name -- ALLEYCAT.dsk runs CAT.EXE. Reading it keeps this working
 * for the other images too instead of hardcoding one game.
 */
static int find_startup_program(char *out, unsigned max)
{
    FIL f;
    if (f_open(&f, "AUTOEXEC.BAT", FA_READ) != FR_OK)
        return -1;

    /* Read it whole and split by hand: f_gets needs FF_USE_STRFUNC, which this
       FatFs build does not enable, and AUTOEXEC.BAT is never large. */
    static char buf[1024];
    UINT got = 0;
    FRESULT rc = f_read(&f, buf, sizeof(buf) - 1, &got);
    f_close(&f);
    if (rc != FR_OK || got == 0)
        return -1;
    buf[got] = 0;

    char *p = buf;
    while (*p) {
        char *line = p;
        while (*p && *p != '\r' && *p != '\n') p++;
        while (*p == '\r' || *p == '\n') *p++ = 0;

        char *q = line;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '@') q++;
        if (!*q || *q == ';' || *q == ':')
            continue;
        /* Skip the scaffolding; the first real command is the program. */
        if (!strncasecmp(q, "REM", 3) || !strncasecmp(q, "ECHO", 4) ||
            !strncasecmp(q, "PROMPT", 6) || !strncasecmp(q, "SET", 3) ||
            !strncasecmp(q, "PATH", 4) || !strncasecmp(q, "CD", 2))
            continue;

        unsigned i = 0;
        while (q[i] && q[i] != ' ' && q[i] != '\t' && i < max - 1) {
            out[i] = q[i];
            i++;
        }
        out[i] = 0;
        return i ? 0 : -1;
    }
    return -1;
}

static FATFS dsk_fs;

static int mount_guest_disk(void)
{
    if (!ACTIVE_FILE || !ACTIVE_FILE->path[0])
        return -1;
    strncpy(dsk_path, ACTIVE_FILE->path, sizeof(dsk_path) - 1);
    x86_disk_set_reader(sd_sector_read, dsk_path, ACTIVE_FILE->size);
    if (f_mount(&dsk_fs, "", 1) != FR_OK)
        return -1;
    dsk_mounted = 1;
    return 0;
}

static void guest_reset(void)
{
    memset(cpu_state_ptr, 0, sizeof(X86CPUState));
    for (int i = 0; i < 6; i++)
        cpu_state_ptr->segs[i].limit = 0xFFFF;   /* real mode: 64 KB segments */
    /* CS.D stays clear: real mode is 16-bit, which is now genuinely different
       from 32-bit rather than the same handlers under another name. */

    x86_unmap_all();
    for (uint32_t i = 0; i < GUEST_RAM_SIZE / 4096u; i++)
        guest_page_table_ptr[i] = FOLD_K((uint32_t)guest_ram + i * 4096, i);
    x86_video_map();

    x86_bios_init();
    /* Take over the vectors we serve. The guest reaches us through a planted
       0F 0B stub, so a program that hooks or chains one still works. */
    static const uint8_t hle_vectors[] = {
        0x08, 0x09, 0x10, 0x16, 0x1A, 0x20, 0x21,
    };
    x86_hle_install(hle_vectors, sizeof(hle_vectors));

    /*
     * Upper memory. A real machine has ROM and adapter space from 0xA0000 up,
     * and a system tool scans it looking for option-ROM signatures. Leaving it
     * unmapped makes every one of those probes fault.
     *
     * Backing it properly would cost 384 KB of pool. Instead one page of 0xFF
     * -- open bus, which is what unpopulated memory reads as -- is ALIASED
     * across the whole region: the fold maps each granule independently, so 96
     * entries can share one page for 4 KB total. A scan reads 0xFF, finds no
     * signature, and moves on.
     */
    if (upper_filler) {
        memset(upper_filler, 0xFF, 4096);
        for (uint32_t lin = 0xA0000u; lin < 0x100000u; lin += 4096u)
            x86_map_fixed(lin, upper_filler, 4096u);
    }
    x86_video_map();                 /* VRAM overlays its part of that range */

    /* The identification bytes need their own page: they must persist, and
       everything else in the region is a shared alias. */
    x86_map_range(0xFF000u, 4096u);
    x86_bios_rom_install();

    /* Everything above 1 MB wraps to the bottom, as it does with A20 masked. */
    x86_mem_wrap_1mb();

    x86_dos_arena_init();

    /* Load the real program if the disk mounted; fall back to the built-in
       demo so a missing or unreadable .dsk shows a picture rather than a black
       screen with no explanation. */
    char startup[64];
    dsk_startup_ok = 0;
    dsk_exec_rc = 0xFFFFFFFF;
    if (dsk_mounted && find_startup_program(startup, sizeof(startup)) == 0) {
        dsk_startup_ok = 1;
        /* TOPBENCH wants "-l" (log/list mode) on its command tail. */
        dsk_exec_rc = (uint32_t)x86_dos_exec(startup, "-l");
    }
    if (dsk_exec_rc != 0) {
        memcpy(guest_ram + 0x100, demo_guest, sizeof(demo_guest));
        cpu_state_ptr->eip = 0x100;
        cpu_state_ptr->regs[4] = 0xFFFE;
    }
    cpu_state_ptr->eflags = 0x0202;              /* IF set, bit 1 reserved-1 */
    cpu_state_ptr->hle_enabled = 1;

    /* A slice of guest instructions per frame. The PIT exit is what ends a
       slice, so this is the emulator's time base until the real 18.2 Hz tick
       is wired to it. */
    cpu_state_ptr->pit_reload = 200000;
    cpu_state_ptr->pit_counter = 200000;
}

/*
 * Gamepad -> scancodes.
 *
 * Edge-triggered: a make code when a button goes down, a break code when it
 * comes up, exactly as a keyboard controller reports. Level-triggering would
 * flood the queue and leave a game seeing an endless key repeat.
 *
 * Modelled at this level on purpose: ports and IRQs behave identically in real
 * and protected mode, so nothing here has to be revisited for protected mode,
 * and a game that installs its own INT 09h and reads port 0x60 directly -- as
 * most action games do -- needs no BIOS layer at all.
 */
static const uint8_t pad_scancode[8] = {
    0x48,  /* UP    */
    0x50,  /* DOWN  */
    0x4B,  /* LEFT  */
    0x4D,  /* RIGHT */
    0x39,  /* A      -> Space */
    0x1D,  /* B      -> Ctrl  */
    0x1C,  /* START  -> Enter */
    0x01,  /* SELECT -> Esc   */
};

static void poll_input(const gw_firmware_abi_t *abi)
{
    odroid_gamepad_state_t pad;
    memset(&pad, 0, sizeof(pad));
    abi->input_ctl(GW_INPUT_READ_GAMEPAD, &pad);

    /* Hand the frame to the firmware before consuming it ourselves. This is
       what makes PAUSE/SET open the Retro-Go overlay, and it owns save states,
       brightness, volume and exit. Reading the gamepad directly and stopping
       there -- which is what this did -- leaves the core running inside
       Retro-Go with no way to get back out of it. */
    abi->common_emu_input_loop(&pad, NULL, NULL);

    uint32_t keys = 0;
    if (pad.values[ODROID_INPUT_UP])     keys |= 1u << 0;
    if (pad.values[ODROID_INPUT_DOWN])   keys |= 1u << 1;
    if (pad.values[ODROID_INPUT_LEFT])   keys |= 1u << 2;
    if (pad.values[ODROID_INPUT_RIGHT])  keys |= 1u << 3;
    if (pad.values[ODROID_INPUT_A])      keys |= 1u << 4;
    if (pad.values[ODROID_INPUT_B])      keys |= 1u << 5;
    if (pad.values[ODROID_INPUT_START])  keys |= 1u << 6;
    if (pad.values[ODROID_INPUT_SELECT]) keys |= 1u << 7;

    uint32_t changed = keys ^ x86_io.input_keys;
    x86_io.input_keys = keys;
    if (!changed)
        return;

    for (unsigned i = 0; i < 8; i++) {
        if (!(changed & (1u << i)))
            continue;
        uint8_t sc = pad_scancode[i];
        x86_kbd_scancode((keys & (1u << i)) ? sc : (uint8_t)(sc | 0x80));
    }

    /* Raise IRQ 1 and pull the next instruction boundary forward, so it is
       delivered now instead of at the end of the slice. */
    if (x86_kbd_pending()) {
        cpu_state_ptr->hard_int_pending = 0x09;
        cpu_state_ptr->pit_counter = 1;
    }
}

static void emulator_loop(const gw_firmware_abi_t *abi)
{
    guest_reset();
    guest_halted = 0;

    abi->audio_ctl(GW_AUDIO_INIT, SAMPLE_RATE);
    uint32_t audio_frames = (uint32_t)abi->audio_ctl(GW_AUDIO_GET_LENGTH, 0);
    abi->audio_ctl(GW_AUDIO_START, audio_frames);

    while (1) {
        /* The host can still ask for the CPU benchmark: it is how tiers 2 and
         * 3 stay comparable with tier 1, and it must not disappear just
         * because there is now a frame loop around it. It reprograms guest
         * state, so put the guest back afterwards. */
        if (run_benchmark_flag && core_ready) {
            run_benchmark();
            itcm_sum_post_bench = itcm_sum();
            itcm_byte_120_post = itcm_byte_120();
            run_benchmark_flag = 0;
            guest_reset();
        }

        poll_input(abi);

        /*
         * Run guest instructions until the slice's PIT budget expires. Other
         * exits are real events: a memory fault or an unimplemented opcode
         * stops the guest rather than looping on the same instruction forever,
         * which would look exactly like a hang.
         */
        while (!guest_halted) {
            x86_cpu_interp_asm();
            uint32_t reason = cpu_state_ptr->exit_reason;
            guest_exit_reason = reason;
            if (reason == X86_EXIT_TIMER) {
                cpu_state_ptr->exit_reason = 0;
                break;
            }
            if (reason == X86_EXIT_HLE_INT) {
                cpu_state_ptr->exit_reason = 0;
                x86_hle_service();
                continue;
            }
            /* Fault, HLT or unimplemented: latch it. Re-entering re-executes
               the same faulting instruction forever, spinning the frame loop
               thousands of times a second and telling us nothing new. */
            guest_halted = 1;
            break;
        }

        /* Video. The beam fraction drives the CGA status register, so a guest
           polling for vertical retrace sees it arrive about once per frame. */
        x86_video_set_beam(0xE000);
        uint8_t *fb = (uint8_t *)abi->lcd_ctl(GW_LCD_BUFFER,
                                              GW_LCD_BUF_ACTIVE, 0, 0);
        fb_addr = (uint32_t)fb;
        if (fb) {
            x86_video_render(fb, FRAME_W);
            fb_sample = *(volatile uint32_t *)(fb + 20 * FRAME_W);
        }
        clut_sample = core_clut[1];
        hle_calls_seen = x86_hle_calls;
        hle_bad_vector = x86_hle_unhandled_vector;
        hle_bad_func = x86_hle_unhandled_func;
        guest_ax = cpu_state_ptr->regs[0] & 0xFFFF;
        x86_video_set_beam(0x0000);

        /* A guest write to 0x3D9 changes the palette; the firmware holds its
         * own copy, so push ours again when it moves. */
        if (cga_clut_dirty) {
            abi->lcd_ctl(GW_LCD_SET_CLUT, (uintptr_t)core_clut, 4, 0);
            cga_clut_dirty = 0;
        }

        /* Audio: one buffer of speaker output per frame. */
        int16_t *abuf = (int16_t *)abi->audio_ctl(GW_AUDIO_GET_ACTIVE, 0);
        if (abuf && audio_frames)
            x86_speaker_render(abuf, audio_frames, SAMPLE_RATE, 6000);

        /* Pace the loop. GW_LCD_WAIT_VBLANK is NOT usable here: gwemu does
           not model vblank, so it never returns and the core never draws --
           the screen stays black and nothing reaches the host. Wait on the
           swap the firmware has already queued instead, which is modelled and
           bounds the loop to the panel rate on both targets. */
        abi->lcd_ctl(GW_LCD_SWAP, 0, 0, 0);
        for (unsigned spins = 0;
             spins < 100000u && abi->lcd_ctl(GW_LCD_IS_SWAP_PENDING, 0, 0, 0);
             spins++) {
            /* bounded: never trade a black screen for a hang */
        }
        frames_rendered++;
        abi->wdog_refresh();
    }
}
