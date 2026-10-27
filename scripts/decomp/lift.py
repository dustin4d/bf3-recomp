#!/usr/bin/env python3
"""Lifts a recompiled BF.Main function into the native tree: copies its generated C++ (with the guest instructions
as comments) into an override sub_X under project/src/native/<group>/, marked BF3_NATIVE(addr, lifted).

The copy is behaviour-identical by construction (same code, now overriding the weak generated symbol; the original
stays callable as __imp__sub_X), so it can be built and booted at once and then refactored step by step (typed,
idiomatic, verified: see project/src/native/native.h and docs/DECOMP.md) under the differential tests.

Usage: lift.py <addr> [--group <dir>] [--name <ClassName::method>] [--out <file>] [--force]
       group defaults to the inventory's (docs/decomp/v01_functions.tsv), name to sub_X.
"""
import argparse
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GROUP_DIRS = {"frostbite-core": "core", "game": "game", "ai": "ai", "ui-scaleform": "ui", "havok": "havok",
              "physics": "physics", "network": "net", "online": "online", "audio": "audio", "render": "render",
              "animation": "anim", "crt-libs": "libs", "unknown": "unsorted"}


def generated_body(addr):
    name = f"sub_{addr:08X}"
    head = f"DEFINE_REX_FUNC({name})"
    for path in glob.glob(os.path.join(ROOT, "project", "generated", "bf_main", "bf3_recomp.*.cpp")):
        with open(path) as f:
            text = f.read()
        i = text.find(head + " {")
        if i < 0:
            continue
        j = text.find("\n}\n", i)
        return name, text[i + len(head) + 2:j + 1]
    sys.exit(f"{name}: not found in project/generated/bf_main")


def inventory_row(addr):
    path = os.path.join(ROOT, "docs", "decomp", "v01_functions.tsv")
    if os.path.exists(path):
        for line in open(path):
            cols = line.rstrip("\n").split("\t")
            if len(cols) > 7 and cols[0] == "BF.Main" and cols[1] == f"0x{addr:08X}":
                return {"size": cols[2], "group": cols[5], "signal": cols[7]}
    return {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("addr")
    ap.add_argument("--group")
    ap.add_argument("--name")
    ap.add_argument("--out")
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args()
    addr = int(a.addr.removeprefix("sub_"), 16)
    name, body = generated_body(addr)
    row = inventory_row(addr)
    group = a.group or GROUP_DIRS.get(row.get("group", "unknown"), "unsorted")
    out = a.out or os.path.join(ROOT, "project", "src", "native", group, f"{name}.cpp")
    if os.path.exists(out) and not a.force:
        sys.exit(f"{out} exists (--force to overwrite)")
    body = body.replace("\tREX_FUNC_PROLOGUE();\n", "")
    callees = sorted(set(re.findall(r"\b(sub_[0-9A-F]{8}|__imp__sub_[0-9A-F]{8})\(ctx, base\)", body)) - {name})
    title = a.name or name
    lines = [
        f"// {title}: BF.Main guest 0x{addr:08X}" + (f", {int(row['size'])} bytes" if row.get("size") else ""),
        f"// Inventory: {row['group']} ({row['signal']})" if row else "// Inventory: not in the v0.1 hit set",
        "// Lifted from the recompiled code by scripts/decomp/lift.py; the original is __imp__" + name + ".",
        "#include \"bf3_pch.h\"",
        "",
        "#include \"native/native.h\"",
        "",
    ]
    lines += [f"REX_EXTERN({c});" for c in callees]
    lines += [
        "",
        f"BF3_NATIVE(0x{addr:08X}, lifted);",
        f"extern \"C\" REX_FUNC({name}) {{",
        "\tREX_FUNC_PROLOGUE();",
    ]
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n" + body + "}\n")
    print(f"{out}: {body.count(chr(10))} lines, {len(callees)} callees")


if __name__ == "__main__":
    main()
