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
#define ARENA_FIRST_MCB X86_DOS_ARENA_SEG
#define ARENA_END_SEG   0x9FFFu

#define MCB_MEMBER      0x4Du       /* 'M': another block follows */
#define MCB_LAST        0x5Au       /* 'Z': end of the chain      */

/*
 * owner == 0 is FREE, and that is not ambiguous -- it is provably unusable as
 * a real owner. An owner field holds a PSP SEGMENT, and a PSP is the first
 * paragraph of the DATA area of an allocated block, so it is always
 * `mcb_seg + 1` for some block in the chain. The chain starts at
 * ARENA_FIRST_MCB (0x0070) and every block sits at or above it, so the
 * smallest owner value any block can ever carry is 0x0071. Segment 0 is the
 * interrupt vector table; nothing is ever allocated there, and DOS itself uses
 * the same encoding for the same reason.
 *
 * What IS a real hazard is a CALLER passing 0 as the owner and meaning "I will
 * fill this in later" -- the program loader did exactly that, and the block
 * holding the running program stayed on the free list until the next AH=48h
 * handed it back out. mcb_alloc() now refuses to record 0: an allocation is by
 * definition not free, so owner 0 is normalised to MCB_OWNER_DOS at the single
 * point where it would otherwise enter the chain.
 */
#define MCB_FREE        0x0000u     /* owner value meaning "on the free list" */
#define MCB_OWNER_DOS   0x0008u     /* blocks DOS itself owns (env, loader)   */

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

/* Chain-walk cost, in header reads. Every one of these is an x86_guest_ptr()
   call, and this counter is the whole reason the arena's cost is a number
   rather than an opinion: the recon pass found the host PC sitting in
   mcb_read/x86_guest_ptr and the guest down to ~40 KIPS, and "it feels faster"
   is not a measurement. Host-visible; four bytes. */
uint32_t x86_mcb_reads;

static int mcb_read(uint16_t seg, mcb_t *m)
{
    x86_mcb_reads++;
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
    m.owner = MCB_FREE;
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
        if (m.owner == MCB_FREE && mcb_read(next, &n) && n.owner == MCB_FREE) {
            m.size = (uint16_t)(m.size + n.size + 1u);
            m.type = n.type;
            mcb_write(seg, &m);
            continue;               /* try to absorb the one after that too */
        }
        seg = next;
    }
}

/* Split `m` (already read, sitting at `seg`) down to `paras` paragraphs,
   handing the remainder back as a free block. Writes `m`. The tail absorbs the
   block after it if that one is free too, so a shrink never leaves two
   adjacent free blocks behind -- which is what made a later grow fail for no
   reason. Local, so it costs two reads instead of a whole-chain coalesce. */
static void mcb_shrink_to(uint16_t seg, mcb_t *m, uint16_t paras)
{
    if (m->size <= paras + 1u)
        return;                     /* not worth a header; keep the slack */

    uint16_t tail = (uint16_t)(seg + paras + 1u);
    mcb_t t;
    t.type = m->type;
    t.owner = MCB_FREE;
    t.size = (uint16_t)(m->size - paras - 1u);

    if (t.type != MCB_LAST) {
        mcb_t n;
        uint16_t after = (uint16_t)(tail + t.size + 1u);
        if (mcb_read(after, &n) && n.owner == MCB_FREE) {
            t.size = (uint16_t)(t.size + n.size + 1u);
            t.type = n.type;
        }
    }
    mcb_write(tail, &t);

    m->type = MCB_MEMBER;
    m->size = paras;
    mcb_write(seg, m);
}

/* Allocate `paras` paragraphs. Returns the DATA segment, or 0 with *largest
   set to the biggest block available -- which is what INT 21h 48h must report
   so a program can retry with a size that fits.
 *
 * One pass. It used to be three -- mcb_coalesce(), then the fitting walk, then
 * mcb_largest_free() -- each an O(n) chain walk through x86_guest_ptr() per
 * node. With a fragmented arena that is where the emulator was spending
 * essentially all of its time (measured: host PC in mcb_alloc/mcb_read/
 * x86_guest_ptr, guest down to ~40 KIPS). Merging free neighbours as the walk
 * passes them, and tracking the largest while it goes, gets the same answers
 * for a third of the reads and no extra state.
 */
static uint16_t mcb_alloc(uint16_t paras, uint16_t owner, uint16_t *largest)
{
    if (owner == MCB_FREE)
        owner = MCB_OWNER_DOS;      /* an allocation is never free; see above */

    uint16_t seg = ARENA_FIRST_MCB, best = 0;
    mcb_t m, n;
    while (mcb_read(seg, &m)) {
        /* Absorb the whole run of free blocks that follows this free one. */
        while (m.owner == MCB_FREE && m.type != MCB_LAST &&
               mcb_read((uint16_t)(seg + m.size + 1u), &n) &&
               n.owner == MCB_FREE) {
            m.size = (uint16_t)(m.size + n.size + 1u);
            m.type = n.type;
            mcb_write(seg, &m);
        }
        if (m.owner == MCB_FREE) {
            if (m.size > best)
                best = m.size;
            if (m.size >= paras) {
                mcb_shrink_to(seg, &m, paras);
                m.owner = owner;
                mcb_write(seg, &m);
                return (uint16_t)(seg + 1u);
            }
        }
        if (m.type == MCB_LAST)
            break;
        seg = (uint16_t)(seg + m.size + 1u);
    }
    if (largest)
        *largest = best;
    return 0;
}

static int mcb_free(uint16_t data_seg)
{
    uint16_t seg = (uint16_t)(data_seg - 1u);
    mcb_t m;
    if (!mcb_read(seg, &m))
        return 0;
    m.owner = MCB_FREE;
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
        mcb_shrink_to(seg, &m, paras);
        return 1;
    }

    /*
     * Grow. PROBE FIRST, COMMIT ONLY ON SUCCESS.
     *
     * This used to merge the next free block in and then recurse, and if the
     * result was still too small it returned 0 with the merge already written.
     * A failed AH=4Ah therefore ATE the free block it had just been told was
     * not enough -- while reporting failure. DOS/4GW probes for the largest
     * available block with BX=0xFFFF, gets "no", and asks again; each "no"
     * consumed more of the arena, so the loop never converged (measured on
     * STAR_WARS_DARKFORCES: 34,234 resizes, zero frees, arena marched to
     * segment 0x896A and the chain walk swamped the emulator).
     *
     * The walk below only READS. It also spans the entire run of adjacent free
     * blocks rather than just the first one, so a grow no longer fails merely
     * because the free space either side of a boundary had not been coalesced.
     */
    uint32_t avail = m.size;
    uint16_t last_type = m.type, next = (uint16_t)(seg + m.size + 1u);
    mcb_t n;
    while (last_type != MCB_LAST && mcb_read(next, &n) && n.owner == MCB_FREE) {
        avail += 1u + n.size;
        last_type = n.type;
        next = (uint16_t)(next + n.size + 1u);
    }

    if (avail < paras) {
        /* DOS reports the largest size this block CAN grow to. Nothing has
           been written: the chain is byte-identical to what we found. */
        if (largest)
            *largest = (uint16_t)(avail > 0xFFFFu ? 0xFFFFu : avail);
        return 0;
    }

    m.size = (uint16_t)avail;
    m.type = last_type;
    mcb_shrink_to(seg, &m, paras);
    mcb_write(seg, &m);
    return 1;
}

/* Test shims. The arena functions are static on purpose -- nothing outside
   INT 21h should allocate -- but the tier-1 harness has to drive them. */
uint16_t dos_test_alloc(uint16_t paras, uint16_t *largest)
{ return mcb_alloc(paras, 0x1234, largest); }
/* Deliberately reachable with owner 0: the point is to prove that a caller who
   asks for a block "to fill the owner in later" cannot leave it on the free
   list. Nothing but the test uses this. */
uint16_t dos_test_alloc_owner(uint16_t paras, uint16_t owner, uint16_t *largest)
{ return mcb_alloc(paras, owner, largest); }
uint16_t dos_test_owner_of(uint16_t data_seg)
{
    mcb_t m;
    if (!mcb_read((uint16_t)(data_seg - 1u), &m))
        return 0xFFFFu;
    return m.owner;
}
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
    /* Built in a local and copied granule-aware: a PSP is placed at an
       arbitrary PARAGRAPH, so 256 bytes from it straddles a 4 KB granule
       whenever the segment lands in the last 240 bytes of a page --
       x86_guest_ptr() refuses that span and the PSP was then never written at
       all, in silence. Same defect class as the DTA. */
    uint8_t p[256];
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

    x86_guest_copy_in(SEG(seg), p, 256);

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

/*
 * Character devices.
 *
 * DOS programs open devices by name -- a full-screen UI typically does
 * Rewrite() on 'CON' and then writes through AH=40h rather than AH=02h, and a
 * report writer opens 'PRN' or 'NUL' the same way. There is no directory entry
 * to find, so these can never come from FatFs; they get handles out of a
 * reserved range instead, which cannot collide with the FatFs slots at 5..20.
 *
 * The standard handles are the same devices under their inherited numbers:
 * 0/1/2 are CON, 3 (AUX) and 4 (PRN) are sinks. Reads and writes on handle 1
 * previously fell into handle_get(), which rejects anything below 5, so a
 * program printing through the handle API got an "invalid handle" error for
 * every line it wrote.
 */
enum { DOS_DEV_NONE = 0, DOS_DEV_CON, DOS_DEV_NUL };
#define DOS_DEV_HANDLE(d)  ((uint16_t)(0x00F0 + (d)))

/* The name of the last file the guest asked us to create or open, for
   diagnosis: an unhandled request is far easier to place with the path in
   hand than with the function number alone. */
char x86_dos_last_path[32];

static int dos_name_is_device(const char *p)
{
    /* Compare the stem only: "CON", "CON:", "A:\CON" are all the device. */
    const char *s = p;
    for (const char *q = p; *q; q++)
        if (*q == '\\' || *q == '/' || *q == ':')
            s = q + 1;

    char n[5];
    unsigned i = 0;
    while (i < 4 && s[i] && s[i] != ':' && s[i] != '.') {
        char c = s[i];
        n[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        i++;
    }
    n[i] = 0;

    if (!strcmp(n, "CON"))
        return DOS_DEV_CON;
    if (!strcmp(n, "NUL") || !strcmp(n, "PRN") || !strcmp(n, "AUX") ||
        (i == 4 && (!strncmp(n, "LPT", 3) || !strncmp(n, "COM", 3))))
        return DOS_DEV_NUL;
    return DOS_DEV_NONE;
}

static int dos_handle_device(uint16_t h)
{
    if (h <= 2)
        return DOS_DEV_CON;                     /* stdin/stdout/stderr */
    if (h == 3 || h == 4)
        return DOS_DEV_NUL;                     /* AUX, PRN */
    if (h == DOS_DEV_HANDLE(DOS_DEV_CON))
        return DOS_DEV_CON;
    if (h == DOS_DEV_HANDLE(DOS_DEV_NUL))
        return DOS_DEV_NUL;
    return DOS_DEV_NONE;
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

/*
 * A DOS path is not a FatFs path.
 *
 * FatFs is built FF_STR_VOLUME_ID=0, so the only drive prefix it understands is
 * a DIGIT ("0:"). A guest naming its own file the way DOS programs habitually
 * do -- "A:DATABASE.INI", "C:\TB\DATABASE.INI" -- hands it a LETTER, which
 * f_open rejects outright with FR_INVALID_DRIVE. We have exactly one volume
 * (the mounted .dsk), so every drive letter means the same disk: strip it.
 *
 * FF_FS_RPATH is 0 too, so FatFs has no current directory of its own. That is
 * fine for a single-floppy title whose files all sit in the root, and WRONG for
 * a title AUTOEXEC.BAT reaches with `CD \DF`: every relative name it then opens
 * ("DARK.GOB") has to resolve against that directory, and with no CWD anywhere
 * in the stack it resolved against the root and missed. So the current
 * directory is kept HERE, as a plain prefix string, and applied to exactly the
 * paths DOS applies it to -- the relative ones. "\DF\TEXT.MSG" and "C:\FOO"
 * still mean the root, as they do under DOS.
 *
 * "C:FOO" is DRIVE-RELATIVE, not absolute: DOS resolves it against that
 * drive's current directory. We have one volume and one CWD, so it gets the
 * prefix too. (The reverted version of this change read "C:FOO" as rooted;
 * that is the wrong reading and it is the one a title in a subdirectory would
 * trip over.)
 */
static char dos_cwd[48];                        /* "" = root; no trailing '/' */

void x86_dos_set_cwd(const char *dir)
{
    unsigned i = 0;
    if (dir) {
        while (*dir == '\\' || *dir == '/')
            dir++;
        while (dir[i] && i < sizeof(dos_cwd) - 1) {
            dos_cwd[i] = (dir[i] == '\\') ? '/' : dir[i];
            i++;
        }
        while (i && dos_cwd[i - 1] == '/')
            i--;
    }
    dos_cwd[i] = 0;
}

const char *x86_dos_get_cwd(void) { return dos_cwd; }

static void dos_path_from_guest(uint32_t lin, char *out, unsigned max)
{
    char raw[64];
    guest_str(lin, raw, sizeof(raw));

    const char *s = raw;
    if (s[0] && s[1] == ':')                    /* drop any "X:" */
        s += 2;
    int absolute = (*s == '\\' || *s == '/');
    while (*s == '\\' || *s == '/')             /* and any leading separator */
        s++;

    unsigned i = 0;
    if (!absolute && dos_cwd[0]) {
        while (dos_cwd[i] && i < max - 2) {
            out[i] = dos_cwd[i];
            i++;
        }
        if (i < max - 1)
            out[i++] = '/';
    }
    unsigned j = 0;
    while (s[j] && i < max - 1) {
        out[i] = (s[j] == '\\') ? '/' : s[j];
        i++;
        j++;
    }
    out[i] = 0;
}

/* --------------------------------------------------------------------------
 * Directory search (AH=4Eh / 4Fh).
 *
 * FatFs is built FF_USE_FIND=0, so the pattern matching is ours. That is not a
 * loss: DOS wildcard rules are not FatFs's. A DOS name is a fixed 8+3 field,
 * '?' matches one padding-included position and '*' fills the REST OF THAT
 * FIELD -- so "*.INI" matches "DATABASE.INI" and "*" alone matches everything,
 * which is what a program enumerating a floppy actually sends.
 */
static char dos_up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* Expand "NAME.EXT" into the 11-character padded field DOS compares in. */
static void dos_83_field(const char *s, char out[11])
{
    unsigned i = 0;
    memset(out, ' ', 11);
    for (; *s && *s != '.' && i < 8; s++, i++)
        out[i] = dos_up(*s);
    while (*s && *s != '.')
        s++;
    if (*s == '.') {
        s++;
        for (i = 0; *s && i < 3; s++, i++)
            out[8 + i] = dos_up(*s);
    }
}

/* Same, but '*' fills the remainder of its own field with '?'. */
static void dos_83_pattern(const char *s, char out[11])
{
    unsigned i = 0;
    memset(out, ' ', 11);
    for (; *s && *s != '.' && i < 8; s++) {
        if (*s == '*') { while (i < 8) out[i++] = '?'; break; }
        out[i++] = dos_up(*s);
    }
    while (*s && *s != '.')
        s++;
    if (*s == '.') {
        s++;
        for (i = 0; *s && i < 3; s++) {
            if (*s == '*') { while (i < 3) out[8 + i++] = '?'; break; }
            out[8 + i++] = dos_up(*s);
        }
    }
}

static int dos_83_match(const char pat[11], const char name[11])
{
    for (unsigned i = 0; i < 11; i++)
        if (pat[i] != '?' && pat[i] != name[i])
            return 0;
    return 1;
}

/* One live search. DOS keeps its state in the first 21 bytes of the DTA; we
   keep ours here and stamp a serial into the DTA so a FindNext against a stale
   or foreign DTA fails cleanly instead of walking someone else's directory. */
static DIR dos_find_dir;
static char dos_find_pat[11];
static uint8_t dos_find_attr;
static uint8_t dos_find_open;
static uint8_t dos_find_serial;
#define DOS_FIND_MAGIC 0xF1

/* Fill the DTA's result half from a directory entry, and return 1. */
/*
 * The 8.3 name of a directory entry.
 *
 * With LFN enabled FatFs puts the short name in `altname` and leaves it EMPTY
 * when the long and short names are the same, so the fallback to `fname` is
 * required either way. With LFN disabled (which is how this FatFs is built --
 * it mounts FAT12 floppies, see lib/FatFs/ffconf.h) there is no `altname`
 * member at all and `fname` IS the 8.3 name. DOS only ever sees 8.3, so both
 * builds must land on the same string.
 */
#if FF_USE_LFN
#define DOS_SHORT_NAME(f)  ((f)->altname[0] ? (f)->altname : (f)->fname)
#else
#define DOS_SHORT_NAME(f)  ((f)->fname)
#endif

/*
 * The DTA is guest memory at an address the GUEST chose, so it can sit
 * anywhere -- including within 43 bytes of the end of a 4 KB granule.
 * `x86_guest_ptr()` refuses a span that crosses one and returns NULL, and
 * this function used to ask it for all 43 bytes at once and treat the refusal
 * as "no match". The caller cannot tell that from "the directory ended", so
 * the guest was told the disk was EMPTY. WOLF3D sets its DTA to DS:0xFFBA and
 * its findfirst("*.WL1") returned AX=0x12 with all eight data files present.
 *
 * `x86_guest_copy_in()` walks granules, so it is the only correct way to put a
 * multi-byte record into guest memory at an address we did not pick.
 *
 * The two writes are separate because bytes 2..0x14 are DOS's own search
 * state, which the caller may not clobber; only our magic/serial at 0..1 and
 * the result record at 0x15..0x2A belong to us.
 */
static int dos_find_report(const FILINFO *fno)
{
    uint8_t hdr[2] = { DOS_FIND_MAGIC, dos_find_serial };
    uint8_t rec[22];

    rec[0x00] = fno->fattrib;
    rec[0x01] = (uint8_t)fno->ftime;       rec[0x02] = (uint8_t)(fno->ftime >> 8);
    rec[0x03] = (uint8_t)fno->fdate;       rec[0x04] = (uint8_t)(fno->fdate >> 8);
    rec[0x05] = (uint8_t)fno->fsize;       rec[0x06] = (uint8_t)(fno->fsize >> 8);
    rec[0x07] = (uint8_t)(fno->fsize >> 16);
    rec[0x08] = (uint8_t)(fno->fsize >> 24);

    memset(rec + 0x09, 0, 13);
    const char *n = DOS_SHORT_NAME(fno);
    for (unsigned i = 0; n[i] && i < 12; i++)
        rec[0x09 + i] = (uint8_t)dos_up(n[i]);

    if (x86_guest_copy_in(dta_lin, hdr, sizeof(hdr)) != sizeof(hdr))
        return 0;
    if (x86_guest_copy_in(dta_lin + 0x15, rec, sizeof(rec)) != sizeof(rec))
        return 0;
    return 1;
}

/* Walk until an entry matches, or the directory ends. */
static int dos_find_step(void)
{
    FILINFO fno;
    for (;;) {
        if (f_readdir(&dos_find_dir, &fno) != FR_OK || fno.fname[0] == 0)
            return 0;
        /* A DOS search returns normal files always, and hidden/system/dir
           entries only if the caller asked for them in CX. */
        uint8_t extra = (uint8_t)(fno.fattrib & (AM_HID | AM_SYS | AM_DIR));
        if (extra & ~dos_find_attr)
            continue;
        char field[11];
        const char *n = DOS_SHORT_NAME(&fno);
        dos_83_field(n, field);
        if (dos_83_match(dos_find_pat, field))
            return dos_find_report(&fno);
    }
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

    /*
     * One block for the PSP, the image and the memory the header asks for.
     *
     * BOTH bounds matter. minalloc is a minimum, not a suggestion: a program
     * that gets less overwrites whatever follows it. maxalloc is what the
     * program actually WANTS, and honouring only the minimum is not a
     * conservative choice -- it is a wrong one. A Borland runtime sizes its
     * heap from the block DOS hands it, and TOPBENCH ships maxalloc=0xFFFF,
     * i.e. "give me everything". Allocated at minalloc it started, found ~100 KB
     * where it expected ~600 KB, and died with `Runtime error 203` (heap
     * overflow) before printing a single result.
     *
     * Real DOS tries max first and falls back to the largest free block, which
     * is why 0xFFFF is the idiom for "all of it" rather than a request that
     * fails.
     */
    uint16_t need = (uint16_t)(PARA(256) + PARA(image) + h.minalloc);
    uint32_t want32 = (uint32_t)PARA(256) + PARA(image) + h.maxalloc;
    uint16_t want = (want32 > 0xFFFFu) ? 0xFFFFu : (uint16_t)want32;
    if (want < need)
        want = need;

    /*
     * THE ACCEPTANCE TEST FOR THE GRANULE SEAM, and nothing else.
     *
     * A program's load address may only move by whole 4 KB granules while the
     * seam is broken: shift it by a granule and every structure keeps its
     * offset within its page, so the same accesses straddle and the same luck
     * holds; shift it by 256 bytes and a DIFFERENT set of accesses lands on a
     * boundary and the guest is silently corrupted. That is exactly how this
     * bug was found -- WOLF3D rendered at PSP 0x710 and at 0x1710, and broke at
     * 0x810 -- so a fix that cannot survive a non-granule-aligned PSP has not
     * been demonstrated.
     *
     * X86_DOS_PSP_SHIFT is paragraphs of dead block allocated ahead of the
     * program, plus the one paragraph of MCB header: 15 gives exactly 256
     * bytes. Default 0, so the shipping path is byte for byte what it was.
     * See tests/test_seam.py and the seam block in x86_seam.c.
     */
#if X86_DOS_PSP_SHIFT
    {
        uint16_t pad_largest = 0;
        (void)mcb_alloc(X86_DOS_PSP_SHIFT, MCB_OWNER_DOS, &pad_largest);
    }
#endif

    uint16_t largest = 0;
    uint16_t block = mcb_alloc(want, MCB_OWNER_DOS, &largest);
    uint16_t granted = want;
    if (!block) {
        uint16_t avail = largest;
        if (avail >= need) {
            block = mcb_alloc(avail, MCB_OWNER_DOS, &largest);
            granted = avail;
        }
    }
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

    /* Top of memory as the program sees it: the end of what it was GRANTED,
       not of what it minimally needed. A Borland startup compares its own top
       against this field and refuses to run if it does not fit. */
    psp_build(psp, (uint16_t)(psp + granted), cmdline);

    /* Entry state, exactly as the header specifies. */
    /* Segment arithmetic wraps at 16 bits; the base must be built from the
       wrapped value or it can land above 1 MB. */
    cpu_state_ptr->segs[1].sel  = (uint16_t)(load_seg + h.cs);
    cpu_state_ptr->segs[1].base = SEG((uint16_t)(load_seg + h.cs));
    cpu_state_ptr->segs[1].limit = 0xFFFF;
    cpu_state_ptr->eip = h.ip;

    cpu_state_ptr->segs[2].sel  = (uint16_t)(load_seg + h.ss);
    cpu_state_ptr->segs[2].base = SEG((uint16_t)(load_seg + h.ss));
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
    /* Real DOS closes the process's handles on exit, and it matters here:
       FatFs is built FF_FS_TINY, so the last sector of a file the guest wrote
       is still sitting in the filesystem window until something flushes it. */
    for (int i = 0; i < DOS_MAX_FILES; i++)
        if (dos_file_used[i]) {
            f_close(&dos_files[i]);
            dos_file_used[i] = 0;
        }
    cpu_state_ptr->exit_reason = X86_EXIT_TERMINATE;
    return 1;
}

static void ok_clear_cf(void)   { x86_hle_set_cf(0); }
static void fail(uint16_t code) { x86_set_ax(code); x86_hle_set_cf(1); }

/*
 * The file-call trace.
 *
 * It used to be a 32-slot WRAPPING ring of two words. That is the wrong shape
 * for the question it gets asked: a guest stuck in a loop overwrites the whole
 * ring with the loop body in milliseconds, and the call that ISSUED the handle
 * the loop is failing on -- the only interesting entry -- is always gone.
 * WOLF3D's 32 identical AH=42h slots said nothing about where its handle came
 * from, which is the entire question.
 *
 * So it SATURATES at DOS_FILE_LOG_SLOTS instead of wrapping, keeping the
 * PROLOGUE of the run, and `x86_dos_file_calls` counts every call whether it
 * was recorded or not. Writing 0 to `x86_dos_file_log_idx` from the host
 * re-arms it, so a later phase can be captured without a rebuild.
 *
 * Four words per slot, not two: a func/AX pair cannot answer "which handle",
 * and BX plus CX:DX is the difference between "the seek is broken" and "the
 * handle was never issued".
 */
#define DOS_FILE_LOG_SLOTS 96
uint32_t x86_dos_file_log[DOS_FILE_LOG_SLOTS * 4];
uint32_t x86_dos_file_log_idx;
uint32_t x86_dos_file_calls;
char x86_dos_path_log[16][32];
static uint32_t dos_path_log_idx;

/* The MEMORY calls are traced too, but they are not FILE calls: they get a
   slot in the log and are NOT counted in x86_dos_file_calls, whose meaning
   other reports depend on. For these, slot+3 carries (ES << 16) | DX rather
   than CX:DX -- ES is the block for 49h/4Ah and DX is meaningless there, and
   "which block" is the whole question a failing resize loop asks. */
static int dos_func_is_mem(uint8_t f)
{
    return f == 0x48 || f == 0x49 || f == 0x4A;
}

static int dos_func_is_file(uint8_t f)
{
    switch (f) {
    case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14:
    case 0x15: case 0x16: case 0x17: case 0x1A: case 0x21: case 0x22:
    case 0x23: case 0x27: case 0x28: case 0x29: case 0x2F:
    case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E:
    case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43: case 0x45:
    case 0x46: case 0x47: case 0x4E: case 0x4F: case 0x56: case 0x57:
    case 0x5A: case 0x5B: case 0x68: case 0x6C:
        return 1;
    default:
        return 0;
    }
}

static void dos_log_path(const char *p)
{
    unsigned slot = dos_path_log_idx & 15;
    strncpy(x86_dos_path_log[slot], p, sizeof(x86_dos_path_log[0]) - 1);
    x86_dos_path_log[slot][sizeof(x86_dos_path_log[0]) - 1] = 0;
    dos_path_log_idx++;
}

static int dos_int21_dispatch(uint8_t func);

int x86_dos_int21(uint8_t func)
{
    int is_mem = dos_func_is_mem(func);
    if (!is_mem && !dos_func_is_file(func))
        return dos_int21_dispatch(func);

    if (!is_mem)
        x86_dos_file_calls++;
    if (x86_dos_file_log_idx >= DOS_FILE_LOG_SLOTS)
        return dos_int21_dispatch(func);

    unsigned slot = x86_dos_file_log_idx * 4;
    x86_dos_file_log[slot] =
        ((uint32_t)func << 8) | (uint8_t)(cpu_state_ptr->regs[0] & 0xFF);
    x86_dos_file_log[slot + 2] = (uint16_t)(cpu_state_ptr->regs[3] & 0xFFFF);
    x86_dos_file_log[slot + 3] = is_mem ?
        (((uint32_t)cpu_state_ptr->segs[0].sel << 16) |
         (uint16_t)(cpu_state_ptr->regs[2] & 0xFFFFu)) :
        (((uint32_t)(cpu_state_ptr->regs[1] & 0xFFFFu) << 16) |
         (uint16_t)(cpu_state_ptr->regs[2] & 0xFFFFu));
    int rc = dos_int21_dispatch(func);
    uint16_t *fl = x86_hle_flags_on_stack();
    x86_dos_file_log[slot + 1] =
        (uint32_t)((fl && (*fl & 1u)) ? 0x10000u : 0u) |
        (uint16_t)(cpu_state_ptr->regs[0] & 0xFFFF);
    x86_dos_file_log_idx++;
    return rc;
}

static int dos_int21_dispatch(uint8_t func)
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

    /*
     * The console family. AH=06h in particular: a program that wants to write
     * without DOS interpreting control characters uses it instead of AH=02h or
     * AH=09h, and TOPBENCH prints its entire report through it. Unanswered, it
     * printed nothing at all and the screen stayed at 80x25 of spaces.
     *
     * DL=0xFF makes AH=06h an INPUT call, and it must NOT block: ZF reports
     * whether a character was there. The genuinely blocking forms return 2,
     * which rewinds the INT so it re-executes on the next slice -- blocking
     * inside the service would stop the frame loop and with it the input that
     * would satisfy it.
     */
    case 0x06: {                                /* direct console I/O */
        uint16_t key;
        if ((dx & 0xFFu) != 0xFFu) {
            x86_video_teletype((uint8_t)dx);
            return 1;
        }
        if (x86_kb_get(&key)) {
            x86_set_al((uint8_t)key);
            x86_hle_set_zf(0);
        } else {
            x86_set_al(0);
            x86_hle_set_zf(1);
        }
        return 1;
    }
    case 0x01:                                  /* input, echoed */
    case 0x07:                                  /* direct input, no echo */
    case 0x08: {                                /* input, no echo */
        uint16_t key;
        if (!x86_kb_get(&key))
            return 2;                           /* nothing yet: retry */
        x86_set_al((uint8_t)key);
        if (func == 0x01)
            x86_video_teletype((uint8_t)key);
        return 1;
    }
    case 0x0B: {                                /* input status */
        uint16_t key;
        x86_set_al(x86_kb_peek(&key) ? 0xFF : 0x00);
        return 1;
    }
    case 0x0C:                                  /* flush, then input */
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
        /* ES:BX, not BX alone. The old form returned the bottom NIBBLE of the
           linear address and nothing else, so a program that saved the DTA and
           restored it later pointed at ES:000x -- the interrupt vector table
           of whatever segment ES held. */
        x86_set_bx((uint16_t)(dta_lin & 0x0F));
        uint16_t dseg = (uint16_t)((dta_lin >> 4) & 0xFFFFu);
        cpu_state_ptr->segs[0].sel = dseg;      /* ES */
        cpu_state_ptr->segs[0].base = SEG(dseg);
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
    /*
     * AH=3Ch, create/truncate. TOPBENCH's full-screen UI opens a stream and
     * dies without one: unanswered, the default arm below reported "invalid
     * function", which Borland's runtime prints as `Runtime error 001` -- and
     * the half-drawn screen behind that message is what looked like a broken
     * renderer.
     *
     * CX carries the file attributes; nothing here has an attribute to store,
     * so it is ignored deliberately rather than by omission.
     */
    case 0x3C: {                                /* create / truncate */
        char path[64];
        dos_path_from_guest(cpu_state_ptr->segs[3].base + dx, path,
                            sizeof(path));
        strncpy(x86_dos_last_path, path, sizeof(x86_dos_last_path) - 1);
        dos_log_path(path);
        int dev = dos_name_is_device(path);
        if (dev) {
            x86_set_ax(DOS_DEV_HANDLE(dev));
            ok_clear_cf();
            return 1;
        }
        int slot = handle_alloc();
        if (slot < 0) { fail(0x04); return 1; }
        if (f_open(&dos_files[slot], path,
                   FA_CREATE_ALWAYS | FA_READ | FA_WRITE) != FR_OK) {
            dos_file_used[slot] = 0;
            fail(0x05);                         /* access denied */
            return 1;
        }
        x86_set_ax((uint16_t)(slot + 5));
        ok_clear_cf();
        return 1;
    }
    case 0x3D: {                                /* open */
        char path[64];
        dos_path_from_guest(cpu_state_ptr->segs[3].base + dx, path,
                            sizeof(path));
        strncpy(x86_dos_last_path, path, sizeof(x86_dos_last_path) - 1);
        dos_log_path(path);
        int dev = dos_name_is_device(path);
        if (dev) {
            x86_set_ax(DOS_DEV_HANDLE(dev));
            ok_clear_cf();
            return 1;
        }
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
    case 0x44:                                  /* IOCTL */
        /* AL=00 get device information. A program asks this about its own
           stdout to find out whether it is talking to the console or to a
           redirected file, and TOPBENCH asks it before deciding how to report.
           Unanswered, it read whatever DX happened to hold and went off into
           memory it does not own.
           Handles 0-2 are the console: 0x80D3 is what a real DOS returns for
           CON (character device, is-stdin, is-stdout, special). Anything else
           is one of our FatFs handles, i.e. a file on drive A. */
        switch ((uint8_t)(r[0] & 0xFF)) {
        case 0x00:
            if (dos_handle_device(bx))
                x86_set_dx(0x80D3);
            else if (handle_get(bx))
                x86_set_dx(0x0000);             /* file, drive A, not a device */
            else { fail(0x06); return 1; }
            x86_set_ax((uint16_t)(cpu_state_ptr->regs[2] & 0xFFFF));
            ok_clear_cf();
            return 1;
        case 0x01:                              /* set device information */
            ok_clear_cf();
            return 1;
        default:
            fail(0x01);                         /* invalid function */
            return 1;
        }
    case 0x3E: {                                /* close */
        if (dos_handle_device(bx)) { ok_clear_cf(); return 1; }
        FIL *f = handle_get(bx);
        if (!f) { fail(0x06); return 1; }
        f_close(f);
        dos_file_used[bx - 5] = 0;
        ok_clear_cf();
        return 1;
    }
    case 0x3F: {                                /* read */
        int dev = dos_handle_device(bx);
        if (dev == DOS_DEV_NUL) {               /* always at end of file */
            x86_set_ax(0);
            ok_clear_cf();
            return 1;
        }
        if (dev == DOS_DEV_CON) {
            /* The console, one line at a time. Blocking here would stop the
               frame loop and with it the keyboard that would satisfy the
               read, so an empty queue rewinds the INT instead (return 2). */
            uint32_t lin = cpu_state_ptr->segs[3].base + dx;
            uint16_t key;
            uint32_t n = 0;
            while (n < cx) {
                if (!x86_kb_peek(&key))
                    break;
                x86_kb_get(&key);
                uint8_t c = (uint8_t)key;
                if (c == 0x0D) c = 0x0A;
                x86_video_teletype(c);          /* DOS echoes a console read */
                if (x86_guest_copy_in(lin + n, &c, 1) != 1)
                    break;
                n++;
                if (c == 0x0A)
                    break;
            }
            if (n == 0 && cx)
                return 2;
            x86_set_ax((uint16_t)n);
            ok_clear_cf();
            return 1;
        }
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
        int dev = dos_handle_device(bx);
        if (dev) {
            uint32_t lin = cpu_state_ptr->segs[3].base + dx;
            if (dev == DOS_DEV_CON) {
                for (uint32_t i = 0; i < cx; i++) {
                    uint8_t *p = x86_guest_ptr(lin + i, 1);
                    if (!p)
                        break;
                    x86_video_teletype(*p);
                }
            }
            x86_set_ax(cx);                     /* a sink accepts everything */
            ok_clear_cf();
            return 1;
        }
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
    /*
     * AH=43h, get/set file attributes -- MEASURED as what stopped TOPBENCH.
     * Its file log read 3Ch/40h/3Eh (create, write, close its own log) and then
     * 43h AL=00 on DATABASE.INI, which the default arm answered with CF set and
     * AX=1 "invalid function". A program cannot tell that from "no such file",
     * so it reported its data file as unreadable and never issued the 3Dh that
     * would have opened it. The open path was never reached, let alone broken.
     *
     * FatFs is FF_USE_CHMOD=0, so SET is accepted for a file that exists and
     * changes nothing: refusing it would break a program that merely clears the
     * archive bit, and the attribute has no meaning on a read-back of the .dsk.
     */
    case 0x43: {                                /* get / set file attributes */
        char path[64];
        dos_path_from_guest(cpu_state_ptr->segs[3].base + dx, path,
                            sizeof(path));
        dos_log_path(path);
        strncpy(x86_dos_last_path, path, sizeof(x86_dos_last_path) - 1);
        if (dos_name_is_device(path)) {         /* devices have no directory */
            x86_set_cx(0x0040);                 /* AM_ARC-ish: "just a file" */
            ok_clear_cf();
            return 1;
        }
        FILINFO fno;
        if (f_stat(path, &fno) != FR_OK) { fail(0x02); return 1; }
        if (((uint8_t)r[0] & 0xFF) == 0x00)
            x86_set_cx(fno.fattrib);
        ok_clear_cf();
        return 1;
    }

    case 0x41: {                                /* delete file */
        char path[64];
        dos_path_from_guest(cpu_state_ptr->segs[3].base + dx, path,
                            sizeof(path));
        dos_log_path(path);
        if (f_unlink(path) != FR_OK) { fail(0x02); return 1; }
        ok_clear_cf();
        return 1;
    }
    case 0x56: {                                /* rename */
        char from[64], to[64];
        dos_path_from_guest(cpu_state_ptr->segs[3].base + dx, from,
                            sizeof(from));
        dos_path_from_guest(cpu_state_ptr->segs[0].base +
                            (uint16_t)cpu_state_ptr->regs[7], to, sizeof(to));
        if (f_rename(from, to) != FR_OK) { fail(0x02); return 1; }
        ok_clear_cf();
        return 1;
    }

    /*
     * FindFirst / FindNext. Old tools reach their data files through these as
     * often as through 3Dh -- a wildcard is how a program discovers what is on
     * the disk at all -- and unanswered they look exactly like an empty floppy.
     */
    case 0x4E: {                                /* find first */
        char path[64];
        dos_path_from_guest(cpu_state_ptr->segs[3].base + dx, path,
                            sizeof(path));
        dos_log_path(path);
        strncpy(x86_dos_last_path, path, sizeof(x86_dos_last_path) - 1);

        /* Split the directory off the pattern. One volume, root only. */
        char *stem = path;
        for (char *q = path; *q; q++)
            if (*q == '\\' || *q == '/')
                stem = q + 1;

        if (dos_find_open) { f_closedir(&dos_find_dir); dos_find_open = 0; }
        if (f_opendir(&dos_find_dir, "") != FR_OK) { fail(0x03); return 1; }
        dos_find_open = 1;
        dos_find_serial++;
        dos_find_attr = (uint8_t)(cx & (AM_HID | AM_SYS | AM_DIR));
        dos_83_pattern(stem, dos_find_pat);
        if (!dos_find_step()) {
            f_closedir(&dos_find_dir);
            dos_find_open = 0;
            fail(0x12);                         /* no more files */
            return 1;
        }
        x86_set_ax(0);
        ok_clear_cf();
        return 1;
    }
    case 0x4F: {                                /* find next */
        uint8_t d[2];
        if (!dos_find_open ||
            x86_guest_copy_out(d, dta_lin, 2) != 2 ||
            d[0] != DOS_FIND_MAGIC || d[1] != dos_find_serial) {
            fail(0x12);
            return 1;
        }
        if (!dos_find_step()) {
            f_closedir(&dos_find_dir);
            dos_find_open = 0;
            fail(0x12);
            return 1;
        }
        x86_set_ax(0);
        ok_clear_cf();
        return 1;
    }

    case 0x19:                                  /* current drive */
        x86_set_al(0);                          /* A: -- the only one there is */
        return 1;
    case 0x0E:                                  /* select drive */
        x86_set_al(1);                          /* one drive in the system */
        return 1;
    case 0x47: {                                /* get current directory */
        /*
         * The CWD, in DOS notation and WITHOUT a leading backslash -- which is
         * what the DOS API specifies and what a caller re-joins with one.
         *
         * "Root, always" was not a free answer. DOS/4GW's stub asks here and
         * then opens `<cwd>\DOS4GW.EXE` as an ABSOLUTE path; told the root it
         * opened `\DOS4GW.EXE`, got file-not-found for a file that is in \DF,
         * printed three lines and exited with errorlevel 1.
         */
        uint32_t lin = cpu_state_ptr->segs[3].base + (uint16_t)r[6];
        const char *c = x86_dos_get_cwd();
        char buf[64];
        unsigned i = 0;
        while (c[i] && i < sizeof(buf) - 1) {
            buf[i] = (c[i] == '/') ? '\\' : c[i];
            i++;
        }
        buf[i] = 0;
        x86_guest_copy_in(lin, buf, i + 1);
        x86_set_ax(0x0100);
        ok_clear_cf();
        return 1;
    }

    case 0x42: {                                /* seek */
        /* A character device has no file pointer, and DOS says so by
           SUCCEEDING with DX:AX = 0 rather than by failing. 3Eh/3Fh/40h/44h
           all consult dos_handle_device(); 42h was the one that did not, so a
           seek on handle 0/1/2 came back "invalid handle" -- and a program
           that treats that as retryable sits in the retry forever. WOLF3D
           issued 81 of them in a row before walking off into 0xFF filler. */
        if (dos_handle_device(bx)) {
            x86_set_ax(0);
            x86_set_dx(0);
            ok_clear_cf();
            return 1;
        }
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
