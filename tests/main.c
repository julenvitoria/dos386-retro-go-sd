#include <stdint.h>
#include <string.h>
#include "../lib/FatFs/ff.h"

#include "../cutils.h"
#include "../x86_cpu.h"
#include "../x86_profile.h"

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

volatile uint32_t run_video_flag = 0;
volatile uint32_t video_blit_cycles = 0;
uint8_t cga_test_vram[X86_VRAM_BYTES];
uint8_t cga_test_out[320 * 240];
/* 32-bit ARGB entries, matching lcd_set_clut(). */
uint32_t cga_test_palette[32];   /* X86_TEXT_CLUT_ENTRIES: 32 is what the
                                    firmware needs for its darkened twins to
                                    line up with LCD_DARKEN_BIT, and what the
                                    anti-aliased text path allocates from */
/* Text-mode arms. run_video_flag 4 selects a mode (so the host can put the
   renderer into mode 3 and get the CLUT reloaded), 5 runs a scroll. The cell
   pattern itself is written straight into cga_test_vram, which IS the text
   buffer -- that is the path a real DOS program takes, poking B800:0 rather
   than going through INT 10h. */
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

        if (run_video_flag == 1) {
            x86_video_init(cga_test_vram, cga_test_palette);
            /* init clears VRAM; the host's pattern is written after the flag
               is armed, so re-arm ordering matters -- see tests/test_video.py */
            run_video_flag = 2;
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
