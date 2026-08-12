#!/usr/bin/env python3
"""
Tier-1 check of the DOS memory arena.

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

        print("\nPASS" if ok else "\nFAIL")
        return ok
    finally:
        runner.stop()


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    add_runner_args(p)
    args = p.parse_args()
    sys.exit(0 if main(args) else 1)
