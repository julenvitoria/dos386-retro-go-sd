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
    /* String-op fast path. `rep_handler` is the dispatch-table entry the F2/F3
       prefix resolved for the string opcode that follows it, so `.str_exit` can
       re-enter that handler directly for the next element instead of rewinding
       EIP and paying a second trip through fetch/decode/dispatch. `rep_batch`
       is the remaining hard cap on how many elements one batch may run before
       it must go back through the fetch loop; the prefix reloads it. */
    uint32_t rep_handler;
    uint32_t rep_batch;
    /* Where the NOR cold-page trap stubs collect their answer.
     *
     * It lives in the CPU state, not in a global, for one mechanical reason:
     * the stub must restore R0-R3 before it can use the resolved pointer (the
     * destination register may be any of them, or R12), so the value cannot
     * come back in R0 -- and one LDR off R8 reaches DTCM, whereas a global
     * would need a literal pool inside the ITCM hot section. Zero means the
     * page is genuinely absent and the stub takes the .mem_fault exit. */
    uint32_t nor_ptr;
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
#define X86_EXIT_HALT          6  /* HLT: idle until an interrupt, not a stop */

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
    uint32_t spk_phase;       /* 32-bit square-wave phase, carried across buffers */
    /* Speaker EVENTS, not a per-frame sample of the speaker.
     *
     * The renderer used to run once a frame and point-sample the CURRENT
     * state, so a tone that started and stopped between two calls was never
     * heard at all -- which is what "half the beeps are missing" was. Every
     * change to the audible frequency is timestamped in HOST milliseconds
     * (x86_io_time_ms) as the guest makes it, and the render then walks the
     * window and lays each segment down where it belongs.
     *
     * X86_SPK_EVENTS is small on purpose: a guest sweeping a siren changes the
     * divisor far faster than the output rate can show, and the overflow is
     * COUNTED (x86_spk_events_dropped) rather than papered over.
     *
     * The ring carries the PIT DIVISOR, not a frequency. It used to carry
     * `1193182 / divisor` as an integer, which threw away the pitch before the
     * generator ever saw it -- see x86_audio_wave.h. 0 means "gated off";
     * a guest reload of 0 is carried as 65536, so this cannot be uint16_t. */
#define X86_SPK_EVENTS 32u
    uint32_t spk_ev_ms[X86_SPK_EVENTS];
    uint32_t spk_ev_div[X86_SPK_EVENTS];
    uint8_t  spk_ev_head, spk_ev_tail;
    uint32_t spk_last_div;    /* divisor at the last logged event            */
    uint32_t spk_render_div;  /* divisor the renderer left off at            */
    uint32_t spk_window_ms;   /* host ms the next render's window starts at  */
    uint8_t  spk_was_on;      /* last rendered run was audible -> edge detect */
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
extern uint8_t x86_video_mode_byte;   /* current BIOS video mode */
void x86_video_set_beam(uint32_t frac);
void x86_video_set_mode(uint8_t mode);
uint8_t x86_video_mode(void);
int x86_video_is_text(void);
void x86_video_set_cursor(uint8_t col, uint8_t row);
/* The first MCB, i.e. where memory becomes the guest's to allocate. Anything
   the emulator plants for its own use has to live BELOW this, because a loaded
   program writes over whatever it finds above it. Shared so the HLE layer can
   check its stub placement against it at compile time. */
#define X86_DOS_ARENA_SEG 0x0070u

uint16_t x86_video_get_cursor(void);
uint16_t x86_video_read_cell(void);
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
/* Scancode -> ASCII, the same decode INT 09h uses. Shared with the on-screen
   keyboard, whose echo line must show what the guest was actually sent. */
uint8_t x86_kbd_ascii(uint8_t sc, int shift);
/* The 8x8 CP437 glyphs handed to the guest at F000:F000. One copy, borrowed by
   the OSK renderer -- see the accessor's comment in x86_hle.c. */
const unsigned char (*x86_font8x8(void))[8];
void x86_hle_set_zf(int set);
int x86_video_map(void);
uint8_t *x86_video_vram(void);
void x86_video_render(uint8_t *dst, uint32_t stride);

void x86_mem_pool_init(void *base, uint32_t bytes);
uint32_t x86_mem_pool_free(void);
int x86_map_range(uint32_t guest, uint32_t bytes);
int x86_map_fixed(uint32_t guest, void *host, uint32_t bytes);
/* Mappings REFUSED (NULL backing, or the page pool ran dry). Non-zero means a
   guest region is deliberately absent; the guest then takes a clean
   X86_EXIT_MEM_FAULT instead of hard-faulting the interpreter. */
extern volatile uint32_t x86_map_failures;

/* NOR cold-page backing (x86_nor.c). x86_nor_attach() takes bytes that are
   ALREADY durable and byte-verifies them before it moves the fold table, so a
   failed snapshot leaves the guest running unchanged out of AXI. The two
   slow_* entries are called from the trap stubs in cpu_exec.S, never from C. */
int  x86_nor_attach(void *nor, uint32_t nor_bytes, void *ram, uint32_t ram_bytes,
                    uint32_t first_page);
uint32_t x86_nor_cow(uint32_t bad);   /* host-side pull-back; 0 = not NOR */
void x86_nor_slow_r(uint32_t bad);
void x86_nor_slow_w(uint32_t bad);
extern volatile uint32_t x86_nor_attached, x86_nor_base_addr, x86_nor_bytes;
extern volatile uint32_t x86_nor_pages_total, x86_nor_pages_nor, x86_nor_pages_axi;
extern volatile uint32_t x86_nor_axi_free, x86_nor_reads, x86_nor_writes;
extern volatile uint32_t x86_nor_cow_pulls, x86_nor_cow_failures;
extern volatile uint32_t x86_nor_slow_cycles, x86_nor_unmapped, x86_nor_attach_err;
extern volatile uint32_t x86_nor_host_cow;
void x86_unmap_all(void);
void x86_mem_wrap_1mb(void);
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
int x86_kb_get(uint16_t *key);
int x86_kb_peek(uint16_t *key);
int x86_bios_time(uint8_t func);
int x86_dos_int21(uint8_t func);
int x86_dos_terminate(void);
void x86_dos_arena_init(void);
void x86_disk_set_memory(const void *base, uint32_t bytes);
void x86_disk_set_reader(int (*rd)(void *, void *, uint32_t, uint32_t),
                         void *ctx, uint32_t bytes);
/* Optional: makes the guest floppy writable. Without it disk_write refuses. */
void x86_disk_set_writer(int (*wr)(void *, const void *, uint32_t, uint32_t));
int x86_disk_ready(void);
/*
 * Disk instrumentation and the sector cache. Counts are exact everywhere;
 * TIMES are meaningless under gwemu, which does not model SD busy state.
 */
void x86_disk_stats_reset(void);
extern volatile uint32_t x86_disk_reads;           /* disk_read() calls       */
extern volatile uint32_t x86_disk_read_sectors;
extern volatile uint32_t x86_disk_backend_reads;   /* reached the SD          */
extern volatile uint32_t x86_disk_backend_sectors;
extern volatile uint32_t x86_disk_cycles_last;
extern volatile uint32_t x86_disk_cycles_worst;
extern volatile uint32_t x86_disk_us_total;
extern volatile uint32_t x86_disk_us_worst;
extern volatile uint32_t x86_disk_cycles_per_us;   /* measured by the core    */
extern volatile uint32_t x86_disk_errors;
extern volatile uint32_t x86_disk_run_hist[8];     /* sectors per call        */
/*
 * Sector cache + read-ahead. `mem` must be at least x86_disk_cache_bytes and
 * is expected to come from the LCD bonus pool (uncached AXI); RAM_EMU has no
 * room. Returns 0 if it could not be backed, and the caller MUST report that:
 * the cache silently absent is a stutter nobody can explain.
 */
#define X86_DISK_CACHE_BYTES (64u * 1024u)   /* 4 windows x 16 KB */
int  x86_disk_cache_init(void *mem, uint32_t bytes);
void x86_disk_cache_flush(void);
extern volatile uint32_t x86_disk_cache_ok;        /* 0 = NOT backed          */
extern volatile uint32_t x86_disk_cache_bytes;
extern volatile uint32_t x86_disk_cache_hits;      /* sectors                 */
extern volatile uint32_t x86_disk_cache_misses;
extern volatile uint32_t x86_disk_cache_fills;
extern volatile uint32_t x86_disk_cache_evictions;
extern volatile uint32_t x86_disk_cache_bypass;
extern volatile uint32_t x86_disk_cache_invalidates;
int x86_dos_exec(const char *path, const char *cmdline);
uint16_t x86_dos_psp(void);
extern uint32_t x86_dos_last_error;
/* Last path the guest opened or created. Diagnostic only: an unserviced file
   request is far easier to place with the name in hand. */
extern char x86_dos_last_path[32];
/*
 * File-family INT 21h trace. x86_hle_log is useless for a file bug: a guest
 * polling the keyboard fills all 32 slots between one open and the next. This
 * ring records ONLY the file calls, as pairs of words --
 *   [2i]   (func << 8) | AL
 *   [2i+1] (CF << 16) | AX-returned
 * -- plus the path of the last four opens, so "which call, on which name, with
 * what answer" is one dump instead of a rebuild.
 */
extern uint32_t x86_dos_file_log[64];
extern uint32_t x86_dos_file_log_idx;
extern char x86_dos_path_log[4][32];
uint16_t *x86_hle_flags_on_stack(void);
uint16_t dos_test_alloc(uint16_t paras, uint16_t *largest);
uint16_t dos_test_free(uint16_t seg);
uint16_t dos_test_resize(uint16_t seg, uint16_t paras, uint16_t *largest);
uint16_t dos_test_walk(void);
extern uint32_t x86_hle_unhandled_vector, x86_hle_unhandled_func;

uint32_t x86_port_read(uint32_t port, uint32_t size);
void x86_port_write(uint32_t port, uint32_t value, uint32_t size);
uint32_t x86_speaker_divisor(void);   /* the signal path's pitch source */
uint32_t x86_speaker_hz(void);        /* reporting only */
void x86_speaker_render(int16_t *dst, unsigned frames, unsigned sample_rate,
                        int16_t amplitude);
/* Host millisecond clock, for timestamping speaker events. The core points
   this at the firmware's HAL_GetTick; the bare-metal harness leaves it NULL,
   which collapses every event onto the start of the window -- i.e. exactly the
   old point-sampled behaviour, so tier-1 expectations are unchanged. */
extern uint32_t (*x86_io_time_ms)(void);
extern volatile uint32_t x86_spk_events;          /* frequency changes logged */
extern volatile uint32_t x86_spk_events_dropped;  /* ring overflows           */
extern volatile uint32_t x86_spk_log[32];         /* (port<<24)|value, last 32 */
extern volatile uint32_t x86_spk_log_idx;
/* Port 0x60: how often the guest asked, and the last 32 bytes it was given. */
extern volatile uint32_t x86_kb_pushes;
extern volatile uint32_t x86_kb_pops;
extern volatile uint32_t x86_kb_peek_hits;
extern volatile uint32_t x86_kbd_port60_reads;
extern volatile uint32_t x86_kbd_read_log[32];
extern volatile uint32_t x86_kbd_read_log_idx;

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
