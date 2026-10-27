#!/usr/bin/env python3
"""v0.1 decompilation progress: how much of the Metro TDM hit set (docs/decomp/v01_functions.tsv) is native.

Native functions are the BF3_NATIVE(<addr>, <stage>) markers in project/src/native/ (native.h). A function counts
for its stage and every stage below it; the headline metric is the share at "verified" (idiomatic and passing the
differential tests), by function count and by guest code bytes, overall and per subsystem group.
Phase-1 hooks (project/src/bf_main/*.cpp overriding sub_X to patch behaviour) are listed separately: they are fixes
around recompiled code, not decompiled code.

Usage: progress.py [--json]
"""
import collections
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
STAGES = ["lifted", "typed", "idiomatic", "verified"]


def main():
    rows = {}
    for line in open(os.path.join(ROOT, "docs", "decomp", "v01_functions.tsv")).read().splitlines()[1:]:
        c = line.split("\t")
        rows[(c[0], int(c[1], 16))] = {"size": int(c[2]), "group": c[5]}
    native = {}
    for path in glob.glob(os.path.join(ROOT, "project", "src", "native", "**", "*.cpp"), recursive=True):
        for m in re.finditer(r"BF3_NATIVE\(0x([0-9A-Fa-f]{8}),\s*(\w+)\)", open(path).read()):
            native[("BF.Main", int(m.group(1), 16))] = m.group(2)
    hooks = set()
    for path in glob.glob(os.path.join(ROOT, "project", "src", "bf_main", "*.cpp")):
        hooks.update(int(a, 16) for a in re.findall(r"REX_FUNC\(sub_([0-9A-F]{8})\)", open(path).read()))

    total_n, total_b = len(rows), sum(r["size"] for r in rows.values())
    by_stage = {s: [0, 0] for s in STAGES}
    by_group = collections.defaultdict(lambda: {"functions": 0, "bytes": 0, **{s: 0 for s in STAGES}})
    for k, r in rows.items():
        g = by_group[r["group"]]
        g["functions"] += 1
        g["bytes"] += r["size"]
        st = native.get(k)
        if st in STAGES:
            for s in STAGES[:STAGES.index(st) + 1]:
                by_stage[s][0] += 1
                by_stage[s][1] += r["size"]
                g[s] += 1
    out = {
        "hit_set": {"functions": total_n, "bytes": total_b},
        "stages": {s: {"functions": n, "bytes": b, "pct_functions": round(100 * n / total_n, 3),
                       "pct_bytes": round(100 * b / total_b, 3)} for s, (n, b) in by_stage.items()},
        "groups": by_group,
        "native_outside_hit_set": sorted(f"0x{a:08X}" for (m, a) in native if (m, a) not in rows),
        "phase1_hooks_in_hit_set": sum(1 for a in hooks if ("BF.Main", a) in rows),
    }
    if "--json" in sys.argv:
        print(json.dumps(out, indent=1, default=dict))
        return
    print(f"v0.1 hit set: {total_n} functions, {total_b / 1024:.0f} KB of guest code")
    for s, v in out["stages"].items():
        print(f"  {s:10} {v['functions']:6} functions ({v['pct_functions']:.2f}%)  {v['bytes'] / 1024:7.1f} KB "
              f"({v['pct_bytes']:.2f}%)")
    print(f"  phase-1 hooks (patches, not decompiled): {out['phase1_hooks_in_hit_set']}")
    for g, v in sorted(by_group.items(), key=lambda kv: -kv[1]["bytes"]):
        print(f"  {g:16} {v['functions']:6} functions {v['bytes'] / 1024:7.0f} KB  lifted {v['lifted']}"
              f"  verified {v['verified']}")


if __name__ == "__main__":
    main()
