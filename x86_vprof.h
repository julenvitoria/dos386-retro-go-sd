/*
 * x86_vprof.h -- VIDEO-PATH cycle attribution. OFF unless X86_VPROF=1.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS EXISTS, WHEN x86_zones.h AND x86_profile.h ALREADY DO
 * ---------------------------------------------------------------------------
 * Neither of them can answer "where do the cycles go in Mode Y".
 *
 *   x86_zones.h is per-FRAME and per-SERVICE-CALL. Its VIDEO zone brackets the
 *   frame loop's render -- but a Mode Y title paints itself from
 *   x86_video_page_flip(), which is called from INSIDE the interpreter, so two
 *   full 64,000-byte paints per present are charged to the GUEST zone and are
 *   invisible in the breakdown. That is precisely the cost under suspicion.
 *
 *   x86_profile.h charges cycles to the x86 OPCODE. A guest `stosw` into the
 *   Mode Y aperture and a `stosw` into ordinary RAM are the same opcode; the
 *   first costs the planar hook and the second does not, and the per-opcode
 *   table averages them into one meaningless mean.
 *
 * So this instrument brackets the FOUR video phases by name, with the DWT
 * cycle counter, and adds a whole-frame accumulator so every share has a
 * MEASURED denominator rather than an assumed core clock:
 *
 *   FRAME   one iteration of the emulator frame loop           (denominator)
 *   STORE   x86_vga_planar_store(), the planar write hook      (inside GUEST)
 *   PAINT   modey_paint_both() / one non-Mode-Y render+OSK     (both callers)
 *   RENDER  x86_video_render() alone -- the de-interleave blit (inside PAINT)
 *   OSK     x86_osk_draw() alone -- the overlay pass           (inside PAINT)
 *
 * PAINT is a PARENT of RENDER and OSK; the three are deliberately NOT
 * exclusive, because `PAINT - (RENDER + OSK)` is itself a wanted number (the
 * buffer-address and mode-check work around the two paints). Everything else
 * is exclusive. STORE is disjoint from PAINT.
 *
 * ---------------------------------------------------------------------------
 * COST, AND WHY IT IS STATED RATHER THAN SUBTRACTED
 * ---------------------------------------------------------------------------
 * A bracket is two reads of 0xE0001004 (the private peripheral bus: not
 * cached, not free), one subtract and one 64-bit accumulate. PAINT/RENDER/OSK/
 * FRAME fire tens of times a second and are noise. STORE fires ~180,000 times
 * a second in Mode Y gameplay and is NOT noise -- it is the one bracket that
 * perturbs its own measurement.
 *
 * x86_vprof_calibrate() measures an empty bracket pair on the target, in the
 * same inline code, and stores it in x86_vprof_ovh. NOTHING here subtracts it.
 * The host report prints the raw total, the corrected total
 * (total - n * ovh) and the calibration itself, and the corrected figure is a
 * LOWER BOUND: the calibration loop stays in cache and predicts perfectly.
 *
 * Grade the perturbation, do not assume it: guest_ips_achieved on this build
 * against the same workload on a clean build is the honest check, and the
 * report is required to quote both.
 *
 * ---------------------------------------------------------------------------
 * WRAPPING
 * ---------------------------------------------------------------------------
 * CYCCNT is 32-bit and wraps every ~15 s at 280 MHz, so a DELTA is taken (and
 * is exact across a wrap by modular arithmetic) and accumulated 64-bit. A
 * 32-bit accumulator would wrap inside one measurement window.
 *
 * Storage is plain BSS, not DTCM: these are read by the host with
 * probe.py --watch and the DTCM pool is the scarcest thing in the project
 * (STATUS.md). The accumulators are hot in the D-cache and the bracket's cost
 * is dominated by the PPB reads either way.
 */
#ifndef X86_VPROF_H
#define X86_VPROF_H

#include <stdint.h>

#ifndef X86_VPROF
#define X86_VPROF 0
#endif

/* Host-visible even when the instrument is compiled out, so probe.py --watch
   resolves the symbols and reads honest zeroes rather than failing. */
typedef struct {
    volatile uint32_t lo;
    volatile uint32_t hi;
    volatile uint32_t n;
    volatile uint32_t worst;
} x86_vprof_acc_t;

extern x86_vprof_acc_t x86_vprof_frame;
extern x86_vprof_acc_t x86_vprof_store;
extern x86_vprof_acc_t x86_vprof_paint;
extern x86_vprof_acc_t x86_vprof_render;
extern x86_vprof_acc_t x86_vprof_osk;
/* Paints reached from the guest's own page flip vs from the frame loop's
   fallback. x86_modey_presents counts only the first, which is why "10 frames
   per second" has never been the whole paint count. */
extern volatile uint32_t x86_vprof_paint_flip;
extern volatile uint32_t x86_vprof_paint_loop;
extern volatile uint32_t x86_vprof_ovh;    /* calibrated cycles per bracket */
extern volatile uint32_t x86_vprof_reset;  /* host writes 1; cleared at frame */

#if X86_VPROF

#define X86_VPROF_CYCCNT (*(volatile uint32_t *)0xE0001004u)

static inline void x86_vprof_add(x86_vprof_acc_t *a, uint32_t d)
{
    uint32_t lo = a->lo + d;
    if (lo < a->lo)
        a->hi++;
    a->lo = lo;
    a->n++;
    if (d > a->worst)
        a->worst = d;
}

#define X86_VPROF_BEGIN(name)  uint32_t vprof_t_##name = X86_VPROF_CYCCNT
#define X86_VPROF_END(name, acc) \
    x86_vprof_add(&(acc), X86_VPROF_CYCCNT - vprof_t_##name)
#define X86_VPROF_COUNT(c)     ((c)++)

void x86_vprof_calibrate(uint32_t iterations);
void x86_vprof_clear(void);
/* Call once per frame, at the frame boundary. Honours x86_vprof_reset so a
   host can open a measurement window without racing a half-open bracket. */
void x86_vprof_frame_boundary(void);

#else

#define X86_VPROF_BEGIN(name)      ((void)0)
#define X86_VPROF_END(name, acc)   ((void)0)
#define X86_VPROF_COUNT(c)         ((void)0)
#define x86_vprof_calibrate(n)     ((void)0)
#define x86_vprof_clear()          ((void)0)
#define x86_vprof_frame_boundary() ((void)0)

#endif /* X86_VPROF */

#endif /* X86_VPROF_H */
