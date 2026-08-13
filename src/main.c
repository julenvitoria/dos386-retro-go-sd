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
#include "x86_profile.h"
#include "x86_osk.h"

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
/*
 * The LUT8 CLUT we hand the firmware. 32 entries, and the COUNT WE PUSH IS
 * ALWAYS 32, even though CGA graphics uses four of them and text sixteen.
 *
 * That is a hard requirement of the firmware's darkening contract, not
 * tidiness: lcd_set_clut() programs the darkened twin of entry i at i+count,
 * while odroid_overlay_darken_all() dims a pixel by ORing LCD_DARKEN_BIT
 * (0x20 == 32) into its index. The two only agree at count == 32. Pushing 4
 * put the twins at 4..7 while the overlay asked for 32..35 -- unprogrammed,
 * i.e. black -- so opening the menu blanked the guest picture underneath.
 * Pushing 32 also stops mode 3 from losing colours 4..15.
 */
#define CORE_CLUT_ENTRIES 32
static uint32_t core_clut[CORE_CLUT_ENTRIES];
static uint8_t *upper_filler;
static uint8_t *bios_id_page;   /* 0xFF000: real storage, not an alias */

/* Host-readable so a failure to place either block is visible rather than a
   black screen. */
volatile uint32_t guest_ram_bytes = 0;
volatile uint32_t guest_vram_ok = 0;
volatile uint32_t upper_filler_ok = 0;   /* 0xA0000-0xFFFFF backed at all */
volatile uint32_t frames_rendered = 0;
/*
 * ===========================================================================
 * THREE CLOCKS, and the fact that they are three.
 * ===========================================================================
 *
 * One constant used to do all three jobs (PIT_SLICE_INSNS: guest instructions
 * per iteration), which is why moving it broke unrelated things -- the frame
 * rate, the BIOS tick rate and the emulated machine's speed were the same
 * number wearing three hats. They are now separate, and none of them is a
 * hardcoded 60:
 *
 *   1. DISPLAY RATE -- follows the hardware. The loop renders, asks for a
 *      swap, and waits on GW_LCD_IS_SWAP_PENDING. That is the panel's own
 *      signal, so an overclocked device or a different refresh rate paces us
 *      correctly with no constant to update. Nothing here assumes 60 fps.
 *
 *   2. GUEST WALL CLOCK -- the firmware's millisecond counter, see the tick
 *      block below. The 18.2065 Hz PIT/BIOS tick derives from elapsed real
 *      time and from nothing else, which is what makes music, delays and
 *      INT 1Ah correct at any frame rate and any host speed.
 *
 *   3. GUEST CPU RATE -- a configurable target, PACED AGAINST WALL TIME. The
 *      instruction budget is per elapsed millisecond:
 *
 *          budget = guest_kips * elapsed_ms          (kips = insns per ms)
 *
 *      so overclocking the H7 does not make the guest faster; it makes the
 *      budget arrive sooner and the leftover become idle, which the panel
 *      wait spends. That is the point: the emulated CPU's speed is a property
 *      of the emulated machine, not of ours.
 *
 * kips is "thousands of instructions per second", which is also exactly
 * "instructions per millisecond" -- so the budget is an integer multiply with
 * no remainder to carry and no division in the loop.
 *
 * WHEN WE CANNOT KEEP UP, IT IS LOUD. Unmet budget accumulates in insn_debt;
 * past GUEST_DEBT_MAX it is written off into guest_insn_deficit, a
 * host-readable count of instructions the emulated CPU owed and never ran.
 * A guest quietly running at half its configured speed is indistinguishable
 * from a correct one, which is the exact failure this project keeps hitting.
 *
 * TWO PACING MODES, one knob (guest_kips):
 *
 *   guest_kips > 0   PACED. The guest runs at that many thousand instructions
 *                    per second and NO FASTER, however fast the silicon is.
 *                    Leftover time becomes idle in the panel wait. This is
 *                    what makes a speed-sensitive title behave, and what makes
 *                    overclocking the H7 change nothing about game speed. The
 *                    number to watch is guest_insn_deficit: it is the answer
 *                    to "I asked for a 386 -- am I getting one?".
 *
 *   guest_kips == 0  UNTHROTTLED. No budget, no idle: run as much as fits
 *                    between panel frames, which is what the benchmark and
 *                    the optimisation work want to see. The slice sizes itself
 *                    from measured execution time against panel_ms_min -- the
 *                    shortest iteration the hardware has ever let us complete,
 *                    i.e. the display's own period -- so it tracks the panel
 *                    rather than a hardcoded 60. A deficit is meaningless
 *                    here; guest_ips_achieved is the number.
 *
 * The BIOS tick is wall-clock in BOTH modes. Unthrottling the CPU must not
 * unthrottle time -- that is the bug being fixed, not a mode.
 */
#define KIPS_MAX              0u   /* as fast as the silicon delivers */
#define KIPS_8088_477       330u   /* 8088 @ 4.77 MHz, ~0.33 MIPS     */
#define KIPS_286_8          800u   /* 286 @ 8 MHz                     */
#define KIPS_286_12        1200u   /* 286 @ 12 MHz                    */
#define KIPS_386SX_16      2500u   /* 386SX @ 16 MHz                  */
#define KIPS_386_33        6000u   /* 386DX @ 33 MHz                  */
/*
 * DEFAULT: unthrottled. The emulated machine runs as fast as the silicon
 * delivers, and the number that describes it is guest_ips_achieved, not a
 * deficit -- with no target there is nothing to be short OF, so the deficit
 * counter is held at zero in this mode rather than left showing a stale value
 * from a paced one. A period-correct machine is one menu entry away
 * (cpu_profile_*, below), and the choice persists across a restart.
 */
volatile uint32_t guest_kips = KIPS_MAX;      /* 0 = unthrottled */
volatile uint32_t guest_slice_now  = 0;  /* instructions this iteration got   */
volatile uint32_t guest_insn_debt  = 0;  /* owed but not yet run              */
volatile uint32_t guest_insn_deficit = 0;/* owed and WRITTEN OFF -- the loud   */
volatile uint32_t guest_ips_achieved = 0;/* measured instructions per second  */
volatile uint32_t guest_fps_achieved = 0;/* measured iterations per second    */
volatile uint32_t guest_insns_total = 0; /* instructions run since boot       */
volatile uint32_t frame_ms_last    = 0;  /* last iteration, host ms           */
volatile uint32_t panel_ms_min     = 0;  /* shortest iteration seen = the panel*/
volatile uint32_t frame_10us_avg = 8u * 1667u; /* mean iteration, 8x, 10us units */
volatile uint32_t guest_exec_ms    = 0;  /* host ms spent RUNNING the guest    */
/* Did the last slice end because the GUEST halted? Then guest_exec_ms is short
   because the guest asked to idle, NOT because we had spare capacity. Growing
   the slice on that reading is how the controller inflated to its ceiling and
   then ran a multi-second slice that tripped the watchdog. */
static uint32_t   last_slice_halted = 0;
static uint32_t   max_slice = 20000u;    /* unthrottled mode's working slice  */
static uint32_t   rate_window_ms;        /* start of the achieved-rate window */
static uint32_t   rate_window_insns;
static uint32_t   rate_window_frames;
/* One iteration's work is bounded so the render, the input poll and the audio
   refill still happen when a guest stretch is expensive: a frame that owes
   200 ms of instructions must not spend 200 ms before drawing anything. */
#define GUEST_SLICE_MAX_MS  33u
#define GUEST_DEBT_MAX_MS  100u
#define MAX_SLICE_FLOOR   1000u
/* A TIME bound expressed in instructions. 50k at ~2 M ips is ~25 ms, inside
   the watchdog window, and still allows 3 M ips at 60 iterations/s. It was
   4,000,000 -- two SECONDS in a single slice with no return to the frame loop,
   which is exactly what tripped the watchdog once the controller had inflated
   to it. Do NOT derive this from measured throughput: the slice limits
   throughput, so a cap computed from throughput ratchets both down to the
   floor (measured: 59,642 ips, pinned there). */
#define MAX_SLICE_CEIL   50000u

/*
 * The guest's clock, and the fact that it is REAL time.
 *
 * 18.2065 Hz is 1193182/65536 -- the PIT input divided by channel 0's boot
 * divisor. A guest counts note durations, delays and animation frames in these
 * ticks, so if the tick rate moves, everything the user hears and sees moves
 * with it. Two earlier drivers both got that wrong, in opposite directions and
 * at the same time (they were merged into one loop):
 *
 *   - one derived the tick from RENDERED FRAMES (182/600 per frame, exact only
 *     at 60 fps). At the measured ~10 fps it fired at ~3 Hz, six times slow.
 *   - one derived it from the instruction SLICE (TICKS_PER_SLICE = 10 per
 *     iteration), which fired at ~100 Hz and moved with our throughput.
 *
 * Both are gone. The tick is now driven from the firmware's millisecond
 * counter, with the remainder carried in `tick_acc` so the long-run rate is
 * exact rather than accumulating a rounding error:
 *
 *      tick_acc += elapsed_ms * 1193182;  while (tick_acc >= 65536000) tick.
 *
 * The rejected alternative was to keep the slice-derived clock because a
 * benchmark divides fixed work by elapsed ticks and a too-slow clock reads as
 * zero elapsed ticks (a #DE). That is a real hazard, but the answer to it is
 * enough guest throughput per real second, not a clock that lies -- a clock
 * slaved to our own speed makes every benchmark measure 1.0 by construction.
 *
 * CATCH-UP IS CAPPED. A debugger halt or a long SD read owes hundreds of ticks
 * at once, and emitting them is worse than not: a guest hooked to INT 08h
 * would run its music routine hundreds of times back to back. Anything past
 * TICK_CATCHUP_MAX is dropped -- and COUNTED, in guest_ticks_dropped, because
 * time silently going missing is precisely the class of bug this whole file
 * exists to make visible.
 */
#define TICK_NUM          1193182u    /* PIT input, Hz                       */
#define TICK_DEN_MS      65536000u    /* 65536 (divisor) * 1000 (ms per s)   */
#define TICK_CATCHUP_MAX        4u    /* ~220 ms of arrears, then drop       */
#define TICK_STALL_MS        1000u    /* longer than this is a stall, not lag */
static uint32_t tick_acc;             /* ms * TICK_NUM, below one tick       */
static uint32_t tick_last_ms;
volatile uint32_t guest_ticks = 0;          /* ticks handed to the guest     */
volatile uint32_t guest_ticks_dropped = 0;  /* owed but refused (see above)  */
volatile uint32_t guest_tick_irq_lost = 0;  /* tick IRQ the 1-deep slot ate  */
volatile uint32_t guest_uptime_ms = 0;      /* host ms since the loop started */

volatile uint32_t guest_exit_reason = 0;
volatile uint32_t guest_halted = 0;
volatile uint32_t fb_addr = 0;        /* the buffer the blit was handed */
volatile uint32_t fb_sample = 0;      /* first 4 pixels of the picture area */
volatile uint32_t clut_sample = 0;    /* colour 1, to prove the CLUT took */
volatile uint32_t hle_calls_seen = 0;
/* Input chain, made observable end to end. The failure mode this exists for is
   "nothing happens when I press a key", which has five candidate breaks and no
   way to tell them apart from outside: the poll not running, the firmware
   handing us an empty pad, the mapping producing no bits, no scancode reaching
   the controller queue, or the IRQ never being delivered. One counter per
   stage, read with scripts/probe.py --watch. */
volatile uint32_t input_polls = 0;      /* poll_input() calls               */
volatile uint32_t input_pad_raw = 0;    /* last raw pad, bit N = values[N]  */
volatile uint32_t input_pad_seen = 0;   /* sticky OR of the above           */
volatile uint32_t input_keys_now = 0;   /* our 8-bit mapped key mask        */
volatile uint32_t input_scancodes = 0;  /* codes handed to the controller   */
volatile uint32_t input_irqs = 0;       /* IRQ 1 raises                     */
volatile uint32_t input_kbd_ht = 0;     /* controller head | tail<<8        */
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

#if X86_PROFILE
    /* Profiling build only. DTCM is a firmware bump pool here, not a load
       target, so the block is allocated like everything else -- and a failure
       must reach core_dtcm_ok, because the dispatch tail dereferences
       x86_profile_ptr on EVERY instruction. */
    if (!x86_profile_init((void *)abi->mem_ctl(GW_MEM_OP_ALLOC, GW_MEM_DTC, 1,
                                               X86_PROFILE_BYTES))) {
        return false;
    }
#endif
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
    guest_ram_bytes = guest_ram ? GUEST_RAM_SIZE : 0;

    /* The open-bus filler comes from the BONUS POOL, not from RAM_EMU.
     *
     * RAM_EMU headroom past our BSS is a few hundred bytes (see TOOLING.md),
     * and this page is the last thing allocated, so it is the first casualty
     * of any growth in the core -- silently, because the region it backs is
     * only ever PROBED by option-ROM scans. The bonus pool is 150 KB of AXI we
     * claim and do not use, and uncached is irrelevant for a page that is
     * written once and read rarely.
     *
     * ram_malloc() returns NULL rather than overrunning, so a failure here is
     * a missing mapping, not corruption -- but it must still be visible. */
    if (bonus && bonus_sz >= 4096)
        upper_filler = bonus;
    else
        upper_filler = (uint8_t *)abi->mem_ctl(GW_MEM_OP_ALLOC, GW_MEM_RAM,
                                               1, 4096);
    upper_filler_ok = upper_filler ? 1 : 0;

    /* The BIOS identification page at 0xFF000 needs REAL, PERSISTENT storage:
     * it is written once and read by every program that identifies the
     * machine, so it cannot alias the open-bus filler. It used to come from
     * x86_map_range()'s page pool -- but that pool is never initialised
     * (x86_mem_pool_init() is called from nowhere), so the request always
     * failed and the page was simply absent. From the bonus pool, like the
     * filler, for the same reasons. */
    if (bonus && bonus_sz >= 8192)
        bios_id_page = bonus + 4096;
    else
        bios_id_page = (uint8_t *)abi->mem_ctl(GW_MEM_OP_ALLOC, GW_MEM_RAM,
                                               1, 4096);

    if (guest_vram) {
        /* The CLUT the LCD scans out through. LUT8 gives 256 entries; CGA
         * mode 4 uses four and the palette registers pick which. */
        /* ARGB8888; lcd_set_clut() caps the count at 32 and reads 32-bit
         * entries. All THIRTY-TWO are pushed, not four and not sixteen.
         *
         * Mode 4 uses only entries 0-3, but text attributes index the whole
         * CGA palette, and pushing four left 4-15 holding whatever retro-go's
         * menu had -- so every bright or coloured attribute drew in a stale
         * colour or in black.
         *
         * Sixteen is not enough either, for a reason that is invisible until
         * someone opens retro-go's menu: lcd_set_clut() writes a darkened twin
         * of entry i at `count + i`, but the overlay reaches those twins by
         * OR-ing LCD_DARKEN_BIT = 0x20 into each framebuffer byte, a fixed
         * +32. The two only agree at count == 32. At 16 the twins sat at 16-31
         * while the overlay read 32-47, which are zeros, so the menu dimmed
         * the game to solid black. Pushing 32 also gives the anti-aliased text
         * renderer the eight four-entry blocks it allocates per frame. */
        x86_video_init(guest_vram, core_clut);
        abi->lcd_ctl(GW_LCD_SET_CLUT, (uintptr_t)core_clut,
                     CORE_CLUT_ENTRIES, 0);
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
 * The write side of the same aperture. There is no ranged SD write in the ABI,
 * so this goes through the firmware's stdio, opening and closing per call to
 * match the read path -- the core holds no handle on the .dsk between
 * requests, which is what lets any image in /roms/dos be swapped in.
 *
 * The guest's floppy really is written. That is what a floppy does: TOPBENCH
 * leaves a `topbench.log` on it and a game leaves its save, and telling the
 * guest a write succeeded while dropping it is the failure mode this project
 * refuses everywhere else.
 */
static int sd_sector_write(void *ctx, const void *src, uint32_t offset,
                           uint32_t len)
{
    const gw_firmware_abi_t *abi = gw_firmware_abi();
    FILE *f = abi->fopen((char *)ctx, "r+b");
    if (!f)
        return -1;
    int rc = -1;
    if (abi->fseek(f, (long)offset, SEEK_SET) == 0 &&
        abi->fwrite(src, 1, len, f) == len)
        rc = 0;
    abi->fclose(f);
    return rc;
}

/*
 * Work out what to run. The disk images are built to boot FreeDOS, so the
 * program to launch is named in AUTOEXEC.BAT rather than being guessable from
 * the .dsk name -- ALLEYCAT.dsk runs CAT.EXE. Reading it keeps this working
 * for the other images too instead of hardcoding one game.
 */
static int find_startup_program(char *out, unsigned max,
                                char *tail, unsigned tail_max)
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
        /* The COMMAND TAIL, which was silently dropped. TOPBENCH.EXE -l and
           TOPBENCH.EXE are different programs -- one scores and exits, the
           other draws a menu and waits for a key that a headless run never
           sends -- so throwing away everything after the first token quietly
           changed what the disk was asked to do. */
        const char *rest = q + i;
        while (*rest == ' ' || *rest == '\t') rest++;
        unsigned j = 0;
        while (rest[j] && j < tail_max - 1) {
            tail[j] = rest[j];
            j++;
        }
        tail[j] = 0;
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
    x86_disk_set_writer(sd_sector_write);
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
    if (bios_id_page) {
        memset(bios_id_page, 0xFF, 4096);
        x86_map_fixed(0xFF000u, bios_id_page, 4096u);
    }
    x86_bios_rom_install();

    /* Everything above 1 MB wraps to the bottom, as it does with A20 masked. */
    x86_mem_wrap_1mb();

    x86_dos_arena_init();

    /* Load the real program if the disk mounted; fall back to the built-in
       demo so a missing or unreadable .dsk shows a picture rather than a black
       screen with no explanation. */
    char startup[64];
    char cmdtail[64];
    dsk_startup_ok = 0;
    dsk_exec_rc = 0xFFFFFFFF;
    if (dsk_mounted && find_startup_program(startup, sizeof(startup),
                                            cmdtail, sizeof(cmdtail)) == 0) {
        dsk_startup_ok = 1;
        dsk_exec_rc = (uint32_t)x86_dos_exec(startup, cmdtail);
    }
    if (dsk_exec_rc != 0) {
        memcpy(guest_ram + 0x100, demo_guest, sizeof(demo_guest));
        cpu_state_ptr->eip = 0x100;
        cpu_state_ptr->regs[4] = 0xFFFE;
    }
    cpu_state_ptr->eflags = 0x0202;              /* IF set, bit 1 reserved-1 */
    cpu_state_ptr->hle_enabled = 1;

    /* A first slice, replaced on the very next iteration by the wall-clock
       budget (see the three-clock block). The PIT exit is what ends a slice.
       It is NOT the clock -- the BIOS tick comes from real time -- it is how
       often we come up for air to render, poll and feed audio. The CGA
       retrace bit is derived from how far this counter has run down, so a
       slice is also the emulated machine's notion of a display frame. */
    x86_osk_reset();
    cpu_state_ptr->pit_reload = guest_slice_now ? guest_slice_now : 20000u;
    cpu_state_ptr->pit_counter = cpu_state_ptr->pit_reload;
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
/*
 * The machine has SIX buttons a guest may use: the d-pad, A, B, GAME and TIME.
 * The firmware reports GAME as ODROID_INPUT_START and TIME as
 * ODROID_INPUT_SELECT (odroid_input.c), so "Start" and "Game" are one physical
 * button and so are "Select" and "Time" -- there is no fourth face button to
 * give away.
 *
 * Which key each one sends is not cosmetic. Alley Cat opens with
 * "Do you want to use a joystick (Y/N)?" and reads port 0x60 for the answer:
 * with the previous table (Ctrl / Enter / Esc) NO button could produce a Y or
 * an N, so the game sat on that prompt forever and every later press went to a
 * program that had not started. That is the whole of "the guest ignores the
 * keyboard" -- the delivery chain underneath was already working.
 *
 * N, not Y, on the shared TIME/SELECT button: answering "no joystick" is what
 * puts the game on the keyboard we actually emulate. Y is unreachable until
 * the GAME button's on-screen keyboard exists.
 */
/* Indexed by the bit numbering poll_input builds below from the ODROID_INPUT_*
   enum -- the compiler resolves the enum, so this order is ours and is correct
   by construction. Do NOT "fix" it against odroid_input.h.
   0 means the button is deliberately bound to nothing. */
static const uint8_t pad_scancode[10] = {
    0x48,  /* UP                      -> Up arrow    */
    0x50,  /* DOWN                    -> Down arrow  */
    0x4B,  /* LEFT                    -> Left arrow  */
    0x4D,  /* RIGHT                   -> Right arrow */
    0x39,  /* A                       -> Space       */
    0x01,  /* B                       -> Esc         */
    0x00,  /* ODROID_INPUT_START = GAME button: reserved for the on-screen
              keyboard toggle (8086tiny docs/input/02 decided this deliberately:
              PAUSE belongs to retro-go, so GAME is the only usable door) */
    0x15,  /* ODROID_INPUT_SELECT = TIME button -> Y  */
    0x1C,  /* ODROID_INPUT_X = physical START   -> Return. Zelda-only in
              8086tiny's notes, but THIS unit reports it (raw pad bit 11). */
    0x31,  /* ODROID_INPUT_Y = physical SELECT  -> N   */
};

/*
 * ===========================================================================
 * The emulated CPU's speed, as a Retro-Go game-options entry.
 * ===========================================================================
 *
 * common_emu_input_loop() takes an options array and shows it in the PAUSE/SET
 * menu; we used to pass NULL, so guest_kips was reachable only over the debug
 * probe. The rungs are the KIPS_* profiles above, MAX first because that is the
 * default.
 *
 * Three things here are load-bearing, each of them a hardfault or a wrong
 * reading if got wrong:
 *
 *   - `value` must point at MUTABLE, caller-owned storage. The callback writes
 *     the rendered value into option->value, so a string literal there is a
 *     write to flash. It is static rather than automatic so it also outlives
 *     the dialog, which keeps the pointer.
 *   - the array is terminated with ODROID_DIALOG_CHOICE_LAST, and the repaint
 *     callback is NEVER NULL (see core_repaint).
 *   - the value is re-rendered on EVERY event, including the plain repaint,
 *     because the dialog draws from the buffer rather than from our state.
 */
/*
 * The settings key is NOT free-form. odroid_settings_app_int32_get/set match
 * the key against a fixed whitelist ("Palette", "SpriteLimit", "Region",
 * "DispScaling", "DispFilter", "DispOverscan", "DistRotation") and SILENTLY
 * ignore anything else -- a set with our own name would store nothing and the
 * get would hand back the default forever, which reads exactly like "the
 * setting does not persist". The slot is per-app (core_cfg_slot()), so
 * "Palette" is ours alone; nothing in this core has a palette setting.
 */
#define CPU_PROFILE_KEY   "Palette"
#define CPU_OPTION_ID     100

static const uint16_t cpu_profile_kips[] = {
    KIPS_MAX, KIPS_8088_477, KIPS_286_8, KIPS_286_12, KIPS_386SX_16, KIPS_386_33,
};
static const char *const cpu_profile_name[] = {
    "Max", "8088 4.77", "286 8MHz", "286 12MHz", "386SX 16", "386DX 33",
};
#define CPU_PROFILE_COUNT ((int)(sizeof(cpu_profile_kips) / sizeof(cpu_profile_kips[0])))

volatile uint32_t cpu_profile_persisted = 0xFF;  /* what the settings store kept */
volatile uint32_t cpu_profile_events = 0;        /* dialog callbacks seen      */
volatile uint32_t cpu_profile_last_event = 0xFF; /* last odroid_dialog_event_t */
static int  cpu_profile_idx;
static char cpu_profile_value[16];

/* Changing the emulated CPU's speed invalidates every number measured against
   the old one. Clearing them is what keeps "am I getting what I asked for?"
   answerable straight after a change instead of a minute later. */
static void cpu_profile_apply(int idx)
{
    if (idx < 0 || idx >= CPU_PROFILE_COUNT)
        idx = 0;
    cpu_profile_idx = idx;
    guest_kips = cpu_profile_kips[idx];
    guest_insn_debt = 0;
    guest_insn_deficit = 0;
    strcpy(cpu_profile_value, cpu_profile_name[idx]);
}

static bool cpu_profile_cb(odroid_dialog_choice_t *option,
                           odroid_dialog_event_t event, uint32_t repeat)
{
    (void)repeat;
    /* The menu path has no other witness: a malformed array simply never
       reaches us, which looks identical to a menu that never opened. */
    cpu_profile_events++;
    cpu_profile_last_event = (uint32_t)event;

    int idx = cpu_profile_idx;
    int max = CPU_PROFILE_COUNT - 1;

    if (event == ODROID_DIALOG_PREV) idx = idx > 0 ? idx - 1 : max;
    if (event == ODROID_DIALOG_NEXT) idx = idx < max ? idx + 1 : 0;

    if (event == ODROID_DIALOG_PREV || event == ODROID_DIALOG_NEXT) {
        const gw_firmware_abi_t *abi = gw_firmware_abi();
        cpu_profile_apply(idx);
        abi->odroid_settings_app_int32_set(CPU_PROFILE_KEY, idx);
        /* Read it straight back. A settings key the firmware does not know is
           ignored in silence, and "it did not persist" is only visible a
           reboot later -- this makes the round trip measurable now. */
        cpu_profile_persisted =
            (uint32_t)abi->odroid_settings_app_int32_get(CPU_PROFILE_KEY, 0xFF);
    }
    strcpy(option->value, cpu_profile_name[cpu_profile_idx]);
    return event == ODROID_DIALOG_ENTER;
}

static void cpu_profile_init(const gw_firmware_abi_t *abi)
{
    cpu_profile_apply(abi->odroid_settings_app_int32_get(CPU_PROFILE_KEY, 0));
}

/*
 * Repaint the guest frame under the overlay.
 *
 * NOT optional. Retro-Go's repaint_overlay() calls this callback
 * UNCONDITIONALLY -- `repaint()` with no NULL test -- so passing NULL to
 * common_emu_input_loop() branches to address 0 the moment PAUSE/SET opens the
 * menu: CFSR INVSTATE, HardFault, BSOD. (The save/load-state handlers ARE
 * NULL-guarded by odroid_system_emu_save_state/load_state; this one is not.)
 *
 * The menu draws into the ACTIVE buffer and calls us whenever it needs the
 * game picture back underneath, so render into the active buffer and do not
 * swap -- the menu owns the swap.
 */
static void core_repaint(void)
{
    const gw_firmware_abi_t *abi = gw_firmware_abi();
    uint8_t *fb = (uint8_t *)abi->lcd_ctl(GW_LCD_BUFFER, GW_LCD_BUF_ACTIVE, 0, 0);
    if (fb) {
        x86_video_render(fb, FRAME_W);
        /* The OSK coexists with retro-go's overlay rather than fighting it:
           the overlay draws ON TOP of whatever we leave in the active buffer
           and asks us for the picture back underneath, so the keyboard is part
           of that picture. */
        x86_osk_draw(fb, FRAME_W);
    }
}

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
    /* The array lives on this stack frame and the menu runs INSIDE the call,
       so it is alive for exactly as long as it is used; only the value buffer
       has to outlive the call, and it is static. */
    odroid_dialog_choice_t options[] = {
        {CPU_OPTION_ID, "CPU Speed", cpu_profile_value, 1, &cpu_profile_cb},
        ODROID_DIALOG_CHOICE_LAST,
    };
    abi->common_emu_input_loop(&pad, options, core_repaint);

    input_polls++;
    {
        uint32_t raw = 0;
        for (unsigned i = 0; i < ODROID_INPUT_MAX && i < 32; i++)
            if (pad.values[i]) raw |= 1u << i;
        input_pad_raw = raw;
        input_pad_seen |= raw;
    }

    uint32_t keys = 0;
    if (pad.values[ODROID_INPUT_UP])     keys |= 1u << 0;
    if (pad.values[ODROID_INPUT_DOWN])   keys |= 1u << 1;
    if (pad.values[ODROID_INPUT_LEFT])   keys |= 1u << 2;
    if (pad.values[ODROID_INPUT_RIGHT])  keys |= 1u << 3;
    if (pad.values[ODROID_INPUT_A])      keys |= 1u << 4;
    if (pad.values[ODROID_INPUT_B])      keys |= 1u << 5;
    if (pad.values[ODROID_INPUT_START])  keys |= 1u << 6;
    if (pad.values[ODROID_INPUT_SELECT]) keys |= 1u << 7;
    if (pad.values[ODROID_INPUT_X])      keys |= 1u << 8;
    if (pad.values[ODROID_INPUT_Y])      keys |= 1u << 9;

    uint32_t prev = x86_io.input_keys;
    uint32_t changed = keys ^ prev;
    x86_io.input_keys = keys;
    input_keys_now = keys;

    /*
     * ---- The on-screen keyboard's door -------------------------------------
     *
     * GAME cycles OFF -> keyboard -> mouse -> OFF (x86_osk.c). This function
     * does not know that: it asks x86_osk_visible() whether the OSK owns the
     * buttons and routes on that alone, so growing the cycle changes nothing
     * here. GAME is bound to nothing in pad_scancode[] for exactly this.
     *
     * The frame the OSK OPENS on and the frame it CLOSES on both belong to the
     * OSK, and neither emits a guest key: a button still held across the
     * transition must not read as a fresh press on either side of it.
     *
     * Anything the guest still has DOWN is released first. Our controller is
     * edge-driven from this table, so a key whose button is "let go" by a mode
     * switch rather than by the user would stay down inside the guest forever
     * -- a break code is the only thing that can clear it.
     */
    {
        const uint32_t game = 1u << 6;
        const int was_up = x86_osk_visible();

        if ((changed & game) && (keys & game)) {
            if (!was_up) {
                for (unsigned i = 0; i < 10; i++) {
                    uint8_t sc = pad_scancode[i];
                    if (sc && (prev & (1u << i))) {
                        x86_kbd_scancode((uint8_t)(sc | 0x80));
                        input_scancodes++;
                    }
                }
            }
            x86_osk_cycle();
        }

        if (x86_osk_visible() || was_up) {
            if (x86_osk_visible() && was_up)
                x86_osk_input(keys);
            goto deliver;
        }
    }

    if (!changed)
        return;

    for (unsigned i = 0; i < 10; i++) {
        if (!(changed & (1u << i)))
            continue;
        uint8_t sc = pad_scancode[i];
        if (!sc)
            continue;              /* bit deliberately unbound */
        x86_kbd_scancode((keys & (1u << i)) ? sc : (uint8_t)(sc | 0x80));
        input_scancodes++;
    }
deliver:
    input_kbd_ht = (uint32_t)x86_io.kbd_head | ((uint32_t)x86_io.kbd_tail << 8);

    /* Raise IRQ 1 and pull the next instruction boundary forward, so it is
       delivered now instead of at the end of the slice. The OSK reaches the
       guest through this SAME path a button does -- x86_kbd_scancode() then
       IRQ 1 -- which is the whole point of porting only the front end. */
    if (x86_kbd_pending()) {
        cpu_state_ptr->hard_int_pending = 0x09;
        cpu_state_ptr->pit_counter = 1;
        input_irqs++;
    }
}

static void emulator_loop(const gw_firmware_abi_t *abi)
{
    /* Give the I/O layer the host clock BEFORE the guest runs: it is what
       timestamps speaker events, and without it every beep collapses onto the
       start of its buffer (the old point-sampled behaviour). */
    x86_io_time_ms = abi->HAL_GetTick;

    cpu_profile_init(abi);

    guest_reset();
    guest_halted = 0;
    tick_last_ms = abi->HAL_GetTick();
    tick_acc = 0;
    x86_io.spk_window_ms = tick_last_ms;
    rate_window_ms = tick_last_ms;
    rate_window_insns = guest_insns_total;
    rate_window_frames = frames_rendered;

    abi->audio_ctl(GW_AUDIO_INIT, SAMPLE_RATE);
    uint32_t audio_frames = (uint32_t)abi->audio_ctl(GW_AUDIO_GET_LENGTH, 0);
    abi->audio_ctl(GW_AUDIO_START, audio_frames);

    /*
     * =======================================================================
     * WHO PACES THE DISPLAY -- one answer, deliberately.
     * =======================================================================
     *
     * CLOCK 1 (the panel's swap signal, at the bottom of this loop) OWNS
     * display pacing. It is the hardware's own period, it is what the learned
     * panel_ms_min is measured from, and it is what sizes the unthrottled
     * slice. Nothing here replaces it.
     *
     * The firmware's common_emu_frame_loop() is a full pacer too -- it decides
     * draw/skip from its own frame_time_10us integrator. Running both would be
     * exactly the two-driver bug that wrecked the BIOS tick. So it DEFERS:
     *
     *   - we call it once per iteration for its BOOKKEEPING half, which is the
     *     only reason the overlay's fps and busy% readouts exist at all
     *     (odroid_system_tick(!draw_frame, 0, cpumon_stats.busy_ms)). Never
     *     calling it is why they read 0.0 (0.0) fps and busy 0.0%.
     *   - we IGNORE its draw_frame verdict; we always draw, and the panel wait
     *     is what regulates us.
     *   - we feed it the MEASURED display period, so its integrator agrees
     *     with the pacer that is actually running and it does not report
     *     phantom skipped frames into the fps the user reads.
     *
     * busy%: cpumon_stats.busy_ms must accumulate BETWEEN frame_loop calls
     * (frame_loop cpumon_reset()s immediately after reading it). cpumon_sleep()
     * is the idle marker -- it closes the busy interval, WFIs, and opens a
     * sleep interval -- so it goes in the panel wait, the only place this loop
     * is genuinely idle. Calling neither leaves busy_ms 0 forever.
     */
    abi->common_emu_frame_loop_reset();

    while (1) {
        /* Feed the observer the period we are ACTUALLY running at, smoothed.
           NOT panel_ms_min: that is the shortest iteration ever completed, and
           handing the firmware a period shorter than the real one makes its
           integrator saturate and mark three frames in four as skipped -- which
           we then render anyway, so the overlay's skipped-fps figure was pure
           fiction (measured: 768 "skipped" of 1032 drawn). */
        common_emu_state.frame_time_10us = (int16_t)(frame_10us_avg / 8u);
        /* Bookkeeping only -- see above. The verdict is deliberately unused. */
        (void)abi->common_emu_frame_loop();

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
            /* The benchmark is seconds of wall clock the guest did not live
               through. Re-base rather than let it read as owed ticks. */
            tick_last_ms = abi->HAL_GetTick();
            tick_acc = 0;
        }

        uint32_t frame_start_ms = abi->HAL_GetTick();

        poll_input(abi);

        /*
         * CLOCKS 2 AND 3, both off the same reading of the host millisecond
         * counter: the 18.2065 Hz PIT/BIOS tick, and the guest's instruction
         * budget. See the three-clock block near the top of this file.
         *
         * The BDA count is advanced HERE rather than inside the INT 08h
         * service, because a game that hooks INT 08h (most do) replaces our
         * stub and the count would stop dead. IRQ 0 is raised alongside it so
         * a hooked handler still runs, and the instruction boundary is pulled
         * in so it lands now instead of at the end of the slice.
         */
        uint32_t slice;
        {
            uint32_t now = abi->HAL_GetTick();
            uint32_t dt  = now - tick_last_ms;      /* wraps correctly */
            tick_last_ms = now;
            guest_uptime_ms += dt;

            uint32_t stalled_ms = 0;
            if (dt > TICK_STALL_MS) {
                stalled_ms = dt - TICK_STALL_MS;
                dt = TICK_STALL_MS;   /* keeps dt * TICK_NUM inside 32 bits */
            }

            /* --- clock 3: how many instructions this iteration may run. */
            uint32_t kips = guest_kips;
            if (kips > 100000u)
                kips = 100000u;                /* keeps dt*kips in 32 bits */

            /*
             * How many instructions FIT in one display period, regulated from
             * measurement. This runs in both modes: unthrottled uses it as the
             * slice, paced uses it as a ceiling so an impossible target
             * degrades into a deficit instead of into a slideshow.
             *
             * It regulates on the time spent EXECUTING, not on the iteration
             * period: at 60 fps with idle to spare the iteration period equals
             * the display period whatever the slice is, so a controller
             * watching that has no gradient and hunts between its rails --
             * which it did, swinging the slice 20x frame to frame.
             *
             * The target is three quarters of a display period, the remaining
             * quarter being the render, the audio refill and the input poll.
             * panel_ms_min is the shortest iteration the hardware has ever let
             * us finish, i.e. the panel's own period -- learned, so an
             * overclocked or differently-timed display needs no constant here.
             */
            {
                uint32_t panel = panel_ms_min ? panel_ms_min : 16u;
                uint32_t target4 = panel * 3u;          /* 4 * (3/4 * panel) */
                if (last_slice_halted) {
                    /* Idle is not headroom: leave max_slice alone. */
                } else if (guest_exec_ms * 4u < target4) {
                    max_slice += max_slice / 8u;
                    if (max_slice > MAX_SLICE_CEIL)
                        max_slice = MAX_SLICE_CEIL;
                } else if (guest_exec_ms * 4u > target4) {
                    max_slice -= max_slice / 8u;
                    if (max_slice < MAX_SLICE_FLOOR)
                        max_slice = MAX_SLICE_FLOOR;
                }
            }

            if (kips == 0u) {
                slice = max_slice;
                guest_insn_debt = 0;           /* no target, so nothing owed */
                /* And nothing CAN be owed, so the deficit is not merely
                   uninteresting here, it is undefined. Held at zero so the
                   readout means the same thing in both modes: in MAX the
                   number to read is guest_ips_achieved. */
                guest_insn_deficit = 0;
            } else {
                uint32_t debt = guest_insn_debt + dt * kips;
                uint32_t debt_max = kips * GUEST_DEBT_MAX_MS;
                if (debt > debt_max) {
                    /* Written off, and COUNTED. This is the emulated CPU
                       running slower than it was asked to, which is otherwise
                       completely invisible. */
                    guest_insn_deficit += debt - debt_max;
                    debt = debt_max;
                }
                slice = debt;
                uint32_t slice_max = kips * GUEST_SLICE_MAX_MS;
                if (slice > slice_max)
                    slice = slice_max;
                if (slice < kips)
                    slice = kips;              /* always make a millisecond's
                                                  worth of progress */
                guest_insn_debt = debt;
            }
            /* The ceiling is what actually FITS in a display period.
               GUEST_SLICE_MAX_MS bounds the slice in the EMULATED machine's
               milliseconds, and ask for a 100 MIPS CPU and 33 of those are
               3.3 M instructions -- a quarter of a second here, with no
               render, no input and no audio in it. */
            if (slice > max_slice)
                slice = max_slice;
            guest_slice_now = slice;
            cpu_state_ptr->pit_reload = slice;

            /* --- clock 2: the BIOS tick. */
            tick_acc += dt * TICK_NUM;
            uint32_t owed = tick_acc / TICK_DEN_MS;
            tick_acc -= owed * TICK_DEN_MS;
            /* Time lost to a stall is still time: count it as dropped rather
               than pretending the target was never away. */
            owed += (stalled_ms * TICK_NUM) / TICK_DEN_MS;

            uint32_t emit = owed > TICK_CATCHUP_MAX ? TICK_CATCHUP_MAX : owed;
            guest_ticks_dropped += owed - emit;
            guest_ticks += emit;
            for (uint32_t i = 0; i < emit; i++)
                x86_bios_tick();

            if (emit) {
                if (cpu_state_ptr->hard_int_pending) {
                    /* One-deep slot, already holding the keyboard's IRQ 1.
                       Counted, not hidden: a guest whose music runs off INT
                       08h loses a beat here and we want to see how often. */
                    guest_tick_irq_lost++;
                } else {
                    cpu_state_ptr->hard_int_pending = 0x08;
                    cpu_state_ptr->pit_counter = 1;
                }
            }
        }

        /*
         * Run guest instructions until the slice's PIT budget expires. Other
         * exits are real events: a memory fault or an unimplemented opcode
         * stops the guest rather than looping on the same instruction forever,
         * which would look exactly like a hang.
         */
        uint32_t exec_start_ms = abi->HAL_GetTick();
        while (!guest_halted) {
            /*
             * Keep the controller draining. A BUTTON produces at most two
             * codes, so one IRQ per frame was enough; an OSK keypress produces
             * up to six (Ctrl/Alt/Shift makes, the key, and their breaks) and
             * the BIOS handler takes exactly ONE code per interrupt. At one
             * IRQ per frame a shifted keystroke would take 100 ms to arrive.
             *
             * Re-raising here costs nothing: delivery happens IN-GUEST at the
             * next boundary (cpu_exec.S .handle_pit_or_irq reloads the PIT and
             * dispatches without exiting), so it does not shorten the slice. A
             * pending tick keeps priority -- the one-deep slot is the emulated
             * 8259A's whole model of one.
             */
            if (x86_kbd_pending() && !cpu_state_ptr->hard_int_pending) {
                cpu_state_ptr->hard_int_pending = 0x09;
                cpu_state_ptr->pit_counter = 1;
                input_irqs++;
            }
            x86_cpu_interp_asm();
            uint32_t reason = cpu_state_ptr->exit_reason;
            last_slice_halted = (reason == X86_EXIT_HALT);
            guest_exit_reason = reason;
            if (reason == X86_EXIT_TIMER) {
                cpu_state_ptr->exit_reason = 0;
                /* The budget was spent, so that much of the debt is paid. */
                guest_insn_debt -= (guest_insn_debt < slice)
                                 ? guest_insn_debt : slice;
                break;
            }
            if (reason == X86_EXIT_HLE_INT) {
                cpu_state_ptr->exit_reason = 0;
                x86_hle_service();
                continue;
            }
            /*
             * HLT means "idle until the next interrupt", which is how a timing
             * loop waits for the periodic tick -- not "this machine is dead".
             * Latching it stopped TOPBENCH the moment it started benchmarking.
             * Raise the tick the guest is waiting for and pull the boundary
             * check forward so it lands immediately; EIP is already past the
             * HLT, which is where the handler's IRET belongs.
             */
            if (reason == X86_EXIT_HALT) {
                cpu_state_ptr->exit_reason = 0;
                if (!(cpu_state_ptr->eflags & 0x200u))
                    break;            /* interrupts masked: nothing can wake it */
                if (cpu_state_ptr->hard_int_pending) {
                    /* Something is genuinely waiting. Pull the boundary in so
                       it lands now; EIP is already past the HLT, which is
                       where the handler's IRET belongs. */
                    cpu_state_ptr->pit_counter = 1;
                    continue;
                }
                /* Nothing is due YET. This used to fabricate an INT 08h on
                   the spot, which is the frame-rate bug in its purest form: a
                   guest that HLTs in a wait loop got a timer interrupt every
                   time it asked, so its music ran at our loop rate instead of
                   at 18.2 Hz. Go and draw a frame instead; the real-time
                   driver raises the tick when it is actually owed. */
                break;
            }
            /* Fault, HLT or unimplemented: latch it. Re-entering re-executes
               the same faulting instruction forever, spinning the frame loop
               thousands of times a second and telling us nothing new. */
            guest_halted = 1;
            break;
        }

        guest_exec_ms = abi->HAL_GetTick() - exec_start_ms;

        /* Video. The beam fraction drives the CGA status register, so a guest
           polling for vertical retrace sees it arrive about once per frame. */
        x86_video_set_beam(0xE000);
        uint8_t *fb = (uint8_t *)abi->lcd_ctl(GW_LCD_BUFFER,
                                              GW_LCD_BUF_ACTIVE, 0, 0);
        fb_addr = (uint32_t)fb;
        if (fb) {
            x86_video_render(fb, FRAME_W);
            x86_osk_draw(fb, FRAME_W);
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
            abi->lcd_ctl(GW_LCD_SET_CLUT, (uintptr_t)core_clut,
                         CORE_CLUT_ENTRIES, 0);
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
        /* CLOCK 1: the DISPLAY rate, and it is the panel's, not a constant.
           Whether we had to wait here is also the only honest measure of
           headroom, so it is what sizes the unthrottled slice. */
        abi->lcd_ctl(GW_LCD_SWAP, 0, 0, 0);
        {
            /* This wait is the loop's ONLY idle, so it is where the CPU
               monitor is told so -- cpumon_sleep() closes the busy interval
               (which is what the overlay's busy% reads) and WFIs until the
               next interrupt. A WFI can rest until the next SysTick, so the
               bound here is wall clock rather than a spin count: a swap that
               never completes must cost one frame, not 100000 sleeps. */
            uint32_t wait_start = abi->HAL_GetTick();
            while (abi->lcd_ctl(GW_LCD_IS_SWAP_PENDING, 0, 0, 0)) {
                if ((uint32_t)(abi->HAL_GetTick() - wait_start) > 100u)
                    break;   /* never trade a black screen for a hang */
                abi->cpumon_sleep();
            }
        }
        frames_rendered++;
        guest_insns_total += slice;
        abi->wdog_refresh();

        /* What this iteration really cost. */
        uint32_t frame_end_ms = abi->HAL_GetTick();
        frame_ms_last = frame_end_ms - frame_start_ms;

        /* The AVERAGE period, 8x oversampled, for the frame-loop observer. A
           mean, unlike panel_ms_min, is what an integrator comparing elapsed
           time against a period needs to sit at zero. */
        frame_10us_avg += (frame_ms_last * 100u) - (frame_10us_avg / 8u);
        if (frame_10us_avg > 8u * 30000u)
            frame_10us_avg = 8u * 30000u;   /* int16_t 10us units: 300 ms cap */

        /* The display period, learned rather than declared. It creeps upward
           so a panel that slows down (or an overclock that goes away) is
           re-learned instead of pinning us to a number that was true once. */
        if (!panel_ms_min || frame_ms_last < panel_ms_min)
            panel_ms_min = frame_ms_last;
        else if ((frames_rendered & 511u) == 0u)
            panel_ms_min++;

        /* Achieved rates, over a one-second window. In unthrottled mode this
           is THE number -- there is no target to be short of, so "what did we
           actually get" is the only report that means anything. */
        if ((uint32_t)(frame_end_ms - rate_window_ms) >= 1000u) {
            uint32_t elapsed = frame_end_ms - rate_window_ms;
            guest_ips_achieved =
                (uint32_t)(((uint64_t)(guest_insns_total - rate_window_insns)
                            * 1000u) / elapsed);
            guest_fps_achieved =
                ((frames_rendered - rate_window_frames) * 1000u) / elapsed;
            rate_window_ms = frame_end_ms;
            rate_window_insns = guest_insns_total;
            rate_window_frames = frames_rendered;
        }
    }
}
