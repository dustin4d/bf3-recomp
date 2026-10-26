#!/usr/bin/env python3
"""Finds code that builds a guest address with lis + addi/lwz/stw/lfs... (Ghidra often misses these references).

Searches the recompiled C++ (project/generated/<module>/bf3_recomp.*.cpp) for "lis rX,<hi>" followed within a few
instructions by an instruction using rX with the low half, and prints the enclosing function.
Usage: scripts/addr_refs.py <hex addr> [module=bf_main] [window=24]
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    addr = int(sys.argv[1], 16)
    module = sys.argv[2] if len(sys.argv) > 2 else "bf_main"
    window = int(sys.argv[3]) if len(sys.argv) > 3 else 24
    lo = addr & 0xFFFF
    lo_s = lo - 0x10000 if lo >= 0x8000 else lo
    hi = ((addr - lo_s) >> 16) & 0xFFFF
    hi_s = hi - 0x10000 if hi >= 0x8000 else hi
    lis = re.compile(rf"// lis (r\d+),{hi_s}$")
    func = re.compile(r"^(?:DEFINE_REX_FUNC|PPC_FUNC_IMPL)\((?:__imp__)?(sub_[0-9A-F]+)\)")
    insn = re.compile(r"^\s*// (\S+) (.*)$")
    for path in sorted(glob.glob(os.path.join(ROOT, "project", "generated", module, "bf3_recomp.*.cpp"))):
        current = "?"
        pending = []  # (register, instructions left)
        with open(path, errors="replace") as f:
            for n, line in enumerate(f, 1):
                m = func.match(line)
                if m:
                    current, pending = m.group(1), []
                    continue
                m = insn.match(line)
                if not m:
                    continue
                op, operands = m.group(1), m.group(2)
                lm = lis.match("// " + op + " " + operands)
                hits = [r for r, _ in pending if re.search(rf"\b{r}\b", operands) and
                        re.search(rf"(,|\(|^){lo_s}(\(|$|,)", operands)]
                for r in hits:
                    print(f"{current}  {os.path.basename(path)}:{n}  {op} {operands}")
                pending = [(r, k - 1) for r, k in pending if k > 1 and r not in hits]
                if lm:
                    pending.append((lm.group(1), window))


if __name__ == "__main__":
    main()
