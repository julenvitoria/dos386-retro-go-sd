#include <stdint.h>
#include <string.h>
#include "../lib/FatFs/ff.h"

#include "../cutils.h"
#include "../x86_cpu.h"
#include "../x86_arena.h"
#include "../x86_profile.h"
#include "../x86_osk.h"

// Address-translation fold. Entries are OFFSETS (host = guest_linear + k), and
// every index the 12-bit lookup can produce must exist -- a short table meant
// the walk read past its end and a zero entry aliased address 0, which is ITCM.
uint32_t guest_page_table[FOLD_ENTRIES] __attribute__((section(".dtcm_bss")));
/* Mode records: four dispatch tables plus their headers. Over-allocated by one
   record so x86_build_mode_records can align the base up to 8 KB, which the
   66/67 EORs require. */
static uint8_t mode_record_block[X86_MODE_RECORDS_BYTES + X86_MODE_RECORDS_ALIGN]
    __attribute__((section(".dtcm_bss")));
extern uint32_t *guest_page_table_ptr;   // what the interpreter dereferences
extern X86CPUState cpu_state_dtcm;
extern void x86_cpu_interp_asm(void);
extern void vdisk_init(void *base, uint32_t size);

// We need to map up to 0xB8000 + 16KB (CGA memory)
// 0xB8000 + 16KB = 753664 bytes (736KB + 16KB = 752KB)
uint8_t test_ram[752 * 1024] __attribute__((aligned(4096)));

extern uint32_t _estack; // Typically defined in linker script, or we can just hardcode end of DTCM
#define DTCM_END 0x20020000

void Reset_Handler(void);
void Fault_Handler(void);

// Cortex-M Vector Table.
//
// This used to be two entries (SP + Reset). With no fault handler a bus error
// escalated to HardFault with nowhere to go, the device wedged, and the Python
// harness saw an indistinguishable timeout -- which is exactly how the
// unconfigured-OCTOSPI bug stayed invisible. Faults are now latched and readable.
__attribute__((section(".isr_vector"), used))
uint32_t vector_table[] = {
    DTCM_END,                   //  0 Initial SP
    (uint32_t)&Reset_Handler,   //  1 Reset
    (uint32_t)&Fault_Handler,   //  2 NMI
    (uint32_t)&Fault_Handler,   //  3 HardFault
    (uint32_t)&Fault_Handler,   //  4 MemManage
    (uint32_t)&Fault_Handler,   //  5 BusFault
    (uint32_t)&Fault_Handler,   //  6 UsageFault
    0, 0, 0, 0,                 //  7-10 reserved
    (uint32_t)&Fault_Handler,   // 11 SVCall
    (uint32_t)&Fault_Handler,   // 12 DebugMonitor
    0,                          // 13 reserved
    (uint32_t)&Fault_Handler,   // 14 PendSV
    (uint32_t)&Fault_Handler,   // 15 SysTick
};

volatile uint32_t run_flag = 0;
volatile uint32_t run_benchmark_flag = 0;
volatile uint32_t benchmark_cycles = 0;
volatile uint32_t benchmark_insns = 0;   // measured, not assumed by the host
volatile uint32_t benchmark_filesize = 0;
volatile uint32_t debug_log[256];
volatile uint32_t debug_log_idx = 0;

// Host-triggered. The guest image lives in memory-mapped external flash, which
// requires OCTOSPI setup this bare-metal firmware does not do, so loading it is
// NOT attempted at reset -- on silicon that bus-faults. The host sets this only
// on targets where extflash is known good. Opcode tests and the benchmark need
// no disk and run regardless.
volatile uint32_t load_guest_flag = 0;

/* Video self-test. The host writes a pattern into cga_test_vram, raises the
   flag, and reads cga_test_out back to check the expansion independently. */
/* DOS arena self-test. The host arms it and reads the results back; the MCB
   chain lives in guest memory so the host can also walk it independently. */
volatile uint32_t run_dos_flag = 0;
volatile uint32_t dos_r[16];

/* HLE stub planting. The stubs and the bare-IRET default sit in guest memory;
   the host checks they land BELOW the DOS arena, because anything at or above
   it is handed to a program and overwritten. */
volatile uint32_t run_hle_flag = 0;
volatile int32_t hle_r = -99;

/* Keyboard self-test: the BDA ring and INT 16h, driven from guest code.
   The producer is the SAME function INT 09h dispatches to, so this exercises
   the real path from a scancode on port 0x60 to a key in AX. */
volatile uint32_t run_kbd_flag = 0;
volatile uint32_t kbd_r[20];

/* On-screen keyboard self-test. The OSK is a front end over the SAME injection
   path the buttons use, so what has to be proved is that a grid press turns
   into the right SCANCODES and that the BIOS decodes them into the right BDA
   entry -- shift included, which is the part with no other witness. */
volatile uint32_t run_osk_flag = 0;
volatile uint32_t osk_r[20];

/* DOS file I/O self-test: INT 21h 3Dh/3Fh/42h/3Eh/43h/4Eh/4Fh, driven from
   GUEST code against a FAT12 volume the HOST built in dosfile_img.

   It needs its own disk rather than the shipped .dsk: the expected bytes have
   to be written by the test, not read out of a title's data file. 64 KB is the
   smallest valid FAT12 geometry (see scripts/common/fat12.py). */
#define DOSFILE_IMG_BYTES (128u * 512u)
uint8_t dosfile_img[DOSFILE_IMG_BYTES] __attribute__((aligned(4)));
volatile uint32_t run_dosfile_flag = 0;
volatile uint32_t dosfile_r[40];

volatile uint32_t run_nor_flag = 0;
volatile uint32_t nor_r[24];

volatile uint32_t run_a20_flag = 0;
volatile uint32_t a20_r[24];

/* The arena runs against a region of the harness's OWN choosing -- 40 pages,
   so every expected number below is arithmetic this test can state, not
   whatever the LCD bonus pool happened to release.
   It is carved out of the TOP of test_ram rather than declared: 160 KB of new
   .bss does not fit in any region of this harness (RAM_EMU, RAM_UC and AHBRAM
   were each tried and each overflowed), and the arena arm runs no guest code,
   so the guest's memory is idle while it does. test_ram is 4096-aligned, so
   the offset keeps the region page-aligned. */
#define ARENA_TEST_PAGES 40u
#define ARENA_TEST_OFF   (512u * 1024u)
#define arena_test_mem   (test_ram + ARENA_TEST_OFF)
volatile uint32_t run_arena_flag = 0;
volatile uint32_t arena_r[32];

volatile uint32_t run_video_flag = 0;
volatile uint32_t video_blit_cycles = 0;
uint8_t cga_test_vram[X86_VRAM_BYTES] __attribute__((section(".test_bss")));
uint8_t cga_test_out[320 * 240] __attribute__((section(".test_bss")));
/* 32-bit ARGB entries, matching lcd_set_clut(). */
uint32_t cga_test_palette[32] __attribute__((section(".test_bss")));
                                /* X86_TEXT_CLUT_ENTRIES: 32 is what the
                                    firmware needs for its darkened twins to
                                    line up with LCD_DARKEN_BIT, and what the
                                    anti-aliased text path allocates from */
/* Text-mode arms. run_video_flag 4 selects a mode (so the host can put the
   renderer into mode 3 and get the CLUT reloaded), 5 runs a scroll. The cell
   pattern itself is written straight into cga_test_vram, which IS the text
   buffer -- that is the path a real DOS program takes, poking B800:0 rather
   than going through INT 10h. */
/* VGA mode 13h. The aperture and the host palette block come from the caller
   in the real core (the LCD bonus pool); here they are just two buffers, which
   is the whole point of taking them as arguments rather than as .bss.
   vga_dac_arg is streamed through the DAC PORTS by arm 6, not memcpy'd into
   the DAC -- 0x3C9 is a stream and a test that bypassed it would not see the
   failure that models it as a latch. */
uint8_t  vga_test_aperture[X86_VGA_APERTURE_BYTES] __attribute__((section(".ahb_bss")));
uint8_t  vga_test_pal[X86_VGA_PALETTE_BYTES] __attribute__((section(".ahb_bss")));
uint8_t  vga_dac_arg[768] __attribute__((section(".ahb_bss")));
uint32_t vga_clut_out[256] __attribute__((section(".ahb_bss")));
volatile uint32_t vga_dac_readback[6];
volatile uint32_t vga_regs_out[9];   /* CRTC/Seq/GC/Attr write-then-read-back */   /* first two entries, read back via 0x3C7/0x3C9 */

volatile uint32_t text_mode_arg = 0x03;
volatile uint32_t text_scroll_arg = 0;      /* (attr << 8) | lines */

// Latched fault state, for the host to read over the debug backend.
volatile uint32_t fault_active = 0;   // 0 = none, else VECTACTIVE
volatile uint32_t fault_cfsr = 0;
volatile uint32_t fault_hfsr = 0;
volatile uint32_t fault_bfar = 0;
volatile uint32_t fault_pc = 0;
volatile uint32_t fault_lr = 0;

#define SCB_ICSR (*(volatile uint32_t *)0xE000ED04)
#define SCB_CFSR (*(volatile uint32_t *)0xE000ED28)
#define SCB_HFSR (*(volatile uint32_t *)0xE000ED2C)
#define SCB_BFAR (*(volatile uint32_t *)0xE000ED38)

__attribute__((naked, used)) void Fault_Handler(void)
{
    // Recover the stacked frame pointer (EXC_RETURN bit 2 selects MSP/PSP),
    // then hand it to C so the faulting PC/LR can be latched.
    __asm__ volatile(
        "tst  lr, #4            \n"
        "ite  eq                \n"
        "mrseq r0, msp          \n"
        "mrsne r0, psp          \n"
        "b    Fault_Handler_C   \n");
}

__attribute__((used)) void Fault_Handler_C(uint32_t *frame)
{
    fault_cfsr = SCB_CFSR;
    fault_hfsr = SCB_HFSR;
    fault_bfar = SCB_BFAR;
    fault_lr = frame[5];
    fault_pc = frame[6];
    fault_active = SCB_ICSR & 0x1FF;

    // Release any pending host wait so the harness reports a fault rather than
    // timing out with no information.
    run_flag = 0;
    run_benchmark_flag = 0;
    while (1) {
    }
}

#define DWT_CTRL (*(volatile uint32_t *)0xE0001000)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004)
#define CoreDebug_DEMCR (*(volatile uint32_t *)0xE000EDFC)

typedef struct {
    uint16_t signature;
    uint16_t bytes_in_last_block;
    uint16_t blocks_in_file;
    uint16_t num_relocs;
    uint16_t header_paragraphs;
    uint16_t min_extra_paragraphs;
    uint16_t max_extra_paragraphs;
    uint16_t ss;
    uint16_t sp;
    uint16_t checksum;
    uint16_t ip;
    uint16_t cs;
    uint16_t reloc_table_offset;
    uint16_t overlay_number;
} MZHeader;

// Load MZ executable into test_ram (which we assume starts at linear address 0)
// We will place the PSP at segment 0x0100 (linear 0x1000)
// The code will be loaded at segment 0x0110 (linear 0x1100)
int load_mz_executable(FIL *file) {
    MZHeader mz;
    UINT br;
    if (f_read(file, &mz, sizeof(MZHeader), &br) != FR_OK) return -1;
    if (mz.signature != 0x5A4D && mz.signature != 0x4D5A) return -1; // "MZ" or "ZM"
    
    uint32_t header_size = mz.header_paragraphs * 16;
    uint32_t load_size = mz.blocks_in_file * 512;
    if (mz.bytes_in_last_block) {
        load_size -= (512 - mz.bytes_in_last_block);
    }
    load_size -= header_size;
    
    uint16_t psp_segment = 0x0100;
    uint16_t load_segment = psp_segment + 0x10;
    uint32_t load_linear = load_segment * 16;
    
    if (load_linear + load_size > sizeof(test_ram)) {
        return -2; // File too large for our test_ram (16KB? Wait, test_ram is 16KB! ALLEYCAT is 55KB!)
    }
    
    // Seek to start of code
    f_lseek(file, header_size);
    f_read(file, &test_ram[load_linear], load_size, &br);
    
    // Read and apply relocations
    f_lseek(file, mz.reloc_table_offset);
    for (int i = 0; i < mz.num_relocs; i++) {
        uint16_t reloc[2]; // [offset, segment]
        if (f_read(file, reloc, 4, &br) != FR_OK) break;
        uint32_t reloc_addr = load_linear + (reloc[1] * 16) + reloc[0];
        uint16_t *target = (uint16_t *)&test_ram[reloc_addr];
        *target += load_segment;
    }
    
    // Initialize CPU state
    memset(&cpu_state_dtcm, 0, sizeof(cpu_state_dtcm));
    
    // Set segments (ES=0, CS=1, SS=2, DS=3, FS=4, GS=5)
    cpu_state_dtcm.segs[1].sel = mz.cs + load_segment;
    cpu_state_dtcm.segs[1].base = (mz.cs + load_segment) * 16;
    
    cpu_state_dtcm.segs[2].sel = mz.ss + load_segment;
    cpu_state_dtcm.segs[2].base = (mz.ss + load_segment) * 16;
    
    cpu_state_dtcm.segs[3].sel = psp_segment;
    cpu_state_dtcm.segs[3].base = psp_segment * 16;
    cpu_state_dtcm.segs[0].sel = psp_segment;
    cpu_state_dtcm.segs[0].base = psp_segment * 16;
    
    cpu_state_dtcm.eip = mz.ip;
    cpu_state_dtcm.regs[4] = mz.sp; // ESP=4
    
    // Set up PSP (CD 20 at offset 0)
    test_ram[psp_segment * 16] = 0xCD;
    test_ram[psp_segment * 16 + 1] = 0x20;
    
    return 0;
}

/* --------------------------------------------------------------------------
 * Keyboard / INT 16h self-test.
 *
 * A scancode is queued on the controller and handed to x86_bios_kbd_irq() --
 * the exact function INT 09h dispatches to -- so the producer side is the
 * shipping one, not a re-implementation. The consumer side is GUEST code:
 * INT 16h AH=01 (peek, ZF reports empty) and AH=00 (blocking read), with the
 * results and the FLAGS the IRET actually delivered stored into guest memory
 * where the host can read them.
 */
#define KBD_CODE_BASE   0x2000u
#define KBD_RES_BASE    0x4000u

static const uint8_t kbd_guest_code[] = {
    /* --- phase A at 0x2000: peek an EMPTY buffer ------------------------ */
    0xB8, 0x00, 0x00,             /* MOV AX,0000                            */
    0xB4, 0x01,                   /* MOV AH,01                              */
    0xCD, 0x16,                   /* INT 16h                                */
    0xA3, 0x00, 0x40,             /* MOV [4000],AX  r0: AX on an empty peek */
    0x9C,                         /* PUSHF                                  */
    0x58,                         /* POP AX                                 */
    0xA3, 0x02, 0x40,             /* MOV [4002],AX  r1: FLAGS, ZF must be 1 */
    0xF4,                         /* HLT                                    */
};

/* Phase B is placed at a fixed offset so the host and the harness agree. */
#define KBD_PHASE_B     0x2020u
static const uint8_t kbd_guest_code_b[] = {
    0xB4, 0x01,                   /* MOV AH,01   peek with a key waiting    */
    0xCD, 0x16,                   /* INT 16h                                */
    0xA3, 0x04, 0x40,             /* MOV [4004],AX  r2: the key             */
    0x9C, 0x58,                   /* PUSHF ; POP AX                         */
    0xA3, 0x06, 0x40,             /* MOV [4006],AX  r3: FLAGS, ZF must be 0 */
    0xB4, 0x01,                   /* MOV AH,01   peek again                 */
    0xCD, 0x16,                   /* INT 16h                                */
    0xA3, 0x08, 0x40,             /* MOV [4008],AX  r4: non-destructive     */
    0xB8, 0x00, 0x00,             /* MOV AX,0000                            */
    0xCD, 0x16,                   /* INT 16h AH=00: dequeue                 */
    0xA3, 0x0A, 0x40,             /* MOV [400A],AX  r5: the key             */
    0xB4, 0x01,                   /* MOV AH,01   drained now                */
    0xCD, 0x16,                   /* INT 16h                                */
    0x9C, 0x58,                   /* PUSHF ; POP AX                         */
    0xA3, 0x0C, 0x40,             /* MOV [400C],AX  r6: FLAGS, ZF must be 1 */
    0xF4,                         /* HLT                                    */
};

/* Phase C: the same read, used after the ring has been walked to its end so
   the push and the pop both have to wrap. */
#define KBD_PHASE_C     0x2080u
static const uint8_t kbd_guest_code_c[] = {
    0xB8, 0x00, 0x00,             /* MOV AX,0000                            */
    0xCD, 0x16,                   /* INT 16h AH=00                          */
    0xA3, 0x0E, 0x40,             /* MOV [400E],AX  r7: the wrapped key     */
    0xF4,                         /* HLT                                    */
};

static void kbd_test_reset_cpu(uint32_t eip)
{
    memset(&cpu_state_dtcm, 0, sizeof(cpu_state_dtcm));
    for (int i = 0; i < 6; i++)
        cpu_state_dtcm.segs[i].limit = 0xFFFF;   /* real mode, CS.D clear */
    cpu_state_dtcm.eip = eip;
    cpu_state_dtcm.regs[4] = 0x1F00;             /* ESP */
    cpu_state_dtcm.eflags = 0x0002;
    cpu_state_dtcm.idtr_base = 0;
    cpu_state_dtcm.idtr_limit = 0x3FF;
    cpu_state_dtcm.hle_enabled = 1;
    cpu_state_dtcm.pit_counter = 20000;
    cpu_state_dtcm.pit_reload = 20000;
}

/* Run until the guest HLTs, servicing HLE exits the way the core's frame loop
   does. Returns the exit reason it stopped on. */
static uint32_t kbd_test_run_to_halt(void)
{
    /* Bounded: a guest that never halts must FAIL the test, not hang the
       harness. 200 x 20,000 instructions is far more than any phase needs. */
    for (int guard = 0; guard < 200; guard++) {
        x86_cpu_interp_asm();
        if (cpu_state_dtcm.exit_reason == X86_EXIT_HLE_INT) {
            x86_hle_service();
            cpu_state_dtcm.exit_reason = 0;
            continue;
        }
        if (cpu_state_dtcm.exit_reason == X86_EXIT_TIMER) {
            cpu_state_dtcm.exit_reason = 0;
            cpu_state_dtcm.pit_counter = 20000;
            continue;
        }
        return cpu_state_dtcm.exit_reason;
    }
    return 0xFFFFFFFFu;
}

static uint16_t kbd_bda_r16(uint32_t off)
{
    return (uint16_t)(test_ram[0x400 + off] | (test_ram[0x401 + off] << 8));
}
static void kbd_bda_w16(uint32_t off, uint16_t v)
{
    test_ram[0x400 + off] = (uint8_t)v;
    test_ram[0x401 + off] = (uint8_t)(v >> 8);
}

/* ---------------------------------------------------------------------------
 * Mode switching, driven by GUEST code through INT 10h.
 *
 * The other video arms call x86_video_set_mode() from the host, which proves
 * the renderer but says nothing about the path a real program takes. Alley
 * Cat's defect lived on that path: the mode was set through the BIOS, followed
 * correctly, and the CHARACTERS printed afterwards were still stored as
 * char/attr pairs into pixel memory. So this arm executes the guest's own
 *
 *      MOV AX,000m / INT 10h        (set mode)
 *      MOV AH,0E / MOV AL,ch / INT 10h   (teletype one character)
 *
 * and the host then renders and compares. Only this can catch a regression in
 * which the mode is adopted and the character path is not told.
 */
#define MSW_CODE_BASE   0x2600u
#define MSW_TTY_BASE    0x2620u

volatile uint32_t run_modeswitch_flag = 0;
volatile uint32_t msw_mode_arg = 3;         /* AL for the mode set   */
volatile uint32_t msw_char_arg = 'A';       /* AL for the teletype   */
volatile uint32_t msw_r[6];

/* 0: mode after   1: is_text   2: text cols   3: BDA 0x449   4: BDA 0x44A
   5: guest exit reason */
static void msw_snapshot(void)
{
    msw_r[0] = x86_video_mode();
    msw_r[1] = (uint32_t)x86_video_is_text();
    msw_r[2] = x86_video_text_cols();
    msw_r[3] = test_ram[0x449];
    msw_r[4] = (uint32_t)(test_ram[0x44A] | (test_ram[0x44B] << 8));
}

static void modeswitch_test_init(void)
{
    static const uint8_t hle_vectors[] = { 0x08, 0x09, 0x10, 0x16, 0x1A, 0x20, 0x21 };
    static const uint8_t set_mode_code[] = {
        0xB8, 0x03, 0x00,             /* MOV AX,0003 (AL patched by the host) */
        0xCD, 0x10,                   /* INT 10h                              */
        0xF4,                         /* HLT                                  */
    };
    static const uint8_t tty_code[] = {
        0xB4, 0x0E,                   /* MOV AH,0E                            */
        0xB0, 0x41,                   /* MOV AL,'A'  (patched by the host)    */
        0xB7, 0x00,                   /* MOV BH,0                             */
        0xB3, 0x03,                   /* MOV BL,3    (graphics fg colour)     */
        0xCD, 0x10,                   /* INT 10h                              */
        0xF4,                         /* HLT                                  */
    };

    x86_video_init(cga_test_vram, cga_test_palette);
    x86_video_set_aperture(vga_test_aperture, vga_test_pal);
    x86_bios_init();
    x86_hle_install(hle_vectors, sizeof(hle_vectors));
    memcpy(&test_ram[MSW_CODE_BASE], set_mode_code, sizeof(set_mode_code));
    memcpy(&test_ram[MSW_TTY_BASE], tty_code, sizeof(tty_code));
    msw_snapshot();
}

static void modeswitch_test_set(void)
{
    test_ram[MSW_CODE_BASE + 1] = (uint8_t)msw_mode_arg;
    kbd_test_reset_cpu(MSW_CODE_BASE);
    msw_r[5] = kbd_test_run_to_halt();
    msw_snapshot();
}

static void modeswitch_test_tty(void)
{
    test_ram[MSW_TTY_BASE + 3] = (uint8_t)msw_char_arg;
    kbd_test_reset_cpu(MSW_TTY_BASE);
    msw_r[5] = kbd_test_run_to_halt();
    msw_snapshot();
}

/* ---------------------------------------------------------------------------
 * The machine DESCRIPTION services, driven by guest code.
 *
 * INT 11h (equipment), INT 12h (conventional KB) and INT 15h AH=88h (extended
 * KB) are how a DOS program asks what machine it is on, and every one of them
 * was reaching the bare IRET stub -- which does not clear a register, so the
 * guest read back its OWN poison value and believed it. That failure has no
 * symptom the host can see: the call "returns", the counters all move, and the
 * program simply configures itself for a machine that does not exist.
 *
 * So the arm POISONS AX before every call and sets CF the wrong way round
 * before the two that carry one. An unserviced vector then reads back the
 * poison, which is the only way to tell "answered 0xFFFF" from "answered
 * nothing" -- and 0xFFFF KB of conventional memory is exactly the shape of
 * answer a program cannot survive.
 *
 * CF comes off the IRET FRAME (PUSHF after the INT), not from a live register,
 * for the same reason the INT 16h arm reads ZF that way.
 */
#define SYS_CODE_BASE   0x2700u
#define SYS_RES_BASE    0x4C00u

volatile uint32_t run_sysinfo_flag = 0;
volatile uint32_t sys_r[16];

static const uint8_t sys_guest_code[] = {
    /* INT 11h -- equipment list. AX poisoned; a bare IRET returns the poison. */
    0xB8, 0xFF, 0xFF,                   /* MOV AX,FFFF                     */
    0xCD, 0x11,                         /* INT 11h                         */
    0xA3, 0x00, 0x4C,                   /* MOV [4C00],AX                   */

    /* INT 12h -- conventional memory in KB. */
    0xB8, 0xFF, 0xFF,                   /* MOV AX,FFFF                     */
    0xCD, 0x12,                         /* INT 12h                         */
    0xA3, 0x02, 0x4C,                   /* MOV [4C02],AX                   */

    /* INT 15h AH=88h -- extended memory in KB. CF poisoned SET; a machine
       with no extended memory answers AX=0 with CF CLEAR. */
    0xB8, 0x00, 0x88,                   /* MOV AX,8800                     */
    0xF9,                               /* STC                             */
    0xCD, 0x15,                         /* INT 15h                         */
    0xA3, 0x04, 0x4C,                   /* MOV [4C04],AX                   */
    0x9C,                               /* PUSHF                           */
    0x58,                               /* POP AX                          */
    0xA3, 0x06, 0x4C,                   /* MOV [4C06],AX                   */

    /* INT 15h AH=C0h -- get system configuration. We have no configuration
       table, and the honest answer is the one a PC/XT BIOS gives: CF SET,
       AH=86h "unsupported function". CF poisoned CLEAR. */
    0xB8, 0x00, 0xC0,                   /* MOV AX,C000                     */
    0xF8,                               /* CLC                             */
    0xCD, 0x15,                         /* INT 15h                         */
    0xA3, 0x08, 0x4C,                   /* MOV [4C08],AX                   */
    0x9C,                               /* PUSHF                           */
    0x58,                               /* POP AX                          */
    0xA3, 0x0A, 0x4C,                   /* MOV [4C0A],AX                   */

    /*
     * The PC speaker, probed the way a program CONFIRMS one is there: program
     * PIT channel 2, ungate it, and watch port 0x61 report back. Bit 5 is the
     * channel's OUT line and bit 4 the DRAM refresh toggle, and both used to
     * be whatever the guest last WROTE to 0x61 -- so both were constants, and
     * a constant is not a wrong value here, it is a hang: the standard idiom
     * is "read 0x61 until the bit flips".
     *
     * BL accumulates OR of every sample and BH accumulates AND, so one word
     * says whether each bit was ever high AND ever low. A stuck bit fails
     * whichever way it is stuck; a single sample could not tell.
     */
    0xB0, 0xB6, 0xE6, 0x43,             /* MOV AL,B6 / OUT 43,AL (ch2 mode3)*/
    0xB0, 0xFB, 0xE6, 0x42,             /* MOV AL,FB / OUT 42,AL (lo)       */
    0xB0, 0x11, 0xE6, 0x42,             /* MOV AL,11 / OUT 42,AL (hi)       */
    0xE4, 0x61, 0x0C, 0x03, 0xE6, 0x61, /* IN 61 / OR AL,3 / OUT 61 (ungate)*/
    0xBB, 0x00, 0xFF,                   /* MOV BX,FF00  BL=OR 0, BH=AND FF  */
    0xB9, 0x00, 0x04,                   /* MOV CX,0400                      */
    0xE4, 0x61,                         /* .loop: IN AL,61                  */
    0x08, 0xC3,                         /*        OR BL,AL                  */
    0x20, 0xC7,                         /*        AND BH,AL                 */
    0xE2, 0xF8,                         /*        LOOP .loop                */
    0x89, 0x1E, 0x0C, 0x4C,             /* MOV [4C0C],BX                    */
    0xE4, 0x61, 0x24, 0xFC, 0xE6, 0x61, /* gate the speaker back off        */

    0xF4,                               /* HLT                             */
};

static void sysinfo_test_run(void)
{
    static const uint8_t hle_vectors[] = {
        0x08, 0x09, 0x10, 0x11, 0x12, 0x15, 0x16, 0x1A, 0x20, 0x21,
    };
    memset(&test_ram[SYS_RES_BASE], 0, 16);
    x86_video_init(cga_test_vram, cga_test_palette);
    x86_video_set_aperture(vga_test_aperture, vga_test_pal);
    x86_bios_init();
    x86_hle_install(hle_vectors, sizeof(hle_vectors));
    memcpy(&test_ram[SYS_CODE_BASE], sys_guest_code, sizeof(sys_guest_code));

    kbd_test_reset_cpu(SYS_CODE_BASE);
    sys_r[8] = kbd_test_run_to_halt();

    for (unsigned i = 0; i < 7; i++)
        sys_r[i] = (uint16_t)(test_ram[SYS_RES_BASE + i * 2] |
                              (test_ram[SYS_RES_BASE + i * 2 + 1] << 8));

    /* The same facts as MEMORY, which is the other half of the contract: a
       program is free to peek the BDA instead of calling, and the two must not
       be able to disagree. */
    sys_r[9]  = kbd_bda_r16(0x10);     /* equipment word            */
    sys_r[10] = kbd_bda_r16(0x13);     /* conventional memory in KB */
    sys_r[11] = kbd_bda_r16(0x63);     /* CRTC base port            */
    sys_r[12] = kbd_bda_r16(0x80);     /* keyboard ring start       */
    sys_r[13] = kbd_bda_r16(0x82);     /* keyboard ring end         */
}

static void kbd_test_run(void)
{
    static const uint8_t hle_vectors[] = {
        0x08, 0x09, 0x10, 0x16, 0x1A, 0x20, 0x21,
    };

    memset(&test_ram[KBD_RES_BASE], 0, 32);
    /* x86_bios_init() puts the renderer into mode 3, which needs a VRAM
       pointer -- the real core calls x86_video_init() first and so must this. */
    x86_video_init(cga_test_vram, cga_test_palette);
    x86_video_set_aperture(vga_test_aperture, vga_test_pal);
    x86_bios_init();
    x86_hle_install(hle_vectors, sizeof(hle_vectors));

    memcpy(&test_ram[KBD_CODE_BASE], kbd_guest_code, sizeof(kbd_guest_code));
    memcpy(&test_ram[KBD_PHASE_B], kbd_guest_code_b, sizeof(kbd_guest_code_b));
    memcpy(&test_ram[KBD_PHASE_C], kbd_guest_code_c, sizeof(kbd_guest_code_c));

    /* What a freshly initialised BDA says. Zero here means the first key is
       written over the IVT and INT 16h compares head==tail forever. */
    kbd_r[8] = kbd_bda_r16(0x1A);
    kbd_r[9] = kbd_bda_r16(0x1C);

    /* Phase A: nothing buffered. */
    kbd_test_reset_cpu(KBD_CODE_BASE);
    kbd_r[19] = 0xC0DE0003;   /* progress marker: a hang must say WHERE */
    kbd_r[16] = kbd_test_run_to_halt();

    /* Inject a key exactly as INT 09h does: a scancode on the controller,
       then the BIOS keyboard interrupt handler. 0x1C is Enter. */
    x86_kbd_scancode(0x1C);
    kbd_r[10] = (uint32_t)x86_bios_kbd_irq();
    kbd_r[11] = kbd_bda_r16(0x1A);
    kbd_r[12] = kbd_bda_r16(0x1C);
    /* The word the producer left in the ring, read straight out of guest
       memory at the offset the head points to -- head is an offset within
       segment 0x40, not an index. */
    kbd_r[13] = (uint16_t)(test_ram[0x400 + kbd_r[11]] |
                           (test_ram[0x401 + kbd_r[11]] << 8));
    /* Drain the break code the controller still holds, so it cannot leak into
       a later phase. */
    x86_kbd_scancode(0x9C);
    x86_bios_kbd_irq();

    /* Phase B: peek, peek again, read, peek empty. */
    kbd_test_reset_cpu(KBD_PHASE_B);
    kbd_r[19] = 0xC0DE0004;
    kbd_r[17] = kbd_test_run_to_halt();

    /* Phase C: walk head and tail to the LAST entry of the ring so the push
       and the pop each have to wrap back to 0x1E. */
    kbd_bda_w16(0x1A, 0x3C);
    kbd_bda_w16(0x1C, 0x3C);
    x86_kbd_scancode(0x39);          /* Space */
    x86_bios_kbd_irq();
    kbd_r[14] = kbd_bda_r16(0x1C);   /* tail must have wrapped to 0x1E */
    kbd_r[15] = (uint16_t)(test_ram[0x400 + 0x3C] |
                           (test_ram[0x401 + 0x3C] << 8));
    x86_kbd_scancode(0xB9);
    x86_bios_kbd_irq();
    kbd_test_reset_cpu(KBD_PHASE_C);
    kbd_r[19] = 0xC0DE0005;
    kbd_r[18] = kbd_test_run_to_halt();
    kbd_r[19] = kbd_bda_r16(0x1A);   /* head must have wrapped to 0x1E */

    /* The seven words the guest stored. */
    for (int i = 0; i < 8; i++)
        kbd_r[i] = (uint16_t)(test_ram[KBD_RES_BASE + i * 2] |
                              (test_ram[KBD_RES_BASE + i * 2 + 1] << 8));
}


/* --------------------------------------------------------------------------
 * On-screen keyboard.
 *
 * Driven exactly as src/main.c drives it: a button BITMASK per frame, then the
 * BIOS keyboard interrupt for every code the controller holds -- so this covers
 * the OSK, x86_kbd_scancode(), the 8042 queue and INT 09h's shift decode in one
 * chain. Nothing is asserted against the OSK's own state; the answers come out
 * of the guest's BDA ring, which is where a program looks.
 */
static void osk_drain_irqs(void)
{
    /* Bounded: a controller that never empties must fail, not hang. */
    for (int guard = 0; guard < 32 && x86_kbd_pending(); guard++)
        x86_bios_kbd_irq();
}

static uint16_t osk_ring_pop(void)
{
    uint16_t head = kbd_bda_r16(0x1A);
    if (head == kbd_bda_r16(0x1C))
        return 0xFFFF;                  /* empty */
    uint16_t w = (uint16_t)(test_ram[0x400 + head] |
                            (test_ram[0x401 + head] << 8));
    head = (uint16_t)(head + 2);
    if (head >= 0x3E)
        head = 0x1E;
    kbd_bda_w16(0x1A, head);
    return w;
}

/* One frame of buttons, then whatever the controller produced. */
static uint16_t osk_frame(uint32_t buttons)
{
    x86_osk_input(buttons);
    osk_drain_irqs();
    return osk_ring_pop();
}

static void osk_test_run(void)
{
    static const uint8_t hle_vectors[] = {
        0x08, 0x09, 0x10, 0x16, 0x1A, 0x20, 0x21,
    };
    x86_video_init(cga_test_vram, cga_test_palette);
    x86_video_set_aperture(vga_test_aperture, vga_test_pal);
    x86_bios_init();
    x86_hle_install(hle_vectors, sizeof(hle_vectors));
    x86_osk_reset();

    /* The layout's own arithmetic. draw_glyph() clips silently, so an
       over-long row loses its OFF key with no fault to notice. */
    osk_r[0] = x86_osk_row_cells_max;
    osk_r[1] = x86_osk_row_overflow;

    /* GAME opens it. The press is consumed by the transition -- the frame the
       OSK opens on emits nothing. */
    osk_r[2] = (uint32_t)x86_osk_visible();      /* expect 0 */
    x86_osk_cycle();
    osk_r[3] = (uint32_t)x86_osk_visible();      /* expect 1 */

    /* Layer 0, row 0, column 0 is 'a'. A presses it. Two frames: the release
       of the buttons has to be seen or the next press is not an edge. */
    (void)osk_frame(0);
    osk_r[4] = osk_frame(X86_OSK_A);             /* expect 0x1E1E ('a')      */
    (void)osk_frame(0);

    /* RIGHT then A is 'b' -- navigation, not just the first cell. */
    (void)osk_frame(X86_OSK_RIGHT);
    (void)osk_frame(0);
    osk_r[5] = osk_frame(X86_OSK_A);             /* expect 0x3062 ('b')      */
    (void)osk_frame(0);

    /* B is Backspace wherever the cursor is. */
    osk_r[6] = osk_frame(X86_OSK_B);             /* expect 0x0E08            */
    (void)osk_frame(0);

    /* TIME cycles the layer and DOWN flips the row, neither of which moves the
       column -- so the cursor is still on column 1 from the 'b' press above,
       and NINE more RIGHTs reach column 10. That cell is '!', a SHIFTED one:
       the BDA must show the shifted ASCII, which is only possible if a real
       Left Shift make reached INT 09h ahead of the key. (Ten RIGHTs lands on
       '@' -- also shifted, and it passed, which is exactly why the column is
       spelled out here rather than counted from zero in a comment.) */
    (void)osk_frame(X86_OSK_TIME);
    (void)osk_frame(0);
    (void)osk_frame(X86_OSK_DOWN);
    (void)osk_frame(0);
    for (int i = 0; i < 9; i++) {
        (void)osk_frame(X86_OSK_RIGHT);
        (void)osk_frame(0);
    }
    osk_r[7] = osk_frame(X86_OSK_A);             /* expect 0x0221 ('!')      */
    osk_r[8] = (uint32_t)test_ram[0x400 + 0x17]; /* BDA shift flags: 0 again */
    (void)osk_frame(0);

    /* Drawing must not touch the guest's framebuffer, in either mode, and must
       put ink in the bars. Rendered into the harness's own output buffer. */
    memset(cga_test_out, 0, 320 * 240);
    {
        uint32_t vram_before = 0, vram_after = 0;
        for (unsigned i = 0; i < X86_VRAM_BYTES; i += 64)
            vram_before += cga_test_vram[i];
        x86_video_render(cga_test_out, 320);
        x86_osk_draw(cga_test_out, 320);
        for (unsigned i = 0; i < X86_VRAM_BYTES; i += 64)
            vram_after += cga_test_vram[i];
        osk_r[9] = (vram_before == vram_after) ? 1 : 0;

        uint32_t top = 0, bottom = 0, picture = 0;
        for (unsigned y = 0; y < 20; y++)
            for (unsigned x = 0; x < 320; x++)
                top += cga_test_out[y * 320 + x] ? 1 : 0;
        for (unsigned y = 220; y < 240; y++)
            for (unsigned x = 0; x < 320; x++)
                bottom += cga_test_out[y * 320 + x] ? 1 : 0;
        for (unsigned y = 20; y < 220; y++)
            for (unsigned x = 0; x < 320; x++)
                picture += cga_test_out[y * 320 + x] ? 1 : 0;
        osk_r[10] = top;
        osk_r[11] = bottom;
        osk_r[12] = picture;
    }

    /* GAME again: mouse mode still owns the buttons; a third closes it. */
    x86_osk_cycle();
    osk_r[13] = x86_osk_mode;                    /* expect 2 (mouse)         */
    osk_r[14] = (uint32_t)x86_osk_visible();     /* expect 1                 */
    x86_osk_cycle();
    osk_r[15] = (uint32_t)x86_osk_visible();     /* expect 0                 */

    /* ---- IRQ 1 arming, which is what decides WHEN a cell press is visible.
     *
     * The grid checks above prove a code reaches the guest's ring; they cannot
     * see the defect that made an OSK keypress take SECONDS, because they call
     * x86_bios_kbd_irq() themselves instead of going through the frame loop's
     * arming rule. That rule is x86_kbd_raise_irq(), and both of its guards
     * were measured failures under gwemu -- see the comment on the definition.
     *
     * The IF=0 case is the one that mattered: the guest is inside its own INT
     * 09h handler, so pulling the instruction boundary in cannot deliver
     * anything and only ends the slice. Doing it once a frame ran the guest at
     * one instruction per frame and the OSK's codes piled up in the queue
     * until the keyboard was closed. */
    while (x86_kbd_pending())
        (void)x86_bios_kbd_irq();

    cpu_state_dtcm.hard_int_pending = 0;
    cpu_state_dtcm.pit_counter = 0x5A5A;
    cpu_state_dtcm.eflags = 0x0002;              /* IF CLEAR */
    x86_kbd_scancode(0x1E);
    osk_r[16] = (uint32_t)x86_kbd_raise_irq(&cpu_state_dtcm);
    osk_r[17] = cpu_state_dtcm.pit_counter;      /* expect 0x5A5A, untouched */

    /* The one-deep pending slot belongs to whoever got there first. An
       unguarded store here discarded the 18.2 Hz tick every time a key was
       held. */
    cpu_state_dtcm.hard_int_pending = 0x08;
    osk_r[18] = (uint32_t)((x86_kbd_raise_irq(&cpu_state_dtcm) << 8) |
                           cpu_state_dtcm.hard_int_pending);  /* expect 0x08 */

    /* With IF set the boundary CAN deliver, so it is pulled in -- the property
       that makes a keypress land this frame rather than at the end of a slice. */
    cpu_state_dtcm.hard_int_pending = 0;
    cpu_state_dtcm.pit_counter = 0x5A5A;
    cpu_state_dtcm.eflags = 0x0202;              /* IF SET */
    osk_r[19] = (uint32_t)((x86_kbd_raise_irq(&cpu_state_dtcm) << 16) |
                           (cpu_state_dtcm.hard_int_pending << 8) |
                           (cpu_state_dtcm.pit_counter == 1u));  /* 0x10901 */

    while (x86_kbd_pending())
        (void)x86_bios_kbd_irq();
}

/* --------------------------------------------------------------------------
 * DOS file I/O, from guest code.
 *
 * Every call goes through INT 21h the way a DOS program issues it, so the CF
 * and AX the test reads are the ones the guest's IRET actually delivered --
 * calling x86_dos_int21() from the host would miss the whole flags-on-stack
 * half of the contract, which is where "file not found" lives.
 */
#define DOSF_CODE   0x3000u
#define DOSF_STR    0x4000u
#define DOSF_RES    0x4100u     /* 24 result words                          */
#define DOSF_NAME1  0x4900u     /* FindFirst's name, copied out of the DTA   */
#define DOSF_NAME2  0x4910u     /* FindNext's                                */
#define DOSF_DTA    0x4800u
#define DOSF_BUF    0x5000u     /* whole-file read                           */
#define DOSF_BUF3   0x6100u     /* 16 bytes read after a seek                */

static const uint8_t dosfile_guest_code[] = {
    /* open TEST.INI, AL=0 read-only */
    0xB8, 0x00, 0x3D,  0xBA, 0x00, 0x40,  0xCD, 0x21,
    0x89, 0xC3,                       /* handle -> BX, before FLAGS eats AX */
    0xA3, 0x00, 0x41,  0x9C, 0x58, 0xA3, 0x02, 0x41,
    /* read 0x400 bytes: the file is shorter, so this is the EOF short read */
    0xB4, 0x3F,  0xB9, 0x00, 0x04,  0xBA, 0x00, 0x50,  0xCD, 0x21,
    0xA3, 0x04, 0x41,  0x9C, 0x58, 0xA3, 0x06, 0x41,
    /* read again, now AT eof: 0 bytes and CF CLEAR, not an error */
    0xB4, 0x3F,  0xB9, 0x00, 0x04,  0xBA, 0x00, 0x60,  0xCD, 0x21,
    0xA3, 0x08, 0x41,  0x9C, 0x58, 0xA3, 0x0A, 0x41,
    /* seek to 0x0100 from the start */
    0xB8, 0x00, 0x42,  0xB9, 0x00, 0x00,  0xBA, 0x00, 0x01,  0xCD, 0x21,
    0xA3, 0x0C, 0x41,  0x89, 0x16, 0x0E, 0x41,
    /* 16 bytes from there */
    0xB4, 0x3F,  0xB9, 0x10, 0x00,  0xBA, 0x00, 0x61,  0xCD, 0x21,
    0xA3, 0x10, 0x41,
    /* seek to end: DX:AX is the file size */
    0xB8, 0x02, 0x42,  0xB9, 0x00, 0x00,  0xBA, 0x00, 0x00,  0xCD, 0x21,
    0xA3, 0x12, 0x41,  0x89, 0x16, 0x14, 0x41,
    /* close */
    0xB4, 0x3E,  0xCD, 0x21,  0x9C, 0x58, 0xA3, 0x16, 0x41,
    /* open a file that is not there: CF set, AX = 02 */
    0xB8, 0x00, 0x3D,  0xBA, 0x10, 0x40,  0xCD, 0x21,
    0xA3, 0x18, 0x41,  0x9C, 0x58, 0xA3, 0x1A, 0x41,
    /* AH=43h get attributes */
    0xB8, 0x00, 0x43,  0xBA, 0x00, 0x40,  0xCD, 0x21,
    0x89, 0x0E, 0x1C, 0x41,  0x9C, 0x58, 0xA3, 0x1E, 0x41,
    /* DTA, then FindFirst "*.INI" */
    0xB4, 0x1A,  0xBA, 0x00, 0x48,  0xCD, 0x21,
    0xB8, 0x00, 0x4E,  0xB9, 0x00, 0x00,  0xBA, 0x20, 0x40,  0xCD, 0x21,
    0xA3, 0x20, 0x41,  0x9C, 0x58, 0xA3, 0x22, 0x41,
    0xBE, 0x1E, 0x48,  0xBF, 0x00, 0x49,  0xB9, 0x0D, 0x00, 0xFC, 0xF3, 0xA4,
    /* FindNext */
    0xB4, 0x4F,  0xCD, 0x21,
    0xA3, 0x24, 0x41,  0x9C, 0x58, 0xA3, 0x26, 0x41,
    0xBE, 0x1E, 0x48,  0xBF, 0x10, 0x49,  0xB9, 0x0D, 0x00, 0xFC, 0xF3, 0xA4,
    /* the same file behind a drive letter: "A:TEST.INI" */
    0xB8, 0x00, 0x3D,  0xBA, 0x30, 0x40,  0xCD, 0x21,
    0x89, 0xC3,  0xA3, 0x28, 0x41,  0x9C, 0x58, 0xA3, 0x2A, 0x41,
    0xB4, 0x3E,  0xCD, 0x21,
    /* and in lower case: "test.ini" */
    0xB8, 0x00, 0x3D,  0xBA, 0x40, 0x40,  0xCD, 0x21,
    0x89, 0xC3,  0xA3, 0x2C, 0x41,  0x9C, 0x58, 0xA3, 0x2E, 0x41,
    0xB4, 0x3E,  0xCD, 0x21,
    /*
     * The same FindFirst/FindNext again, with the DTA STRADDLING a 4 KB
     * granule boundary (0x6FE0: the 43-byte result record ends at 0x700B).
     *
     * This is not a corner case dressed up as one. x86_guest_ptr() refuses any
     * span that crosses a granule, dos_find_report() asked it for all 43 bytes
     * at once, and a refusal there was indistinguishable from "no more files"
     * -- so a program whose DTA happens to sit near the end of a page sees an
     * EMPTY DISK. WOLF3D's DTA is at DS:0xFFBA and that is exactly what
     * happened to its findfirst("*.WL1").
     */
    0xB4, 0x1A,  0xBA, 0xE0, 0x6F,  0xCD, 0x21,
    0xB8, 0x00, 0x4E,  0xB9, 0x00, 0x00,  0xBA, 0x20, 0x40,  0xCD, 0x21,
    0xA3, 0x30, 0x41,  0x9C, 0x58, 0xA3, 0x32, 0x41,
    0xBE, 0xFE, 0x6F,  0xBF, 0x20, 0x49,  0xB9, 0x0D, 0x00, 0xFC, 0xF3, 0xA4,
    0xB4, 0x4F,  0xCD, 0x21,
    0xA3, 0x34, 0x41,  0x9C, 0x58, 0xA3, 0x36, 0x41,
    0xBE, 0xFE, 0x6F,  0xBF, 0x30, 0x49,  0xB9, 0x0D, 0x00, 0xFC, 0xF3, 0xA4,
    /*
     * AH=42h on handle 0 -- a CHARACTER DEVICE, not a file. DOS succeeds with
     * DX:AX = 0; it does not report "invalid handle". 3Eh/3Fh/40h/44h all
     * route through dos_handle_device() and 42h did not, so a seek on stdin
     * failed, and a program that treats that as retryable never leaves the
     * retry. WOLF3D issued 81 in a row.
     */
    0xB8, 0x00, 0x42,  0xBB, 0x00, 0x00,  0xB9, 0x00, 0x00,  0xBA, 0x00, 0xFC,
    0xCD, 0x21,
    0xA3, 0x38, 0x41,  0x89, 0x16, 0x3A, 0x41,  0x9C, 0x58, 0xA3, 0x3C, 0x41,
    0xF4,                                                       /* HLT */
};

static FATFS dosfile_fs;

/* ---------------------------------------------------------------------- *
 * NOR cold-page backing, driven by GUEST code.
 *
 * The "NOR" here is the extflash window at 0x90000000 itself, written with
 * plain stores. Under gwemu that region is real backing memory, which is
 * exactly what makes this a tier-1 test: no flash programming, no OCTOSPI, no
 * SD card and no firmware -- but the ADDRESS is genuine, so bit 31 is set and
 * the interpreter's fold check traps for the real reason it will on silicon.
 * A buffer in .bss would have bit 31 CLEAR and would never trap at all, so it
 * would test nothing.
 *
 * The guest program is what exercises it. Reading the counters after a host-C
 * memcpy would only prove that C works; a read served out of NOR and a write
 * that pulls a page back into AXI are properties of the trap stubs, and only
 * executing guest instructions can show them.
 * ---------------------------------------------------------------------- */
#define NOR_TEST_PAGES  8u
#define NOR_TEST_BYTES  (NOR_TEST_PAGES * 4096u)
#define NOR_TEST_FIRST  200u                       /* guest page, clear of test_ram */
#define NOR_TEST_SEG    (NOR_TEST_FIRST * 4096u / 16u)   /* 0xC800 */
#define NOR_FAKE_BASE   ((uint8_t *)0x90000000u)

static uint8_t nor_test_ram[NOR_TEST_BYTES]
    __attribute__((aligned(4096)));

static uint8_t nor_pattern_byte(uint32_t off)
{
    return (uint8_t)(off * 7u + (off >> 8) * 31u + 1u);   /* never 0, never uniform */
}

static void nor_test_run(void)
{
    for (uint32_t i = 0; i < 24; i++)
        nor_r[i] = 0;

    for (uint32_t i = 0; i < NOR_TEST_BYTES; i++)
        nor_test_ram[i] = nor_pattern_byte(i);

    for (uint32_t i = 0; i < NOR_TEST_PAGES; i++) {
        guest_page_table[NOR_TEST_FIRST + i] =
            FOLD_K((uint32_t)nor_test_ram + i * 4096u, NOR_TEST_FIRST + i);
    }

    /* ---- 1. A snapshot that does not match must be REFUSED outright.
     * This is the invariant the whole design turns on: a half-applied remap
     * must be impossible, so the fold table is not touched until the bytes in
     * NOR have been read back and verified. Corrupt one byte and the attach
     * has to fail with the guest still running out of AXI. */
    memcpy(NOR_FAKE_BASE, nor_test_ram, NOR_TEST_BYTES);
    NOR_FAKE_BASE[0x2345] ^= 0xFF;
    nor_r[0] = (uint32_t)x86_nor_attach(NOR_FAKE_BASE, NOR_TEST_BYTES,
                                        nor_test_ram, NOR_TEST_BYTES,
                                        NOR_TEST_FIRST);
    nor_r[1] = x86_nor_attach_err;
    nor_r[2] = x86_nor_attached;
    nor_r[3] = (guest_page_table[NOR_TEST_FIRST]
                == FOLD_K((uint32_t)nor_test_ram, NOR_TEST_FIRST));

    /* ---- 2. The real thing. */
    NOR_FAKE_BASE[0x2345] ^= 0xFF;
    nor_r[4] = (uint32_t)x86_nor_attach(NOR_FAKE_BASE, NOR_TEST_BYTES,
                                        nor_test_ram, NOR_TEST_BYTES,
                                        NOR_TEST_FIRST);
    nor_r[5] = x86_nor_attached;
    nor_r[6] = x86_nor_pages_total;
    nor_r[7] = x86_nor_pages_nor;
    nor_r[8] = x86_nor_axi_free;

    /* ---- 3. Guest code. 16-bit real mode, CS = 0, DS = the snapshot.
     *
     *   MOV AX, NOR_TEST_SEG      B8 lo hi
     *   MOV DS, AX                8E D8
     *   MOV AL, [3000h]           A0 00 30     read, served out of NOR
     *   MOV [5000h], AL           A2 00 50     write, pulls page 5 into AXI
     *   INC AL                    FE C0
     *   MOV [5001h], AL           A2 01 50     write again, now the fast path
     *   MOV AL, [5000h]           A0 00 50     read back through the fold
     *   HLT                       F4
     *
     * The read-back lands in AL and is checked there. An extra store to park
     * it somewhere would have gone through DS -- i.e. into the snapshot -- and
     * COWed a SECOND page, which is exactly what the first version of this
     * test did and why its counters read 2 instead of 1.
     */
    static const uint8_t prog[] = {
        0xB8, (uint8_t)(NOR_TEST_SEG & 0xFF), (uint8_t)(NOR_TEST_SEG >> 8),
        0x8E, 0xD8,
        0xA0, 0x00, 0x30,
        0xA2, 0x00, 0x50,
        0xFE, 0xC0,
        0xA2, 0x01, 0x50,
        0xA0, 0x00, 0x50,
        0xF4,
    };
    memcpy(test_ram, prog, sizeof(prog));

    memset(&cpu_state_dtcm, 0, sizeof(cpu_state_dtcm));
    for (int i = 0; i < 6; i++)
        cpu_state_dtcm.segs[i].limit = 0xFFFFFFFF;
    cpu_state_dtcm.pit_reload = 0xFFFFFFFF;
    cpu_state_dtcm.pit_counter = 0xFFFFFFFF;
    cpu_state_dtcm.regs[4] = 0x3000;

    uint32_t reads0 = x86_nor_reads;
    x86_cpu_interp_asm();

    nor_r[9]  = cpu_state_dtcm.exit_reason;      /* 6 = HLT, i.e. it finished */
    nor_r[10] = x86_nor_reads - reads0;
    nor_r[11] = x86_nor_writes;
    nor_r[12] = x86_nor_cow_pulls;
    nor_r[13] = x86_nor_cow_failures;
    nor_r[14] = x86_nor_pages_nor;
    nor_r[15] = x86_nor_pages_axi;
    nor_r[16] = x86_nor_axi_free;
    nor_r[17] = x86_nor_unmapped;

    /* What the guest actually moved. The byte at 0x3000 came out of NOR, was
       stored into page 5 and read straight back; AL must hold it. */
    nor_r[18] = nor_pattern_byte(0x3000);
    nor_r[19] = cpu_state_dtcm.regs[0] & 0xFFu;

    /* The COW page took the write; the NOR copy must be UNTOUCHED. If a store
       ever reached NOR this is the only thing that would notice. */
    nor_r[20] = NOR_FAKE_BASE[0x5000];
    nor_r[21] = NOR_FAKE_BASE[0x5001];
    nor_r[22] = nor_pattern_byte(0x5000);

    /* Page 5 is on the fast path now: its fold entry must be an AXI address
       from the COW pool, i.e. bit 31 CLEAR. */
    {
        uint32_t idx = NOR_TEST_FIRST + 5;
        uint32_t host = guest_page_table[idx] + (idx << 12);
        nor_r[23] = (host >> 31) & 1u;
    }
}

/*
 * The A20 gate and the wrap it controls.
 *
 * Two properties are being separated here, and no existing test could see
 * either. First, the wrap must cover exactly the 64 KB above 1 MB that
 * segment:offset can reach -- the old code aliased all 15 MB above 1 MB onto
 * the low megabyte, which is an 8086's address space and means there is no
 * memory above 1 MB at all. Second, the wrap must be CONDITIONAL: while A20 is
 * masked the HMA reads the bottom of memory, and while it is open it does not.
 *
 * The wrap arms are driven by GUEST code through a segment override, because
 * that is the only way an address above 0xFFFF is formed in real mode and it is
 * the path a program that relies on the wrap actually takes. The gate arms go
 * through the PORTS -- all three doors -- for the same reason: a guest opens
 * A20 by talking to hardware, and a host-side call to x86_a20_set() would test
 * the function while leaving the ports, which is where every real failure is,
 * completely uncovered.
 */
static void a20_test_run(void)
{
    for (uint32_t i = 0; i < 24; i++)
        a20_r[i] = 0;

    /* Start from the state a machine boots in: gate masked, low memory mapped,
       everything above the HMA unmapped. */
    x86_io.a20_open = 0;
    x86_io.kbd_cmd = 0;
    x86_io.port92 = 0;
    x86_a20_changes = 0;
    for (uint32_t i = X86_HMA_FIRST_PAGE; i < FOLD_ENTRIES; i++)
        guest_page_table[i] = FOLD_INVALID;
    x86_mem_wrap_1mb();

    a20_r[0] = x86_io.a20_open;                       /* masked at boot */
    /* The alias, at both ends of the window. */
    a20_r[1] = (guest_page_table[X86_HMA_FIRST_PAGE]
                == guest_page_table[0] - (X86_HMA_FIRST_PAGE << FOLD_SHIFT));
    a20_r[2] = (guest_page_table[X86_HMA_FIRST_PAGE + X86_HMA_PAGES - 1]
                == guest_page_table[X86_HMA_PAGES - 1]
                   - (X86_HMA_FIRST_PAGE << FOLD_SHIFT));
    /* And the page just PAST it, which is the arm the old whole-space wrap
       fails: 0x110000 is extended memory and must not alias anything. */
    a20_r[3] = (guest_page_table[X86_HMA_FIRST_PAGE + X86_HMA_PAGES]
                == FOLD_INVALID);
    /* Nor may anything further up. 2 MB is where an 8086-style wrap would put
       another copy of low memory and a 286+ puts real RAM. */
    a20_r[4] = (guest_page_table[0x200000u >> FOLD_SHIFT] == FOLD_INVALID);

    /*
     * Guest arm 1: write a byte at 0000:0400 and read it back through
     * FFFF:0410, which is linear 0x100400. With the gate masked those are the
     * same byte.
     *
     * The marker is at 0x400 and not at 0x10 for a reason worth keeping: the
     * first version of this put it at 0x10, which is INSIDE this program, so
     * the store overwrote the 0x26 segment-override prefix of the very
     * instruction being tested. The read then ran unprefixed against DS and
     * returned a byte of the program -- a test that failed for a reason that
     * had nothing to do with A20.
     *
     *   MOV AX,0         B8 00 00      MOV AX,0FFFFh    B8 FF FF
     *   MOV ES,AX        8E C0         MOV ES,AX        8E C0
     *   MOV AL,5Ah       B0 5A         MOV AL,ES:[410h] 26 A0 10 04
     *   MOV ES:[400h],AL 26 A2 00 04   HLT              F4
     */
    static const uint8_t prog_wrap[] = {
        0xB8, 0x00, 0x00,  0x8E, 0xC0,
        0xB0, 0x5A,        0x26, 0xA2, 0x00, 0x04,
        0xB8, 0xFF, 0xFF,  0x8E, 0xC0,
        0x26, 0xA0, 0x10, 0x04,
        0xF4,
    };
    memcpy(test_ram, prog_wrap, sizeof(prog_wrap));
    memset(&cpu_state_dtcm, 0, sizeof(cpu_state_dtcm));
    for (int i = 0; i < 6; i++)
        cpu_state_dtcm.segs[i].limit = 0xFFFFFFFF;
    cpu_state_dtcm.pit_reload = 0xFFFFFFFF;
    cpu_state_dtcm.pit_counter = 0xFFFFFFFF;
    x86_cpu_interp_asm();
    a20_r[5] = cpu_state_dtcm.exit_reason;            /* 6 = HLT: it finished */
    /* AL is read straight out of the CPU state rather than stored somewhere in
       guest memory first: a store would go through DS and could only confuse
       the question this arm asks. */
    a20_r[6] = cpu_state_dtcm.regs[0] & 0xFFu;        /* must be 0x5A */
    a20_r[7] = test_ram[0x400];

    /*
     * Guest arm 2: open A20 through the FAST GATE at port 0x92, then reach for
     * the same HMA address. The alias is gone, nothing owns the HMA yet, so
     * this is a clean X86_EXIT_MEM_FAULT rather than a silent read of the
     * interrupt vector table.
     *
     *   MOV AL,2        B0 02
     *   OUT 92h,AL      E6 92
     *   MOV AX,0FFFFh   B8 FF FF
     *   MOV ES,AX       8E C0
     *   MOV AL,ES:[20h] 26 A0 20 00
     *   HLT             F4
     */
    static const uint8_t prog_open[] = {
        0xB0, 0x02,  0xE6, 0x92,
        0xB8, 0xFF, 0xFF,  0x8E, 0xC0,
        0x26, 0xA0, 0x20, 0x00,
        0xF4,
    };
    memcpy(test_ram, prog_open, sizeof(prog_open));
    memset(&cpu_state_dtcm, 0, sizeof(cpu_state_dtcm));
    for (int i = 0; i < 6; i++)
        cpu_state_dtcm.segs[i].limit = 0xFFFFFFFF;
    cpu_state_dtcm.pit_reload = 0xFFFFFFFF;
    cpu_state_dtcm.pit_counter = 0xFFFFFFFF;
    x86_cpu_interp_asm();
    a20_r[8]  = x86_io.a20_open;                      /* the OUT took */
    a20_r[9]  = x86_a20_changes;                      /* exactly one transition */
    a20_r[10] = cpu_state_dtcm.exit_reason;           /* 5 = MEM_FAULT */
    a20_r[11] = (guest_page_table[X86_HMA_FIRST_PAGE] == FOLD_INVALID);
    a20_r[12] = x86_port_read(0x92, 1) & 0x02u;       /* reads back as open */

    /* Masking it again must RESTORE the alias -- a gate that only works in one
       direction passes every test written in the opening direction only. */
    x86_port_write(0x92, 0x00, 1);
    a20_r[13] = x86_io.a20_open;
    a20_r[14] = x86_a20_changes;                      /* two transitions now */
    a20_r[15] = (guest_page_table[X86_HMA_FIRST_PAGE]
                 == guest_page_table[0] - (X86_HMA_FIRST_PAGE << FOLD_SHIFT));

    /* The 8042 output-port door: 0x64 = D1, then the value to 0x60. */
    x86_port_write(0x64, 0xD1, 1);
    x86_port_write(0x60, 0x02, 1);
    a20_r[16] = x86_io.a20_open;
    /* ... and read it back through 0x64 = D0, whose answer arrives on 0x60. */
    x86_port_write(0x64, 0xD0, 1);
    a20_r[17] = x86_port_read(0x60, 1) & 0x02u;

    /* The 0xDD / 0xDF shortcuts. */
    x86_port_write(0x64, 0xDD, 1);
    a20_r[18] = x86_io.a20_open;                      /* masked */
    x86_port_write(0x64, 0xDF, 1);
    a20_r[19] = x86_io.a20_open;                      /* open */

    /* A command that is NOT an output-port write must clear the pending latch.
       A stale 0xD1 turns the guest's next keyboard command byte into an
       output-port write and masks A20 in the middle of a running program --
       silently, which is the whole reason this arm exists. */
    x86_port_write(0x64, 0xD1, 1);
    x86_port_write(0x64, 0xAE, 1);       /* enable keyboard: not D1 */
    x86_port_write(0x60, 0x00, 1);       /* would mask A20 if the latch stuck */
    a20_r[20] = x86_io.a20_open;                      /* still open */

    /* Probing an already-open gate is not a transition. */
    uint32_t changes = x86_a20_changes;
    x86_port_write(0x92, 0x02, 1);
    a20_r[21] = (x86_a20_changes == changes);

    /* Leave the machine as the rest of the harness expects to find it. */
    x86_a20_set(0);
    a20_r[22] = x86_io.a20_open;
    a20_r[23] = x86_a20_state;
}

/*
 * The VRAM arena, its all-or-nothing rule, and the pin state the pager will
 * consult.
 *
 * The rule under test is the one that unblocks EGA: a claim that cannot be
 * fully placed must change NOTHING and must say by how much it missed. The
 * previous port measured a partial EGA card as producing worse output than no
 * EGA at all -- 8 KB per plane truncates cleanly, but 16 KB and 32 KB alias
 * the floating pages onto each other and fill the screen with noise that
 * reads as a rendering defect rather than a memory shortage. So "refused, and
 * here is the deficit" is the behaviour, and a half-placed claim is the bug.
 */
#define KB(n) ((n) * 1024u)
static void arena_test_run(void)
{
    for (uint32_t i = 0; i < 32; i++)
        arena_r[i] = 0;

    void *cga = NULL, *v13 = NULL, *ega[4] = { NULL, NULL, NULL, NULL };

    x86_arena_reset();
    arena_r[0] = (x86_arena_add_region(arena_test_mem,
                                       ARENA_TEST_PAGES * 4096u) == 0);
    arena_r[1] = x86_arena_bytes;                       /* 163,840 */
    arena_r[2] = x86_arena_free;

    /* ---- the two claims a real core makes at boot */
    arena_r[3] = (x86_arena_claim(X86_ARENA_ID_CGA, 1, KB(32), &cga) == 0);
    arena_r[4] = x86_arena_used;                        /* 32,768 */
    arena_r[5] = (x86_arena_claim(X86_ARENA_ID_VGA13, 1, KB(64), &v13) == 0);
    arena_r[6] = x86_arena_used;                        /* 98,304 */
    arena_r[7] = x86_arena_free;                        /* 65,536 */
    /* The two blocks must not overlap, which is the one way a bitmap
       allocator fails silently: both callers write and each sees the other's
       bytes appear in its own buffer. */
    arena_r[8] = (((uint8_t *)v13 >= (uint8_t *)cga + KB(32))
                  || ((uint8_t *)cga >= (uint8_t *)v13 + KB(64)));

    /* ---- EGA: four planes of 64 KB against 64 KB free. REFUSED. */
    uint32_t used_before = x86_arena_used;
    arena_r[9] = (x86_arena_claim(X86_ARENA_ID_EGA, 4, KB(64), ega) != 0);
    arena_r[10] = x86_arena_deficit;                    /* 3 planes: 196,608 */
    arena_r[11] = (x86_arena_used == used_before);      /* nothing moved */
    arena_r[12] = (ega[0] == NULL);                     /* out untouched */
    /* And the arena still WORKS after a refusal -- a failed claim that left
       the bitmap half-marked would show up here and nowhere else. */
    void *probe = NULL;
    arena_r[13] = (x86_arena_claim(X86_ARENA_ID_SPARE, 1, KB(64), &probe) == 0);
    x86_arena_release(X86_ARENA_ID_SPARE);

    /* ---- release the display claims and try again. Still refused: 163,840
       bytes cannot hold 262,144, and the deficit says so exactly. */
    x86_arena_release(X86_ARENA_ID_CGA);
    x86_arena_release(X86_ARENA_ID_VGA13);
    arena_r[14] = x86_arena_free;                       /* the whole region */
    arena_r[15] = (x86_arena_claim(X86_ARENA_ID_EGA, 4, KB(64), ega) != 0);
    arena_r[16] = x86_arena_deficit;                    /* 2 planes: 131,072 */

    /* ---- would_fit agrees with claim and does not change anything. Two
       answers that can disagree is how a mode gets ADOPTED and then fails to
       allocate, which is the exact failure the refusal rule exists to stop. */
    arena_r[17] = (x86_arena_would_fit(X86_ARENA_ID_EGA, 4, KB(64)) == 0);
    arena_r[18] = (x86_arena_would_fit(X86_ARENA_ID_EGA, 2, KB(64)) == 1);
    arena_r[19] = x86_arena_used;                       /* still 0 */

    /* ---- a re-claim releases the old one rather than leaking it */
    arena_r[20] = (x86_arena_claim(X86_ARENA_ID_CGA, 1, KB(32), &cga) == 0);
    arena_r[21] = (x86_arena_claim(X86_ARENA_ID_CGA, 1, KB(32), &cga) == 0);
    arena_r[22] = x86_arena_used;                       /* 32,768, not 65,536 */

    /* ---- pins, which is what the victim picker will test */
    uint32_t pinned0 = x86_pinned_pages;
    x86_pin_pages(0xB8000u >> 12, 8);
    arena_r[23] = (x86_pinned_pages - pinned0);         /* 8 */
    arena_r[24] = x86_page_is_pinned(0xB8000u >> 12);
    arena_r[25] = x86_page_is_pinned((0xB8000u >> 12) - 1u);   /* 0 */
    x86_pin_pages(0xB8000u >> 12, 8);                   /* idempotent */
    arena_r[26] = (x86_pinned_pages - pinned0);         /* still 8 */
    x86_unpin_pages(0xB8000u >> 12, 8);
    arena_r[27] = (x86_pinned_pages - pinned0);         /* 0 */

    /* ---- the live-pointer ring.
     *
     * The first check is the one that matters and it is not obvious: the ring
     * is .bss, so it starts as a row of zeroes, and page 0 is a real and
     * constantly used page. Without an armed flag every eviction of page 0
     * would be vetoed from boot by a pointer that was never handed out. */
    x86_livemap_clear();
    arena_r[28] = (x86_livemap_holds(0) == 0);
    x86_livemap_note(0x42);
    x86_livemap_note(0x43);
    arena_r[29] = (x86_livemap_holds(0x42) && x86_livemap_holds(0x43)
                   && !x86_livemap_holds(0x44));
    x86_livemap_clear();
    arena_r[30] = (x86_livemap_holds(0x42) == 0);

    /* Leave the arena empty so nothing downstream inherits a claim. */
    x86_arena_reset();
    arena_r[31] = x86_arena_used;
}

static void dosfile_test_run(void)
{
    static const uint8_t hle_vectors[] = {
        0x08, 0x09, 0x10, 0x16, 0x1A, 0x20, 0x21,
    };

    for (int i = 0; i < 40; i++)
        dosfile_r[i] = 0;
    memset(&test_ram[DOSF_RES], 0, 128);
    memset(&test_ram[DOSF_BUF], 0, 1024);
    memset(&test_ram[DOSF_NAME1], 0, 64);
    memset(&test_ram[0x6FE0], 0, 64);

    x86_video_init(cga_test_vram, cga_test_palette);
    x86_video_set_aperture(vga_test_aperture, vga_test_pal);
    x86_bios_init();
    x86_hle_install(hle_vectors, sizeof(hle_vectors));

    /* The image the host wrote into dosfile_img IS the guest's floppy. */
    x86_disk_set_memory(dosfile_img, DOSFILE_IMG_BYTES);
    dosfile_r[36] = (uint32_t)f_mount(&dosfile_fs, "", 1);

    memcpy(&test_ram[DOSF_STR + 0x00], "TEST.INI", 9);
    memcpy(&test_ram[DOSF_STR + 0x10], "NOPE.INI", 9);
    memcpy(&test_ram[DOSF_STR + 0x20], "*.INI", 6);
    memcpy(&test_ram[DOSF_STR + 0x30], "A:TEST.INI", 11);
    memcpy(&test_ram[DOSF_STR + 0x40], "test.ini", 9);
    memcpy(&test_ram[DOSF_CODE], dosfile_guest_code,
           sizeof(dosfile_guest_code));

    kbd_test_reset_cpu(DOSF_CODE);
    dosfile_r[37] = kbd_test_run_to_halt();

    for (int i = 0; i < 31; i++)
        dosfile_r[i] = (uint16_t)(test_ram[DOSF_RES + i * 2] |
                                  (test_ram[DOSF_RES + i * 2 + 1] << 8));
}

// External arrays from x86_cpu.c
extern uint32_t (*io_port_read_table[1024])(void *opaque, uint32_t offset, int size_log2);
extern void *io_port_opaque_table[1024];

uint32_t cga_status_read(void *opaque, uint32_t offset, int size_log2) {
    static uint32_t vblank = 0;
    vblank ^= 8; // Toggle bit 3 (VBLANK)
    return vblank;
}

// Dummy Reset_Handler for QEMU execution
void Reset_Handler(void) {
    // Copy .data from its flash load address (nothing did this before, so any
    // non-zero initializer silently read as garbage).
    extern uint32_t _sidata, _sdata, _edata;
    for (uint32_t *src = &_sidata, *dst = &_sdata; dst < &_edata; src++, dst++) {
        *dst = *src;
    }

    // Copy the hot execution loop into ITCM before anything branches into it.
    extern uint32_t _sitcm_load, _sitcm, _eitcm;
    for (uint32_t *src = &_sitcm_load, *dst = &_sitcm; dst < &_eitcm; src++, dst++) {
        *dst = *src;
    }

    // Zero out BSS
    extern uint32_t _sbss;
    extern uint32_t _ebss;
    for (uint32_t *p = &_sbss; p < &_ebss; p++) {
        *p = 0;
    }

    // The harness's own scratch lives in RAM_UC, outside .bss, so it needs
    // zeroing here or a test reads whatever the previous run left.
    extern uint32_t _stestbss, _etestbss;
    for (uint32_t *p = &_stestbss; p < &_etestbss; p++) {
        *p = 0;
    }
    // Same for the mode 13h buffers, which are in AHBRAM for want of room in
    // either AXI region. AHB SRAM's clock is on -- SystemInit enables it.
    extern uint32_t _sahbbss, _eahbbss;
    for (uint32_t *p = &_sahbbss; p < &_eahbbss; p++) {
        *p = 0;
    }

    // Enable DWT Cycle Counter
    CoreDebug_DEMCR |= (1 << 24); 
    DWT_CYCCNT = 0;
    DWT_CTRL |= 1;

#if X86_PROFILE
    /* After DWT is running, before anything dispatches: the profiler reads
       CYCCNT on every dispatch and calibrates itself here. */
    x86_profile_init(0);
#endif

    io_port_read_table[0x3DA] = cga_status_read;

    // Initialize BDA at 0x0400
    // 0x0413: Memory size in KB (uint16_t)
    uint16_t *bda_mem_size = (uint16_t *)&test_ram[0x0413];
    *bda_mem_size = 640;
    
    // Point the interpreter at our statically-placed DTCM state. A retro-go
    // core sets these to dtc_malloc'd memory instead; the assembly is the same.
    guest_page_table_ptr = guest_page_table;
    x86_build_mode_records(mode_record_block);

    // Every page starts unmapped, so a stray access faults loudly instead of
    // landing in ITCM. test_ram is identity-mapped from guest 0, which makes
    // every k identical -- k = host - (idx << 12) = test_ram.
    for (uint32_t i = 0; i < FOLD_ENTRIES; i++) {
        guest_page_table[i] = FOLD_INVALID;
    }
    for (uint32_t i = 0; i < sizeof(test_ram) / 4096; i++) {
        guest_page_table[i] = FOLD_K((uint32_t)test_ram + i * 4096, i);
    }

    // Disable PIT during opcode tests to prevent unexpected IRQs
    cpu_state_dtcm.pit_reload = 0xFFFFFFFF;
    cpu_state_dtcm.pit_counter = 0xFFFFFFFF;

    while (1) {
        // Load the guest from memory-mapped external flash. Host-triggered
        // because reaching 0x90000000 needs OCTOSPI configuration this firmware
        // does not perform -- doing it unconditionally at reset bus-faults on
        // silicon while passing under gwemu, which models extflash as plain RAM.
        if (load_guest_flag) {
            vdisk_init((void*)0x90000000, 1474560);

            FATFS fs;
            FRESULT res = f_mount(&fs, "0:", 1);
            if (res == FR_OK) {
                FIL file;
                res = f_open(&file, "0:CAT.EXE", FA_READ);
                if (res == FR_OK) {
                    int load_res = load_mz_executable(&file);
                    benchmark_filesize =
                        (load_res == 0) ? 0x1337 : (0xDD000000 | -load_res);
                    f_close(&file);
                } else {
                    benchmark_filesize = 0xEEEE0000 | res;
                }
            } else {
                benchmark_filesize = 0xFFFF0000 | res;
            }
            load_guest_flag = 0;
        }

        if (run_flag) {
            x86_cpu_interp_asm();
            run_flag = 0;
        }

        if (run_benchmark_flag == 2) {
            // Run the game!
            // Set limits for all segments to 0xFFFF
            for (int i = 0; i < 6; i++) {
                cpu_state_dtcm.segs[i].limit = 0xFFFF;
            }
            
            // Set a dummy IDTR
            cpu_state_dtcm.idtr_base = 0x0000;
            cpu_state_dtcm.idtr_limit = 0x3FF;
            
            // Limit execution to 1,000,000 instructions before yielding
            cpu_state_dtcm.pit_counter = 1000000;
            cpu_state_dtcm.pit_reload = 1000000;
            
            while (1) {
                x86_cpu_interp_asm();
                if (cpu_state_dtcm.exit_reason == 2) {
                    uint32_t int_num = cpu_state_dtcm.exit_info;
                    uint32_t ah = (cpu_state_dtcm.regs[0] >> 8) & 0xFF; // EAX=0
                    uint32_t al = cpu_state_dtcm.regs[0] & 0xFF;
                    
                    if (debug_log_idx < 256) {
                        debug_log[debug_log_idx++] = (int_num << 16) | (ah << 8) | al;
                    }
                    
                    if (int_num == 0x20) {
                        // Terminate
                        break;
                    } else if (int_num == 0x21) {
                        if (ah == 0x4C || ah == 0x00) {
                            // Terminate
                            break; 
                        } else if (ah == 0x09) {
                            // Print string (DS:DX)
                        }
                    } else if (int_num == 0x10) {
                        if (ah == 0x00) {
                            // Set video mode
                        }
                    } else if (int_num == 0x08) {
                        // Timer tick
                        uint32_t *bda_timer = (uint32_t *)&test_ram[0x046C];
                        *bda_timer += 1;
                        // Clear pending
                        cpu_state_dtcm.regs[2192/4] = 0; // wait, hard_int_pending is offset 2192, so we need to clear it! Wait, cpu_state_dtcm is a struct, let's just use the pointer
                        *(uint32_t*)((uint8_t*)&cpu_state_dtcm + 2192) = 0;
                    }
                    
                    // After handling, reset exit_reason and resume
                    cpu_state_dtcm.exit_reason = 0;
                } else if (cpu_state_dtcm.exit_reason == 3) { // timer exit
                    cpu_state_dtcm.exit_reason = 0;
                    cpu_state_dtcm.pit_counter = 1000000;
                    // Allow python script to read EIP!
                } else {
                    // Halting or other exit reason
                    break;
                }
            }
            run_benchmark_flag = 0;
        }
        
        if (run_nor_flag == 1) {
            nor_test_run();
            run_nor_flag = 0;
        }

        if (run_a20_flag == 1) {
            a20_test_run();
            run_a20_flag = 0;
        }

        if (run_arena_flag == 1) {
            arena_test_run();
            run_arena_flag = 0;
        }

        if (run_dos_flag == 1) {
            /* Exercise the arena the way a program does: allocate, allocate,
               free the first, allocate something that only fits if the freed
               block was really reclaimed, then resize. */
            x86_dos_arena_init();
            uint16_t largest0 = 0;
            uint16_t a = dos_test_alloc(0x100, &largest0);
            uint16_t b = dos_test_alloc(0x100, &largest0);
            dos_r[0] = a;
            dos_r[1] = b;
            dos_r[2] = (b > a) ? (b - a) : 0;      /* spacing: 0x101 paras */
            dos_r[3] = dos_test_free(a);
            uint16_t c = dos_test_alloc(0x80, &largest0);
            dos_r[4] = c;
            dos_r[5] = (c == a) ? 1 : 0;           /* reused the hole */
            uint16_t largest = 0;
            dos_r[6] = dos_test_resize(b, 0x40, &largest);
            dos_r[7] = dos_test_alloc(0xFFFF, &largest);
            dos_r[8] = largest;                    /* must report a real size */
            dos_r[9] = dos_test_walk();            /* chain still consistent */
            run_dos_flag = 0;
        }

        if (run_hle_flag == 1) {
            /* Plant the same vector set the real core does, so the host can
               check every IVT entry lands in memory DOS will never hand out. */
            static const uint8_t hle_vectors[] = {
                0x08, 0x09, 0x10, 0x16, 0x1A, 0x20, 0x21,
            };
            hle_r = x86_hle_install(hle_vectors, sizeof(hle_vectors));
            run_hle_flag = 0;
        }

        if (run_kbd_flag == 1) {
            kbd_test_run();
            run_kbd_flag = 0;
        }

        if (run_sysinfo_flag == 1) {
            sysinfo_test_run();
            run_sysinfo_flag = 0;
        }

        if (run_osk_flag == 1) {
            osk_test_run();
            run_osk_flag = 0;
        }

        if (run_dosfile_flag == 1) {
            dosfile_test_run();
            run_dosfile_flag = 0;
        }

        if (run_video_flag == 1) {
            x86_video_init(cga_test_vram, cga_test_palette);
            x86_video_set_aperture(vga_test_aperture, vga_test_pal);
            /* init clears VRAM; the host's pattern is written after the flag
               is armed, so re-arm ordering matters -- see tests/test_video.py */
            run_video_flag = 2;
        }
        if (run_modeswitch_flag == 1) {
            modeswitch_test_init();
            run_modeswitch_flag = 0;
        }
        if (run_modeswitch_flag == 2) {
            modeswitch_test_set();
            run_modeswitch_flag = 0;
        }
        if (run_modeswitch_flag == 3) {
            modeswitch_test_tty();
            run_modeswitch_flag = 0;
        }
        if (run_video_flag == 4) {
            x86_video_set_mode((uint8_t)text_mode_arg);
            run_video_flag = 0;
        }
        if (run_video_flag == 5) {
            x86_video_scroll((uint8_t)text_scroll_arg,
                             (uint8_t)(text_scroll_arg >> 8));
            run_video_flag = 0;
        }
        if (run_video_flag == 6) {
            /* Stream the palette exactly as a guest does: index once to
               0x3C8, then 768 consecutive bytes to 0x3C9. Then read the first
               two entries back through 0x3C7/0x3C9, which is what a fade loop
               does and what a write-only DAC would fail. */
            unsigned i;
            x86_port_write(0x3C8, 0, 1);
            for (i = 0; i < 768; i++)
                x86_port_write(0x3C9, vga_dac_arg[i], 1);
            x86_port_write(0x3C7, 0, 1);
            for (i = 0; i < 6; i++)
                vga_dac_readback[i] = x86_port_read(0x3C9, 1);
            run_video_flag = 0;
        }
        if (run_video_flag == 8) {
            /* The VGA register FILES, probed the way a DOS program detects a
               display: write a register, read it back. WOLF3D does exactly
               `OUT 3D4,0F / OUT 3D5,66 / IN 3D5` and then repeats it against
               the mono alias 3B4/3B5 -- measured off x86_io_log. With no
               register file both readbacks were 0xFF and its detection walked
               off into its own heap. */
            x86_port_write(0x3D4, 0x0F, 1);
            x86_port_write(0x3D5, 0x66, 1);
            vga_regs_out[0] = x86_port_read(0x3D5, 1);        /* 0x66 */
            /* The mono pair must be SILENT while Misc Output bit 0 selects
               the colour addresses -- a card decodes one pair, not both, and
               that silence is what tells a probe which display it has. */
            x86_port_write(0x3B4, 0x0F, 1);
            vga_regs_out[1] = x86_port_read(0x3B5, 1);        /* 0xFF: silent */
            x86_port_write(0x3C4, 0x04, 1);
            x86_port_write(0x3C5, 0x06, 1);
            vga_regs_out[2] = x86_port_read(0x3C5, 1);        /* 0x06 */
            x86_port_write(0x3CE, 0x05, 1);
            x86_port_write(0x3CF, 0x40, 1);
            vga_regs_out[3] = x86_port_read(0x3CF, 1);        /* 0x40 */
            /* The attribute controller: ONE port, sequenced by a flip-flop
               that a read of 0x3DA resets to "index". Without that reset the
               two halves swap roles permanently. */
            (void)x86_port_read(0x3DA, 1);
            x86_port_write(0x3C0, 0x10, 1);                   /* index */
            x86_port_write(0x3C0, 0x41, 1);                   /* data  */
            (void)x86_port_read(0x3DA, 1);
            x86_port_write(0x3C0, 0x10, 1);
            vga_regs_out[4] = x86_port_read(0x3C1, 1);        /* 0x41 */
            /* And the index latch itself reads back, which is what a probe
               that writes only the index relies on. */
            vga_regs_out[5] = x86_port_read(0x3C4, 1);        /* 0x04 */
            /* Clear Misc Output bit 0 and the mono pair takes over. */
            x86_port_write(0x3C2, 0x66, 1);
            x86_port_write(0x3B4, 0x0F, 1);
            vga_regs_out[6] = x86_port_read(0x3B5, 1);        /* 0x66 now */
            vga_regs_out[7] = x86_port_read(0x3D5, 1);        /* same file */
            x86_port_write(0x3C2, 0x67, 1);                   /* colour again */
            vga_regs_out[8] = x86_port_read(0x3B5, 1);        /* silent again */
            run_video_flag = 0;
        }
        if (run_video_flag == 7) {
            const uint32_t *c = x86_video_clut256();
            unsigned i;
            for (i = 0; i < 256; i++)
                vga_clut_out[i] = c ? c[i] : 0xDEADBEEFu;
            run_video_flag = 0;
        }
        if (run_video_flag == 3) {
            uint32_t t0 = DWT_CYCCNT;
            x86_video_render(cga_test_out, 320);
            video_blit_cycles = DWT_CYCCNT - t0;
            run_video_flag = 0;
        }

        if (run_benchmark_flag == 1) {
            // Setup benchmark state
            memset(&cpu_state_dtcm, 0, sizeof(cpu_state_dtcm));
            for (int i = 0; i < 6; i++) {
                cpu_state_dtcm.segs[i].limit = 0xFFFFFFFF;
            }
            /* This benchmark's guest code is 32-bit flat. A zeroed struct
               leaves CS.D clear, which now really means a 16-bit code
               segment. */
            cpu_state_dtcm.segs[1].flags |= X86_DESC_D_FLAG;
            
            // Setup IDT at guest 0x1000
            uint16_t idt_base = 0x1000;
            cpu_state_dtcm.idtr_base = idt_base;
            cpu_state_dtcm.idtr_limit = 0x3FF;
            
            // Set EFLAGS.IF
            cpu_state_dtcm.eflags = 0x200; // IF enabled
            
            // Write IDT entry for INT 8 (PIT) -> ISR at 0x2000
            // cpu_exec.S uses 8-byte gate descriptors
            uint16_t isr_addr = 0x2000;
            uint32_t idt_entry_addr = idt_base + 8 * 8;
            test_ram[idt_entry_addr + 0] = isr_addr & 0xFF;
            test_ram[idt_entry_addr + 1] = (isr_addr >> 8) & 0xFF;
            test_ram[idt_entry_addr + 2] = 0x00;
            test_ram[idt_entry_addr + 3] = 0x00;
            test_ram[idt_entry_addr + 4] = 0x00;
            test_ram[idt_entry_addr + 5] = 0x00;
            test_ram[idt_entry_addr + 6] = 0x00;
            test_ram[idt_entry_addr + 7] = 0x00;
            
            // Write ISR: INC EBX (0x43), IRET (0xCF)
            test_ram[isr_addr + 0] = 0x43;
            test_ram[isr_addr + 1] = 0xCF;
            
            // Write execution loop at 0: 
            // 0: DEC EAX (0x48)
            // 1: JNZ rel8 (0x75 0xFD)
            // 3: HLT (0xF4)
            test_ram[0] = 0x48;
            test_ram[1] = 0x75;
            test_ram[2] = 0xFD;
            test_ram[3] = 0xF4;
            
            // The PIT is held off so the guest loop runs to its own HLT and the
            // instruction count is a real number. It used to be set to 20,
            // which exited after ~20 instructions -- while the Python side
            // divided by a hardcoded 1,000,000 and printed the result as MIPS.
            // (20 was necessary back when DEC/JNZ could not terminate, because
            // JNZ read a ZF that nothing ever set. The flag layer fixed that.)
            cpu_state_dtcm.pit_reload = 0xFFFFFFFF;
            cpu_state_dtcm.pit_counter = 0xFFFFFFFF;
            
            // Initialize EAX (iterations)
            cpu_state_dtcm.regs[0] = 500000; 
            // Initialize EBX (interrupt counter)
            cpu_state_dtcm.regs[3] = 0;
            // Initialize ESP (Stack Pointer) for INT 8 to push onto
            cpu_state_dtcm.regs[4] = 0x3000;
            
            uint32_t start_cycles = DWT_CYCCNT;
            x86_cpu_interp_asm();
            uint32_t end_cycles = DWT_CYCCNT;

            benchmark_cycles = end_cycles - start_cycles;
            // Report what actually executed rather than letting the host assume:
            // each loop iteration is DEC EAX + JNZ, plus the terminating HLT.
            benchmark_insns = 2u * (500000u - cpu_state_dtcm.regs[0]) + 1u;
            run_benchmark_flag = 0;
        }
        __asm__ volatile ("nop");
    }
}
