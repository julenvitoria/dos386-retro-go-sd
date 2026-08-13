#!/usr/bin/env python3
"""
Run a guest .dsk under a REFERENCE PC (QEMU) and read its memory out.

    python3 scripts/ref_dos.py --dsk references/.../WOLF3D.dsk --settle 35 \\
        --save build/artifacts/ref_wolf3d.bin
    python3 scripts/ref_dos.py --dsk ... --screen build/artifacts/ref.ppm
    python3 scripts/ref_dos.py --dsk ... --no-hooks       # neutralise CONFIG.SYS

------------------------------------------------------------------------------
WHY THIS AND NOT refdos.py
------------------------------------------------------------------------------
`ref_trace.py`/`refdos.py` run the EXECUTABLE under Unicorn against a hand-built
minimal DOS. That works up to the first thing the shim does not implement, and
for a title like Wolfenstein 3D it stops immediately: refdos has no file I/O and
no PORT I/O at all, so it cannot follow a guest past its first VGA access. That
gap is what stalled an investigation for a whole session.

This takes the opposite route and implements NOTHING. The .dsk our titles ship
as is a bootable MS-DOS volume -- IO.SYS, MSDOS.SYS, COMMAND.COM, CONFIG.SYS and
an AUTOEXEC.BAT that launches the game -- so a real PC emulator boots it and
gets file I/O, port I/O, the BIOS and DOS itself for free, with none of it
written by us and none of it able to agree with our x86_dos.c on the same
mistake. What it gives back is the thing that actually settles arguments: the
reference machine's whole low megabyte, byte-comparable against
`probe.py --guest-save`.

------------------------------------------------------------------------------
THE TWO TRAPS, BOTH OF WHICH COST REAL TIME HERE
------------------------------------------------------------------------------
1. **CHS geometry.** These images are built with a BPB of 1 HEAD and 63
   sectors/track, and the MS-DOS boot record reads IO.SYS through INT 13h using
   those numbers. QEMU's IDE default geometry for a 3 MB disk is not that, so
   the VBR loads the wrong sectors and SeaBIOS sits at 0x7C04 forever with
   "Booting from Hard Disk..." on screen and no error. The geometry is therefore
   read OUT OF THE BPB and passed to `ide-hd` explicitly, never guessed.
2. **The project's own CONFIG.SYS hooks.** `XMSHOOK.SYS`/`EMSHOOK.SYS` are
   written for OUR emulator; on a reference machine they can hang the boot
   before AUTOEXEC.BAT ever runs, which reads identically to "the game crashed".
   `--no-hooks` rewrites CONFIG.SYS/AUTOEXEC.BAT in a COPY of the image (the
   original .dsk is never modified) down to the minimum that still launches the
   title.

------------------------------------------------------------------------------
WHAT IT IS NOT
------------------------------------------------------------------------------
It is not a timing reference and not a claim about our silicon. And a reference
that is merely RUNNING is not a reference that has reached the same place the
title reaches under us -- check `--screen` and the register sample before
trusting a comparison, or the two images are snapshots of different moments and
every data difference between them is noise.
"""

import argparse
import os
import shutil
import socket
import struct
import subprocess
import sys
import time

SECTOR = 512


def part_offset(data):
    """Byte offset of the first partition, or 0 for a bare volume."""
    if data[510:512] != b"\x55\xaa":
        return 0
    for i in range(4):
        e = data[446 + 16 * i:446 + 16 * i + 16]
        if e[4]:
            return struct.unpack_from("<I", e, 8)[0] * SECTOR
    return 0


def geometry(path):
    """(cyls, heads, secs) from the volume's own BPB -- see trap 1."""
    with open(path, "rb") as f:
        data = f.read(SECTOR * 64)
    base = part_offset(data)
    secs = struct.unpack_from("<H", data, base + 24)[0]
    heads = struct.unpack_from("<H", data, base + 26)[0]
    total = os.path.getsize(path) // SECTOR
    if not secs or not heads:
        return None
    return max(1, total // (heads * secs)), heads, secs


def rewrite_boot_files(path, launch):
    """Replace CONFIG.SYS/AUTOEXEC.BAT in-place in a COPY -- see trap 2."""
    d = bytearray(open(path, "rb").read())
    base = part_offset(bytes(d))
    spc = d[base + 13]
    res = struct.unpack_from("<H", d, base + 14)[0]
    nfat = d[base + 16]
    rde = struct.unpack_from("<H", d, base + 17)[0]
    spf = struct.unpack_from("<H", d, base + 22)[0]
    root = res + nfat * spf
    rsec = (rde * 32 + SECTOR - 1) // SECTOR
    dstart = root + rsec
    r0 = base + root * SECTOR

    def entry(name):
        for i in range(0, rsec * SECTOR, 32):
            e = d[r0 + i:r0 + i + 32]
            if not e[0] or e[0] == 0xE5:
                continue
            n = e[0:8].decode("latin1").rstrip()
            x = e[8:11].decode("latin1").rstrip()
            if ((n + "." + x) if x else n).upper() == name:
                return r0 + i, struct.unpack_from("<H", e, 26)[0]
        return None, None

    for name, body in (("CONFIG.SYS", b"FILES=20\r\nBUFFERS=15\r\n"),
                       ("AUTOEXEC.BAT",
                        b"@echo off\r\n" + launch.encode() + b"\r\n")):
        ent, clus = entry(name)
        if ent is None:
            continue
        off = base + (dstart + (clus - 2) * spc) * SECTOR
        d[off:off + SECTOR] = body + b"\x1a" * (SECTOR - len(body))
        struct.pack_into("<I", d, ent + 28, len(body))
    open(path, "wb").write(bytes(d))


class Monitor:
    def __init__(self, sock_path, timeout=20.0):
        deadline = time.time() + timeout
        while True:
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(sock_path)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.5)
        time.sleep(0.5)
        self._drain()

    def _drain(self):
        self.s.settimeout(0.5)
        try:
            self.s.recv(1 << 20)
        except OSError:
            pass

    def cmd(self, c, wait=2.5):
        self.s.sendall((c + "\n").encode())
        time.sleep(wait)
        self.s.settimeout(1.0)
        try:
            return self.s.recv(1 << 20).decode("latin1")
        except OSError:
            return ""


def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dsk", required=True)
    p.add_argument("--settle", type=float, default=35.0,
                   help="seconds to let the reference boot and run")
    p.add_argument("--save", help="write its low megabyte here")
    p.add_argument("--screen", help="write a screendump (.ppm) here")
    p.add_argument("--no-hooks", action="store_true",
                   help="neutralise CONFIG.SYS's emulator-specific drivers")
    p.add_argument("--launch", default=None,
                   help="AUTOEXEC line to use with --no-hooks "
                        "(default: the image's own)")
    p.add_argument("--mem", type=int, default=16)
    p.add_argument("--work", default="build/run/refdos")
    args = p.parse_args()

    os.makedirs(args.work, exist_ok=True)
    img = os.path.join(args.work, "ref.img")
    shutil.copyfile(args.dsk, img)          # never touch the shipped .dsk
    if args.no_hooks:
        launch = args.launch or os.path.basename(args.dsk).upper().replace(
            ".DSK", ".EXE")
        rewrite_boot_files(img, launch)

    geo = geometry(img)
    if geo is None:
        sys.exit("could not read a BPB geometry out of the image")
    cyls, heads, secs = geo
    print(f"geometry from the BPB: cyls={cyls} heads={heads} secs={secs}")

    sock = os.path.join(args.work, "mon.sock")
    if os.path.exists(sock):
        os.unlink(sock)
    proc = subprocess.Popen(
        ["qemu-system-i386",
         "-drive", f"file={img},format=raw,if=none,id=d0,snapshot=on",
         "-device", f"ide-hd,drive=d0,cyls={cyls},heads={heads},secs={secs}",
         "-m", str(args.mem), "-display", "none",
         "-monitor", f"unix:{sock},server,nowait"],
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    try:
        print(f"letting the reference run {args.settle:.0f}s...")
        time.sleep(args.settle)
        mon = Monitor(sock)
        regs = mon.cmd("info registers")
        for line in regs.splitlines():
            if line.startswith(("EIP", "CS ", "DS ", "SS ", "ESP")):
                print("  " + line.strip())
        if args.save:
            os.makedirs(os.path.dirname(args.save) or ".", exist_ok=True)
            mon.cmd(f"pmemsave 0 0x100000 {os.path.abspath(args.save)}")
            print(f"wrote {args.save}")
        if args.screen:
            os.makedirs(os.path.dirname(args.screen) or ".", exist_ok=True)
            mon.cmd(f"screendump {os.path.abspath(args.screen)}")
            print(f"wrote {args.screen}")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
