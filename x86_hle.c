/*
 * HLE service dispatch.
 *
 * A guest reaches us by executing 0F 0B nn. The loader plants a three-byte
 * stub -- 0F 0B nn followed by IRET -- for each serviced vector and points the
 * IVT at it, so a program that hooks INT 21h and chains to the previous vector
 * still ends up here, and one that replaces it takes over cleanly. Nothing in
 * the guest has to cooperate.
 *
 * The split matters for protected mode later. The hypercall itself is
 * mode-independent: 0F 0B is #UD on every x86 since the 286, in both modes,
 * and it is not a vector, so no mode switch can alias it. Only the PLANTING is
 * real-mode -- protected mode would install the same stubs through the IDT --
 * so the services below never need to change.
 */
#include <stdint.h>
#include <string.h>

#include "cutils.h"
#include "x86_cpu.h"

/* Guest register accessors. The 8086 register file is 16-bit halves of the
   32-bit slots; naming them keeps the service code readable. */
#define GR(n)     (cpu_state_ptr->regs[n])
#define AX        ((uint16_t)GR(0))
#define BX        ((uint16_t)GR(3))
#define CX        ((uint16_t)GR(1))
#define DX        ((uint16_t)GR(2))
#define AH        ((uint8_t)(GR(0) >> 8))
#define AL        ((uint8_t)GR(0))

void x86_set_ax(uint16_t v) { GR(0) = (GR(0) & 0xFFFF0000u) | v; }
void x86_set_bx(uint16_t v) { GR(3) = (GR(3) & 0xFFFF0000u) | v; }
void x86_set_cx(uint16_t v) { GR(1) = (GR(1) & 0xFFFF0000u) | v; }
void x86_set_dx(uint16_t v) { GR(2) = (GR(2) & 0xFFFF0000u) | v; }
void x86_set_al(uint8_t v)  { GR(0) = (GR(0) & 0xFFFFFF00u) | v; }
void x86_set_ah(uint8_t v)  { GR(0) = (GR(0) & 0xFFFF00FFu) | ((uint32_t)v << 8); }

/*
 * The guest's FLAGS image sits on its stack, pushed by the INT that reached
 * the stub: SS:SP holds IP, CS, FLAGS. A service that reports success or
 * failure in CF has to edit THAT copy, not the live register -- the stub's
 * IRET is about to overwrite the live one with it.
 */
static uint16_t *guest_flags_on_stack(void)
{
    uint32_t sp = cpu_state_ptr->regs[4] & 0xFFFFu;
    uint32_t lin = cpu_state_ptr->segs[2].base + ((sp + 4u) & 0xFFFFu);
    return (uint16_t *)x86_guest_ptr(lin, 2);
}

/* ZF lives in the same stacked FLAGS word, for the same reason. */
void x86_hle_set_zf(int set)
{
    uint16_t *f = guest_flags_on_stack();
    if (!f)
        return;
    if (set)
        *f |= (1u << 6);
    else
        *f &= (uint16_t)~(1u << 6);
}

void x86_hle_set_cf(int set)
{
    uint16_t *f = guest_flags_on_stack();
    if (!f)
        return;
    if (set)
        *f |= 1u;
    else
        *f &= (uint16_t)~1u;
}

/* --------------------------------------------------------------------------
 * Vector stubs.
 *
 * Planted in a scratch page the guest is told about only through the IVT.
 * Three bytes of code per vector plus the IRET; laid out contiguously so one
 * page covers every service we take over.
 */
#define STUB_SEG      0x0060u        /* linear 0x600: below the 640 KB arena,
                                        above the IVT and the BIOS data area */
#define STUB_BYTES    4u

int x86_hle_install(const uint8_t *vectors, unsigned count)
{
    uint32_t base = (uint32_t)STUB_SEG << 4;
    uint8_t *stubs = (uint8_t *)x86_guest_ptr(base, count * STUB_BYTES);
    uint8_t *ivt = (uint8_t *)x86_guest_ptr(0, 1024);
    if (!stubs || !ivt)
        return -1;

    for (unsigned i = 0; i < count; i++) {
        uint8_t v = vectors[i];
        uint8_t *s = stubs + i * STUB_BYTES;
        s[0] = 0x0F;                 /* UD2 ... */
        s[1] = 0x0B;                 /* ... the hypercall */
        s[2] = v;                    /* service selector = the vector number */
        s[3] = 0xCF;                 /* IRET */

        uint16_t off = (uint16_t)(i * STUB_BYTES);
        ivt[v * 4 + 0] = (uint8_t)off;
        ivt[v * 4 + 1] = (uint8_t)(off >> 8);
        ivt[v * 4 + 2] = (uint8_t)STUB_SEG;
        ivt[v * 4 + 3] = (uint8_t)(STUB_SEG >> 8);
    }
    cpu_state_ptr->hle_enabled = 1;
    return 0;
}

/* --------------------------------------------------------------------------
 * Dispatch. Called by the frame loop when the interpreter exits with
 * X86_EXIT_HLE_INT; exit_info carries the vector number.
 */
uint32_t x86_hle_unhandled_vector;
uint32_t x86_hle_unhandled_func;

/*
 * Rewind the stacked return address by two bytes so the stub's IRET lands back
 * on the INT that reached us, re-executing it next slice. Used by a blocking
 * service with nothing to return yet -- INT 16h AH=00 with an empty buffer.
 * Spinning inside the service instead would stop the frame loop, and with it
 * the input that would have satisfied the call.
 */
static void hle_retry(void)
{
    uint32_t sp = cpu_state_ptr->regs[4] & 0xFFFFu;
    uint32_t lin = cpu_state_ptr->segs[2].base + sp;
    uint16_t *ip = (uint16_t *)x86_guest_ptr(lin, 2);
    if (ip)
        *ip = (uint16_t)(*ip - 2u);
}

int x86_hle_dispatch(uint8_t vector)
{
    switch (vector) {
    case 0x10:
        return x86_bios_video(AH);
    case 0x16:
        return x86_bios_keyboard(AH);
    case 0x1A:
        return x86_bios_time(AH);
    case 0x08:                       /* PIT tick: keep the BDA count moving */
        x86_bios_tick();
        return 1;
    case 0x21:
        return x86_dos_int21(AH);
    case 0x20:
        return x86_dos_terminate();
    default:
        /* Loud, not silent. A service we never implemented that returns as
           though it worked is indistinguishable from one that does, and the
           guest fails much later somewhere unrelated. */
        x86_hle_unhandled_vector = vector;
        x86_hle_unhandled_func = AH;
        return 0;
    }
}

uint32_t x86_hle_calls;

/* What the frame loop calls. Returns non-zero if the guest should keep
   running. */
int x86_hle_service(void)
{
    uint8_t vector = (uint8_t)cpu_state_ptr->exit_info;
    int r = x86_hle_dispatch(vector);
    x86_hle_calls++;
    if (r == 2)
        hle_retry();
    return 1;
}
