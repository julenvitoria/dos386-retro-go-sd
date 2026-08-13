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
    """'REGION sym+0xoff'.

    ITCM used to be reported as a bare "(interpreter)" because it held only the
    fetch loop. It now holds several hundred named handlers, and which one is
    running is the whole question when a guest misbehaves -- so resolve it like
    anywhere else.
    """
    region = region_of(pc)
    merged = {}
    for m in symbol_maps:
        merged.update(m)
    if not merged:
        return region
    return f"{region} {nearest_symbol(merged, pc)}"


# ARMv7-M fault status registers. Reading these is how you stop guessing why
# execution stopped: VECTACTIVE names the active exception, CFSR the cause.
SCB = {"ICSR": 0xE000ED04, "CFSR": 0xE000ED28, "HFSR": 0xE000ED2C,
       "MMFAR": 0xE000ED34, "BFAR": 0xE000ED38}
VECTORS = {0: "thread", 2: "NMI", 3: "HardFault", 4: "MemManage",
           5: "BusFault", 6: "UsageFault", 11: "SVCall", 14: "PendSV",
           15: "SysTick"}


def read_faults(backend, out=print, symbol_maps=()):
    """Report the CPU's exception/fault state, including WHERE it faulted.

    CFSR and BFAR say what went wrong and to which address; neither says which
    instruction did it, and the sampled PC is inside the fault handler by then.
    ARMv7-M stacks R0-R3, R12, LR, PC and xPSR on entry, so the faulting PC is
    six words up the stack the exception came from -- EXC_RETURN bit 2 picks
    MSP or PSP. Resolving that PC against the ELF is usually the whole answer.
    """
    from .faults import decode_cfsr
    vals = {k: backend.read_uint32(a) for k, a in SCB.items()}
    active = vals["ICSR"] & 0x1FF
    out(f"  ICSR  0x{vals['ICSR']:08x}  VECTACTIVE={active} "
        f"({VECTORS.get(active, 'IRQ' + str(active - 16))})")
    out(f"  CFSR  0x{vals['CFSR']:08x}  [{decode_cfsr(vals['CFSR'])}]")
    out(f"  HFSR  0x{vals['HFSR']:08x}")
    out(f"  BFAR  0x{vals['BFAR']:08x}   MMFAR 0x{vals['MMFAR']:08x}")
    if not active:
        return vals
    try:
        lr = backend.read_register("lr")
        sp_name = "psp" if (lr & 0x4) else "msp"
        try:
            sp = backend.read_register(sp_name)
        except Exception:
            sp = backend.read_register("sp")
        frame = [backend.read_uint32(sp + 4 * i) for i in range(8)]
    except Exception as e:                       # nothing to unwind
        out(f"  (could not read the stacked frame: {e})")
        return vals
    names = ["R0", "R1", "R2", "R3", "R12", "LR", "PC", "xPSR"]
    vals["frame"] = dict(zip(names, frame))
    out(f"  stacked frame via {sp_name}=0x{sp:08x}:")
    for n, v in zip(names, frame):
        note = ""
        if n in ("PC", "LR"):
            note = "  " + describe_pc(v & ~1, symbol_maps or ())
        out(f"    {n:<4} 0x{v:08x}{note}")
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


def parse_dump(spec):
    """'x86_cs_log:8x2=CSLOG' -> ('x86_cs_log', 8, 2, 'CSLOG')

    count:stride reads the symbol's OWN storage, which is what an array needs.
    --deref cannot: it follows the first word as a pointer. `x2` groups the
    words into pairs so a log of (a, b) records prints as pairs.
    """
    label = None
    if "=" in spec:
        spec, label = spec.split("=", 1)
    group = 1
    if "x" in spec.split(":")[-1]:
        head, g = spec.rsplit("x", 1)
        spec, group = head, int(g, 0)
    sym, _, cnt = spec.partition(":")
    sym = sym.strip()
    return sym, int(cnt or 8, 0), group, (label or sym)


def dump_symbol_name(sym):
    """The ELF symbol a dump spec depends on, or None for a literal address."""
    sym = sym.lstrip("*")
    sym = sym.split("+", 1)[0].strip()
    if sym.startswith("0x") or sym.startswith("0X"):
        return None
    return sym


def _dump_base(sym, syms, backend=None):
    """Resolve a dump target to an address.

    Three forms, because the interesting memory comes in three shapes:
      `name`          the symbol's own storage (an array)
      `0xADDR`        memory with no symbol (a peripheral, a stack frame)
      `*name+OFF`     through a runtime pointer, which is the only way to
                      reach guest RAM: `guest_ram` is a pointer the core is
                      handed at startup, so its value is not in the ELF.
    """
    deref = sym.startswith("*")
    sym = sym.lstrip("*")
    off = 0
    if "+" in sym:
        sym, off_s = sym.split("+", 1)
        off = int(off_s, 0)
    sym = sym.strip()
    if sym.startswith("0x") or sym.startswith("0X"):
        base = int(sym, 16)
    else:
        base = syms.get(sym)
        if base is None:
            return None
    if deref:
        if backend is None:
            return None
        base = backend.read_uint32(base)
    return base + off


def dump_arrays(target, syms, dumps, out=print, symbol_maps=()):
    """Print each requested array as words, grouped.

    Words that land inside a known function are annotated with it. Half of what
    these logs hold is code addresses -- a saved LR, a handler entry -- and
    resolving them by hand against nm output is slow and easy to get wrong.
    """
    maps = symbol_maps or [syms]
    for sym, count, group, label in dumps:
        base = _dump_base(sym, syms, target.backend)
        if base is None:
            out(f"  {label}: symbol not found")
            continue
        words = [target.backend.read_uint32(base + 4 * i) for i in range(count)]
        out(f"  {label} @0x{base:08x}:")
        for i in range(0, count, group):
            chunk = words[i:i + group]
            cells = []
            for w in chunk:
                note = describe_pc(w, maps) if w > 0x100 else ""
                # Only keep it when it actually resolved to a name.
                if note and "+" not in note and not note.endswith(")"):
                    note = ""
                cells.append(f"0x{w:08x}" + (f" <{note}>" if note else f" ({w})"))
            out(f"    [{i // group}] " + "  ".join(cells))


def sample(target, syms, watches=(), derefs=(), count=10, interval=1.0,
           symbol_maps=(), out=print, on_sample=None):
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

        # While still halted: anything reading target memory (the log
        # drainer) must run before the resume.
        if on_sample is not None:
            on_sample(target.backend)

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
