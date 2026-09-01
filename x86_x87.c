/*
 * x86_x87.c -- the 387 escape group (D8-DF), to the depth the shipped library
 * actually demands.
 *
 * ---- Why this exists at all -----------------------------------------------
 *
 * A library-wide static scan (docs/15) found that exactly ONE title in the
 * tree computes in floating point: Dark Forces. `DF E0 9E` -- FNSTSW AX; SAHF,
 * a byte idiom that can only be x87 -- appears 32x in DF/DF.EXE and 12x in
 * DF/KEYCONFI.EXE and ZERO times anywhere else in the library. DF.EXE at
 * +0xa7608 is a genuine double-precision compare:
 *
 *     d9ee        fldz
 *     dc5d10      fcomp qword ptr [ebp+0x10]
 *     dfe0        fnstsw ax
 *     9e          sahf
 *     730a        jae ...
 *
 * Under the old `.op_esc_x87` no-coprocessor stub none of that faulted: FLDZ
 * and FCOMP did nothing, FNSTSW left AX alone, and SAHF then set the flags
 * from whatever junk was in AH. Dark Forces did not crash -- it SILENTLY TOOK
 * THE WRONG BRANCH. That is what this file replaces, and it is why a partial
 * implementation that is CORRECT is strictly better than the stub while a
 * partial implementation that is WRONG is indistinguishable from it.
 *
 * ---- THE PRECISION DECISION -- read this before filing a last-ULP bug ------
 *
 * A real x87 register is 80-bit extended (64-bit significand, 15-bit exponent).
 * THESE ARE 64-BIT DOUBLES. That is a deliberate trade and it is observable:
 *
 *   - the significand is 53 bits, not 64, so a long chain of accumulations can
 *     differ from real silicon in the last few ULPs;
 *   - the exponent range is smaller, so a value that a real 387 held as an
 *     unnormalised extended intermediate may overflow or underflow here;
 *   - FLD m80 / FSTP m80 (DB /5, DB /7) cannot round-trip a value we never
 *     held at that width, and are therefore NOT implemented (see below).
 *
 * Why take it anyway. This part is an STM32H7B0VB, whose FPv5-D16 FPU is
 * HARDWARE DOUBLE PRECISION -- the core is built `-mfpu=fpv5-d16
 * -mfloat-abi=hard` (sdk/Makefile:64-65) and SystemInit grants CP10/CP11 full
 * access, so `double` here is single VFP instructions. True 80-bit would be a
 * software significand library on the interpreter's hot path, for a title
 * whose floating point is geometry and audio mixing, not numerics. DOSBox and
 * DOSBox-X made the identical call for the identical reason, and their default
 * builds run Dark Forces. If a title ever disagrees with real hardware in the
 * last few bits, THIS PARAGRAPH IS THE ANSWER and 80-bit is the fix.
 *
 * ABI note: the interpreter's assembly holds no FP registers, so nothing has
 * to be saved across the call into here. AAPCS makes d0-d7 caller-saved (we
 * have none live) and GCC's prologue saves d8-d15 itself. Executing a VFP
 * instruction sets CONTROL.FPCA, so the exception entry that follows stacks an
 * FP frame -- with lazy stacking (the reset default) that is address space,
 * not cycles, unless the handler itself touches the FPU.
 *
 * ---- What is implemented, and what is deliberately not -------------------
 *
 * Built: the instructions Dark Forces demonstrably ISSUES, taken from a
 * census of the LE code object (`build/run/x87scan.py`'s method; see the lane
 * report). Everything with a count of 3 or more, plus the cheap neighbours
 * that sit in the same switch.
 *
 * NOT built, and each is COUNTED rather than silently ignored, in
 * `x86_x87_escapes` -- which keeps that counter's old meaning exactly ("an ESC
 * the interpreter did not execute"):
 *
 *   - transcendentals: FSIN, FCOS, FPTAN, FPATAN, F2XM1, FYL2X, FYL2XP1,
 *     FSCALE (1-2 static sites each in DF.EXE, all in phase-ambiguous
 *     regions);
 *   - FPREM / FPREM1;
 *   - BCD: FBLD, FBSTP;
 *   - FSAVE / FRSTOR / FLDENV / FNSTENV (the 94/108-byte environment images);
 *   - m80 extended load and store, for the precision reason above;
 *   - the IEEE exception machinery beyond the status-word bits FNSTSW needs:
 *     no exception masking, no #MF, no denormal/precision reporting. The
 *     invalid-operation flag IS raised for stack faults and for compares
 *     against a NaN, because the condition codes depend on it.
 *
 * If `x86_x87_escapes` is non-zero after a Dark Forces run, one of the above
 * was reached and the arithmetic downstream of it is wrong. That number is the
 * whole degradation story and it must be read, never inferred from silence.
 */

#include <stdint.h>
#include <string.h>

#include "cutils.h"
#include "x86_cpu.h"

/* ---- state ---------------------------------------------------------------
 *
 * A file-static, NOT a member of X86CPUState. The assembly never reads any of
 * it, so putting it in the DTCM context would spend the scarcest memory in the
 * build (5,856 B free) on something only C touches. It costs RAM_EMU .bss
 * instead: 8 doubles + 8 tags + two halfwords = 80 bytes.
 */
#define X87_TAG_VALID  0
#define X87_TAG_ZERO   1
#define X87_TAG_SPEC   2
#define X87_TAG_EMPTY  3

static double  x87_st[8];
static uint8_t x87_tag[8] = {
    X87_TAG_EMPTY, X87_TAG_EMPTY, X87_TAG_EMPTY, X87_TAG_EMPTY,
    X87_TAG_EMPTY, X87_TAG_EMPTY, X87_TAG_EMPTY, X87_TAG_EMPTY,
};
static uint16_t x87_sw;                 /* status word, TOP in bits 13:11 */
static uint16_t x87_cw = 0x037F;        /* control word, the FNINIT value  */
static uint8_t  x87_top;                /* kept out of sw; folded in on read */

/* Instructions EXECUTED here. The companion to x86_x87_escapes, which keeps
   its old meaning: an ESC the interpreter did NOT execute. */
volatile uint32_t x86_x87_ops = 0;

/* Status-word condition-code bits. These are the whole point of the file: the
   FNSTSW AX / SAHF idiom moves C0 -> CF, C2 -> PF and C3 -> ZF, which is how
   every x87 compare becomes a conditional branch. */
#define SW_IE   0x0001                  /* invalid operation                */
#define SW_C0   0x0100
#define SW_C1   0x0200
#define SW_C2   0x0400
#define SW_C3   0x4000
#define SW_ES   0x0080                  /* error summary                    */
#define SW_CC   (SW_C0 | SW_C1 | SW_C2 | SW_C3)

static inline double *st_ref(unsigned i)  { return &x87_st[(x87_top + i) & 7]; }
static inline uint8_t *tag_ref(unsigned i) { return &x87_tag[(x87_top + i) & 7]; }

static void x87_set_tag(unsigned i, double v)
{
    uint8_t t = X87_TAG_VALID;
    if (v == 0.0)
        t = X87_TAG_ZERO;
    else if (!(v == v) || v * 0.5 == v)  /* NaN, or an infinity */
        t = X87_TAG_SPEC;
    *tag_ref(i) = t;
}

static void x87_push(double v)
{
    x87_top = (uint8_t)((x87_top - 1) & 7);
    if (*tag_ref(0) != X87_TAG_EMPTY)
        x87_sw |= SW_IE | SW_ES;         /* stack overflow */
    *st_ref(0) = v;
    x87_set_tag(0, v);
}

static void x87_pop(void)
{
    if (*tag_ref(0) == X87_TAG_EMPTY)
        x87_sw |= SW_IE | SW_ES;         /* stack underflow */
    *tag_ref(0) = X87_TAG_EMPTY;
    x87_top = (uint8_t)((x87_top + 1) & 7);
}

/*
 * FCOM's condition codes. This is the table the whole lane exists for, so it
 * is written out rather than derived:
 *
 *   ST(0) > src        C3 C2 C0 = 0 0 0
 *   ST(0) < src        C3 C2 C0 = 0 0 1
 *   ST(0) = src        C3 C2 C0 = 1 0 0
 *   unordered (a NaN)  C3 C2 C0 = 1 1 1, and IE is raised
 *
 * After FNSTSW AX; SAHF that becomes CF=C0, ZF=C3, PF=C2 -- i.e. exactly the
 * flags a CMP would have left, which is why JB/JE/JA read correctly and why
 * the unordered case (CF=ZF=PF=1) looks like "below" to a JAE. C1 is cleared
 * on a compare that is not a stack fault.
 */
static void x87_compare(double a, double b)
{
    x87_sw &= (uint16_t)~SW_CC;
    if (!(a == a) || !(b == b)) {                  /* either operand is NaN */
        x87_sw |= SW_C0 | SW_C2 | SW_C3 | SW_IE | SW_ES;
    } else if (a > b) {
        /* all three clear */
    } else if (a < b) {
        x87_sw |= SW_C0;
    } else {
        x87_sw |= SW_C3;
    }
}

/* Unaligned by construction: the guest chooses the address. memcpy is the only
   portable spelling that GCC still turns into the right load. */
static double load_f32(const void *p)
{
    float f;
    memcpy(&f, p, 4);
    return (double)f;
}

static double load_f64(const void *p)
{
    double d;
    memcpy(&d, p, 8);
    return d;
}

static void store_f32(void *p, double v)
{
    float f = (float)v;
    memcpy(p, &f, 4);
}

static void store_f64(void *p, double v) { memcpy(p, &v, 8); }

static int32_t load_i16(const void *p)
{
    int16_t v;
    memcpy(&v, p, 2);
    return v;
}

static int32_t load_i32(const void *p)
{
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}

static int64_t load_i64(const void *p)
{
    int64_t v;
    memcpy(&v, p, 8);
    return v;
}

/* Round to integer under the control word's RC field (bits 11:10):
   0 nearest-even, 1 down, 2 up, 3 truncate toward zero. FIST/FISTP and
   FRNDINT all obey it; getting this wrong is a silent off-by-one in every
   float-to-int conversion the guest makes, which for Dark Forces is 109 static
   FISTP sites. */
static double x87_round_int(double v)
{
    /* No integer cast anywhere in here, deliberately. `(double)(int64_t)v` is
       __aeabi_d2lz plus __floatdidf out of libgcc -- several hundred bytes of
       RAM_EMU (dos_core.ld puts .text in RAM_EMU, so code IS pager frames) for
       something the FPU does in one instruction. Adding and then subtracting
       2^52 forces every bit below the units place off the end of the
       significand under the FPU's own rounding mode, which is round-to-nearest
       -EVEN -- exactly the x87 default. Values at or above 2^52 are already
       integers and are returned untouched. */
    const double BIG = 4503599627370496.0;             /* 2^52 */
    double m = v < 0.0 ? -v : v;
    double r;

    if (!(m < BIG))                                    /* also catches NaN/inf */
        return v;
    r = v < 0.0 ? (v - BIG) + BIG : (v + BIG) - BIG;   /* nearest-even         */
    switch ((x87_cw >> 10) & 3) {
    case 1: if (r > v) r -= 1.0; break;                /* toward -inf          */
    case 2: if (r < v) r += 1.0; break;                /* toward +inf          */
    case 3:                                            /* toward zero          */
        if (v < 0.0) { if (r < v) r += 1.0; }
        else         { if (r > v) r -= 1.0; }
        break;
    default: break;                                    /* nearest-even         */
    }
    return r;
}

/* The x87 "integer indefinite": what a conversion that cannot be represented
   stores, and what the guest sees instead of a wrapped value. */
static void store_int(void *p, unsigned bytes, double v)
{
    double r = x87_round_int(v);
    if (!(r == r)) {                               /* NaN */
        x87_sw |= SW_IE | SW_ES;
        r = 0.0;
        if (bytes == 2) { uint16_t x = 0x8000u;             memcpy(p, &x, 2); return; }
        if (bytes == 4) { uint32_t x = 0x80000000u;         memcpy(p, &x, 4); return; }
        { uint64_t x = 0x8000000000000000ull;               memcpy(p, &x, 8); return; }
    }
    if (bytes == 2) {
        if (r < -32768.0 || r > 32767.0) {
            uint16_t x = 0x8000u; x87_sw |= SW_IE | SW_ES; memcpy(p, &x, 2); return;
        }
        int16_t x = (int16_t)r; memcpy(p, &x, 2);
    } else if (bytes == 4) {
        if (r < -2147483648.0 || r > 2147483647.0) {
            uint32_t x = 0x80000000u; x87_sw |= SW_IE | SW_ES; memcpy(p, &x, 4); return;
        }
        int32_t x = (int32_t)r; memcpy(p, &x, 4);
    } else {
        if (r < -9223372036854775808.0 || r >= 9223372036854775808.0) {
            uint64_t x = 0x8000000000000000ull; x87_sw |= SW_IE | SW_ES;
            memcpy(p, &x, 8); return;
        }
        int64_t x = (int64_t)r; memcpy(p, &x, 8);
    }
}

/* The eight arithmetic sub-opcodes, shared by every escape that has them.
   `rev` swaps the operands, which is how the DC/DE register forms encode
   FSUB/FSUBR and FDIV/FDIVR the other way round from the D8/DC memory forms. */
static double x87_arith(unsigned sub, double a, double b)
{
    switch (sub) {
    case 0: return a + b;                          /* FADD  */
    case 1: return a * b;                          /* FMUL  */
    case 4: return a - b;                          /* FSUB  */
    case 5: return b - a;                          /* FSUBR */
    case 6: return a / b;                          /* FDIV  */
    case 7: return b / a;                          /* FDIVR */
    default: return a;                             /* 2/3 are FCOM/FCOMP */
    }
}

void x86_x87_init(void)
{
    unsigned i;
    x87_sw = 0;
    x87_cw = 0x037F;
    x87_top = 0;
    for (i = 0; i < 8; i++) {
        x87_st[i] = 0.0;
        x87_tag[i] = X87_TAG_EMPTY;
    }
}

/* The status word as the guest sees it: TOP folded into bits 13:11. */
uint16_t x86_x87_status(void)
{
    return (uint16_t)((x87_sw & ~0x3800u) | ((unsigned)x87_top << 11));
}

/* Host-readable state, for tests/test_x87.py. The harness cannot reach a
   file-static, and exposing the array wholesale would let a test read a
   register the guest never wrote. */
double   x86_x87_peek_st(unsigned i) { return *st_ref(i); }
unsigned x86_x87_peek_top(void)      { return x87_top; }
unsigned x86_x87_peek_tag(unsigned i){ return *tag_ref(i); }
uint16_t x86_x87_peek_cw(void)       { return x87_cw; }

/*
 * The single entry point from `.op_esc_x87`.
 *
 *   op    -- the escape opcode, D8-DF
 *   modrm -- the ModRM byte
 *   mem   -- host pointer to the memory operand, already folded, seam-synced
 *            and trapped by the caller; NULL when mod == 3
 *
 * Returns 0 when the instruction was executed and non-zero when it was not
 * (the caller counts that into x86_x87_escapes and moves on, which is the old
 * no-coprocessor behaviour preserved for exactly the opcodes listed at the top
 * of this file).
 */
int x86_x87_exec(X86CPUState *cpu, uint32_t op, uint32_t modrm, void *mem)
{
    unsigned sub = (modrm >> 3) & 7;
    unsigned rm  = modrm & 7;
    unsigned esc = op & 7;
    double a, b;

    x86_x87_ops++;

    if (mem) {
        /* ---- memory forms ------------------------------------------- */
        switch (esc) {
        case 0:                                    /* D8: m32 real       */
        case 4:                                    /* DC: m64 real       */
            b = (esc == 0) ? load_f32(mem) : load_f64(mem);
            if (sub == 2 || sub == 3) {            /* FCOM / FCOMP       */
                x87_compare(*st_ref(0), b);
                if (sub == 3) x87_pop();
            } else {
                *st_ref(0) = x87_arith(sub, *st_ref(0), b);
                x87_set_tag(0, *st_ref(0));
            }
            return 0;

        case 2:                                    /* DA: m32 integer    */
        case 6:                                    /* DE: m16 integer    */
            b = (esc == 2) ? (double)load_i32(mem) : (double)load_i16(mem);
            if (sub == 2 || sub == 3) {            /* FICOM / FICOMP     */
                x87_compare(*st_ref(0), b);
                if (sub == 3) x87_pop();
            } else {
                *st_ref(0) = x87_arith(sub, *st_ref(0), b);
                x87_set_tag(0, *st_ref(0));
            }
            return 0;

        case 1:                                    /* D9                 */
            switch (sub) {
            case 0: x87_push(load_f32(mem)); return 0;          /* FLD m32   */
            case 2: store_f32(mem, *st_ref(0)); return 0;       /* FST m32   */
            case 3: store_f32(mem, *st_ref(0)); x87_pop(); return 0; /* FSTP */
            case 5:                                             /* FLDCW     */
                memcpy(&x87_cw, mem, 2);
                return 0;
            case 7: {                                           /* FNSTCW    */
                uint16_t v = x87_cw;
                memcpy(mem, &v, 2);
                return 0;
            }
            default: break;                        /* /4 FLDENV, /6 FNSTENV */
            }
            break;

        case 3:                                    /* DB                 */
            switch (sub) {
            case 0: x87_push((double)load_i32(mem)); return 0;  /* FILD m32  */
            case 2: store_int(mem, 4, *st_ref(0)); return 0;    /* FIST m32  */
            case 3: store_int(mem, 4, *st_ref(0)); x87_pop(); return 0;
            default: break;                        /* /5, /7 are m80        */
            }
            break;

        case 5:                                    /* DD                 */
            switch (sub) {
            case 0: x87_push(load_f64(mem)); return 0;          /* FLD m64   */
            case 2: store_f64(mem, *st_ref(0)); return 0;       /* FST m64   */
            case 3: store_f64(mem, *st_ref(0)); x87_pop(); return 0;
            case 7: {                                           /* FNSTSW m16 */
                uint16_t v = x86_x87_status();
                memcpy(mem, &v, 2);
                return 0;
            }
            default: break;                        /* /4 FRSTOR, /6 FNSAVE  */
            }
            break;

        case 7:                                    /* DF                 */
            switch (sub) {
            case 0: x87_push((double)load_i16(mem)); return 0;  /* FILD m16  */
            case 2: store_int(mem, 2, *st_ref(0)); return 0;    /* FIST m16  */
            case 3: store_int(mem, 2, *st_ref(0)); x87_pop(); return 0;
            case 5: x87_push((double)load_i64(mem)); return 0;  /* FILD m64  */
            case 7: store_int(mem, 8, *st_ref(0)); x87_pop(); return 0;
            default: break;                        /* /4 FBLD, /6 FBSTP     */
            }
            break;
        }
        x86_x87_escapes++;
        return 1;
    }

    /* ---- register forms (mod == 3) ---------------------------------- */
    switch (esc) {
    case 0:                                        /* D8: ST(0) op ST(i) */
        if (sub == 2 || sub == 3) {
            x87_compare(*st_ref(0), *st_ref(rm));
            if (sub == 3) x87_pop();
        } else {
            *st_ref(0) = x87_arith(sub, *st_ref(0), *st_ref(rm));
            x87_set_tag(0, *st_ref(0));
        }
        return 0;

    case 4:                                        /* DC: ST(i) op ST(0) */
    case 6:                                        /* DE: same, then pop */
        if (esc == 6 && sub == 3) {                /* DE D9 = FCOMPP     */
            if (rm != 1) break;
            x87_compare(*st_ref(0), *st_ref(1));
            x87_pop();
            x87_pop();
            return 0;
        }
        if (sub == 2 || sub == 3) break;           /* no such encoding   */
        {
            /* The reversal: in these forms /4 is FSUBR and /5 is FSUB,
               /6 is FDIVR and /7 is FDIV -- the opposite of D8/DC memory.
               Feeding x87_arith the swapped sub number is the whole fix. */
            unsigned s = sub;
            if (s == 4) s = 5; else if (s == 5) s = 4;
            else if (s == 6) s = 7; else if (s == 7) s = 6;
            *st_ref(rm) = x87_arith(s, *st_ref(rm), *st_ref(0));
            x87_set_tag(rm, *st_ref(rm));
            if (esc == 6) x87_pop();
        }
        return 0;

    case 1:                                        /* D9                 */
        switch (sub) {
        case 0: x87_push(*st_ref(rm)); return 0;               /* FLD ST(i) */
        case 1:                                                /* FXCH      */
            a = *st_ref(0); b = *st_ref(rm);
            *st_ref(0) = b; *st_ref(rm) = a;
            { uint8_t t = *tag_ref(0); *tag_ref(0) = *tag_ref(rm); *tag_ref(rm) = t; }
            x87_sw &= (uint16_t)~SW_C1;
            return 0;
        case 2: if (rm == 0) return 0; break;                  /* FNOP      */
        case 4:
            switch (rm) {
            case 0: *st_ref(0) = -*st_ref(0); return 0;        /* FCHS      */
            case 1: if (*st_ref(0) < 0.0) *st_ref(0) = -*st_ref(0); return 0; /* FABS */
            case 4: x87_compare(*st_ref(0), 0.0); return 0;    /* FTST      */
            case 5:                                            /* FXAM      */
                x87_sw &= (uint16_t)~SW_CC;
                if (*st_ref(0) < 0.0) x87_sw |= SW_C1;
                if (*tag_ref(0) == X87_TAG_EMPTY)      x87_sw |= SW_C0 | SW_C3;
                else if (*tag_ref(0) == X87_TAG_ZERO)  x87_sw |= SW_C3;
                else if (!(*st_ref(0) == *st_ref(0)))  x87_sw |= SW_C0;
                else                                   x87_sw |= SW_C2;
                return 0;
            default: break;
            }
            break;
        case 5:                                                /* the constants */
            switch (rm) {
            case 0: x87_push(1.0); return 0;                   /* FLD1      */
            case 1: x87_push(3.321928094887362348); return 0;  /* FLDL2T    */
            case 2: x87_push(1.442695040888963407); return 0;  /* FLDL2E    */
            case 3: x87_push(3.141592653589793239); return 0;  /* FLDPI     */
            case 4: x87_push(0.301029995663981195); return 0;  /* FLDLG2    */
            case 5: x87_push(0.693147180559945309); return 0;  /* FLDLN2    */
            case 6: x87_push(0.0); return 0;                   /* FLDZ      */
            default: break;
            }
            break;
        case 6:
            if (rm == 6) { x87_top = (uint8_t)((x87_top - 1) & 7); return 0; } /* FDECSTP */
            if (rm == 7) { x87_top = (uint8_t)((x87_top + 1) & 7); return 0; } /* FINCSTP */
            break;
        case 7:
            if (rm == 4) {                                     /* FRNDINT   */
                *st_ref(0) = x87_round_int(*st_ref(0));
                x87_set_tag(0, *st_ref(0));
                return 0;
            }
            break;
        default: break;
        }
        break;

    case 3:                                        /* DB                 */
        if (sub == 4 && rm == 2) {                 /* FNCLEX             */
            x87_sw &= (uint16_t)~(SW_IE | SW_ES | 0x3F);
            return 0;
        }
        if (sub == 4 && rm == 3) {                 /* FNINIT             */
            x86_x87_init();
            return 0;
        }
        break;

    case 5:                                        /* DD                 */
        switch (sub) {
        case 0: *tag_ref(rm) = X87_TAG_EMPTY; return 0;        /* FFREE     */
        case 2: *st_ref(rm) = *st_ref(0);                      /* FST ST(i) */
                x87_set_tag(rm, *st_ref(rm)); return 0;
        case 3: *st_ref(rm) = *st_ref(0);                      /* FSTP ST(i)*/
                x87_set_tag(rm, *st_ref(rm)); x87_pop(); return 0;
        case 4: x87_compare(*st_ref(0), *st_ref(rm)); return 0; /* FUCOM    */
        case 5: x87_compare(*st_ref(0), *st_ref(rm));           /* FUCOMP   */
                x87_pop(); return 0;
        default: break;
        }
        break;

    case 7:                                        /* DF                 */
        if (sub == 4 && rm == 0) {                 /* FNSTSW AX          */
            cpu->regs[0] = (cpu->regs[0] & 0xFFFF0000u) | x86_x87_status();
            return 0;
        }
        break;

    default:
        break;
    }

    x86_x87_escapes++;
    return 1;
}
