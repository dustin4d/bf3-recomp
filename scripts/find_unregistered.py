#!/usr/bin/env python3
"""List code addresses referenced from data sections (or built with lis+addi/ori in code)
that codegen did not register as functions. Adapted from the Trials HD port.

  python -I scripts/find_unregistered.py "dump/Disc 1/BF.Main.image.bin" project/generated/bf_main/bf3_register.cpp

Sections come from the image's PE header: executable sections are code, initialized data sections are scanned
for pointers (.pdata/.reloc/.XBLD/.idata/.edata skipped: they hold addresses that aren't call targets).
Prints candidates that look like function entries (previous word is blr/bctr/b/padding) as TOML-ready lines
for the module's [modules.functions] (or [entrypoint.functions]).
"""
import bisect
import re
import struct
import sys

SKIP_DATA = {".pdata", ".reloc", ".XBLD", ".idata", ".edata"}


def sections(img):
    pe = struct.unpack_from("<I", img, 0x3C)[0]
    nsec = struct.unpack_from("<H", img, pe + 6)[0]
    opt = struct.unpack_from("<H", img, pe + 20)[0]
    base = struct.unpack_from("<I", img, pe + 0x34)[0]
    out = []
    for i in range(nsec):
        sec = pe + 24 + opt + i * 40
        name = img[sec:sec + 8].rstrip(b"\0").decode()
        vsize, va = struct.unpack_from("<II", img, sec + 8)
        flags = struct.unpack_from("<I", img, sec + 36)[0]
        end = min(base + va + vsize, base + len(img))
        out.append((name, base + va, end, bool(flags & 0x20000000)))
    return base, out


def main(image, register_cpp):
    img = open(image, "rb").read()
    base, secs = sections(img)
    code = [(lo, hi) for name, lo, hi, x in secs if x]
    data = [(lo, hi) for name, lo, hi, x in secs if not x and name not in SKIP_DATA]

    def word(a):
        return struct.unpack_from(">I", img, a - base)[0]

    def in_code(v):
        return v % 4 == 0 and any(lo <= v < hi for lo, hi in code)

    def is_terminator(insn):
        if insn in (0x4E800020, 0x4E800420, 0x00000000):  # blr, bctr, padding
            return True
        return (insn >> 26) == 18 and (insn & 3) == 0  # b (no link, relative)

    def entry_size(addr):
        """Size up to and including the first terminator (only a hint)."""
        end = next(hi for lo, hi in code if lo <= addr < hi)
        a = addr
        while a < end:
            w = word(a)
            if w and is_terminator(w):
                return a + 4 - addr
            a += 4
        return 0

    registered = {int(m, 16) for m in re.findall(r"SetFunction\(0x([0-9A-Fa-f]+)", open(register_cpp).read())}
    refs = {}
    for lo, hi in data:
        for a in range(lo, hi - 3, 4):
            v = word(a)
            if in_code(v) and v not in registered:
                refs.setdefault(v, []).append(a)
    # Pointers built in code: lis rX,hi followed within 8 insns by addi/ori rY,rX,lo.
    for lo_c, hi_c in code:
        for a in range(lo_c + 32, hi_c, 4):
            insn = word(a)
            op, rs, imm = insn >> 26, (insn >> 16) & 31, insn & 0xFFFF
            if op not in (14, 24) or rs == 0:  # addi / ori with a base register
                continue
            if 0x4E800420 in (word(a + 4 * k) for k in range(1, 5) if a + 4 * k < hi_c):
                continue  # relative switch: lis/addi build the jump-table base label, then bctr
            for back in range(1, 9):
                prev = word(a - 4 * back)
                if prev >> 26 == 15 and (prev >> 16) & 31 == 0 and (prev >> 21) & 31 == rs:  # lis rs,hi
                    lo = imm - 0x10000 if op == 14 and imm & 0x8000 else imm
                    v = ((prev & 0xFFFF) << 16) + lo & 0xFFFFFFFF
                    if in_code(v) and v not in registered:
                        refs.setdefault(v, []).append(a)
                    break
    # A target inside a .pdata function is an interior label (exception handler/scope target, state-machine
    # label reached by bdz), not an entry: registering it splits that function (seen in all three BF3 modules).
    pdata = next(((lo, hi) for name, lo, hi, x in secs if name == ".pdata"), None)
    ranges = []
    if pdata:
        for a in range(pdata[0], pdata[1] - 7, 8):
            begin, info = struct.unpack_from(">II", img, a - base)
            if begin:
                ranges.append((begin, begin + ((info >> 8) & 0x3FFFFF) * 4))
    ranges.sort()
    starts = [b for b, _ in ranges]

    def inside_pdata_function(t):
        j = bisect.bisect_right(starts, t) - 1
        return j >= 0 and starts[j] < t < ranges[j][1]

    hits = sorted(t for t in refs if is_terminator(word(t - 4)) and not inside_pdata_function(t))
    interior = sum(1 for t in refs if is_terminator(word(t - 4)) and inside_pdata_function(t))
    print(f"# {len(refs)} unregistered code targets referenced from data/code; {len(hits)} look like entries"
          f" ({interior} more skipped: inside a .pdata function)")
    for t in hits:
        froms = ", ".join(f"0x{f:08X}" for f in refs[t][:3])
        print(f"0x{t:08X} = {{}}  # ~0x{entry_size(t):X} bytes; from {froms}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
