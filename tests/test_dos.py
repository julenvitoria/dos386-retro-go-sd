#!/usr/bin/env python3
"""
Tier-1 check of the DOS layer: the memory arena, and FILE I/O from guest code.

Part two is the read path a DOS program uses to reach its own data files, and
it exists because TOPBENCH could not read DATABASE.INI on the device. What the
INT 21h file log actually showed was 3Ch/40h/3Eh (its own log file) and then
**AH=43h on DATABASE.INI answered with CF set and AX=1, "invalid function"** --
so the program gave up before ever issuing the 3Dh that would have opened it.
An unimplemented call in the middle of the sequence is indistinguishable, to
the guest, from a missing file.

The disk is built by the test (scripts/common/fat12.py) and handed to the
target as raw bytes, so the expected contents are the test's own, not a
title's. Every call is executed by GUEST code through INT 21h: CF and AX have
to be read off the IRET frame, which a host-side call to x86_dos_int21() would
never exercise.

Covered: 3Dh open, 3Fh short read at EOF (partial count, CF CLEAR) and the
zero-length read after it, 42h seek from start and from end, 3Eh close, the
not-found path (CF set, AX=02), 43h get-attributes, 4Eh/4Fh FindFirst and
FindNext through the DTA, and the two name forms a program actually sends --
a drive-letter prefix ("A:TEST.INI") and lower case.

Drives the MCB chain the way a program does -- allocate, allocate, free the
first, allocate into the hole, resize, then ask for more than exists -- and
independently walks the chain in guest memory afterwards. Walking it from the
host matters: a chain that allocates correctly but links wrongly still returns
plausible segments, and only shows up when a program enumerates memory.
"""
import argparse
import sys
import time

sys.path.insert(0, ".")
from scripts.common.runner import add_runner_args, build_and_create_runner
from scripts.common.elf import get_symbols

ARENA_FIRST_MCB = 0x0070
ARENA_END_SEG = 0x9FFF
MCB_MEMBER, MCB_LAST = 0x4D, 0x5A


def walk(backend, ram_addr):
    """Return the chain as [(seg, type, owner, size)], from guest memory."""
    out = []
    seg = ARENA_FIRST_MCB
    for _ in range(64):
        raw = bytes(backend.read_memory(ram_addr + seg * 16, 5))
        typ = raw[0]
        owner = raw[1] | (raw[2] << 8)
        size = raw[3] | (raw[4] << 8)
        out.append((seg, typ, owner, size))
        if typ == MCB_LAST:
            return out
        if typ != MCB_MEMBER:
            return out
        seg = seg + size + 1
    return out


def raw_chain(backend, ram_addr):
    """The chain as raw 16-byte headers, keyed by segment.

    walk() decodes; this photographs. Proving that a failed AH=4Ah left the
    chain alone needs the BYTES, not an interpretation of them -- the whole
    defect was a write that produced a chain still perfectly well-formed.
    """
    out = []
    seg = ARENA_FIRST_MCB
    for _ in range(64):
        hdr = bytes(backend.read_memory(ram_addr + seg * 16, 16))
        out.append((seg, hdr))
        if hdr[0] != MCB_MEMBER:
            break
        seg = seg + (hdr[3] | (hdr[4] << 8)) + 1
    return out


def stage(b, syms, n):
    """Run one arena stage on the target and wait for it."""
    b.write_uint32(syms["run_dos_flag"], n)
    for _ in range(100):
        if b.read_uint32(syms["run_dos_flag"]) == 0:
            return True
        time.sleep(0.05)
    return False


def arena_hardening(b, syms, ram):
    """The four MCB defects, each with its own observable."""
    ok = True

    def fail(msg):
        nonlocal ok
        print(f"FAIL: {msg}")
        ok = False

    for n in (2, 3):
        if n == 3:
            before = raw_chain(b, ram)
        if not stage(b, syms, n):
            fail(f"arena stage {n} never completed")
            return False
    after = raw_chain(b, ram)
    r = [b.read_uint32(syms["dos_r"] + 4 * i) for i in range(25)]
    A, B, C = r[10], r[11], r[12]
    grew, grow_largest = r[13], r[14]
    print(f"\narena hardening: A=0x{A:04x} B=0x{B:04x} (freed) C=0x{C:04x}")
    print(f"  failed grow of A to 0xFFFF: returned {grew}, "
          f"largest 0x{grow_largest:04x}")

    if A == 0 or B == 0 or C == 0:
        fail("the three-block setup did not allocate")
    if grew:
        fail("resizing A to 0xFFFF paragraphs succeeded; there is not that "
             "much memory and C is in the way")
    # DEFECT 1: the chain must be untouched, byte for byte.
    if [s for s, _ in before] != [s for s, _ in after]:
        fail(f"a failed grow changed the SHAPE of the chain: "
             f"{[hex(s) for s, _ in before]} -> {[hex(s) for s, _ in after]}")
    else:
        diff = [(s, h1, h2) for (s, h1), (_, h2) in zip(before, after)
                if h1 != h2]
        if diff:
            s, h1, h2 = diff[0]
            fail(f"a failed grow mutated the chain: MCB 0x{s:04x} was "
                 f"{h1[:5].hex()} and is now {h2[:5].hex()}. A 4Ah that "
                 f"reports failure and eats the free block anyway is a loop "
                 f"the guest can never win")
        else:
            print(f"  chain byte-identical across the failed grow "
                  f"({len(before)} blocks)")
    # DOS reports what the block COULD grow to: A + header + B.
    if grow_largest != 0x201:
        fail(f"a failed 4Ah reported largest 0x{grow_largest:04x}, expected "
             f"0x0201 (A 0x100 + 1 header + free B 0x100)")

    # DEFECT: a failed 48h must name the true largest free block.
    if not stage(b, syms, 4):
        fail("arena stage 4 never completed")
        return False
    r = [b.read_uint32(syms["dos_r"] + 4 * i) for i in range(25)]
    chain = walk(b, ram)
    true_largest = max([sz for _, _, own, sz in chain if own == 0] or [0])
    print(f"  failed 48h: returned 0x{r[15]:04x}, largest 0x{r[16]:04x} "
          f"(host walk says 0x{true_largest:04x})")
    if r[15] != 0:
        fail("allocating 0xFFFF paragraphs succeeded")
    if r[16] != true_largest:
        fail(f"a failed AH=48h reported BX=0x{r[16]:04x} but the largest free "
             f"block in the chain is 0x{true_largest:04x}. A client that "
             f"retries with BX retries with a lie")

    # DEFECT: adjacent free blocks must merge.
    if not stage(b, syms, 5):
        fail("arena stage 5 never completed")
        return False
    r = [b.read_uint32(syms["dos_r"] + 4 * i) for i in range(25)]
    print(f"  after freeing C, alloc(0x201) -> 0x{r[18]:04x} "
          f"(B was 0x{B:04x})")
    if not r[17]:
        fail("freeing C failed")
    if not r[19]:
        fail(f"a 0x201-paragraph request landed at 0x{r[18]:04x}, not at B "
             f"(0x{B:04x}); B and C are adjacent and free, so it only misses "
             f"them if they were never coalesced")

    # DEFECT 2: owner 0 must never survive into the chain.
    if not stage(b, syms, 6):
        fail("arena stage 6 never completed")
        return False
    r = [b.read_uint32(syms["dos_r"] + 4 * i) for i in range(25)]
    print(f"  alloc with owner=0 -> 0x{r[20]:04x}, recorded owner "
          f"0x{r[21]:04x}; next alloc -> 0x{r[22]:04x}")
    if r[20] == 0:
        fail("the owner-0 allocation failed outright")
    if r[21] == 0:
        fail("a block allocated with owner 0 was recorded with owner 0, which "
             "IS the free-list encoding -- it is live memory sitting on the "
             "free list")
    if not r[23]:
        fail(f"the next allocation returned 0x{r[22]:04x}, the SAME memory as "
             f"the live block at 0x{r[20]:04x}")
    if r[24] == 0:
        fail("the chain is malformed after the owner-0 allocation")

    # The DOS/4GW loop, and what it costs. Not a timing measurement: MCB header
    # reads, each one an x86_guest_ptr() call, which is where the emulator was
    # measured to be spending its time.
    if not stage(b, syms, 7):
        fail("arena stage 7 never completed")
        return False
    r = [b.read_uint32(syms["dos_r"] + 4 * i) for i in range(30)]
    reads, blocks0, blocks1, steady, answer = r[25], r[26], r[27], r[28], r[29]
    print(f"\n  256 x (4Ah 0xFFFF probe + 48h 0xFFFF): {reads} MCB header "
          f"reads, {reads / 512.0:.1f} per call")
    print(f"  chain {blocks0} blocks before, {blocks1} after; answer "
          f"0x{answer:04x}, steady across all 256: {steady}")
    if blocks1 != blocks0:
        fail(f"256 FAILED probes moved the chain from {blocks0} blocks to "
             f"{blocks1}. A loop that fails must be a fixed point, or the "
             f"arena is eaten by the probing itself -- this is the Dark "
             f"Forces pathology")
    if not steady:
        fail("the probe loop returned a different largest size on a later "
             "iteration despite every call failing; nothing changed, so "
             "nothing about the answer may change either")
    if reads > 512 * 16:
        fail(f"{reads / 512.0:.1f} header reads per INT 21h memory call on a "
             f"{blocks0}-block chain is not a bounded walk")
    return ok


CF = 1

TEST_INI = bytes(((i * 7 + 11) & 0xFF) for i in range(700))
DATA_INI = b"[data]\r\nkey=value\r\n"
OTHER_TXT = b"not an ini\r\n"

DOSF_RES_NAMES = (
    "open TEST.INI AX", "open FLAGS",
    "read 0x400 AX", "read FLAGS",
    "read at EOF AX", "read at EOF FLAGS",
    "seek 0x100 AX", "seek 0x100 DX",
    "read after seek AX",
    "seek end AX", "seek end DX",
    "close FLAGS",
    "open missing AX", "open missing FLAGS",
    "attr CX", "attr FLAGS",
    "findfirst AX", "findfirst FLAGS",
    "findnext AX", "findnext FLAGS",
    "open A:TEST.INI AX", "open A:TEST.INI FLAGS",
    "open test.ini AX", "open test.ini FLAGS",
    "findfirst split-DTA AX", "findfirst split-DTA FLAGS",
    "findnext split-DTA AX", "findnext split-DTA FLAGS",
    "seek device AX", "seek device DX", "seek device FLAGS",
)


def file_test(b, syms):
    """Guest code opens and reads a file on a FAT12 disk the host just built."""
    from scripts.common.fat12 import build_image

    for n in ("run_dosfile_flag", "dosfile_r", "dosfile_img", "test_ram"):
        if n not in syms:
            print(f"missing symbol {n} -- rebuild tests/main.c")
            return False
    ram = syms["test_ram"]

    img = build_image([("TEST.INI", TEST_INI),
                       ("DATA.INI", DATA_INI),
                       ("OTHER.TXT", OTHER_TXT)])
    b.write_memory(syms["dosfile_img"], img)

    b.write_uint32(syms["run_dosfile_flag"], 1)
    for _ in range(400):
        if b.read_uint32(syms["run_dosfile_flag"]) == 0:
            break
        time.sleep(0.05)
    else:
        print("FAIL: the file arm never completed")
        return False

    r = [b.read_uint32(syms["dosfile_r"] + 4 * i) for i in range(38)]
    mount, exit_reason = r[36], r[37]
    print(f"\nf_mount on the test image  {mount} (0 = FR_OK)")
    print(f"guest exit reason          {exit_reason} (1 = HLT/unimplemented)")
    for i, name in enumerate(DOSF_RES_NAMES):
        print(f"  {name:<26} 0x{r[i]:04x}")

    ok = True

    def fail(msg):
        nonlocal ok
        print("FAIL: " + msg)
        ok = False

    if mount != 0:
        fail(f"our FatFs could not mount the test image (FRESULT {mount})")
        return False

    if r[1] & CF:
        fail(f"AH=3Dh on TEST.INI set CF and returned AX=0x{r[0]:04x} "
             f"(DOS error {r[0]}) -- the file is there")
    elif r[0] < 5:
        fail(f"AH=3Dh returned handle {r[0]}; a real file handle is >= 5")

    if r[3] & CF:
        fail("AH=3Fh set CF on a short read at EOF. A partial read is NOT an "
             "error: DOS returns the count with CF clear")
    if r[2] != len(TEST_INI):
        fail(f"AH=3Fh returned {r[2]} bytes for a {len(TEST_INI)}-byte file "
             f"asked for 0x400")
    else:
        got = bytes(b.read_memory(ram + 0x5000, len(TEST_INI)))
        if got != TEST_INI:
            first = next(i for i in range(len(TEST_INI))
                         if got[i] != TEST_INI[i])
            fail(f"the bytes read differ from the file at offset {first}: "
                 f"0x{got[first]:02x} != 0x{TEST_INI[first]:02x}")

    if r[4] != 0 or (r[5] & CF):
        fail(f"a read AT end-of-file returned AX={r[4]} CF={r[5] & CF}; "
             f"expected 0 bytes with CF clear")

    if (r[6], r[7]) != (0x0100, 0):
        fail(f"AH=42h AL=00 to 0x100 reported DX:AX = "
             f"0x{r[7]:04x}:0x{r[6]:04x}")
    if r[8] != 16:
        fail(f"the 16-byte read after the seek returned {r[8]}")
    else:
        got = bytes(b.read_memory(ram + 0x6100, 16))
        if got != TEST_INI[0x100:0x110]:
            fail(f"the seek did not move the file pointer: got {got.hex()} "
                 f"expected {TEST_INI[0x100:0x110].hex()}")
    size = (r[10] << 16) | r[9]
    if size != len(TEST_INI):
        fail(f"AH=42h AL=02 (seek to end) reported a size of {size}, "
             f"expected {len(TEST_INI)}")
    if r[11] & CF:
        fail("AH=3Eh close set CF on a handle it had just handed out")

    if not (r[13] & CF) or r[12] != 0x02:
        fail(f"opening a file that does not exist returned AX=0x{r[12]:04x} "
             f"CF={r[13] & CF}; DOS returns CF set with AX=02 "
             f"(file not found)")

    if r[15] & CF:
        fail(f"AH=43h AL=00 (get attributes) failed with AX=0x{r[14]:04x}. "
             f"This is the call TOPBENCH stopped on")

    if r[17] & CF:
        fail("AH=4Eh FindFirst on '*.INI' found nothing")
    else:
        n1 = bytes(b.read_memory(ram + 0x4900, 13)).split(b"\0")[0]
        n2 = bytes(b.read_memory(ram + 0x4910, 13)).split(b"\0")[0]
        print(f"  {'findfirst name':<26} {n1!r}")
        print(f"  {'findnext name':<26} {n2!r}")
        if n1 != b"TEST.INI":
            fail(f"FindFirst put {n1!r} in the DTA, expected b'TEST.INI'")
        if r[19] & CF:
            fail("AH=4Fh FindNext ended the search, but DATA.INI also "
                 "matches '*.INI'")
        elif n2 != b"DATA.INI":
            fail(f"FindNext put {n2!r} in the DTA, expected b'DATA.INI' "
                 f"(OTHER.TXT must not match)")

    if (r[21] & CF) or r[20] < 5:
        fail(f"opening 'A:TEST.INI' returned AX=0x{r[20]:04x} CF={r[21] & CF}."
             f" A drive letter is not a FatFs volume id; it has to be "
             f"stripped, or every program that names its own drive fails")
    # A DTA that straddles a 4 KB granule. x86_guest_ptr() refuses a span that
    # crosses one, and the refusal used to surface as "no more files" -- an
    # empty disk, for a program whose DTA merely sits near the end of a page.
    # WOLF3D's is at DS:0xFFBA, so this is the call that stopped it dead.
    if r[25] & CF:
        fail("AH=4Eh FindFirst found nothing with the DTA straddling a 4 KB "
             "boundary; the same search over an aligned DTA works, so this is "
             "the granule refusal in x86_guest_ptr(), not the matcher")
    else:
        n3 = bytes(b.read_memory(ram + 0x4920, 13)).split(b"\0")[0]
        n4 = bytes(b.read_memory(ram + 0x4930, 13)).split(b"\0")[0]
        print(f"  {'split-DTA findfirst name':<26} {n3!r}")
        print(f"  {'split-DTA findnext name':<26} {n4!r}")
        if n3 != b"TEST.INI":
            fail(f"split-DTA FindFirst wrote {n3!r}, expected b'TEST.INI' -- "
                 f"a partial record is worse than none")
        if r[27] & CF:
            fail("split-DTA FindNext ended the search early")
        elif n4 != b"DATA.INI":
            fail(f"split-DTA FindNext wrote {n4!r}, expected b'DATA.INI'")

    if (r[30] & CF) or (r[28], r[29]) != (0, 0):
        fail(f"AH=42h on handle 0 (a character device) returned "
             f"DX:AX = 0x{r[29]:04x}:0x{r[28]:04x} CF={r[30] & CF}; DOS "
             f"succeeds with 0:0. Failing it makes a retry loop immortal")

    if (r[23] & CF) or r[22] < 5:
        fail(f"opening 'test.ini' in lower case returned AX=0x{r[22]:04x} "
             f"CF={r[23] & CF}; FAT names are case-insensitive")

    return ok


# ---------------------------------------------------------------------------
# The MZ LOAD test.
#
# WHY IT EXISTS. A DOS-layer change was merged, and reverted, that loaded
# WOLF3D.EXE as garbage -- the game booted to a screen of one repeating byte
# pattern tiled across all 25 rows. Every tier-1 test passed before AND after
# it, because nothing here ran a real MZ image through the PSP/environment
# path. The non-regression argument offered at the time was that the guest
# executed 64 million instructions "and is still running", which a guest
# printing rubbish does very happily.
#
# So this asserts the only thing that actually matters about a program load:
# THE IMAGE IS INTACT WHERE THE PROGRAM IS ABOUT TO EXECUTE. The host built
# the .EXE, so the host knows every byte that must appear at load_seg, and it
# compares them itself rather than trusting a checksum from the code under
# test. It also checks the PSP the program is about to read describes the block
# it was actually given, and that the entry state matches the header.
#
# The body is a pseudo-random pattern rather than a constant: a load that
# duplicates, drops or shifts a chunk still matches a constant fill.
# ---------------------------------------------------------------------------
EXE_BODY = bytes(((i * 37 + i // 251 + 5) & 0xFF) for i in range(9000))
EXE_HDR_PARAS = 2
EXE_CS, EXE_IP = 0x0010, 0x0023
EXE_SS, EXE_SP = 0x0180, 0x0100
EXE_MINALLOC, EXE_MAXALLOC = 0x0200, 0xFFFF
EXEC_CMDTAIL = b"-x"


def build_exe():
    """A real MZ .EXE: header, no relocations, a known body."""
    import struct
    hdr_bytes = EXE_HDR_PARAS * 16
    total = hdr_bytes + len(EXE_BODY)
    pages = (total + 511) // 512
    last = total % 512
    h = struct.pack("<14H", 0x5A4D, last, pages, 0, EXE_HDR_PARAS,
                    EXE_MINALLOC, EXE_MAXALLOC, EXE_SS, EXE_SP, 0,
                    EXE_IP, EXE_CS, 0x001C, 0)
    return h + bytes(hdr_bytes - len(h)) + EXE_BODY


def check_image(b, ram, load_seg, expect, label):
    """Compare the loaded image against the file. Returns (ok, message)."""
    got = bytes(b.read_memory(ram + load_seg * 16, len(expect)))
    if got == expect:
        return True, f"{label}: image intact, {len(expect)} bytes"
    first = next(i for i in range(len(expect)) if got[i] != expect[i])
    nbad = sum(1 for i in range(len(expect)) if got[i] != expect[i])
    return False, (f"{label}: image DIFFERS -- {nbad} of {len(expect)} bytes, "
                   f"first at offset 0x{first:04x} "
                   f"(expected 0x{expect[first]:02x}, got 0x{got[first]:02x})")


def exec_test(b, syms):
    from scripts.common.fat12 import build_image

    for n in ("run_dosexec_flag", "dosexec_r", "dosfile_img", "test_ram"):
        if n not in syms:
            print(f"missing symbol {n}")
            return False
    ram = syms["test_ram"]
    exe = build_exe()
    b.write_memory(syms["dosfile_img"],
                   build_image([("LOAD.EXE", exe)]))

    b.write_uint32(syms["run_dosexec_flag"], 1)
    for _ in range(200):
        if b.read_uint32(syms["run_dosexec_flag"]) == 0:
            break
        time.sleep(0.05)
    else:
        print("FAIL: the MZ load test never completed")
        return False

    r = [b.read_uint32(syms["dosexec_r"] + 4 * i) for i in range(16)]
    (mount, rc, cs, ip, ss, sp, ds, es, psp, nblocks, csbase) = r[:11]
    load_seg = (psp + 16) & 0xFFFF

    print("\nMZ load:")
    print(f"  mount           {mount} (0 = FR_OK)")
    print(f"  x86_dos_exec    {rc if rc < 0x80000000 else rc - (1 << 32)}")
    print(f"  PSP             0x{psp:04x}   load_seg 0x{load_seg:04x}")
    print(f"  CS:IP           {cs:04x}:{ip:04x}  (base 0x{csbase:05x})")
    print(f"  SS:SP           {ss:04x}:{sp:04x}")
    print(f"  DS / ES         {ds:04x} / {es:04x}")
    print(f"  blocks in chain {nblocks}")

    ok = True
    if mount != 0 or rc != 0:
        print("FAIL: the .EXE did not load at all")
        return False
    if psp == 0:
        print("FAIL: no PSP segment")
        return False

    # THE CHECK THIS TEST EXISTS FOR.
    good, msg = check_image(b, ram, load_seg, EXE_BODY, "positive")
    print(f"  {'PASS' if good else 'FAIL'}: {msg}")
    if not good:
        ok = False

    # The entry state has to name the header's entry point, or an intact image
    # is being entered in the wrong place -- which looks identical from a
    # counter and produces exactly the same "runs, draws rubbish" screen.
    if cs != ((load_seg + EXE_CS) & 0xFFFF) or ip != EXE_IP:
        print(f"FAIL: entry is {cs:04x}:{ip:04x}, expected "
              f"{(load_seg + EXE_CS) & 0xFFFF:04x}:{EXE_IP:04x}")
        ok = False
    if ss != ((load_seg + EXE_SS) & 0xFFFF) or sp != EXE_SP:
        print(f"FAIL: stack is {ss:04x}:{sp:04x}, expected "
              f"{(load_seg + EXE_SS) & 0xFFFF:04x}:{EXE_SP:04x}")
        ok = False
    if ds != psp or es != psp:
        print(f"FAIL: DS/ES are {ds:04x}/{es:04x}, expected the PSP {psp:04x}")
        ok = False
    if csbase != cs * 16:
        print(f"FAIL: CS base 0x{csbase:05x} does not match selector {cs:04x}")
        ok = False

    # The PSP itself, read out of guest memory.
    p = bytes(b.read_memory(ram + psp * 16, 256))
    mem_top = p[2] | (p[3] << 8)
    env = p[0x2C] | (p[0x2D] << 8)
    tail_len = p[0x80]
    tail = p[0x81:0x81 + tail_len]
    print(f"  PSP: INT 20h {p[0]:02x}{p[1]:02x}  mem_top 0x{mem_top:04x}  "
          f"env 0x{env:04x}  tail {tail_len} {tail!r}")
    if p[0] != 0xCD or p[1] != 0x20:
        print("FAIL: PSP does not start with INT 20h")
        ok = False
    if mem_top <= load_seg:
        print(f"FAIL: PSP mem_top 0x{mem_top:04x} is not above the image")
        ok = False
    if tail != EXEC_CMDTAIL:
        print(f"FAIL: command tail is {tail!r}, expected {EXEC_CMDTAIL!r}")
        ok = False
    # An environment segment, if one is handed out, must not overlap the
    # program. This is the exact shape of the reverted change's failure.
    if env:
        if load_seg <= env < mem_top or env == psp:
            print(f"FAIL: environment 0x{env:04x} overlaps the program "
                  f"(PSP 0x{psp:04x}, load 0x{load_seg:04x}, "
                  f"top 0x{mem_top:04x})")
            ok = False

    # The whole program, PSP included, must lie inside conventional memory and
    # above the HLE stubs.
    if psp * 16 < ARENA_FIRST_MCB * 16 or mem_top > ARENA_END_SEG:
        print(f"FAIL: the program does not lie inside the arena")
        ok = False

    # NEGATIVE CONTROL. Poke ONE byte of the loaded image and re-run the same
    # comparison: if it still passes, the check above is decorative and the
    # next corrupted load ships too. One byte is the hardest case on purpose.
    poke = len(EXE_BODY) // 3
    orig = bytes(b.read_memory(ram + load_seg * 16 + poke, 1))
    b.write_memory(ram + load_seg * 16 + poke, bytes([orig[0] ^ 0xFF]))
    bad, msg = check_image(b, ram, load_seg, EXE_BODY, "negative control")
    b.write_memory(ram + load_seg * 16 + poke, orig)
    if bad:
        print("FAIL: negative control -- a corrupted image still compared "
              "equal, so the image check proves nothing")
        ok = False
    else:
        print(f"  PASS: {msg}")

    return ok


# ---------------------------------------------------------------------------
# The EXEC test -- INT 21h AH=4Bh.
#
# WHY IT EXISTS. Dark Forces died with x86_hle_unhandled_func = 0x4B: DARK.EXE's
# stub EXECs DOS4GW.EXE, got "invalid function", printed its own generic "Bad
# format on exec" and derailed into the interrupt vector table. AH=4Bh did not
# exist in this tree at all.
#
# What the MZ LOAD test above cannot say anything about is RE-ENTRANCY. A child
# that loads perfectly and never gives the machine back looks, from the host,
# exactly like one that works -- the load counters are identical and the guest
# is still executing instructions. So both halves run as REAL GUEST PROGRAMS
# and every claim below is a byte one of them wrote:
#
#   PARENT.EXE  marks 0x1111, fills in the runtime segments of an EXEC
#               parameter block, EXECs CHILD.EXE, and -- only if it is resumed
#               -- records the FLAGS and AX it came back with, its restored DS
#               and SP, and the child's return code from AH=4Dh.
#   CHILD.EXE   marks 0x2222 and reports its own PSP, the PARENT PSP LINK at
#               PSP:0016, its environment segment, the word its RELOCATION
#               entry should have fixed up, its command tail and its CS.
#               Exits AH=4Ch with AL=0x5A.
#
# The child's reported reloc word and parent link are what the two negative
# controls at the bottom of this file mutate.
# ---------------------------------------------------------------------------
EXEC2_MARK = 0x0500              # DOS scratch, below the arena at 0x700
EXEC2_TAIL = b" -k"
EXEC2_RETCODE = 0x5A
EXEC2_FCB1 = bytes([0x00]) + b"CHILD   EXE" + bytes(4)
EXEC2_FCB2 = bytes([0x02]) + b"SECOND  FCB" + bytes(4)
# Where each program's data lives inside its own image, and therefore what the
# hand-assembled displacements below mean.
E2_PBLK, E2_FNAME, E2_TAIL, E2_FCB1, E2_FCB2 = 0x200, 0x210, 0x220, 0x230, 0x240
E2_RELOC = 0x200                 # the child's relocated word
E2_IMAGE = 0x500                 # parent image length; its SS:SP is 0x40:0x100
E2_SS, E2_SP = 0x0040, 0x0100
# The CHILD's stack and BP are deliberately NOTHING like the parent's. With the
# same SS:SP in both, a core that forgets to restore the parent's registers
# still lands on a correct-looking frame and the 'resume' negative control
# passes with the defect present -- which is how this test first shipped.
E2_CHILD_SS, E2_CHILD_SP, E2_CHILD_IMAGE = 0x0060, 0x0080, 0x0700
E2_PARENT_BP, E2_CHILD_BP = 0xBEEF, 0x1234


def _mz(body, relocs=(), ss=E2_SS, sp=E2_SP):
    """A real MZ .EXE. hdr_paras is 2, so 28 bytes of header leave exactly one
    4-byte relocation entry at 0x1C -- which is all either program needs."""
    import struct
    assert len(relocs) <= 1
    hdr_paras = 2
    total = hdr_paras * 16 + len(body)
    pages = (total + 511) // 512
    h = struct.pack("<14H", 0x5A4D, total % 512, pages, len(relocs), hdr_paras,
                    0x0060, 0x0080, ss, sp, 0, 0x0000, 0x0000, 0x001C, 0)
    for off in relocs:
        h += struct.pack("<2H", off, 0)
    return h + bytes(hdr_paras * 16 - len(h)) + body


def build_parent_exe():
    """DS=ES=PSP on entry; CS=DS=the image after the first two instructions."""
    code = bytes([
        0x8C, 0xC8,                          # mov ax,cs
        0x8E, 0xD8,                          # mov ds,ax
        # The EXEC parameter block's three far pointers name segments only the
        # running program knows, so it fills them in itself.
        0xA3, (E2_PBLK + 4) & 0xFF, E2_PBLK >> 8,             # mov [pblk+4],ax   tail seg
        0xA3, (E2_PBLK + 8) & 0xFF, E2_PBLK >> 8,             # mov [pblk+8],ax   fcb1 seg
        0xA3, (E2_PBLK + 12) & 0xFF, E2_PBLK >> 8,            # mov [pblk+12],ax  fcb2 seg
        0xB8, 0x50, 0x00,                    # mov ax,0050
        0x8E, 0xC0,                          # mov es,ax
        0x26, 0xC7, 0x06, 0x00, 0x00, 0x11, 0x11,   # mov es:[0000],1111
        0x8C, 0xC8,                          # mov ax,cs
        0x8E, 0xC0,                          # mov es,ax
        0xBA, E2_FNAME & 0xFF, E2_FNAME >> 8,                # mov dx,fname    DS:DX = path
        0xBB, E2_PBLK & 0xFF, E2_PBLK >> 8,                 # mov bx,pblk     ES:BX = block
        0xBD, E2_PARENT_BP & 0xFF, E2_PARENT_BP >> 8,   # mov bp,BEEF
        0xB8, 0x00, 0x4B,                    # mov ax,4B00
        0xCD, 0x21,                          # int 21h
        # Everything from here on only executes if the EXEC came back.
        0x9C, 0x59,                          # pushf ; pop cx   FLAGS
        0x89, 0xC6,                          # mov si,ax
        0x8C, 0xDA,                          # mov dx,ds        restored DS
        0x89, 0xE3,                          # mov bx,sp        restored SP
        0xB8, 0x50, 0x00,                    # mov ax,0050
        0x8E, 0xC0,                          # mov es,ax
        0x26, 0x89, 0x0E, 0x0A, 0x00,        # mov es:[000A],cx
        0x26, 0x89, 0x36, 0x0C, 0x00,        # mov es:[000C],si
        0x26, 0x89, 0x16, 0x10, 0x00,        # mov es:[0010],dx
        0x26, 0x89, 0x1E, 0x12, 0x00,        # mov es:[0012],bx
        0x26, 0x89, 0x2E, 0x16, 0x00,        # mov es:[0016],bp  restored BP
        0xB4, 0x4D,                          # mov ah,4D        child's code
        0xCD, 0x21,                          # int 21h
        0x26, 0xA3, 0x0E, 0x00,              # mov es:[000E],ax
        0xB8, 0x00, 0x4C,                    # mov ax,4C00
        0xCD, 0x21,                          # int 21h
    ])
    img = bytearray(E2_IMAGE)
    img[:len(code)] = code
    import struct
    img[E2_PBLK:E2_PBLK + 14] = struct.pack(
        "<7H", 0, E2_TAIL, 0, E2_FCB1, 0, E2_FCB2, 0)
    img[E2_FNAME:E2_FNAME + 10] = b"CHILD.EXE\0"
    img[E2_TAIL] = len(EXEC2_TAIL)
    img[E2_TAIL + 1:E2_TAIL + 1 + len(EXEC2_TAIL)] = EXEC2_TAIL
    img[E2_TAIL + 1 + len(EXEC2_TAIL)] = 0x0D
    img[E2_FCB1:E2_FCB1 + 16] = EXEC2_FCB1
    img[E2_FCB2:E2_FCB2 + 16] = EXEC2_FCB2
    return _mz(bytes(img))


def build_child_exe():
    code = bytes([
        0xBD, E2_CHILD_BP & 0xFF, E2_CHILD_BP >> 8,     # mov bp,1234
        0xB8, 0x50, 0x00,                    # mov ax,0050
        0x8E, 0xC0,                          # mov es,ax
        0x26, 0xC7, 0x06, 0x02, 0x00, 0x22, 0x22,   # mov es:[0002],2222
        0x8C, 0xD8,                          # mov ax,ds        = our PSP
        0x26, 0xA3, 0x04, 0x00,              # mov es:[0004],ax
        0xA1, 0x16, 0x00,                    # mov ax,[0016]    parent link
        0x26, 0xA3, 0x06, 0x00,              # mov es:[0006],ax
        0xA1, 0x2C, 0x00,                    # mov ax,[002C]    environment
        0x26, 0xA3, 0x08, 0x00,              # mov es:[0008],ax
        0x2E, 0xA1, E2_RELOC & 0xFF, E2_RELOC >> 8,          # mov ax,cs:[reloc]
        0x26, 0xA3, 0x14, 0x00,              # mov es:[0014],ax
        0xA1, 0x80, 0x00,                    # mov ax,[0080]    tail len+char
        0x26, 0xA3, 0x1A, 0x00,              # mov es:[001A],ax
        0x8C, 0xC8,                          # mov ax,cs
        0x26, 0xA3, 0x1C, 0x00,              # mov es:[001C],ax
        0xB8, EXEC2_RETCODE, 0x4C,           # mov ax,4C5A
        0xCD, 0x21,                          # int 21h
    ])
    img = bytearray(E2_CHILD_IMAGE)
    img[:len(code)] = code
    # The relocated word starts at ZERO. The loader must add load_seg to it, so
    # the child reading it back is reading the relocation itself.
    img[E2_RELOC:E2_RELOC + 2] = b"\0\0"
    return _mz(bytes(img), relocs=(E2_RELOC,),
               ss=E2_CHILD_SS, sp=E2_CHILD_SP)


def exec2_test(b, syms):
    from scripts.common.fat12 import build_image

    for n in ("run_dosexec2_flag", "dosexec2_r", "dosfile_img", "test_ram"):
        if n not in syms:
            print(f"missing symbol {n} -- rebuild tests/main.c")
            return False
    ram = syms["test_ram"]
    parent, child = build_parent_exe(), build_child_exe()
    b.write_memory(syms["dosfile_img"],
                   build_image([("PARENT.EXE", parent),
                                ("CHILD.EXE", child)]))

    b.write_uint32(syms["run_dosexec2_flag"], 1)
    for _ in range(400):
        if b.read_uint32(syms["run_dosexec2_flag"]) == 0:
            break
        time.sleep(0.05)
    else:
        print("FAIL: the EXEC test never completed")
        return False

    r = [b.read_uint32(syms["dosexec2_r"] + 4 * i) for i in range(16)]
    (mount, rc, ppsp, exit_reason, execs, fails, last_err,
     depth_max, nblocks, psp_after, largest_after) = r[:11]

    m = bytes(b.read_memory(ram + EXEC2_MARK, 32))
    w = lambda o: m[o] | (m[o + 1] << 8)
    parent_mark, child_mark = w(0x00), w(0x02)
    child_psp, child_parent, child_env = w(0x04), w(0x06), w(0x08)
    back_flags, back_ax, child_code = w(0x0A), w(0x0C), w(0x0E)
    back_ds, back_sp = w(0x10), w(0x12)
    child_reloc, child_tail, child_cs = w(0x14), w(0x1A), w(0x1C)
    back_bp = w(0x16)

    print("\nEXEC (AH=4Bh):")
    print(f"  mount {mount}   x86_dos_exec(PARENT.EXE) {rc}   "
          f"parent PSP 0x{ppsp:04x}")
    print(f"  exit_reason {exit_reason} (1 = the PARENT terminated)")
    print(f"  execs {execs}  fails {fails}  last_err 0x{last_err:04x}  "
          f"depth_max {depth_max}")
    print(f"  parent mark 0x{parent_mark:04x}   child mark 0x{child_mark:04x}")
    print(f"  child PSP 0x{child_psp:04x}  parent link 0x{child_parent:04x}  "
          f"env 0x{child_env:04x}  CS 0x{child_cs:04x}")
    print(f"  child reloc word 0x{child_reloc:04x}  "
          f"tail len {child_tail & 0xFF} first {child_tail >> 8:#04x}")
    print(f"  back: FLAGS 0x{back_flags:04x} AX 0x{back_ax:04x} "
          f"DS 0x{back_ds:04x} SP 0x{back_sp:04x}  "
          f"BP 0x{back_bp:04x}  AH=4Dh 0x{child_code:04x}")
    print(f"  PSP after 0x{psp_after:04x}  chain {nblocks} blocks  "
          f"largest free after 0x{largest_after:04x}")

    # The service conversation, in order. When an EXEC goes wrong the question
    # is always "what did the guest ask for next", and one counter cannot say.
    if "x86_hle_log" in syms and "x86_hle_log_idx" in syms:
        idx = b.read_uint32(syms["x86_hle_log_idx"])
        raw = [b.read_uint32(syms["x86_hle_log"] + 4 * i) for i in range(32)]
        seq = [raw[i & 31] for i in range(max(0, idx - 32), idx)]
        print("  services: " + " ".join(
            f"{(v >> 8) & 0xFF:02X}/{v & 0xFF:02X}{'!' if v & 0x10000 else ''}"
            for v in seq))

    ok = True

    def fail(msg):
        nonlocal ok
        print("FAIL: " + msg)
        ok = False

    if mount != 0 or rc != 0:
        print("FAIL: PARENT.EXE did not load at all")
        return False
    if parent_mark != 0x1111:
        fail("the parent never ran; nothing below means anything")
        return False
    if execs != 1:
        fail(f"x86_dos_execs is {execs}, expected exactly 1 "
             f"(fails {fails}, last_err 0x{last_err:04x})")
    if child_mark != 0x2222:
        fail("THE CHILD NEVER RAN. The EXEC was refused or transferred "
             "control somewhere else")
        return False

    # THE PARENT-PSP LINK. A program reads PSP:0016 to find who started it, and
    # a zero there is a pointer at the interrupt vector table.
    if child_parent != ppsp:
        fail(f"the child's parent link at PSP:0016 is 0x{child_parent:04x}, "
             f"expected the parent's PSP 0x{ppsp:04x}")
    if child_psp == 0 or child_psp == ppsp:
        fail(f"the child's PSP is 0x{child_psp:04x}; it must be its own block, "
             f"not the parent's 0x{ppsp:04x}")

    # THE RELOCATION. The word shipped as zero and must now name the segment
    # the child was actually loaded at -- which the child also reports as CS.
    expect_reloc = (child_psp + 0x10) & 0xFFFF
    if child_reloc != expect_reloc:
        fail(f"the child's relocated word is 0x{child_reloc:04x}, expected its "
             f"load segment 0x{expect_reloc:04x} -- the relocation table was "
             f"not applied")
    if child_cs != expect_reloc:
        fail(f"the child's CS is 0x{child_cs:04x}, expected 0x{expect_reloc:04x}")

    # THE RETURN. Everything from here proves the parent got its machine back.
    if back_flags == 0 and back_ax == 0 and back_ds == 0:
        fail("THE PARENT NEVER RESUMED -- it wrote nothing after its INT 21h")
        return False
    if back_flags & CF:
        fail(f"the parent resumed with CF SET (FLAGS 0x{back_flags:04x}); a "
             f"successful EXEC returns CF clear")
    if back_ds != ppsp + 0x10:
        fail(f"the parent's DS came back as 0x{back_ds:04x}, expected its own "
             f"code segment 0x{ppsp + 0x10:04x} -- registers were not restored")
    if back_sp != E2_SP:
        fail(f"the parent's SP came back as 0x{back_sp:04x}, expected "
             f"0x{E2_SP:04x} -- its own stack, with its own INT 21h frame "
             f"popped by the stub's IRET and nothing else left behind")
    if back_bp != E2_PARENT_BP:
        fail(f"the parent's BP came back as 0x{back_bp:04x}, expected "
             f"0x{E2_PARENT_BP:04x} -- the child left 0x{E2_CHILD_BP:04x} in "
             f"it, so the general registers were not restored")
    if child_code != EXEC2_RETCODE:
        fail(f"AH=4Dh reported 0x{child_code:04x}, expected 0x{EXEC2_RETCODE:04x} "
             f"(exit type 0, the code the child passed to AH=4Ch)")
    if exit_reason != 1:
        fail(f"the run stopped with exit_reason {exit_reason}, expected 1 -- "
             f"the PARENT's AH=4Ch is the one that stops the machine")
    if psp_after != ppsp:
        fail(f"the current PSP is 0x{psp_after:04x} after the child exited, "
             f"expected the parent's 0x{ppsp:04x}")

    # The command tail and the FCBs, read out of the child's own PSP.
    p = bytes(b.read_memory(ram + child_psp * 16, 256))
    tail = p[0x81:0x81 + p[0x80]]
    if p[0x80] != len(EXEC2_TAIL) or tail != EXEC2_TAIL:
        fail(f"the child's command tail is {tail!r} (len {p[0x80]}), expected "
             f"{EXEC2_TAIL!r}")
    if (child_tail & 0xFF) != len(EXEC2_TAIL):
        fail(f"the child read tail length {child_tail & 0xFF} from its own PSP")
    if p[0x5C:0x5C + 16] != EXEC2_FCB1:
        fail(f"the child's first FCB is {p[0x5C:0x5C + 16]!r}")
    if p[0x6C:0x6C + 16] != EXEC2_FCB2:
        fail(f"the child's second FCB is {p[0x6C:0x6C + 16]!r}")

    # The environment. We built one because the parent had none to inherit; it
    # must exist, be owned separately and not overlap the child's image.
    if child_env == 0:
        fail("the child has no environment segment")
    else:
        top = p[2] | (p[3] << 8)
        if child_psp <= child_env < top:
            fail(f"the environment 0x{child_env:04x} is inside the child's own "
                 f"block (0x{child_psp:04x}..0x{top:04x})")

    # The child's memory must be BACK. A leaked block is invisible until the
    # second EXEC, which is a place no test would ever look.
    if largest_after < 0x60:
        fail(f"only 0x{largest_after:04x} paragraphs are free after the child "
             f"exited -- its block was not returned to the arena")
    if nblocks == 0:
        fail("the MCB chain is malformed after the EXEC")

    return ok


def main(args):
    runner = build_and_create_runner(args)
    try:
        runner.start()
        b = runner.backend
        syms = get_symbols("test_core.elf")
        for n in ("run_dos_flag", "dos_r", "test_ram"):
            if n not in syms:
                print(f"missing symbol {n}")
                return False
        ram = syms["test_ram"]
        time.sleep(0.5)

        b.write_uint32(syms["run_dos_flag"], 1)
        for _ in range(100):
            if b.read_uint32(syms["run_dos_flag"]) == 0:
                break
            time.sleep(0.05)
        else:
            print("FAIL: arena test never completed")
            return False

        r = [b.read_uint32(syms["dos_r"] + 4 * i) for i in range(10)]
        a, blk, spacing, freed, c, reused, resized, over, largest, nblocks = r
        print(f"alloc A          0x{a:04x}")
        print(f"alloc B          0x{blk:04x}")
        print(f"B - A            0x{spacing:04x} paragraphs")
        print(f"free A           {freed}")
        print(f"alloc C          0x{c:04x} (reused A's hole: {reused})")
        print(f"resize B         {resized}")
        print(f"over-allocate    {over} (largest reported 0x{largest:04x})")
        print(f"blocks in chain  {nblocks}")

        ok = True
        if a == 0 or blk == 0:
            print("FAIL: the first two allocations must succeed")
            ok = False
        # A 0x100-paragraph block plus its 1-paragraph header puts the next
        # block exactly 0x101 further on.
        if spacing != 0x101:
            print(f"FAIL: blocks are 0x{spacing:04x} apart, expected 0x101 "
                  f"(size + one header paragraph)")
            ok = False
        if not freed:
            print("FAIL: free rejected a block it had just handed out")
            ok = False
        if not reused:
            print("FAIL: the freed hole was not reused, so free/coalesce is "
                  "not returning memory to the chain")
            ok = False
        if not resized:
            print("FAIL: shrinking a block in place failed")
            ok = False
        if over != 0:
            print("FAIL: allocating 0xFFFF paragraphs should fail")
            ok = False
        if largest == 0 or largest == 0xFFFF:
            print(f"FAIL: a failed allocation must report a usable largest "
                  f"block, got 0x{largest:04x}")
            ok = False

        chain = walk(b, ram)
        print("\nchain from guest memory:")
        for seg, typ, owner, size in chain:
            kind = {MCB_MEMBER: "M", MCB_LAST: "Z"}.get(typ, f"?{typ:02x}")
            print(f"  seg 0x{seg:04x} {kind} owner 0x{owner:04x} "
                  f"size 0x{size:04x}")
        if not chain or chain[-1][1] != MCB_LAST:
            print("FAIL: chain does not terminate with a Z block")
            ok = False
        if len(chain) != nblocks:
            print(f"FAIL: host walked {len(chain)} blocks, target counted "
                  f"{nblocks}")
            ok = False
        # Every block must sit inside conventional memory.
        end = chain[-1][0] + chain[-1][3] + 1
        if end > ARENA_END_SEG:
            print(f"FAIL: chain runs past conventional memory to 0x{end:04x}")
            ok = False

        if not arena_hardening(b, syms, ram):
            ok = False

        # The HLE stubs and their bare-IRET default live in guest memory. DOS
        # hands out everything from ARENA_FIRST_MCB up, so any IVT entry that
        # points at or above it is aimed at memory a program will overwrite --
        # and then an interrupt executes the program's data as code.
        if "run_hle_flag" in syms and "hle_r" in syms:
            b.write_uint32(syms["run_hle_flag"], 1)
            for _ in range(100):
                if b.read_uint32(syms["run_hle_flag"]) == 0:
                    break
                time.sleep(0.05)
            else:
                print("FAIL: HLE install never completed")
                return False
            if b.read_uint32(syms["hle_r"]) != 0:
                print("FAIL: x86_hle_install reported failure")
                ok = False
            ivt = bytes(b.read_memory(ram, 1024))
            arena_lin = ARENA_FIRST_MCB * 16
            bad = []
            for v in range(256):
                off = ivt[v * 4] | (ivt[v * 4 + 1] << 8)
                seg = ivt[v * 4 + 2] | (ivt[v * 4 + 3] << 8)
                lin = seg * 16 + off
                if lin >= arena_lin:
                    bad.append((v, seg, off, lin))
            if bad:
                v, seg, off, lin = bad[0]
                print(f"FAIL: {len(bad)} IVT entries point into the DOS arena, "
                      f"e.g. INT {v:02X}h -> {seg:04x}:{off:04x} "
                      f"(linear 0x{lin:05x} >= 0x{arena_lin:05x})")
                ok = False
            else:
                print(f"\nall 256 IVT entries land below the arena "
                      f"(0x{arena_lin:05x})")

        if not file_test(b, syms):
            ok = False

        if not exec_test(b, syms):
            ok = False

        if not exec2_test(b, syms):
            ok = False

        print("\nPASS" if ok else "\nFAIL")
        return ok
    finally:
        runner.stop()


# --------------------------------------------------------------------------
# Negative controls. Each rebuilds x86_dos.c with ONE fix mutated back to the
# defective form it had before this pass, and requires the test to FAIL. A
# check that cannot fail proves nothing, and every one of these defects
# produced a chain that was still perfectly well-formed afterwards -- exactly
# the shape a green-by-construction test hides.
MUTATIONS = {
    # Commit the merge BEFORE deciding, which is what the old mcb_resize did by
    # merging and recursing: a failed grow eats the free block it was just told
    # was not enough.
    "rollback": ("failed grow commits the merge anyway", [
        ("    if (avail < paras) {",
         "    m.size = (uint16_t)avail; m.type = last_type;\n"
         "    mcb_write(seg, &m);\n"
         "    if (avail < paras) {"),
    ]),
    # Report the FIRST free block instead of the largest -- the shape of every
    # "close enough" answer to BX, and fatal to a client that retries with it.
    "largest": ("failed 48h reports a free block, not the LARGEST one", [
        ("            if (m.size > best)\n                best = m.size;",
         "            if (best == 0)\n                best = m.size;"),
    ]),
    # mcb_free() coalesces the whole chain, which would hide the loss of the
    # merge in mcb_alloc, so both go.
    "coalesce": ("adjacent free blocks are never merged", [
        ("        while (m.owner == MCB_FREE && m.type != MCB_LAST &&\n"
         "               mcb_read((uint16_t)(seg + m.size + 1u), &n) &&\n"
         "               n.owner == MCB_FREE) {\n"
         "            m.size = (uint16_t)(m.size + n.size + 1u);\n"
         "            m.type = n.type;\n"
         "            mcb_write(seg, &m);\n"
         "        }\n", ""),
        ("    mcb_write(seg, &m);\n    mcb_coalesce();\n    return 1;\n}",
         "    mcb_write(seg, &m);\n    return 1;\n}"),
    ]),
    # EXEC. The child's parent link is what a program reads to find who started
    # it; zero there points at the interrupt vector table, and it is the field
    # whose absence Dark Forces derailed into.
    "pspparent": ("the child's PSP:0016 parent link is left at zero", [
        ("    p[0x16] = (uint8_t)parent;                  "
         "/* parent PSP -- see above */\n"
         "    p[0x17] = (uint8_t)(parent >> 8);",
         "    p[0x16] = 0;\n    p[0x17] = 0;"),
    ]),
    # An MZ image loaded without its relocations applied runs, and computes
    # every far address from segment zero. DOS4GW.EXE is exactly this shape.
    "reloc": ("the MZ relocation table is not applied", [
        ("            uint16_t *w = (uint16_t *)x86_guest_ptr(lin, 2);\n"
         "            if (w)\n"
         "                *w = (uint16_t)(*w + load_seg);",
         "            uint16_t *w = (uint16_t *)x86_guest_ptr(lin, 2);\n"
         "            (void)w;"),
    ]),
    # The way back. Without the saved SS:SP the stub's IRET pops the CHILD's
    # stack, so the parent resumes at whatever happens to be on it.
    "resume": ("the parent's registers and SS:SP are not restored", [
        ("        for (int i = 0; i < 8; i++)\n"
         "            cpu_state_ptr->regs[i] = fr->regs[i];\n", ""),
    ]),
    "owner": ("owner 0 is recorded as-is: live memory on the free list", [
        ("    if (owner == MCB_FREE)\n"
         "        owner = MCB_OWNER_DOS;      "
         "/* an allocation is never free; see above */\n", ""),
    ]),
}


def negative_control(args):
    """Rebuild with one fix mutated out and require this test to FAIL."""
    import pathlib
    src = pathlib.Path("x86_dos.c")
    original = src.read_text()
    names = list(MUTATIONS) if args.negative == "all" else [args.negative]
    for n in names:
        for find, _ in MUTATIONS[n][1]:
            if find not in original:
                print(f"FAIL: the mutation target for '{n}' is not in "
                      f"x86_dos.c -- this control would pass without testing "
                      f"anything")
                return False
    all_ok = True
    try:
        for n in names:
            why, edits = MUTATIONS[n]
            text = original
            for find, repl in edits:
                text = text.replace(find, repl, 1)
            src.write_text(text)
            print(f"\n-- negative control '{n}': {why} --")
            args.negative = None
            failed = not main(args)
            if failed:
                print(f"PASS (negative control '{n}'): the test rejects the "
                      f"defective core")
            else:
                print(f"FAIL (negative control '{n}'): the test PASSED with "
                      f"the defect present, so it measures nothing")
                all_ok = False
    finally:
        src.write_text(original)
        print("-- x86_dos.c restored --")
    return all_ok


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    add_runner_args(p)
    p.add_argument("--negative", nargs="?", const="all",
                   choices=["all", *MUTATIONS],
                   help="rebuild with a fix mutated out and require a FAIL")
    args = p.parse_args()
    if args.negative:
        sys.exit(0 if negative_control(args) else 1)
    sys.exit(0 if main(args) else 1)
