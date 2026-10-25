#!/usr/bin/env python3
"""Resolve <module>+0xRVA frames from crash_trace log lines.

Usage: python -I scripts/resolve_rva.py project/out/build/win-cross-release logs/win-run.log
- bf3.exe / bf3_*.dll: lld-link /MAP files in the build dir (<module stem>.map).
- other DLLs found in the build dir (rexruntime.dll, rexgpu-xenos.dll): nearest export (llvm-readobj).
- "[crash_trace] thread <tid>" lines get the guest thread name from the runtime's
  "XThread::Execute thid N (handle=..., '<name>', native=<tid>" lines.
Adapted from the Trials HD port (single exe) for BF3's one-DLL-per-module layout.
"""
import bisect
import os
import re
import subprocess
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


def load_exports(path):
    out = subprocess.run(["llvm-readobj", "--coff-exports", path], capture_output=True, text=True).stdout
    syms = {}
    for name, rva in re.findall(r"Name: (\S+)\s+RVA: 0x([0-9A-Fa-f]+)", out):
        syms.setdefault(int(rva, 16), name)
    items = sorted(syms.items())
    return [a for a, _ in items], [n for _, n in items]


def main(build_dir, log_path):
    lines = open(log_path, errors="replace").read().splitlines()
    names = {}
    for line in lines:
        m = re.search(r"XThread::Execute thid \d+ \(handle=\w+, '([^']*)', native=([0-9A-Fa-f]+)", line)
        if m:
            names[int(m.group(2), 16)] = m.group(1)
    # Guest threads: "[t<decimal tid>] SetThreadName(N, name)" is logged by the creator and by the thread itself;
    # a thread that logs exactly one name is that thread.
    named = {}
    for line in lines:
        m = re.search(r"\[t(\d+)\] SetThreadName\(\d+, (.*)\)$", line)
        if m:
            named.setdefault(int(m.group(1)), set()).add(m.group(2))
    for tid, ns in named.items():
        if len(ns) == 1:
            names.setdefault(tid, next(iter(ns)))
    tables = {}
    for line in lines:
        if "[crash_trace]" not in line:
            continue
        m = re.search(r"\[crash_trace\] thread ([0-9A-Fa-f]+)$", line)
        if m:
            print(f"{line}  {names.get(int(m.group(1), 16), '(host thread)')}")
            continue
        m = re.search(r"([\w.\-]+\.(?:exe|dll))\+0x([0-9A-Fa-f]+)", line, re.I)
        if not m:
            print(line)
            continue
        module, rva = m.group(1), int(m.group(2), 16)
        key = module.lower()
        if key not in tables:
            stem = os.path.splitext(module)[0]
            map_path = os.path.join(build_dir, stem + ".map")
            dll_path = next((os.path.join(build_dir, f) for f in os.listdir(build_dir) if f.lower() == key), None)
            if os.path.exists(map_path):
                tables[key] = load_map(map_path)
            elif dll_path:
                tables[key] = load_exports(dll_path)
            else:
                tables[key] = ([], [])
        addrs, syms = tables[key]
        i = bisect.bisect_right(addrs, rva) - 1
        sym = f"{syms[i]}+0x{rva - addrs[i]:X}" if i >= 0 else ""
        print(f"{line}  {sym}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
