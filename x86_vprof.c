/*
 * Storage and calibration for the video-path cycle instrument. The design, the
 * nesting rule and the honest statement of its cost are all in x86_vprof.h.
 *
 * The ACCUMULATORS are defined unconditionally so that probe.py --watch
 * resolves every symbol on any build and reads a truthful zero on an
 * uninstrumented one. A host tool that cannot tell "instrument off" from
 * "symbol missing" is how a measurement gets silently skipped.
 */

#include "x86_vprof.h"

x86_vprof_acc_t x86_vprof_frame;
x86_vprof_acc_t x86_vprof_store;
x86_vprof_acc_t x86_vprof_paint;
x86_vprof_acc_t x86_vprof_render;
x86_vprof_acc_t x86_vprof_osk;
volatile uint32_t x86_vprof_paint_flip;
volatile uint32_t x86_vprof_paint_loop;
volatile uint32_t x86_vprof_ovh;
volatile uint32_t x86_vprof_reset;

#if X86_VPROF

void x86_vprof_clear(void)
{
    x86_vprof_acc_t *a[] = {&x86_vprof_frame, &x86_vprof_store,
                            &x86_vprof_paint, &x86_vprof_render,
                            &x86_vprof_osk};
    for (unsigned i = 0; i < sizeof a / sizeof a[0]; i++) {
        a[i]->lo = a[i]->hi = a[i]->n = a[i]->worst = 0;
    }
    x86_vprof_paint_flip = 0;
    x86_vprof_paint_loop = 0;
}

/*
 * An EMPTY bracket pair, in the same inline code, against a scratch
 * accumulator. This is a LOWER BOUND on the instrument's cost: the loop stays
 * in the I-cache and its accumulator stays in the D-cache, where the real
 * STORE bracket runs in code that is competing for both. Nothing on the target
 * subtracts it -- the host report prints raw and corrected side by side.
 */
void x86_vprof_calibrate(uint32_t iterations)
{
    static x86_vprof_acc_t scratch;
    uint32_t t0, i;

    if (!iterations)
        iterations = 1000u;
    t0 = X86_VPROF_CYCCNT;
    for (i = 0; i < iterations; i++) {
        X86_VPROF_BEGIN(cal);
        X86_VPROF_END(cal, scratch);
    }
    x86_vprof_ovh = (X86_VPROF_CYCCNT - t0) / iterations;
    scratch.lo = scratch.hi = scratch.n = scratch.worst = 0;
}

void x86_vprof_frame_boundary(void)
{
    if (x86_vprof_reset) {
        x86_vprof_reset = 0;
        x86_vprof_clear();
    }
}

#endif /* X86_VPROF */
