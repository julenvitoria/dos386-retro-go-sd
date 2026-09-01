"""Build the guest memory image a DOS .EXE is loaded into.

This mirrors `x86_dos_exec` in x86_dos.c: same PSP segment, same load segment,
same relocation pass, same entry register state. It exists so a reference
emulator can be started from EXACTLY the state our interpreter starts from --
if the two disagree about the setup, comparing their traces proves nothing.

It also carries a minimal FAT12 reader, because guest titles ship as .dsk
floppy images and the executable has to come out of one.
"""

import struct

SECTOR = 512


class Fat12:
    """Enough FAT12 to find a file in the root directory and read it whole."""

    def __init__(self, data):
        self.d = data
        self.bytes_per_sec = struct.unpack_from("<H", data, 11)[0]
        self.sec_per_clus = data[13]
        self.reserved = struct.unpack_from("<H", data, 14)[0]
        self.num_fats = data[16]
        self.root_entries = struct.unpack_from("<H", data, 17)[0]
        self.sec_per_fat = struct.unpack_from("<H", data, 22)[0]
        self.fat_start = self.reserved
        self.root_start = self.fat_start + self.num_fats * self.sec_per_fat
        self.root_sectors = (self.root_entries * 32 + self.bytes_per_sec - 1) \
            // self.bytes_per_sec
        self.data_start = self.root_start + self.root_sectors

    def _sec(self, n, count=1):
        off = n * self.bytes_per_sec
        return self.d[off:off + count * self.bytes_per_sec]

    def _fat_entry(self, cluster):
        base = self.fat_start * self.bytes_per_sec
        i = base + cluster + cluster // 2
        v = self.d[i] | (self.d[i + 1] << 8)
        return (v >> 4) if (cluster & 1) else (v & 0x0FFF)

    def listdir(self):
        root = self._sec(self.root_start, self.root_sectors)
        for i in range(0, len(root), 32):
            e = root[i:i + 32]
            if not e[0] or e[0] == 0xE5 or e[11] & 0x08:
                continue
            name = e[0:8].decode("ascii", "replace").rstrip()
            ext = e[8:11].decode("ascii", "replace").rstrip()
            yield ((name + "." + ext) if ext else name,
                   struct.unpack_from("<H", e, 26)[0],
                   struct.unpack_from("<I", e, 28)[0])

    def read(self, filename):
        want = filename.upper()
        for name, cluster, size in self.listdir():
            if name.upper() != want:
                continue
            out = bytearray()
            while 2 <= cluster < 0xFF0 and len(out) < size:
                sec = self.data_start + (cluster - 2) * self.sec_per_clus
                out += self._sec(sec, self.sec_per_clus)
                cluster = self._fat_entry(cluster)
            return bytes(out[:size])
        raise KeyError(filename)


class MZ:
    FIELDS = ("sig last_page pages relocs hdr_paras minalloc maxalloc "
              "ss sp checksum ip cs reloc_off overlay").split()

    def __init__(self, data):
        vals = struct.unpack_from("<14H", data, 0)
        for k, v in zip(self.FIELDS, vals):
            setattr(self, k, v)
        self.data = data
        self.hdr_bytes = self.hdr_paras * 16
        n = self.pages * 512
        if self.last_page:
            n = n - 512 + self.last_page
        self.image_bytes = n - self.hdr_bytes

    def image(self):
        return self.data[self.hdr_bytes:self.hdr_bytes + self.image_bytes]

    def relocations(self):
        off = self.reloc_off
        for _ in range(self.relocs):
            o, s = struct.unpack_from("<HH", self.data, off)
            yield o, s
            off += 4


def load(exe, ram, psp=0x71, cmdline="", ram_paras=0xA000):
    """Place `exe` into the bytearray `ram` exactly as x86_dos_exec does.

    Returns the entry state as a dict of segment/register values.
    """
    mz = MZ(exe)
    load_seg = psp + 0x10
    base = load_seg * 16
    ram[base:base + mz.image_bytes] = mz.image()

    for off, seg in mz.relocations():
        at = base + seg * 16 + off
        v = (struct.unpack_from("<H", ram, at)[0] + load_seg) & 0xFFFF
        struct.pack_into("<H", ram, at, v)

    # PSP. Only the fields a startup stub actually reads are filled: the INT 20h
    # at :0000, the top-of-memory paragraph at :0002 (which the Borland stub
    # compares against to decide it has room), and the command tail at :0080.
    p = psp * 16
    ram[p:p + 256] = bytes(256)
    struct.pack_into("<H", ram, p + 0, 0x20CD)
    struct.pack_into("<H", ram, p + 2, ram_paras)
    tail = cmdline.encode("ascii")[:126]
    ram[p + 0x80] = len(tail)
    ram[p + 0x81:p + 0x81 + len(tail)] = tail
    ram[p + 0x81 + len(tail)] = 0x0D

    return {
        "cs": (load_seg + mz.cs) & 0xFFFF, "ip": mz.ip,
        "ss": (load_seg + mz.ss) & 0xFFFF, "sp": mz.sp,
        "ds": psp, "es": psp, "ax": 0,
        "psp": psp, "load_seg": load_seg, "mz": mz,
    }


class FatVolume:
    """FAT12/FAT16 reader with partition and subdirectory support.

    `Fat12` above is deliberately minimal: unpartitioned, root directory only,
    which is all a 1.44 MB floppy `.dsk` needs. The larger titles ship as
    partitioned hard-disk images instead (an MBR at LBA 0, one FAT16 primary at
    LBA 63) and keep their executables in subdirectories, so a static scan of a
    title's binaries cannot reach them through `Fat12`.

    This is that reader. Same read-only, no-cache shape; it just also finds the
    partition, handles 12- and 16-bit FATs, and walks a path.
    """

    def __init__(self, data, part_lba=None):
        if part_lba is None:
            part_lba = self._find_partition(data)
        self.base = part_lba * SECTOR
        self.d = data
        b = self.base
        self.bytes_per_sec = struct.unpack_from("<H", data, b + 11)[0]
        self.sec_per_clus = data[b + 13]
        self.reserved = struct.unpack_from("<H", data, b + 14)[0]
        self.num_fats = data[b + 16]
        self.root_entries = struct.unpack_from("<H", data, b + 17)[0]
        self.sec_per_fat = struct.unpack_from("<H", data, b + 22)[0]
        total = struct.unpack_from("<H", data, b + 19)[0]
        if total == 0:
            total = struct.unpack_from("<I", data, b + 32)[0]
        self.fat_start = self.reserved
        self.root_start = self.fat_start + self.num_fats * self.sec_per_fat
        self.root_sectors = (self.root_entries * 32 + self.bytes_per_sec - 1) \
            // self.bytes_per_sec
        self.data_start = self.root_start + self.root_sectors
        clusters = (total - self.data_start) // max(1, self.sec_per_clus)
        self.fat_bits = 12 if clusters < 4085 else 16
        self.eoc = 0xFF8 if self.fat_bits == 12 else 0xFFF8

    @staticmethod
    def _find_partition(data):
        if struct.unpack_from("<H", data, 510)[0] != 0xAA55:
            return 0
        for i in range(4):
            e = data[446 + 16 * i:446 + 16 * i + 16]
            if e[4] in (0x01, 0x04, 0x06, 0x0E):
                return struct.unpack_from("<I", e, 8)[0]
        # No MBR partition entry: treat the image as a bare volume.
        return 0

    def _sec(self, n, count=1):
        off = self.base + n * self.bytes_per_sec
        return self.d[off:off + count * self.bytes_per_sec]

    def _fat_entry(self, cluster):
        base = self.base + self.fat_start * self.bytes_per_sec
        if self.fat_bits == 12:
            i = base + cluster + cluster // 2
            v = self.d[i] | (self.d[i + 1] << 8)
            return (v >> 4) if (cluster & 1) else (v & 0x0FFF)
        return struct.unpack_from("<H", self.d, base + 2 * cluster)[0]

    def _chain(self, cluster, size=None):
        out = bytearray()
        while 2 <= cluster < self.eoc:
            sec = self.data_start + (cluster - 2) * self.sec_per_clus
            out += self._sec(sec, self.sec_per_clus)
            if size is not None and len(out) >= size:
                break
            cluster = self._fat_entry(cluster)
        return bytes(out if size is None else out[:size])

    @staticmethod
    def _entries(raw):
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if not e[0] or e[0] == 0xE5:
                continue
            if e[11] & 0x08 or (e[11] & 0x0F) == 0x0F:   # volume label / LFN
                continue
            name = e[0:8].decode("ascii", "replace").rstrip()
            ext = e[8:11].decode("ascii", "replace").rstrip()
            yield ((name + "." + ext) if ext else name,
                   struct.unpack_from("<H", e, 26)[0],
                   struct.unpack_from("<I", e, 28)[0],
                   bool(e[11] & 0x10))

    def listdir(self, path=""):
        """Yield (name, cluster, size, is_dir) for a directory path."""
        raw = self._sec(self.root_start, self.root_sectors)
        for part in [p for p in path.replace("\\", "/").split("/") if p]:
            for name, clus, _sz, isdir in self._entries(raw):
                if isdir and name.upper() == part.upper():
                    raw = self._chain(clus)
                    break
            else:
                raise KeyError(path)
        return list(self._entries(raw))

    def walk(self):
        """Yield (fullpath, size) for every file in the volume."""
        stack = [""]
        while stack:
            d = stack.pop()
            for name, _clus, size, isdir in self.listdir(d):
                if name in (".", ".."):
                    continue
                full = (d + "/" + name) if d else name
                if isdir:
                    stack.append(full)
                else:
                    yield full, size

    def read(self, path):
        d, _, fname = path.replace("\\", "/").rpartition("/")
        for name, clus, size, isdir in self.listdir(d):
            if not isdir and name.upper() == fname.upper():
                return self._chain(clus, size)
        raise KeyError(path)
