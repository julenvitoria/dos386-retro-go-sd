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

    r = [b.read_uint32(syms["dosfile_r"] + 4 * i) for i in range(26)]
    mount, exit_reason = r[24], r[25]
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
    if (r[23] & CF) or r[22] < 5:
        fail(f"opening 'test.ini' in lower case returned AX=0x{r[22]:04x} "
             f"CF={r[23] & CF}; FAT names are case-insensitive")

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

        print("\nPASS" if ok else "\nFAIL")
        return ok
    finally:
        runner.stop()


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    add_runner_args(p)
    args = p.parse_args()
    sys.exit(0 if main(args) else 1)
