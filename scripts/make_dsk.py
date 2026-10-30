#!/usr/bin/env python3
"""Build a game's .dsk from a folder of files.

The owner sends games as archives of loose files; the core wants one FAT disk
image per title (README, "Las imágenes .dsk"). This turns a folder into that
image, so every new title is made the same way instead of by a hand-typed
mformat/mcopy sequence.

    # a floppy (1.44 MB, or 2.88 MB if it does not fit)
    python3 scripts/make_dsk.py --src build/run/games/rw --out RISKY.dsk \
        --autoexec RISKY

    # too big for a floppy: a hard disk with one partition (FAT12/FAT16)
    python3 scripts/make_dsk.py --src build/run/games/dn --out DUNE2.dsk \
        --autoexec DUNE2

Layout choices, and why:

- Up to 2.88 MB it is a plain floppy (no partition table): every DOS tool and
  the core read those, and a floppy .img "basta con renombrarla".
- Larger, it is a hard disk the way MS-DOS 5 laid one out: an MBR with ONE
  primary partition starting at LBA 63 (CHS 16 heads x 63 sectors), the same
  shape as the MONKEY/LOOM images the owner's tools produced and the core has
  run since October. The partition type is read back from the boot sector
  mformat wrote (0x01 FAT12, 0x04 FAT16 < 32 MB, 0x06 FAT16 >= 32 MB), so the
  MBR never claims a FAT width the volume does not have.
- The disk is sized at 1.25x the content plus 1 MB, rounded up to a whole MB:
  room for the saves and setup files games write next to themselves.

`--autoexec` writes a one-line AUTOEXEC.BAT (CRLF), which is the second thing
the core looks at after a .cfg's [autoexec]. A folder that already carries an
AUTOEXEC.BAT keeps it unless --autoexec is given.

Needs mtools (mformat, mcopy, mdir), as the SD-card build already does. Writes
only --out; nothing goes to /tmp.
"""

import argparse
import os
import struct
import subprocess
import sys

SECTOR = 512
HEADS, SPT = 16, 63
PART_LBA = 63
FLOPPIES_KB = (1440, 2880)


def tree_bytes(src):
    """Bytes the content takes, rounded per 4 KB cluster (a fair upper bound)."""
    total = 0
    for root, dirs, files in os.walk(src):
        total += 4096 * (len(dirs) + 1)
        for f in files:
            sz = os.path.getsize(os.path.join(root, f))
            total += (sz + 4095) // 4096 * 4096
    return total


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"error: {' '.join(cmd)}\n{r.stdout}{r.stderr}")
    return r.stdout


def mbr(total_sectors, ptype):
    """One primary partition from LBA 63 to the end of the disk."""
    count = total_sectors - PART_LBA

    def chs(lba):
        c = lba // (HEADS * SPT)
        h = (lba // SPT) % HEADS
        s = lba % SPT + 1
        if c > 1023:
            c, h, s = 1023, HEADS - 1, SPT
        return bytes([h, ((c >> 2) & 0xC0) | s, c & 0xFF])

    e = bytes([0x80]) + chs(PART_LBA) + bytes([ptype]) + \
        chs(total_sectors - 1) + struct.pack("<II", PART_LBA, count)
    m = bytearray(SECTOR)
    m[446:462] = e
    m[510:512] = b"\x55\xaa"
    return bytes(m)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--src", required=True, help="folder whose CONTENTS become the disk root")
    ap.add_argument("--out", required=True, help=".dsk to write (overwritten)")
    ap.add_argument("--label", help="volume label (default: --out's name)")
    ap.add_argument("--autoexec", help="one-line AUTOEXEC.BAT, e.g. 'DUNE2' or 'CD GAME' ")
    ap.add_argument("--size-mb", type=int, help="force a hard disk of this size")
    ap.add_argument("--floppy-kb", type=int, choices=FLOPPIES_KB,
                    help="force a floppy of this size")
    a = ap.parse_args()

    if not os.path.isdir(a.src):
        sys.exit(f"error: no folder {a.src}")
    label = (a.label or os.path.splitext(os.path.basename(a.out))[0])[:11].upper()
    need = tree_bytes(a.src) + 8192

    if os.path.exists(a.out):
        os.remove(a.out)

    if a.size_mb is None and (a.floppy_kb or need <= FLOPPIES_KB[-1] * 1024 - 64 * 1024):
        kb = a.floppy_kb or next(k for k in FLOPPIES_KB if need <= k * 1024 - 64 * 1024)
        run(["mformat", "-C", "-f", str(kb), "-v", label, "-i", a.out, "::"])
        img = a.out
        what = f"floppy {kb} KB"
    else:
        mb = a.size_mb or -(-(need * 5 // 4 + (1 << 20)) // (1 << 20))
        total = mb * (1 << 20) // SECTOR
        total -= total % (HEADS * SPT)
        with open(a.out, "wb") as f:
            f.truncate(total * SECTOR)
        img = f"{a.out}@@{PART_LBA * SECTOR}"
        run(["mformat", "-i", img, "-T", str(total - PART_LBA), "-h", str(HEADS),
             "-s", str(SPT), "-H", str(PART_LBA), "-v", label, "::"])
        with open(a.out, "r+b") as f:
            f.seek(PART_LBA * SECTOR + 54)
            fs = f.read(8)
            ptype = 0x01 if fs.startswith(b"FAT12") else \
                (0x04 if total * SECTOR < (32 << 20) else 0x06)
            f.seek(0)
            f.write(mbr(total, ptype))
        what = f"hard disk {mb} MB, partition type 0x{ptype:02X} ({fs.decode().strip()})"

    entries = [os.path.join(a.src, n) for n in sorted(os.listdir(a.src))]
    if a.autoexec:
        entries = [e for e in entries if os.path.basename(e).upper() != "AUTOEXEC.BAT"]
    if entries:
        run(["mcopy", "-s", "-m", "-i", img] + entries + ["::"])
    if a.autoexec:
        bat = a.out + ".autoexec.tmp"
        try:
            with open(bat, "wb") as f:
                f.write(a.autoexec.encode("ascii") + b"\r\n")
            run(["mcopy", "-i", img, bat, "::AUTOEXEC.BAT"])
        finally:
            os.remove(bat)

    free = run(["mdir", "-i", img, "::"]).strip().splitlines()[-1]
    print(f"{a.out}: {what}; {free.strip()}")


if __name__ == "__main__":
    main()
