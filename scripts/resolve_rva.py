#!/usr/bin/env python3
"""Resolve <module>+0xRVA frames from crash_trace log lines against lld-link /MAP files.

Usage: python -I scripts/resolve_rva.py project/out/build/win-cross-release logs/win-run.log
Each module (bf3.exe, bf3_BF_Main.dll, ...) is resolved with <build dir>/<module stem>.map.
Adapted from the Trials HD port (single exe) for BF3's one-DLL-per-module layout.
"""
import bisect
import os
import re
import sys


def load_map(path):
    """Parse an MSVC-style map (lld-link /MAP): ' 0001:0005a3b0  sym  000000014005b3b0  obj'."""
    base, syms = 0, {}
    for line in open(path, errors="replace"):
        m = re.search(r"Preferred load address is ([0-9a-fA-F]+)", line)
        if m:
            base = int(m.group(1), 16)
        m = re.match(r"\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s", line)
        if m:
            syms.setdefault(int(m.group(2), 16), m.group(1))  # first name wins (sub_X over __imp__sub_X)
    items = sorted(syms.items())
    return [a - base for a, _ in items], [n for _, n in items]


def main(build_dir, log_path):
    maps = {}
    for line in open(log_path, errors="replace"):
        if "[crash_trace]" not in line:
            continue
        m = re.search(r"(bf3[\w.]*?\.(?:exe|dll))\+0x([0-9A-Fa-f]+)", line, re.I)
        if not m:
            print(line.rstrip())
            continue
        module, rva = m.group(1), int(m.group(2), 16)
        if module not in maps:
            path = os.path.join(build_dir, os.path.splitext(module)[0] + ".map")
            maps[module] = load_map(path) if os.path.exists(path) else ([], [])
        addrs, names = maps[module]
        i = bisect.bisect_right(addrs, rva) - 1
        sym = f"{names[i]}+0x{rva - addrs[i]:X}" if i >= 0 else "? (no map)"
        print(f"{line.rstrip()}  {sym}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
