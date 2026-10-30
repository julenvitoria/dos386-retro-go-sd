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
#include "x86_probe.h"
#include "x86_xms.h"
#include "x86_ems.h"
#include "x86_mouse.h"
#include "font8x8.h"

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

uint16_t *x86_hle_flags_on_stack(void) { return guest_flags_on_stack(); }

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
/* linear 0x600: above the IVT, the BIOS data area and the AH=52h SysVars block,
   below the DOS arena. Shared through x86_cpu.h because x86_dos.c asserts the
   SysVars block ends before it -- one definition, or the two drift apart. */
#define STUB_SEG      X86_DOS_STUB_SEG
#define STUB_BYTES    4u
/* The stub block must fit between STUB_SEG and the first MCB of the DOS arena
   at segment 0x0070 -- 0x600..0x6FF, 256 bytes. It used to be sized to the
   4 KB page instead, which put the bare IRET at linear 0x900: inside the arena,
   so the first program loaded overwrote it, and the next interrupt to an
   unserved vector executed the program's data as code. */
#define STUB_LIMIT    0x0100u        /* 0x600..0x6FF; 0x700 is the arena */
#define IRET_OFF      0x00FCu        /* a bare IRET, last dword of the block */

/* A compile-time check because both sides are compile-time constants: a test
   can only catch this after someone has already shipped it. */
_Static_assert((STUB_SEG << 4) + IRET_OFF < (X86_DOS_ARENA_SEG << 4),
               "HLE stubs must sit below the DOS arena");

/*
 * The XMS control entry lives in the same block, and it is the one stub that is
 * NOT reached by an interrupt: a guest gets its address from INT 2Fh AX=4310h
 * and CALL FARs it, so it must end 0xCB (RETF) and not 0xCF (IRET). Returning
 * with IRET there would pop the caller's own stack as a FLAGS word and leave
 * SP two bytes light -- a corruption that surfaces nowhere near XMS.
 *
 * Both ends are compile-time constants, so both are asserted here rather than
 * discovered by a guest: it must sit above every vector stub and below the bare
 * IRET at IRET_OFF.
 */
_Static_assert(X86_XMS_ENTRY_SEG == STUB_SEG,
               "the XMS entry point must be in the HLE stub block");
_Static_assert(X86_XMS_ENTRY_OFF + 4u <= IRET_OFF,
               "the XMS entry stub must sit below the bare IRET");

/*
 * EMS shares the same block, and it needs TWO things in it rather than one: a
 * DOS device header whose name field a title reads at offset 0x0A of the INT
 * 67h vector's SEGMENT, and the stub the vector points at. x86_ems.h lays the
 * whole block out; what is asserted here is that neither runs into the vector
 * stubs below it or the XMS entry above it, because both ends are compile-time
 * constants and a test could only catch this after it shipped.
 *
 * x86_ems.c plants them, not x86_hle_install(), and only when EMS is real --
 * the header IS the detection, so planting it unconditionally would advertise
 * an expanded-memory manager with nothing behind it.
 */
_Static_assert(X86_EMS_HDR_LIN >= (STUB_SEG << 4) + 64u,
               "the EMS device header must sit above the vector stubs");
_Static_assert(X86_EMS_ENTRY_LIN + 4u <= (STUB_SEG << 4) + X86_XMS_ENTRY_OFF,
               "the EMS entry stub must sit below the XMS control entry");
_Static_assert(X86_EMS_HDR_LIN + 0x12u <= X86_EMS_ENTRY_LIN,
               "the EMS device header must not overlap its own stub");

/* Where a PC keeps its ROM identification. A system tool reads these directly
   rather than asking, so they have to exist as memory. */
#define BIOS_ROM_LIN  0xFF000u
#define BIOS_FONT_LIN 0xFF000u   /* F000:F000 -- the 8x8 glyphs live here */
#define BIOS_DATE_OFF 0xFF5u         /* F000:FFF5, "MM/DD/YY" */
#define BIOS_MODEL_OFF 0xFFEu        /* F000:FFFE, model byte */

/* --------------------------------------------------------------------------
 * INT 33h EVENT HANDLERS (AX=000Ch): the driver calls the program.
 *
 * Dune II never polls: it installs a handler with AX=000Ch and waits for the
 * driver to call it on every move and click. Accepting the call and never
 * calling back (what x86_mouse.c did) left its menu dead to every button.
 *
 * A real driver calls the handler from its IRQ 12 service routine, so this
 * does exactly that: on an event the frame loop raises INT 74h (IRQ 12's
 * vector), whose IVT entry points at a stub in the HLE block:
 *
 *   0x670  dd   the handler, ES:DX from AX=000Ch
 *   0x674  pusha / push ds / push es
 *          0F 0B FA     MOUSE_SEL_EV: AX = event bits, BX/CX/DX = AX=0003h
 *          call far [cs:0070h]
 *          0F 0B F9     MOUSE_SEL_DONE: the next event may be raised
 *          pop es / pop ds / popa / iret
 *
 * One event at a time (ms_busy), raised only when the interrupt slot is free;
 * an event that cannot go out this frame goes out the next. Delivery needs IF
 * set, like any interrupt. No PIC involvement: nothing to EOI.
 */
#define MOUSE_SEL_EV    0xFAu
#define MOUSE_SEL_DONE  0xF9u
#define MOUSE_VEC       0x74u
#define MOUSE_PTR_OFF   0x0070u
#define MOUSE_STUB_OFF  0x0074u
static uint16_t ms_mask, ms_pend;
X86_UC_BSS(ms_raised) volatile uint32_t x86_mouse_ev_raised;   /* INT 74h raised */
X86_UC_BSS(ms_done) volatile uint32_t x86_mouse_ev_done;       /* handler returned */
static uint8_t  ms_busy, ms_last_b;
static uint16_t ms_last_x, ms_last_y;
_Static_assert(MOUSE_STUB_OFF + 18u <= X86_XMS_ENTRY_OFF,
               "the mouse event stub must end before the XMS entry");

static void mouse_handler_call(uint16_t ax)
{
    if (ax == 0x0000u || ax == 0x0021u) {       /* a reset drops the handler */
        ms_mask = 0;
        ms_busy = 0;
        return;
    }
    if (ax != 0x000Cu)
        return;
    uint8_t *p = (uint8_t *)x86_guest_ptr(((uint32_t)STUB_SEG << 4) + MOUSE_PTR_OFF, 4);
    uint8_t *ivt = (uint8_t *)x86_guest_ptr(MOUSE_VEC * 4u, 4);
    uint16_t off = (uint16_t)cpu_state_ptr->regs[2], seg = cpu_state_ptr->segs[0].sel;
    ms_mask = (off | seg) ? (uint16_t)cpu_state_ptr->regs[1] : 0u;
    ms_busy = 0;
    if (!p || !ivt)
        return;
    p[0] = (uint8_t)off; p[1] = (uint8_t)(off >> 8);
    p[2] = (uint8_t)seg; p[3] = (uint8_t)(seg >> 8);
    ivt[0] = (uint8_t)MOUSE_STUB_OFF; ivt[1] = 0;
    ivt[2] = (uint8_t)STUB_SEG; ivt[3] = (uint8_t)(STUB_SEG >> 8);
}

/* Once per frame, after input: raise INT 74h if the pointer moved or a button
   changed and the program's mask wants it. The state is read through the
   driver's own AX=0003h, with the guest's registers put back. */
void x86_mouse_events(void)
{
    if (!ms_mask || ms_busy || !x86_mouse_enabled)
        return;
    uint32_t save[4];
    for (int i = 0; i < 4; i++) save[i] = cpu_state_ptr->regs[i];
    cpu_state_ptr->regs[0] = 3u;
    x86_mouse_int33();
    x86_mouse_calls--;                       /* ours, not the guest's */
    uint16_t b = (uint16_t)cpu_state_ptr->regs[3], x = (uint16_t)cpu_state_ptr->regs[1],
             y = (uint16_t)cpu_state_ptr->regs[2];
    for (int i = 0; i < 4; i++) cpu_state_ptr->regs[i] = save[i];
    uint16_t ev = (x != ms_last_x || y != ms_last_y) ? 1u : 0u;
    uint8_t ch = (uint8_t)(b ^ ms_last_b);
    if (ch & 1u) ev |= (b & 1u) ? 0x02u : 0x04u;
    if (ch & 2u) ev |= (b & 2u) ? 0x08u : 0x10u;
    ev &= ms_mask;
    if (!ev || cpu_state_ptr->hard_int_pending)
        return;              /* nothing wanted yet, or the slot is taken */
    ms_last_x = x; ms_last_y = y; ms_last_b = (uint8_t)b;
    ms_pend = ev;
    ms_busy = 1;
    x86_mouse_ev_raised++;
    cpu_state_ptr->hard_int_pending = MOUSE_VEC;
    x86_pit_counter_forced(1);
}

int x86_hle_install(const uint8_t *vectors, unsigned count)
{
    uint32_t base = (uint32_t)STUB_SEG << 4;
    uint8_t *stubs;
    if (count * STUB_BYTES > X86_XMS_ENTRY_OFF)
        return -1;                   /* would run into the XMS entry stub */
    stubs = (uint8_t *)x86_guest_ptr(base, STUB_LIMIT);
    uint8_t *ivt = (uint8_t *)x86_guest_ptr(0, 1024);
    if (!stubs || !ivt)
        return -1;

    /* Every vector gets a bare IRET first. An unserved interrupt otherwise
       lands on whatever the IVT happens to hold -- zero, i.e. 0000:0000, where
       the guest executes its own zeroed memory. A stray INT should be a no-op,
       not a wander. */
    stubs[IRET_OFF] = 0xCF;
    for (unsigned v = 0; v < 256; v++) {
        ivt[v * 4 + 0] = (uint8_t)IRET_OFF;
        ivt[v * 4 + 1] = (uint8_t)(IRET_OFF >> 8);
        ivt[v * 4 + 2] = (uint8_t)STUB_SEG;
        ivt[v * 4 + 3] = (uint8_t)(STUB_SEG >> 8);
    }

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

    /* The XMS control entry. Planted unconditionally: INT 2Fh AX=4310h hands
       out this address only when a backing store exists, but the four bytes
       cost nothing and a guest that cached the address across a reset then
       lands on a stub rather than on whatever is at 0060:00C0. */
    {
        uint8_t *x = stubs + X86_XMS_ENTRY_OFF;
        x[0] = 0x0F;
        x[1] = 0x0B;
        x[2] = X86_HLE_SEL_XMS;
        x[3] = 0xCB;                 /* RETF -- entered by CALL FAR */
    }
    {
        static const uint8_t ms[18] = {
            0x60, 0x1E, 0x06,                       /* pusha, push ds, es */
            0x0F, 0x0B, MOUSE_SEL_EV,
            0x2E, 0xFF, 0x1E, (uint8_t)MOUSE_PTR_OFF, 0x00, /* call far [cs:70] */
            0x0F, 0x0B, MOUSE_SEL_DONE,
            0x07, 0x1F, 0x61, 0xCF                  /* pop es, ds, popa, iret */
        };
        memcpy(stubs + MOUSE_STUB_OFF, ms, sizeof(ms));
        ms_mask = 0;
        ms_busy = 0;
    }
    cpu_state_ptr->hle_enabled = 1;
    return 0;
}

/*
 * A minimal BIOS ROM image. One page is enough: the fields a program actually
 * reads all sit in the last 16 bytes of the segment, and mapping the whole
 * 64 KB would cost pool pages for memory that is never touched.
 */
int x86_bios_rom_install(void)
{
    uint8_t *rom = (uint8_t *)x86_guest_ptr(BIOS_ROM_LIN, 4096);
    if (!rom)
        return -1;
    memset(rom, 0xFF, 4096);

    /* F000:FFF0 is the power-on entry: far jump to the same place, so a guest
       that reaches it stops there instead of running off into 0xFF filler. */
    rom[0xFF0] = 0xEA;
    rom[0xFF1] = 0xF0; rom[0xFF2] = 0xFF;
    rom[0xFF3] = 0x00; rom[0xFF4] = 0xF0;

    /* The 8x8 character generator. This page is F000:F000, so that is where
       the glyphs are. INT 10h AH=11h AL=30h hands
       a program a pointer to it, and a program that asks then READS through
       that pointer -- so it has to be real memory with the real glyphs, not a
       plausible-looking address. 2 KB of the page we already map. */
    memcpy(rom, font8x8_basic, 128 * 8);
    memset(rom + 128 * 8, 0, 128 * 8);

    memcpy(rom + BIOS_DATE_OFF, "01/01/90", 8);
    rom[BIOS_MODEL_OFF] = 0xFC;      /* AT-class machine */
    rom[0xFFF] = 0x00;
    return 0;
}

/* --------------------------------------------------------------------------
 * Dispatch. Called by the frame loop when the interpreter exits with
 * X86_EXIT_HLE_INT; exit_info carries the vector number.
 */
uint32_t x86_hle_unhandled_vector;
X86_UC_BSS(x86_hle_log) uint32_t x86_hle_log[16];
uint32_t x86_hle_log_idx;
uint32_t x86_hle_unhandled_func;

/*
 * Make a blocking service with nothing to return yet -- INT 16h AH=00 with an
 * empty buffer -- run again on the next re-entry. Spinning inside the service
 * instead would stop the frame loop, and with it the input that would have
 * satisfied the call.
 *
 * NOT BY REWINDING THE CALLER. This used to subtract 2 from the stacked return
 * IP, which is right only when the stub was reached by a two-byte `INT nn`. A
 * guest that hooks INT 16h and chains to us with `pushf; call far [old]` --
 * Prince of Persia's INT 16h hook does exactly that -- has a FOUR-byte call in
 * front of that IP, so the rewind landed in the middle of it and the
 * guest executed the far pointer as code.
 *
 * Instead the guest stays IN THE STUB: EIP goes back over the 0F 0B nn it just
 * executed, so the same hypercall runs again on re-entry whatever frame
 * brought it here, and IF is set the way the real BIOS's wait loop sets it
 * (it STIs and spins) -- without that the keyboard IRQ that would fill the
 * buffer could never be taken inside the stub, since the INT that entered it
 * cleared IF.
 */
static void hle_retry(void)
{
    cpu_state_ptr->eip = (cpu_state_ptr->eip - 3u) & 0xFFFFu;
    cpu_state_ptr->eflags |= 0x200u;
}

int x86_hle_dispatch(uint8_t vector)
{
    /* Diagnostic: the last 32 services requested, as (vector << 8) | AH, with
       bit 16 set once the handler reports it did not know the function. One
       counter per vector was not enough -- what a program does when a probe
       goes unanswered only makes sense against the whole conversation. */
    x86_hle_log[x86_hle_log_idx & 15] = ((uint32_t)vector << 8) | AH;
    x86_hle_log_idx++;

    switch (vector) {
    case 0x10:
        return x86_bios_video(AH);
    case 0x11:
        return x86_bios_equipment();
    case 0x12:
        return x86_bios_memsize();
    case 0x15:
        return x86_bios_system(AH);
    case 0x16:
        return x86_bios_keyboard(AH);
    case 0x1A:
        return x86_bios_time(AH);
    case 0x08:
        x86_pic_eoi_level(0);            /* the BIOS handler's EOI */
        /* Nothing to do. The BDA tick count is advanced by the frame loop,
           not here: a guest that hooks INT 08h never reaches this stub, and
           the count has to keep moving anyway. */
        return 1;
    case 0x09:
        return x86_bios_kbd_irq();
    case 0x21:
        return x86_dos_int21(AH);
    case 0x2F:
        /* The multiplex interrupt. Only AX=43xx is answered; every other
           subfunction comes back with the guest's registers untouched, which is
           exactly what the bare IRET this replaces did. Do not widen this arm
           without deciding what "absent" means for the subfunction you add. */
        return x86_xms_int2f();
    case 0x33:
        /* The mouse driver (x86_mouse.h). Opt-in per title: with the .cfg not
           asking, it returns without touching a register -- the bare IRET this
           stub replaced, so "no driver" reads exactly as it always did. */
        if (x86_mouse_enabled)
            mouse_handler_call((uint16_t)cpu_state_ptr->regs[0]);
        return x86_mouse_int33();
    case MOUSE_SEL_EV:
        /* Inside the event stub: the handler's registers. AX=0003h through
           the driver itself fills BX buttons, CX x, DX y. */
        cpu_state_ptr->regs[0] = (cpu_state_ptr->regs[0] & 0xFFFF0000u) | 3u;
        x86_mouse_int33();
        x86_set_ax(ms_pend);
        cpu_state_ptr->regs[6] &= 0xFFFF0000u;          /* SI, DI: mickeys */
        cpu_state_ptr->regs[7] &= 0xFFFF0000u;
        return 1;
    case MOUSE_SEL_DONE:
        ms_busy = 0;
        x86_mouse_ev_done++;
        return 1;
    case 0x67:
        /* Expanded memory. Reached through the stub at 0064:0020, which the
           IVT points at only while EMS has backing -- so this arm is not
           reachable at all on a machine with no pagefile. */
        return x86_ems_int67();
    case X86_HLE_SEL_XMS:
        /* Not a vector: the XMS control function, entered by CALL FAR through
           the stub at 0060:00C0. Its stub ends RETF, so nothing here may touch
           the stacked FLAGS -- there is none. */
        return x86_xms_control();
    case 0x20:
        return x86_dos_terminate();
    case 0x27:
        return x86_dos_int27();
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
    unsigned slot = x86_hle_log_idx & 15;
#if X86_PROBE
    /* Captured BEFORE dispatch: a handler is free to overwrite AX, and AH is
       the key. The unhandled-marker snapshot is what lets the probe report
       distinguish "a subfunction fell through" from "the whole service is
       missing" without an annotation macro in x86_bios.c / x86_dos.c -- those
       six default arms already set these two words, and comparing them across
       the call is free. */
    uint8_t p_ah = AH;
    uint8_t p_sub = x86_probe_sub(vector, AH, AL,
                                  (uint8_t)(GR(3) & 0xFFu));
    uint32_t p_mark_v = x86_hle_unhandled_vector;
    uint32_t p_mark_f = x86_hle_unhandled_func;
#endif
    int r = x86_hle_dispatch(vector);
    if (r == 0)
        x86_hle_log[slot] |= 0x10000u;    /* nobody answered this one */
#if X86_PROBE
    {
        uint16_t *fl = x86_hle_flags_on_stack();
        X86_PROBE_INT(vector, p_ah, p_sub, r,
                      (x86_hle_unhandled_vector != p_mark_v
                       || x86_hle_unhandled_func != p_mark_f),
                      AX, BX, ((uint32_t)CX << 16) | DX,
                      fl ? (*fl & 1u) : 0);
    }
#endif
    x86_hle_calls++;
    if (r == 2)
        hle_retry();
    return 1;
}

/*
 * The 8x8 CP437 font, borrowed rather than duplicated.
 *
 * The on-screen keyboard draws with the same glyphs the guest is handed at
 * F000:F000 (x86_bios_rom_install, above). It lives in another translation
 * unit, and #including font8x8.h there would link a SECOND 2 KB copy -- against
 * the ~4 KB of RAM_EMU headroom the whole core has left (TOOLING.md). One
 * accessor costs nothing and cannot drift from what the guest sees.
 */
const unsigned char (*x86_font8x8(void))[8]
{
    return font8x8_basic;
}
