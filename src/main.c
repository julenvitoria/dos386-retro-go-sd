/*
 * TinyEMU x86 — Retro-Go SD external core.
 *
 * SPIKE SCOPE: this is not the emulator yet. It exists to prove the pieces the
 * whole architecture rests on, before any of Phase 2 is built on top:
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
#define GUEST_RAM_SIZE   (64 * 1024)
#define GUEST_PAGES      (GUEST_RAM_SIZE / 4096)

extern void x86_cpu_interp_asm(void);

/* The interpreter dereferences these; it never references the symbols
 * directly, which is what lets one assembly source serve both builds. */
extern X86CPUState *cpu_state_ptr;
extern uint32_t *guest_page_table_ptr;

static uint8_t *guest_ram;

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
    for (uint32_t i = 0; i < GUEST_PAGES; i++) {
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

    guest_ram = (uint8_t *)abi->mem_ctl(GW_MEM_OP_ALLOC, GW_MEM_RAM,
                                        1, GUEST_RAM_SIZE);

    core_ready = (core_dtcm_ok && guest_ram) ? 1 : 0;

    itcm_sum_pre_bench = itcm_sum();
    itcm_byte_120_pre = itcm_byte_120();

    /* The benchmark is NOT run automatically. It faults, the device reboots,
     * and the reboot wipes the very globals we need to read -- so polling from
     * the host races a reset. Staying alive here makes the checkpoints
     * readable; the host triggers the run deliberately via run_benchmark_flag. */

    while (1) {
        if (run_benchmark_flag && core_ready) {
            run_benchmark();
            itcm_sum_post_bench = itcm_sum();
            itcm_byte_120_post = itcm_byte_120();
            run_benchmark_flag = 0;
        }
        gw_firmware_abi()->wdog_refresh();
    }
}
