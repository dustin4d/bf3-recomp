#!/usr/bin/env python3
"""Frostbite 2 reflection data of BF.Main (sections typeinfo 0x84155000 and fieldinf 0x84198A00).

Static type records (in typeinfo, each followed by zeroed space the runtime TypeInfo objects live in):
  +0 char* name ("" for array types)   +4 u16 flags, u16 size   +8 module info*   +0xC u8 alignment, u8 field count
  class records (kind 0x35) continue: +0x10 super class TypeInfo*, +0x14 create function, +0x18 field array
  value types (kind 0x29: structs, messages): +0x10 default instance, +0x14 field array
  enums (kind 0x89): +0x10 value array (12-byte records {char* name, 0, u32 value})
  field array (fieldinf: 12 bytes per field {char* name, u32 offset (enum: value), TypeInfo* type}).
  array records (kind 0x41): +0x10 element TypeInfo*.

Usage: reflection.py scan            -> statistics
       reflection.py json <out.json> -> every record with its fields
"""
import json
import struct
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
IMAGE = ROOT / "dump" / "Disc 1" / "BF.Main.image.bin"
BASE = 0x82D80000
TYPEINFO = (0x84155000, 0x84155000 + 0x43858)
FIELDINF = (0x84198A00, 0x84198A00 + 0x27A80)

_d = IMAGE.read_bytes()


def u32(a):
    return struct.unpack_from(">I", _d, a - BASE)[0]


def in_image(a):
    return BASE <= a < BASE + len(_d) - 4


def cstr(a, allow_empty=False):
    if not in_image(a):
        return None
    end = _d.find(b"\0", a - BASE, a - BASE + 256)
    if end < 0:
        return None
    s = _d[a - BASE:end]
    if not s:
        return "" if allow_empty else None
    if not all(32 <= c < 127 for c in s):
        return None
    return s.decode()


def records():
    """Yields (addr, name, flags, size, module, align, count, words[4..]) for every record in typeinfo."""
    a = TYPEINFO[0]
    while a < TYPEINFO[1] - 16:
        name_ptr, fs, module, ac = u32(a), u32(a + 4), u32(a + 8), u32(a + 12)
        name = cstr(name_ptr, allow_empty=True) if name_ptr else None
        if (name is not None and 0x84000000 <= module < 0x84100000 and (ac & 0xFFFF) in (0, 0x100)
                and (fs >> 16) < 0x1000):
            yield a, name, fs >> 16, fs & 0xFFFF, module, ac >> 24, (ac >> 16) & 0xFF
            a += 16
            continue
        a += 4


def fields_of(ptr, count, kind):
    out = []
    for i in range(count):
        n, o, t = u32(ptr + 12 * i), u32(ptr + 12 * i + 4), u32(ptr + 12 * i + 8)
        out.append({"name": cstr(n, True), "offset": o, "type": t})
    return out


def scan():
    kinds = Counter()
    recs = list(records())
    for r in recs:
        kinds[r[2]] += 1
    print(len(recs), "records")
    for k, n in kinds.most_common():
        ex = [r[1] for r in recs if r[2] == k][:6]
        print(f"  flags 0x{k:04X}: {n:6}  e.g. {ex}")


KIND = {0x35: "class", 0x29: "class29", 0x89: "enum", 0x41: "array", 0x45: "arraybase", 0x0D: "void", 0x1D: "dbobject"}


def export():
    out = []
    for a, name, flags, size, module, align, count in records():
        r = {"record": a, "name": name, "kind": KIND.get(flags, hex(flags)), "flags": flags, "size": size,
             "align": align, "module": module}
        if flags in (0x35, 0x1D, 0x45):
            r["super"], r["create"], fptr = u32(a + 16), u32(a + 20), u32(a + 24)
            r["fields"] = fields_of(fptr, count, flags) if count and FIELDINF[0] <= fptr < FIELDINF[1] else []
        elif flags == 0x29:  # value type (struct, message): +0x10 default instance, +0x14 fields
            r["default"], fptr = u32(a + 16), u32(a + 20)
            r["fields"] = fields_of(fptr, count, flags) if count and FIELDINF[0] <= fptr < FIELDINF[1] else []
        elif flags == 0x89:
            fptr = u32(a + 16)
            vals = fields_of(fptr, count, flags) if count and FIELDINF[0] <= fptr < FIELDINF[1] else []
            r["values"] = [{"name": v["name"], "value": v["type"]} for v in vals]  # enum: {name, 0, value}
        elif flags == 0x41:
            r["element"] = u32(a + 16)
        out.append(r)
    return out


if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] == "scan":
        scan()
    elif sys.argv[1] == "json":
        recs = export()
        Path(sys.argv[2]).write_text(json.dumps(recs, indent=0))
        cls = [r for r in recs if "fields" in r]
        print(f"{len(recs)} records, {len(cls)} classes with {sum(len(r['fields']) for r in cls)} fields, "
              f"{sum(1 for r in recs if 'values' in r)} enums")
