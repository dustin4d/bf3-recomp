#!/usr/bin/env python3
"""Per-function cross references from the recompiled C++ (ReXGlue writes each guest instruction as a comment), so the
decompilation tooling reuses the codegen's own function boundaries and disassembly instead of decoding PowerPC again.

For every function of a module: direct calls (bl), branches that leave the function (tail calls: b/bc to another
function's start), and constant addresses built with lis + addi/ori/load/store (strings, vtables, globals, type info).

Usage: xrefs.py [module=bf_main] [out=logs/decomp/xrefs_<module>.json]
Output: {"0x8346D590": {"calls": [...], "tails": [...], "refs": [...], "insns": n}, ...} (addresses as ints).
"""
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

FUNC = re.compile(r"^DEFINE_REX_FUNC\((\w+)\)")
INSN = re.compile(r"^\t// (\S+) ?(.*)$")
HIT = re.compile(r"^\tREX_COVERAGE_HIT\(0x([0-9A-F]+)\)")
REG = re.compile(r"r(\d+)")
# base+displacement operands: "lwz r3,-1234(r11)"
DISP = re.compile(r"^(?:r|f|v)\d+,(-?\d+)\(r(\d+)\)$")
IMM_OPS = {"addi", "ori", "addic", "la"}


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v >= 0x8000 else v


def parse_module(module):
    funcs = {}
    names = {}
    for path in sorted(glob.glob(os.path.join(ROOT, "project", "generated", module, "bf3_recomp.*.cpp"))):
        cur = None
        his = {}  # register -> (high part << 16, instructions left)
        with open(path, errors="replace") as f:
            for line in f:
                if line.startswith("DEFINE_REX_FUNC("):
                    m = FUNC.match(line)
                    name = m.group(1)
                    cur = {"calls": set(), "branches": set(), "refs": set(), "insns": 0}
                    his = {}
                    # address from the name (sub_X) or the coverage hit that follows
                    key = int(name[4:], 16) if name.startswith("sub_") else None
                    pending_name = name
                    if key is not None:
                        funcs[key] = cur
                        names[key] = name
                    continue
                if cur is None:
                    continue
                if line.startswith("\tREX_COVERAGE_HIT(") and pending_name and not pending_name.startswith("sub_"):
                    key = int(HIT.match(line).group(1), 16)
                    funcs[key], names[key] = cur, pending_name
                    pending_name = None
                    continue
                if not line.startswith("\t// "):
                    continue
                m = INSN.match(line)
                if not m:
                    continue
                op, args = m.group(1), m.group(2)
                cur["insns"] += 1
                if op == "bl":
                    cur["calls"].add(int(args, 16))
                elif op == "b" or (op.startswith("b") and "0x" in args and op not in ("bl",)):
                    t = args.rsplit(",", 1)[-1]
                    if t.startswith("0x"):
                        cur["branches"].add(int(t, 16))
                if op == "lis":
                    r, v = args.split(",")
                    his[r] = ((int(v) & 0xFFFF) << 16, 12)
                    continue
                parts = args.split(",")
                if op in IMM_OPS and len(parts) == 3 and parts[1] in his:
                    addr = (his[parts[1]][0] + (s16(int(parts[2])) if op != "ori" else int(parts[2]))) & 0xFFFFFFFF
                    cur["refs"].add(addr)
                    if parts[0] != parts[1]:
                        his.pop(parts[0], None)
                    continue
                dm = DISP.match(args) if "(" in args else None
                if dm and "r" + dm.group(2) in his:
                    cur["refs"].add((his["r" + dm.group(2)][0] + int(dm.group(1))) & 0xFFFFFFFF)
                # age the pending high halves; a write to the register ends it
                dst = parts[0] if parts and REG.fullmatch(parts[0] or "") else None
                his = {r: (v, k - 1) for r, (v, k) in his.items() if k > 1 and r != dst}
    return funcs, names


def main():
    module = sys.argv[1] if len(sys.argv) > 1 else "bf_main"
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "logs", "decomp", f"xrefs_{module}.json")
    funcs, names = parse_module(module)
    starts = set(funcs)
    result = {}
    for a, f in funcs.items():
        result[a] = {"name": names[a], "calls": sorted(f["calls"]),
                     "tails": sorted(t for t in f["branches"] if t in starts and t != a),
                     "refs": sorted(f["refs"]), "insns": f["insns"]}
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w") as f:
        json.dump(result, f)
    print(f"{out}: {len(result)} functions, {sum(len(v['calls']) for v in result.values())} calls, "
          f"{sum(len(v['refs']) for v in result.values())} constant refs")


if __name__ == "__main__":
    main()
