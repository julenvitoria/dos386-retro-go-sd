"""
Sample what a target is actually doing: PC, watched globals, and pointer
dereferences, over time.

This is the "is it progressing, and if not where is it stuck" instrument. It
answers questions that a pass/fail harness cannot:

  * Is the CPU in our ITCM interpreter, in firmware, or in a fault handler?
  * Is the guest advancing (watch EAX count down) or wedged?
  * Did an allocation return a plausible pointer, or garbage that a non-NULL
    check happily accepted?

It found a real bug on first use: two DTCM allocations returning the same
address, which a `if (!ptr) fail` check reported as success.

Works against any Target, so gwemu and silicon are the same call.
"""

import time

from .elf import get_symbols, nearest_symbol

# Rough address-space map, for labelling PC without a symbol lookup.
REGIONS = (
    (0x00000000, 0x00010000, "ITCM"),
    (0x08000000, 0x08200000, "INTFLASH"),
    (0x20000000, 0x20020000, "DTCM"),
    (0x24000000, 0x24100000, "AXI"),
    (0x30000000, 0x30020000, "AHB"),
    (0x90000000, 0x94000000, "EXTFLASH"),
)


def region_of(addr):
    for lo, hi, name in REGIONS:
        if lo <= addr < hi:
            return name
    return "?"


def describe_pc(pc, symbol_maps):
    """'ITCM' for our interpreter, else 'REGION sym+0xoff'."""
    region = region_of(pc)
    if region == "ITCM":
        return "ITCM (interpreter)"
    merged = {}
    for m in symbol_maps:
        merged.update(m)
    return f"{region} {nearest_symbol(merged, pc)}" if merged else region


# ARMv7-M fault status registers. Reading these is how you stop guessing why
# execution stopped: VECTACTIVE names the active exception, CFSR the cause.
SCB = {"ICSR": 0xE000ED04, "CFSR": 0xE000ED28, "HFSR": 0xE000ED2C,
       "MMFAR": 0xE000ED34, "BFAR": 0xE000ED38}
VECTORS = {0: "thread", 2: "NMI", 3: "HardFault", 4: "MemManage",
           5: "BusFault", 6: "UsageFault", 11: "SVCall", 14: "PendSV",
           15: "SysTick"}


def read_faults(backend, out=print):
    """Report the CPU's exception/fault state. Returns the raw values."""
    from .faults import decode_cfsr
    vals = {k: backend.read_uint32(a) for k, a in SCB.items()}
    active = vals["ICSR"] & 0x1FF
    out(f"  ICSR  0x{vals['ICSR']:08x}  VECTACTIVE={active} "
        f"({VECTORS.get(active, 'IRQ' + str(active - 16))})")
    out(f"  CFSR  0x{vals['CFSR']:08x}  [{decode_cfsr(vals['CFSR'])}]")
    out(f"  HFSR  0x{vals['HFSR']:08x}")
    out(f"  BFAR  0x{vals['BFAR']:08x}   MMFAR 0x{vals['MMFAR']:08x}")
    return vals


def parse_watch(spec):
    """'a,b,c' -> ['a','b','c']"""
    return [s.strip() for s in spec.split(",") if s.strip()]


def parse_deref(spec):
    """'cpu_state_ptr+8=EAX' -> ('cpu_state_ptr', 8, 'EAX')"""
    label = None
    if "=" in spec:
        spec, label = spec.split("=", 1)
    off = 0
    if "+" in spec:
        spec, off_s = spec.split("+", 1)
        off = int(off_s, 0)
    sym = spec.strip()
    return sym, off, (label or f"*{sym}+{off:#x}")


def sample(target, syms, watches=(), derefs=(), count=10, interval=1.0,
           symbol_maps=(), out=print):
    """Halt/read/resume `count` times. Returns the rows it printed.

    watches: symbol names read as uint32.
    derefs:  (symbol, offset, label) -- reads uint32 at (*symbol)+offset, which
             is how you follow a runtime-allocated pointer into its struct.
    """
    rows = []
    for i in range(count):
        time.sleep(interval)
        target.backend.halt()
        row = {"t": round(i * interval, 2)}
        pc = target.backend.read_register("pc")
        row["pc"] = pc
        row["where"] = describe_pc(pc, symbol_maps or [syms])

        for name in watches:
            row[name] = (target.backend.read_uint32(syms[name])
                         if name in syms else None)

        for sym, off, label in derefs:
            val = None
            if sym in syms:
                ptr = target.backend.read_uint32(syms[sym])
                row[f"{label}@"] = ptr
                if region_of(ptr) != "?":
                    val = target.backend.read_uint32(ptr + off)
            row[label] = val

        target.backend.resume()
        rows.append(row)

        parts = [f"t+{row['t']:5.1f}s", f"PC=0x{pc:08x}", f"{row['where']:<34}"]
        for name in watches:
            parts.append(f"{name}={row[name]}")
        for _, _, label in derefs:
            ptr = row.get(f"{label}@")
            parts.append(f"{label}={row[label]}"
                         + (f" (@0x{ptr:08x})" if ptr is not None else ""))
        out("  " + " ".join(parts))
    return rows


def wait_for_value(backend, addr, want, timeout, poll=0.25):
    """Wait for an EXACT value. 'non-zero' is not a usable signal on RAM that
    holds firmware data until our code loads over it."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if backend.read_uint32(addr) == want:
            return True
        time.sleep(poll)
    return False


def load_symbol_maps(*elfs):
    """Merge symbol tables; later ELFs win on collisions."""
    merged = {}
    maps = []
    for e in elfs:
        if e:
            m = get_symbols(e)
            maps.append(m)
            merged.update(m)
    return merged, maps
