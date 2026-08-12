/*
 * DOS layer: memory arena, PSP, MZ loader and INT 21h.
 *
 * There is no DOS kernel in the guest. These services ARE DOS, so the things a
 * program can inspect have to be real rather than implied: the memory control
 * block chain is a genuine chain in guest memory, because programs walk it,
 * and the PSP is a real PSP because programs read their command tail and their
 * environment segment straight out of it.
 *
 * Anything not implemented records its function number and fails. A DOS call
 * that quietly returns success is indistinguishable from one that works until
 * the program acts on the result, and then the failure surfaces somewhere
 * unrelated -- the most expensive shape of bug this project has hit, three
 * times now.
 */
#include <stdint.h>
#include <string.h>

#include "cutils.h"
#include "x86_cpu.h"
#include "lib/FatFs/ff.h"

#define SEG(x)          ((uint32_t)(x) << 4)
#define PARA(bytes)     (((bytes) + 15u) >> 4)

/* Guest low memory:
 *   0000  interrupt vector table
 *   0400  BIOS data area
 *   0500  DOS scratch
 *   0600  our 0F 0B stubs (x86_hle.c)
 *   0700  first memory control block -- the arena starts here
 *   9FFF  end of conventional memory
 */
#define ARENA_FIRST_MCB 0x0070u
#define ARENA_END_SEG   0x9FFFu

#define MCB_MEMBER      0x4Du       /* 'M': another block follows */
#define MCB_LAST        0x5Au       /* 'Z': end of the chain      */

uint32_t x86_dos_last_error;        /* host-visible: last failing function */

/* --------------------------------------------------------------------------
 * Memory control blocks.
 *
 * Each block is a 16-byte header in guest memory followed by the memory it
 * describes; the data starts one paragraph after the header. Walking is
 * therefore: next = this + 1 + size.
 */
typedef struct {
    uint8_t  type;
    uint16_t owner;
    uint16_t size;                  /* paragraphs, excluding the header */
} mcb_t;

static int mcb_read(uint16_t seg, mcb_t *m)
{
    uint8_t *p = x86_guest_ptr(SEG(seg), 16);
    if (!p)
        return 0;
    m->type = p[0];
    m->owner = (uint16_t)(p[1] | (p[2] << 8));
    m->size = (uint16_t)(p[3] | (p[4] << 8));
    return m->type == MCB_MEMBER || m->type == MCB_LAST;
}

static void mcb_write(uint16_t seg, const mcb_t *m)
{
    uint8_t *p = x86_guest_ptr(SEG(seg), 16);
    if (!p)
        return;
    p[0] = m->type;
    p[1] = (uint8_t)m->owner;
    p[2] = (uint8_t)(m->owner >> 8);
    p[3] = (uint8_t)m->size;
    p[4] = (uint8_t)(m->size >> 8);
    memset(p + 5, 0, 11);
}

void x86_dos_arena_init(void)
{
    mcb_t m;
    m.type = MCB_LAST;
    m.owner = 0;                    /* free */
    m.size = (uint16_t)(ARENA_END_SEG - ARENA_FIRST_MCB - 1u);
    mcb_write(ARENA_FIRST_MCB, &m);
}

/* Merge each run of adjacent free blocks into one. Done before every
   allocation so a program that frees in pieces can still allocate the whole
   again -- without it the arena fragments permanently. */
static void mcb_coalesce(void)
{
    uint16_t seg = ARENA_FIRST_MCB;
    mcb_t m, n;
    while (mcb_read(seg, &m)) {
        if (m.type == MCB_LAST)
            break;
        uint16_t next = (uint16_t)(seg + m.size + 1u);
        if (m.owner == 0 && mcb_read(next, &n) && n.owner == 0) {
            m.size = (uint16_t)(m.size + n.size + 1u);
            m.type = n.type;
            mcb_write(seg, &m);
            continue;               /* try to absorb the one after that too */
        }
        seg = next;
    }
}

static uint16_t mcb_largest_free(void)
{
    uint16_t seg = ARENA_FIRST_MCB, best = 0;
    mcb_t m;
    while (mcb_read(seg, &m)) {
        if (m.owner == 0 && m.size > best)
            best = m.size;
        if (m.type == MCB_LAST)
            break;
        seg = (uint16_t)(seg + m.size + 1u);
    }
    return best;
}

/* Allocate `paras` paragraphs. Returns the DATA segment, or 0 with *largest
   set to the biggest block available -- which is what INT 21h 48h must report
   so a program can retry with a size that fits. */
static uint16_t mcb_alloc(uint16_t paras, uint16_t owner, uint16_t *largest)
{
    mcb_coalesce();

    uint16_t seg = ARENA_FIRST_MCB;
    mcb_t m;
    while (mcb_read(seg, &m)) {
        if (m.owner == 0 && m.size >= paras) {
            if (m.size > paras + 1u) {
                /* Split: the tail becomes a new free block. */
                uint16_t tail = (uint16_t)(seg + paras + 1u);
                mcb_t t;
                t.type = m.type;
                t.owner = 0;
                t.size = (uint16_t)(m.size - paras - 1u);
                mcb_write(tail, &t);
                m.type = MCB_MEMBER;
                m.size = paras;
            }
            m.owner = owner;
            mcb_write(seg, &m);
            return (uint16_t)(seg + 1u);
        }
        if (m.type == MCB_LAST)
            break;
        seg = (uint16_t)(seg + m.size + 1u);
    }
    if (largest)
        *largest = mcb_largest_free();
    return 0;
}

static int mcb_free(uint16_t data_seg)
{
    uint16_t seg = (uint16_t)(data_seg - 1u);
    mcb_t m;
    if (!mcb_read(seg, &m))
        return 0;
    m.owner = 0;
    mcb_write(seg, &m);
    mcb_coalesce();
    return 1;
}

/* Grow or shrink in place. Growing only succeeds if the next block is free and
   big enough -- DOS does not move memory, and a program that assumed it did
   would already be broken on real hardware. */
static int mcb_resize(uint16_t data_seg, uint16_t paras, uint16_t *largest)
{
    uint16_t seg = (uint16_t)(data_seg - 1u);
    mcb_t m;
    if (!mcb_read(seg, &m))
        return 0;

    if (paras <= m.size) {
        if (m.size > paras + 1u) {
            uint16_t tail = (uint16_t)(seg + paras + 1u);
            mcb_t t;
            t.type = m.type;
            t.owner = 0;
            t.size = (uint16_t)(m.size - paras - 1u);
            mcb_write(tail, &t);
            m.type = MCB_MEMBER;
            m.size = paras;
            mcb_write(seg, &m);
            mcb_coalesce();
        }
        return 1;
    }

    if (m.type != MCB_LAST) {
        uint16_t next = (uint16_t)(seg + m.size + 1u);
        mcb_t n;
        if (mcb_read(next, &n) && n.owner == 0 &&
            (uint32_t)m.size + 1u + n.size >= paras) {
            m.size = (uint16_t)(m.size + n.size + 1u);
            m.type = n.type;
            mcb_write(seg, &m);
            return mcb_resize(data_seg, paras, largest);
        }
    }
    if (largest)
        *largest = m.size;
    return 0;
}

/* Test shims. The arena functions are static on purpose -- nothing outside
   INT 21h should allocate -- but the tier-1 harness has to drive them. */
uint16_t dos_test_alloc(uint16_t paras, uint16_t *largest)
{ return mcb_alloc(paras, 0x1234, largest); }
uint16_t dos_test_free(uint16_t seg)   { return (uint16_t)mcb_free(seg); }
uint16_t dos_test_resize(uint16_t seg, uint16_t paras, uint16_t *largest)
{ return (uint16_t)mcb_resize(seg, paras, largest); }

/* Walk the whole chain and report how many blocks it has, or 0 if it is
   malformed -- a chain that does not terminate is the failure mode that
   silently eats memory. */
uint16_t dos_test_walk(void)
{
    uint16_t seg = ARENA_FIRST_MCB, n = 0;
    mcb_t m;
    while (mcb_read(seg, &m)) {
        if (++n > 64)
            return 0;
        if (m.type == MCB_LAST)
            return n;
        seg = (uint16_t)(seg + m.size + 1u);
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Program Segment Prefix.
 */
static uint16_t psp_seg;
static uint32_t dta_lin;            /* disk transfer area, default PSP:0080 */

static void psp_build(uint16_t seg, uint16_t mem_top, const char *tail)
{
    uint8_t *p = x86_guest_ptr(SEG(seg), 256);
    if (!p)
        return;
    memset(p, 0, 256);

    p[0] = 0xCD; p[1] = 0x20;                   /* INT 20h */
    p[2] = (uint8_t)mem_top;                    /* first segment past us */
    p[3] = (uint8_t)(mem_top >> 8);
    p[5] = 0x9A;                                /* far call to the dispatcher */
    p[0x0A] = 0x00; p[0x0B] = 0x00;             /* INT 22h terminate vector */
    p[0x2C] = 0x00; p[0x2D] = 0x00;             /* no environment block */
    p[0x50] = 0xCD; p[0x51] = 0x21; p[0x52] = 0xCB;  /* INT 21h / RETF */

    unsigned n = tail ? (unsigned)strlen(tail) : 0;
    if (n > 126) n = 126;
    p[0x80] = (uint8_t)n;
    if (n) memcpy(p + 0x81, tail, n);
    p[0x81 + n] = 0x0D;

    psp_seg = seg;
    dta_lin = SEG(seg) + 0x80u;
}

uint16_t x86_dos_psp(void) { return psp_seg; }

/* --------------------------------------------------------------------------
 * File handles. DOS handles 0-4 are the standard devices; real files start at
 * 5 and index the table below.
 */
#define DOS_MAX_FILES 16
static FIL dos_files[DOS_MAX_FILES];
static uint8_t dos_file_used[DOS_MAX_FILES];

static int handle_alloc(void)
{
    for (int i = 0; i < DOS_MAX_FILES; i++)
        if (!dos_file_used[i]) { dos_file_used[i] = 1; return i; }
    return -1;
}

static FIL *handle_get(uint16_t h)
{
    if (h < 5 || h - 5 >= DOS_MAX_FILES)
        return NULL;
    return dos_file_used[h - 5] ? &dos_files[h - 5] : NULL;
}

/* Pull a NUL-terminated guest string out, one page at a time. */
static void guest_str(uint32_t lin, char *out, unsigned max)
{
    unsigned i = 0;
    while (i < max - 1) {
        uint8_t *p = x86_guest_ptr(lin + i, 1);
        if (!p || *p == 0)
            break;
        out[i++] = (char)*p;
    }
    out[i] = 0;
}

/* --------------------------------------------------------------------------
 * MZ loader.
 */
typedef struct {
    uint16_t sig, last_page, pages, relocs, hdr_paras, minalloc, maxalloc;
    uint16_t ss, sp, checksum, ip, cs, reloc_off, overlay;
} mz_header_t;

int x86_dos_exec(const char *path, const char *cmdline)
{
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        x86_dos_last_error = 0x02;          /* file not found */
        return -1;
    }

    mz_header_t h;
    UINT got = 0;
    if (f_read(&f, &h, sizeof(h), &got) != FR_OK || got != sizeof(h)) {
        f_close(&f);
        x86_dos_last_error = 0x1E;
        return -1;
    }
    if (h.sig != 0x5A4D && h.sig != 0x4D5A) {
        f_close(&f);
        x86_dos_last_error = 0x0B;          /* invalid format */
        return -1;
    }

    uint32_t hdr_bytes = (uint32_t)h.hdr_paras * 16u;
    uint32_t image = (uint32_t)h.pages * 512u;
    if (h.last_page)
        image = image - 512u + h.last_page;
    if (image < hdr_bytes) {
        f_close(&f);
        x86_dos_last_error = 0x0B;
        return -1;
    }
    image -= hdr_bytes;

    /* One block for the PSP, the image and whatever the header asks to keep.
       minalloc is a minimum, not a suggestion: a program that gets less will
       overwrite whatever follows it. */
    uint16_t need = (uint16_t)(PARA(256) + PARA(image) + h.minalloc);
    uint16_t largest = 0;
    uint16_t block = mcb_alloc(need, 0, &largest);
    if (!block) {
        f_close(&f);
        x86_dos_last_error = 0x08;          /* insufficient memory */
        return -1;
    }

    uint16_t psp = block;
    uint16_t load_seg = (uint16_t)(psp + PARA(256));

    /* Copy the image in, a page at a time so it can cross granules. */
    f_lseek(&f, hdr_bytes);
    uint32_t done = 0;
    while (done < image) {
        uint8_t buf[512];
        UINT n = 0;
        UINT want = (image - done > sizeof(buf)) ? sizeof(buf)
                                                 : (UINT)(image - done);
        if (f_read(&f, buf, want, &n) != FR_OK || n == 0)
            break;
        if (x86_guest_copy_in(SEG(load_seg) + done, buf, n) != n)
            break;
        done += n;
    }

    /* Relocations: each entry names a word holding a segment, which has to be
       biased by where we actually loaded. */
    if (h.relocs) {
        f_lseek(&f, h.reloc_off);
        for (unsigned i = 0; i < h.relocs; i++) {
            uint16_t ro[2];
            UINT n = 0;
            if (f_read(&f, ro, 4, &n) != FR_OK || n != 4)
                break;
            uint32_t lin = SEG(load_seg) + SEG(ro[1]) + ro[0];
            uint16_t *w = (uint16_t *)x86_guest_ptr(lin, 2);
            if (w)
                *w = (uint16_t)(*w + load_seg);
        }
    }
    f_close(&f);

    psp_build(psp, (uint16_t)(psp + need), cmdline);

    /* Entry state, exactly as the header specifies. */
    cpu_state_ptr->segs[1].sel  = (uint16_t)(load_seg + h.cs);
    cpu_state_ptr->segs[1].base = SEG(load_seg + h.cs);
    cpu_state_ptr->segs[1].limit = 0xFFFF;
    cpu_state_ptr->eip = h.ip;

    cpu_state_ptr->segs[2].sel  = (uint16_t)(load_seg + h.ss);
    cpu_state_ptr->segs[2].base = SEG(load_seg + h.ss);
    cpu_state_ptr->segs[2].limit = 0xFFFF;
    cpu_state_ptr->regs[4] = h.sp;

    /* DS and ES point at the PSP on entry, which is where a program looks for
       its command tail. */
    for (int i = 0; i < 6; i += 3) {            /* ES (0) and DS (3) */
        cpu_state_ptr->segs[i].sel  = psp;
        cpu_state_ptr->segs[i].base = SEG(psp);
        cpu_state_ptr->segs[i].limit = 0xFFFF;
    }
    cpu_state_ptr->regs[0] = 0;
    return 0;
}

/* --------------------------------------------------------------------------
 * INT 21h
 */
int x86_dos_terminate(void)
{
    cpu_state_ptr->exit_reason = X86_EXIT_TERMINATE;
    return 1;
}

static void ok_clear_cf(void)   { x86_hle_set_cf(0); }
static void fail(uint16_t code) { x86_set_ax(code); x86_hle_set_cf(1); }

int x86_dos_int21(uint8_t func)
{
    uint32_t *r = cpu_state_ptr->regs;
    uint16_t dx = (uint16_t)r[2];
    uint16_t cx = (uint16_t)r[1];
    uint16_t bx = (uint16_t)r[3];

    switch (func) {
    case 0x00:
    case 0x4C:
        return x86_dos_terminate();

    case 0x02:                                  /* character output */
        x86_video_teletype((uint8_t)dx);
        return 1;

    case 0x09: {                                /* $-terminated string */
        uint32_t lin = cpu_state_ptr->segs[3].base + dx;
        for (unsigned i = 0; i < 4096; i++) {
            uint8_t *p = x86_guest_ptr(lin + i, 1);
            if (!p || *p == '$')
                break;
            x86_video_teletype(*p);
        }
        return 1;
    }

    case 0x1A:                                  /* set DTA */
        dta_lin = cpu_state_ptr->segs[3].base + dx;
        return 1;
    case 0x2F:                                  /* get DTA */
        x86_set_bx((uint16_t)(dta_lin & 0x0F));
        return 1;

    case 0x25: {                                /* set interrupt vector */
        uint8_t *ivt = x86_guest_ptr((uint32_t)(uint8_t)r[0] * 4u, 4);
        if (ivt) {
            ivt[0] = (uint8_t)dx;
            ivt[1] = (uint8_t)(dx >> 8);
            ivt[2] = (uint8_t)cpu_state_ptr->segs[3].sel;
            ivt[3] = (uint8_t)(cpu_state_ptr->segs[3].sel >> 8);
        }
        return 1;
    }
    case 0x35: {                                /* get interrupt vector */
        uint8_t *ivt = x86_guest_ptr((uint32_t)(uint8_t)r[0] * 4u, 4);
        if (ivt) {
            x86_set_bx((uint16_t)(ivt[0] | (ivt[1] << 8)));
            uint16_t seg = (uint16_t)(ivt[2] | (ivt[3] << 8));
            cpu_state_ptr->segs[0].sel = seg;           /* ES */
            cpu_state_ptr->segs[0].base = SEG(seg);
        }
        return 1;
    }

    case 0x30:                                  /* DOS version 5.00 */
        x86_set_ax(0x0005);
        x86_set_bx(0);
        x86_set_cx(0);
        return 1;

    case 0x2C:                                  /* get time */
        x86_set_cx(0x0C00);
        x86_set_dx(0);
        return 1;
    case 0x2A:                                  /* get date */
        x86_set_cx(1990);
        x86_set_dx(0x0101);
        return 1;

    /* ---- memory ---------------------------------------------------- */
    case 0x48: {                                /* allocate */
        uint16_t largest = 0;
        uint16_t seg = mcb_alloc(bx, psp_seg, &largest);
        if (seg) {
            x86_set_ax(seg);
            ok_clear_cf();
        } else {
            x86_set_bx(largest);
            fail(0x08);
        }
        return 1;
    }
    case 0x49:                                  /* free */
        if (mcb_free(cpu_state_ptr->segs[0].sel))
            ok_clear_cf();
        else
            fail(0x09);
        return 1;
    case 0x4A: {                                /* resize */
        uint16_t largest = 0;
        if (mcb_resize(cpu_state_ptr->segs[0].sel, bx, &largest)) {
            ok_clear_cf();
        } else {
            x86_set_bx(largest);
            fail(0x08);
        }
        return 1;
    }

    /* ---- files ------------------------------------------------------ */
    case 0x3D: {                                /* open */
        char path[64];
        guest_str(cpu_state_ptr->segs[3].base + dx, path, sizeof(path));
        int slot = handle_alloc();
        if (slot < 0) { fail(0x04); return 1; }
        BYTE mode = ((uint8_t)r[0] & 3) == 0 ? FA_READ : FA_READ | FA_WRITE;
        if (f_open(&dos_files[slot], path, mode) != FR_OK) {
            dos_file_used[slot] = 0;
            fail(0x02);
            return 1;
        }
        x86_set_ax((uint16_t)(slot + 5));
        ok_clear_cf();
        return 1;
    }
    case 0x3E: {                                /* close */
        FIL *f = handle_get(bx);
        if (!f) { fail(0x06); return 1; }
        f_close(f);
        dos_file_used[bx - 5] = 0;
        ok_clear_cf();
        return 1;
    }
    case 0x3F: {                                /* read */
        FIL *f = handle_get(bx);
        if (!f) { fail(0x06); return 1; }
        uint32_t lin = cpu_state_ptr->segs[3].base + dx;
        uint32_t total = 0;
        while (total < cx) {
            uint8_t buf[512];
            UINT want = (cx - total > sizeof(buf)) ? sizeof(buf)
                                                   : (UINT)(cx - total);
            UINT n = 0;
            if (f_read(f, buf, want, &n) != FR_OK || n == 0)
                break;
            if (x86_guest_copy_in(lin + total, buf, n) != n)
                break;
            total += n;
        }
        x86_set_ax((uint16_t)total);
        ok_clear_cf();
        return 1;
    }
    case 0x40: {                                /* write */
        FIL *f = handle_get(bx);
        if (!f) { fail(0x06); return 1; }
        uint32_t lin = cpu_state_ptr->segs[3].base + dx;
        uint32_t total = 0;
        while (total < cx) {
            uint8_t buf[512];
            uint32_t want = (cx - total > sizeof(buf)) ? sizeof(buf)
                                                       : (cx - total);
            uint32_t got = x86_guest_copy_out(buf, lin + total, want);
            if (!got) break;
            UINT n = 0;
            if (f_write(f, buf, got, &n) != FR_OK || n == 0)
                break;
            total += n;
        }
        x86_set_ax((uint16_t)total);
        ok_clear_cf();
        return 1;
    }
    case 0x42: {                                /* seek */
        FIL *f = handle_get(bx);
        if (!f) { fail(0x06); return 1; }
        uint32_t off = ((uint32_t)cx << 16) | dx;
        uint8_t whence = (uint8_t)r[0];
        FSIZE_t base = (whence == 1) ? f_tell(f)
                     : (whence == 2) ? f_size(f) : 0;
        FSIZE_t pos = base + (FSIZE_t)off;
        if (f_lseek(f, pos) != FR_OK) { fail(0x19); return 1; }
        x86_set_ax((uint16_t)pos);
        x86_set_dx((uint16_t)(pos >> 16));
        ok_clear_cf();
        return 1;
    }

    default:
        x86_hle_unhandled_vector = 0x21;
        x86_hle_unhandled_func = func;
        x86_dos_last_error = func;
        fail(0x01);                             /* invalid function */
        return 0;
    }
}
