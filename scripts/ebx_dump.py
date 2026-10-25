#!/usr/bin/env python3
"""Dump a Frostbite 2 EBX partition from the Xbox 360 build (big-endian, u32 header fields) as readable text.

  scripts/ebx_dump.py <file.ebx> [--raw]

Layout (as found in BF3 Xenon data): header 0x40 {magic 0x0FB2D1CE, abs_string_offset, len_string_to_eof, num_imports,
num_guid_repeater, num_instance_repeater, num_complex, num_field, len_name, len_string, num_array_repeater,
len_payload, ...}, file GUID, imports {file GUID, instance GUID}[num_imports], keyword names, fields 16 B
{name hash, u16 type, u16 ref, u32 offset, u32 secondary}, complexes 16 B {name hash, u32 first field, u8 count,
u8 align, u16 type, u16 size, u16 secondary}, instance repeaters 12 B {0, count, complex}, array repeaters 12 B
{offset, count, complex}, then strings at abs_string_offset, the instances after them (16-aligned) and the array data
after the instances (offsets relative to its start). Names are matched by djb2-xor hash."""
import struct, sys


def djbx(s):
    h = 5381
    for c in s.encode():
        h = ((h * 33) ^ c) & 0xFFFFFFFF
    return h


class Ebx:
    def __init__(self, d):
        self.d = d
        (magic, self.abs_str, self.len_to_eof, self.n_imports, self.n_guid_rep, self.n_inst_rep, self.n_complex,
         self.n_field, self.len_name, self.len_string, self.n_array, self.len_payload) = struct.unpack_from(">12I", d, 0)
        assert magic == 0x0FB2D1CE, hex(magic)
        p = 0x40
        self.file_guid = d[p:p + 16]; p += 16
        self.imports = [(d[p + 32 * i:p + 32 * i + 16], d[p + 32 * i + 16:p + 32 * i + 32]) for i in range(self.n_imports)]
        p += 32 * self.n_imports
        self.names = {}
        for raw in d[p:p + self.len_name].split(b"\0"):
            if raw:
                self.names[djbx(raw.decode("latin1"))] = raw.decode("latin1")
        p += self.len_name
        self.fields = [struct.unpack_from(">IHHII", d, p + 16 * i) for i in range(self.n_field)]
        p += 16 * self.n_field
        self.complexes = [struct.unpack_from(">IIBBHHH", d, p + 16 * i) for i in range(self.n_complex)]
        p += 16 * self.n_complex
        self.inst_reps = [struct.unpack_from(">III", d, p + 12 * i) for i in range(self.n_inst_rep)]
        p += 12 * self.n_inst_rep
        p = (p + 15) & ~15
        self.arrays = [struct.unpack_from(">III", d, p + 12 * i) for i in range(self.n_array)]
        self.payload = self.abs_str + self.len_string
        self.payload = (self.payload + 15) & ~15

    def name(self, h):
        return self.names.get(h, f"#{h:08X}")

    def string(self, off):
        if off == 0xFFFFFFFF:
            return None
        s = self.abs_str + off
        return self.d[s:self.d.index(b"\0", s)].decode("latin1")


def main():
    e = Ebx(open(sys.argv[1], "rb").read())
    raw = "--raw" in sys.argv
    print(f"imports {e.n_imports} guidrep {e.n_guid_rep} instrep {e.n_inst_rep} complex {e.n_complex} field {e.n_field}"
          f" arrays {e.n_array} payload {e.payload:#x}+{e.len_payload:#x}")
    if raw:
        for i, c in enumerate(e.complexes):
            print(f"complex {i}: {e.name(c[0])} fields {c[1]}+{c[2]} align {c[3]} type {c[4]:#06x} size {c[5]:#x}")
            for f in e.fields[c[1]:c[1] + c[2]]:
                print(f"    {e.name(f[0]):30} type {f[1]:#06x} ref {f[2]:3} off {f[3]:#x}")
        for r in e.inst_reps:
            print("instrep", [hex(x) for x in r])
        for a in e.arrays:
            print("array", [hex(x) for x in a])
    dump_instances(e)


def dump_instances(e):
    d = e.d
    p = e.payload
    idx = 0
    for rep_i, (a, b, c) in enumerate(e.inst_reps):
        complex_i, count = c, b  # repeater: {0, count, complex}
        cx = e.complexes[complex_i]
        for _ in range(count):
            align = max(cx[3], 4)
            p = (p + align - 1) & ~(align - 1)
            guid = None
            if True:  # every instance in these partitions starts with its GUID
                guid = d[p:p + 16].hex(); p += 16
            print(f"\n[{idx}] {e.name(cx[0])} {'guid ' + guid if guid else ''} @{p:#x}")
            dump_complex(e, complex_i, p, 1)
            p += cx[5]
            idx += 1


def dump_complex(e, complex_i, base, depth):
    cx = e.complexes[complex_i]
    for f in e.fields[cx[1]:cx[1] + cx[2]]:
        fname = e.name(f[0])
        if fname == "$":  # base class: inline
            dump_complex(e, f[2], base + f[3], depth)
            continue
        val = field_value(e, f, base + f[3], depth)
        if val is not None:
            print("  " * depth + f"{fname} = {val}")


def field_value(e, f, at, depth):
    d = e.d
    t = (f[1] >> 4) & 0x1F
    if t == 0x02:  # struct
        print("  " * depth + f"{e.name(f[0])}:")
        dump_complex(e, f[2], at, depth + 1)
        return None
    if t == 0x04:  # array
        ai = struct.unpack_from(">I", d, at)[0]
        if ai >= len(e.arrays):
            return f"<array {ai}>"
        off, count, cpx = e.arrays[ai]
        start = e.payload + e.len_payload + off  # array data follows the instances
        print("  " * depth + f"{e.name(f[0])}: [{count}]")
        acx = e.complexes[cpx]
        member = e.fields[acx[1]] if acx[2] else None
        for k in range(count):
            if member is None:
                break
            ms = e.complexes[member[2]][5] if ((member[1] >> 4) & 0x1F) == 0x02 else field_size(member)
            v = field_value(e, member, start + k * ms, depth + 1)
            if v is not None:
                print("  " * (depth + 1) + f"[{k}] {v}")
        return None
    if t == 0x07:  # CString
        return repr(e.string(struct.unpack_from(">I", d, at)[0]))
    if t == 0x08:  # enum
        return f"enum {struct.unpack_from('>I', d, at)[0]}"
    if t == 0x03:  # pointer
        v = struct.unpack_from(">I", d, at)[0]
        if v == 0:
            return "null"
        if v >> 31:
            imp = e.imports[v & 0x7FFFFFFF]
            return f"import {imp[0].hex()}/{imp[1].hex()}"
        return f"-> [{v - 1}]"
    if t == 0x0A:
        return str(bool(d[at]))
    sizes = {0x0B: ">b", 0x0C: ">B", 0x0D: ">h", 0x0E: ">H", 0x0F: ">i", 0x10: ">I", 0x11: ">q", 0x12: ">Q",
             0x13: ">f", 0x14: ">d"}
    if t in sizes:
        return str(struct.unpack_from(sizes[t], d, at)[0])
    if t == 0x15:
        return d[at:at + 16].hex()
    if t == 0x16:
        return d[at:at + 20].hex()
    return f"<type {f[1]:#06x}> {d[at:at + 8].hex()}"


def field_size(f):
    t = (f[1] >> 4) & 0x1F
    return {0x03: 4, 0x04: 4, 0x07: 4, 0x08: 4, 0x0A: 1, 0x0B: 1, 0x0C: 1, 0x0D: 2, 0x0E: 2, 0x0F: 4, 0x10: 4,
            0x11: 8, 0x12: 8, 0x13: 4, 0x14: 8, 0x15: 16, 0x16: 20}.get(t, 4)


if __name__ == "__main__":
    main()
