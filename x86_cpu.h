/*
 * x86 CPU emulator
 * 
 * Copyright (c) 2011-2017 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "iomem.h"

typedef struct {
    uint16_t sel;
    uint16_t flags;
    uint32_t base;
    uint32_t limit;
} X86CPUSeg;

/* CS.flags bit 6 is DESC_D_MASK: it is bit 22 of the 32-bit word the
   interpreter loads at SEG_CS_SEL, and it selects the DEFAULT operand and
   address size. Zeroing the CPU struct therefore selects 16-bit, which is
   architecturally right for a real-mode reset but means any harness running
   32-bit guest code must set it explicitly. */
#define X86_DESC_D_FLAG  (1u << 6)

struct X86CPUState {
    uint32_t eip;
    uint32_t eflags;
    uint32_t regs[8];
    uint32_t idtr_base;
    uint16_t idtr_limit;
    X86CPUSeg segs[6];
    
    // 386 System Registers
    uint32_t cr0;
    uint32_t cr2;
    uint32_t cr3;
    uint32_t cr4;
    
    // Virtual to Host Physical TLB. Direct-mapped, 128 read + 128 write entries.
    // Each entry maps (Virtual Page Number) -> (Host Physical Base).
    // Tag array: bits 31:12 = VPN, bit 0 = Valid.
    //
    // RESERVED, NOT YET USED -- 2 KB of DTCM currently doing nothing. Kept
    // deliberately: this is the backing for the software TLB that will serve
    // addresses above the direct fold window, which is what removes the current
    // 8 MB aliasing bug (the 11-bit VPN in MEM_GET_PTR is untagged, so any
    // access at or above 8 MB silently wraps into the low 8 MB). See PLAN.md
    // Phase 1. Do not repurpose without reading that first.
    uint32_t tlb_read_tag[128];
    uint32_t tlb_read_host[128];

    uint32_t tlb_write_tag[128];
    uint32_t tlb_write_host[128];
    
    // Bare-metal PIT Channel 0 state
    uint32_t pit_counter;
    uint32_t pit_reload;
    uint32_t hard_int_pending;
    
    // CPU Loop Exit State
    uint32_t exit_reason;
    uint32_t exit_info;

    // Active opcode dispatch table (opcode_table_16 or opcode_table_32).
    // Selected from CS.D at loop entry and reloaded from DTCM on every fetch.
    // Kept here rather than in a reserved register so handlers may use R12
    // freely as scratch (it is also the ARM intra-procedure scratch register).
    uint32_t opcode_table_base;
    /* REP state. Set by the F2/F3 prefix, consumed and cleared at the next
       instruction boundary -- see .fetch_decode_loop. */
    uint32_t rep_active;
    uint32_t rep_resume;      /* linear address of the prefix byte */
    uint32_t rep_zf_sense;    /* 1 = REP/REPE (stop on ZF==0), 0 = REPNE */
    /* Non-zero enables the 0F 0B hypercall. When 0 it raises a real #UD, so a
       guest can be run with HLE off and still behave like a plain 386. */
    uint32_t hle_enabled;
};
typedef struct X86CPUState X86CPUState;

/* Why x86_cpu_interp_asm returned. Emitted into x86_offsets.inc by offset.c so
   the assembly and the C/Python consumers cannot disagree. */
#define X86_EXIT_NONE          0
#define X86_EXIT_TERMINATE     1  /* guest asked to exit (INT 21h AH=4Ch) */
#define X86_EXIT_HLE_INT       2  /* interrupt the host should service */
#define X86_EXIT_TIMER         3  /* PIT counter expired */
#define X86_EXIT_UNIMPLEMENTED 4  /* HLT, or an opcode with no handler */
#define X86_EXIT_MEM_FAULT     5  /* guest access to an unmapped page */
#define X86_EXIT_GPF           13 /* EXCP0D_GPF */

/* Address-translation fold.
 *
 * Each entry is an OFFSET k, not a base: host = guest_linear + k. A mapped
 * page's k puts the result in RAM (0x20.. / 0x24..), so bit 31 is clear; an
 * unmapped page stores FOLD_INVALID, which drives the result negative. One
 * ADDS + BMI therefore validates every access at zero extra cost, replacing
 * the old BFC/BFI pair that trusted the entry blindly -- and a zero entry used
 * to alias address 0, which is ITCM, so a stray guest write corrupted the
 * interpreter's own code. */
/* Mode-record geometry. Mirrored by the .equ block in cpu_exec.S. */
/* Guest I/O device state. Lives outside X86CPUState: the interpreter never
   touches it, only the C port handlers do. */
typedef struct {
    uint16_t pit2_reload;     /* channel 2 divisor -> speaker frequency */
    uint8_t  pit2_access;     /* latched access mode from port 0x43 */
    uint8_t  pit2_latched;    /* half-way through a lo/hi pair */
    uint8_t  port61;          /* bit0 = ch2 gate, bit1 = speaker data */
    uint32_t spk_phase;       /* 16.16 square-wave phase, carried across buffers */
    uint8_t  cga_mode;        /* 0x3D8 mode control */
    uint8_t  cga_color_select;/* 0x3D9 background + palette + intensity */
    uint16_t cga_beam;        /* 0..0xFFFF through the frame, set by the core */
    uint32_t cga_status_reads;/* advances per 0x3DA read so poll loops progress */
    uint32_t input_keys;      /* host buttons, latched each frame */
    /* 8042-style scancode queue. Deliberately modelled at the port level:
       ports and IRQs behave identically in real and protected mode, whereas an
       INT 16h shim planted in the IVT is real-mode only. A game that installs
       its own INT 09h and reads 0x60 directly -- which most action games do --
       needs nothing above this layer. */
    uint8_t  kbd_queue[16];
    uint8_t  kbd_head, kbd_tail;
    uint8_t  kbd_last;        /* last byte handed out on port 0x60 */
} X86IOState;

extern X86IOState x86_io;

/* The fold table the interpreter dereferences. Defined in x86_cpu.c and
   repointed at runtime by the core, which allocates DTCM through mem_ctl. */
extern uint32_t *guest_page_table_ptr;
extern X86CPUState *cpu_state_ptr;

/* Dedicated CGA VRAM. Sized for the full 32 KB aperture at 0xB8000 even though
   mode 4 uses 16 KB, so text and the other CGA modes fit without remapping. */
#define X86_VRAM_BYTES 32768u

void x86_video_init(void *vram, void *lut8_palette);
void x86_video_refresh_palette(void);
extern uint32_t cga_clut_dirty;   /* frame loop re-pushes the CLUT when set */
void x86_video_set_beam(uint32_t frac);
void x86_video_set_mode(uint8_t mode);
uint8_t x86_video_mode(void);
int x86_video_is_text(void);
void x86_video_set_cursor(uint8_t col, uint8_t row);
uint16_t x86_video_get_cursor(void);
void x86_video_scroll(uint8_t lines, uint8_t attr);
void x86_video_putc_attr(uint8_t ch, uint8_t attr, uint16_t count);
void x86_video_teletype(uint8_t ch);
void x86_video_putpixel(uint16_t x, uint16_t y, uint8_t colour);
void x86_video_set_palette_bios(uint8_t which, uint8_t value);
void x86_bios_init(void);
void x86_bios_tick(void);
void x86_kb_push(uint8_t scancode, uint8_t ascii);
void x86_kbd_scancode(uint8_t code);
int x86_kbd_pending(void);
int x86_bios_kbd_irq(void);
void x86_hle_set_zf(int set);
int x86_video_map(void);
uint8_t *x86_video_vram(void);
void x86_video_render(uint8_t *dst, uint32_t stride);

void x86_mem_pool_init(void *base, uint32_t bytes);
uint32_t x86_mem_pool_free(void);
int x86_map_range(uint32_t guest, uint32_t bytes);
int x86_map_fixed(uint32_t guest, void *host, uint32_t bytes);
void x86_unmap_all(void);
void *x86_guest_ptr(uint32_t lin, uint32_t bytes);
uint32_t x86_guest_copy_out(void *dst, uint32_t lin, uint32_t bytes);
uint32_t x86_guest_copy_in(uint32_t lin, const void *src, uint32_t bytes);

/* HLE services. */
int x86_hle_install(const uint8_t *vectors, unsigned count);
int x86_bios_rom_install(void);
int x86_hle_dispatch(uint8_t vector);
int x86_hle_service(void);
extern uint32_t x86_hle_calls;
void x86_hle_set_cf(int set);
void x86_set_ax(uint16_t v);
void x86_set_bx(uint16_t v);
void x86_set_cx(uint16_t v);
void x86_set_dx(uint16_t v);
void x86_set_al(uint8_t v);
void x86_set_ah(uint8_t v);
int x86_bios_video(uint8_t func);
int x86_bios_keyboard(uint8_t func);
int x86_bios_time(uint8_t func);
int x86_dos_int21(uint8_t func);
int x86_dos_terminate(void);
void x86_dos_arena_init(void);
void x86_disk_set_memory(const void *base, uint32_t bytes);
void x86_disk_set_reader(int (*rd)(void *, void *, uint32_t, uint32_t),
                         void *ctx, uint32_t bytes);
int x86_disk_ready(void);
int x86_dos_exec(const char *path, const char *cmdline);
uint16_t x86_dos_psp(void);
extern uint32_t x86_dos_last_error;
uint16_t dos_test_alloc(uint16_t paras, uint16_t *largest);
uint16_t dos_test_free(uint16_t seg);
uint16_t dos_test_resize(uint16_t seg, uint16_t paras, uint16_t *largest);
uint16_t dos_test_walk(void);
extern uint32_t x86_hle_unhandled_vector, x86_hle_unhandled_func;

uint32_t x86_port_read(uint32_t port, uint32_t size);
void x86_port_write(uint32_t port, uint32_t value, uint32_t size);
uint32_t x86_speaker_hz(void);
void x86_speaker_render(int16_t *dst, unsigned frames, unsigned sample_rate,
                        int16_t amplitude);

#define X86_MODE_REC_STRIDE     2048
#define X86_MODE_REC_TABLE_OFF  1024
#define X86_MODE_RECORDS_BYTES  (4 * X86_MODE_REC_STRIDE)
#define X86_MODE_RECORDS_ALIGN  8192

void x86_build_mode_records(void *dtcm_block);
uint32_t x86_mode_table(int op32, int addr32);
extern uint8_t *mode_records_ptr;

#define FOLD_SHIFT       12                 /* 4 KB granules */
#define FOLD_ENTRIES     4096               /* 16 MB direct window */
#define FOLD_INVALID     0x80000000u        /* forces bit 31 of the result */
#define FOLD_K(host, idx)  ((uint32_t)(host) - ((uint32_t)(idx) << FOLD_SHIFT))

/* get_reg/set_reg additional constants */
#define X86_CPU_REG_EIP 8
#define X86_CPU_REG_CR0 9
#define X86_CPU_REG_CR2 10

#define X86_CPU_SEG_ES 0
#define X86_CPU_SEG_CS 1
#define X86_CPU_SEG_SS 2
#define X86_CPU_SEG_DS 3
#define X86_CPU_SEG_FS 4
#define X86_CPU_SEG_GS 5
#define X86_CPU_SEG_LDT 6
#define X86_CPU_SEG_TR 7
#define X86_CPU_SEG_GDT 8
#define X86_CPU_SEG_IDT 9

X86CPUState *x86_cpu_init(PhysMemoryMap *mem_map);
void x86_cpu_end(X86CPUState *s);
void x86_cpu_interp(X86CPUState *s, int max_cycles1);
void x86_cpu_set_irq(X86CPUState *s, BOOL set);
void x86_cpu_set_reg(X86CPUState *s, int reg, uint32_t val);
uint32_t x86_cpu_get_reg(X86CPUState *s, int reg);
void x86_cpu_set_seg(X86CPUState *s, int seg, const X86CPUSeg *sd);
void x86_cpu_set_get_hard_intno(X86CPUState *s,
                                int (*get_hard_intno)(void *opaque),
                                void *opaque);
void x86_cpu_set_get_tsc(X86CPUState *s,
                         uint64_t (*get_tsc)(void *opaque),
                         void *opaque);
void x86_cpu_set_port_io(X86CPUState *s, 
                         DeviceReadFunc *port_read, DeviceWriteFunc *port_write,
                         void *opaque);
void x86_cpu_sync_port_map(X86CPUState *s, PhysMemoryMap *port_map);
int64_t x86_cpu_get_cycles(X86CPUState *s);
BOOL x86_cpu_get_power_down(X86CPUState *s);
void x86_cpu_flush_tlb_write_range_ram(X86CPUState *s,
                                       uint8_t *ram_ptr, size_t ram_size);
