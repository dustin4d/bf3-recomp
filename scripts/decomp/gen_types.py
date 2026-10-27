#!/usr/bin/env python3
"""Typed C++ declarations of BF.Main's reflected Frostbite types (prototype; see docs/DECOMP.md).

Source: the static type records (reflection.py -> logs/decomp/reflection.json) plus the typeinfo section of a running
game (coverage_session.py saves it), which links each TypeInfo object to its record (field types, super classes).
Type flags: basic type = (flags >> 4) & 0x1F (Frostbite 2): 2 ValueType, 3 Class, 4 Array, 7 CString, 8 Enum,
10 Boolean, 11 Int8, 12 Uint8, 13 Int16, 14 Uint16, 15 Int32, 16 Uint32, 17 Int64, 18 Uint64, 19 Float32,
20 Float64, 21 Guid. A field's 32-bit offset word carries the field's own flags in its high half (0x0048: Array).

The structs describe the objects as they sit in guest memory (big-endian, 32-bit pointers), with ReXGlue's
rex::be<T> and TypedGuestPointer<T>, so native overrides can use them while the game still runs on the recomp.
Classes (DataContainers) start with their vtable pointer and are referenced by pointer; value types are embedded.
Every field offset and every type size is checked with static_assert.

Usage: gen_types.py <typeinfo.bin> [out=project/src/native/fb/reflected_types.h] [--only Name,Name...]
"""
import collections
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from reflection import TYPEINFO, cstr, u32  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

BASIC = {10: ("bool", 1), 11: ("int8_t", 1), 12: ("uint8_t", 1), 13: ("rex::be<int16_t>", 2),
         14: ("rex::be<uint16_t>", 2), 15: ("rex::be<int32_t>", 4), 16: ("rex::be<uint32_t>", 4),
         17: ("rex::be<int64_t>", 8), 18: ("rex::be<uint64_t>", 8), 19: ("rex::be<float>", 4),
         20: ("rex::be<double>", 8), 21: ("fb::Guid", 16), 7: ("fb::CString", 4)}
ALIGN = {1: 1, 2: 2, 4: 4, 8: 8, 16: 4}


def ident(name):
    n = re.sub(r"\W", "_", name)
    return n if not n[0].isdigit() else "_" + n


def qual(name):
    """A reflected type referenced from a field: qualified, since fields are often named like their type."""
    return "::fb::reflected::" + ident(name)


class Types:
    def __init__(self, typeinfo_bin):
        self.ti = open(typeinfo_bin, "rb").read()
        self.recs = {r["record"]: r for r in json.load(open(os.path.join(ROOT, "logs", "decomp", "reflection.json")))}

    def record_of(self, typeinfo):
        """TypeInfo object -> its record (address of the name pointer), from the runtime section."""
        if not (TYPEINFO[0] <= typeinfo < TYPEINFO[1]):
            return None
        rec = struct.unpack_from(">I", self.ti, typeinfo - TYPEINFO[0])[0]
        return rec if TYPEINFO[0] <= rec < TYPEINFO[1] else None

    def info(self, typeinfo):
        """(name, basic type, size) of a TypeInfo object, also for primitives (records not in reflection.json)."""
        rec = self.record_of(typeinfo)
        if rec is None:
            return None
        name = cstr(u32(rec), True)
        fs = u32(rec + 4)
        return name, (fs >> 20) & 0x1F, fs & 0xFFFF, rec


def field_cpp(types, f, deps):
    """C++ type and size of a field."""
    flags = f["offset"] >> 16
    t = types.info(f["type"])
    if (flags >> 4) & 0x1F == 4 or (t and t[1] == 4):
        elem = types.info(u32(t[3] + 16)) if t and t[1] == 4 else None
        inner = "void"
        if elem and elem[0]:
            inner, _ = scalar_cpp(elem, deps, ref=True)
        return f"fb::Array<{inner}>", 4
    if not t:
        return "rex::be<uint32_t> /* unknown type */", 4
    return scalar_cpp(t, deps)


def scalar_cpp(t, deps, ref=False):
    name, basic, size, rec = t
    if basic in BASIC:
        return BASIC[basic]
    if basic == 8:
        return f"fb::Enum<{qual(name)}>", 4
    if basic == 3:
        deps["ref"].add(name)
        return f"TypedGuestPointer<{qual(name)}>", 4
    if basic == 2:
        if not ref:
            deps["value"].add(name)
        return qual(name), size
    return f"rex::be<uint32_t> /* {name} kind {basic} */", 4


def generate(types, only=None):
    recs = list(types.recs.values())
    by_name = {}
    for r in recs:
        if r["kind"] in ("class", "class29", "enum") and r["name"]:
            by_name.setdefault(r["name"], r)
    names = set(only) if only else set(by_name)
    # pull in what the selection needs (supers and embedded value types), then order by dependency
    deps_of = {}
    pending = list(names)
    while pending:
        n = pending.pop()
        r = by_name.get(n)
        if not r or n in deps_of:
            continue
        deps = {"ref": set(), "value": set(), "super": None}
        if r["kind"] == "class" and r.get("super"):
            s = types.info(r["super"])
            if s and s[0] and s[0] != n:
                deps["super"] = s[0]
        for f in r.get("fields", []):
            field_cpp(types, f, deps)
            t = types.info(f["type"])
            if t and t[1] == 8:
                deps["value"].add(t[0])
            if t and t[1] == 4:
                e = types.info(u32(t[3] + 16))
                if e and e[1] == 8:
                    deps["value"].add(e[0])
        deps_of[n] = deps
        need = deps["value"] | ({deps["super"]} if deps["super"] else set()) | deps["ref"]
        pending += [d for d in need if d in by_name and d not in deps_of]
    order, seen = [], set()

    def visit(n, stack=()):
        if n in seen or n not in deps_of:
            return
        seen.add(n)
        d = deps_of[n]
        for m in sorted(d["value"]) + ([d["super"]] if d["super"] else []):
            visit(m)
        order.append(n)

    for n in sorted(deps_of):
        visit(n)
    refs = sorted({m for d in deps_of.values() for m in d["ref"]} | set(deps_of))
    out = [HEADER]
    out.append("// Forward declarations (classes are referenced through guest pointers)")
    out += [f"struct {ident(n)};" for n in refs if by_name.get(n, {}).get("kind") != "enum"]
    out.append("")
    for n in order:
        r = by_name[n]
        if r["kind"] == "enum":
            out.append(f"// {n}: enum, type record 0x{r['record']:08X}")
            out.append(f"enum class {ident(n)} : uint32_t {{")
            used = collections.Counter()
            for v in r.get("values", []):
                e = ident(v["name"])
                used[e] += 1
                out.append(f"  {e}{'' if used[e] == 1 else '_' + str(used[e])} = {v['value']},")
            out.append("};\n")
            continue
        d = deps_of[n]
        sup = d["super"] if d["super"] in by_name else None
        start = by_name[sup]["size"] if sup else (4 if r["kind"] == "class" else 0)
        out.append(f"// {n}: {'class' if r['kind'] == 'class' else 'value type'}, size 0x{r['size']:X}, "
                   f"type record 0x{r['record']:08X}" + (f", create sub_{r['create']:08X}" if r.get("create") else ""))
        out.append(f"struct {ident(n)}" + (f" : {ident(sup)}" if sup else "") + " {")
        if not sup and r["kind"] == "class":
            out.append("  rex::be<uint32_t> vtable;  // +0x0")
        pos, asserts = start, []
        fields = sorted(r.get("fields", []), key=lambda f: f["offset"] & 0xFFFF)
        for f in fields:
            off = f["offset"] & 0xFFFF
            cpp, size = field_cpp(types, f, {"ref": set(), "value": set()})
            if off < pos:
                out.append(f"  // overlapping field {f['name']} at +0x{off:X} ({cpp}) omitted")
                continue
            if off > pos:
                out.append(f"  uint8_t pad_{pos:X}[0x{off - pos:X}];")
            out.append(f"  {cpp} {ident(f['name'])};  // +0x{off:X}")
            asserts.append(f"static_assert(offsetof({ident(n)}, {ident(f['name'])}) == 0x{off:X});")
            pos = off + size
        if r["size"] > pos:
            out.append(f"  uint8_t pad_{pos:X}[0x{r['size'] - pos:X}];")
        out.append("};")
        out.append(f"static_assert(sizeof({ident(n)}) == 0x{r['size']:X});")
        out += asserts
        out.append("")
    out.append("}  // namespace fb::reflected")
    return "\n".join(out) + "\n", len(order)


HEADER = """// Generated by scripts/decomp/gen_types.py from BF.Main's Frostbite reflection data - DO NOT EDIT.
// Layout of reflected types as they sit in guest memory (big-endian, 32-bit guest pointers).
#pragma once

#include <cstddef>
#include <cstdint>

#include <rex/system/xtypes.h>
#include <rex/types.h>

#include "native/fb/types.h"

namespace fb::reflected {

using rex::TypedGuestPointer;
#pragma pack(push, 1)
"""


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    only = None
    for a in sys.argv[1:]:
        if a.startswith("--only="):
            only = a.split("=", 1)[1].split(",")
    out = args[1] if len(args) > 1 else os.path.join(ROOT, "project", "src", "native", "fb", "reflected_types.h")
    types = Types(args[0])
    text, n = generate(types, only)
    text = text.replace("}  // namespace fb::reflected", "#pragma pack(pop)\n}  // namespace fb::reflected")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w") as f:
        f.write(text)
    print(f"{out}: {n} types, {len(text)} bytes")


if __name__ == "__main__":
    main()
