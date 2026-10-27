#!/usr/bin/env python3
"""Markdown tables for docs/DECOMP.md from the inventory (docs/decomp/v01_functions.tsv) and the coverage runs.

Usage: tables.py <run dir>...   (the same runs the inventory was built from)
"""
import collections
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    runs = sys.argv[1:]
    rows = [l.split("\t") for l in open(os.path.join(ROOT, "docs", "decomp", "v01_functions.tsv")).read()
            .splitlines()[1:]]
    nruns = int(rows[0][8].split("/")[1])
    allf = 0
    allb = 0
    for d in ("default", "bf_main", "engine_buildinfo"):
        for l in open(os.path.join(ROOT, "project", "generated", d, "bf3_functions.tsv")).read().splitlines()[1:]:
            c = l.split("\t")
            if c[4] == "code":
                allf += 1
                allb += int(c[1], 16)

    # workload per group
    g = collections.defaultdict(lambda: collections.Counter())
    for r in rows:
        t = g[r[5]]
        t["n"] += 1
        t["b"] += int(r[2])
        t[r[6]] += 1
    tot_n, tot_b = len(rows), sum(int(r[2]) for r in rows)
    print(f"Hit set: **{tot_n} functions, {tot_b / 1024:.0f} KB of guest code** ({100 * tot_n / allf:.0f}% of the "
          f"{allf} functions, {100 * tot_b / allb:.0f}% of the {allb / 1024:.0f} KB in the three modules), "
          f"union of {nruns} recorded sessions.\n")
    print("| Group | Functions | KB | high / medium / low confidence |")
    print("|---|---:|---:|---|")
    for k, t in sorted(g.items(), key=lambda kv: -kv[1]["b"]):
        print(f"| {k} | {t['n']} | {t['b'] / 1024:.0f} | {t['high']} / {t['medium']} / {t['low']} |")
    print(f"| **total** | **{tot_n}** | **{tot_b / 1024:.0f}** | |\n")

    # phases: new functions per phase in session order (first run that has them all)
    print("New functions per phase (first complete run, in session order):\n")
    files = sorted(glob.glob(os.path.join(runs[0], "*.tsv")))
    seen = set()
    out = []
    for f in files:
        s = {l.strip() for l in open(f) if l.strip()}
        new = len(s - seen)
        seen |= s
        out.append(f"{re.sub(r'^[0-9]+-', '', os.path.basename(f)[:-4])} +{new}")
    print("`" + "`, `".join(out) + "`\n")

    # rare paths
    one = [r for r in rows if r[8].startswith("1/")]
    by_phase = collections.Counter(r[9] for r in one)
    by_group = collections.Counter(r[5] for r in one)
    print(f"Rare paths: {len(one)} functions ({sum(int(r[2]) for r in one) / 1024:.0f} KB) showed up in only one of "
          f"the {nruns} sessions; by first phase: " + ", ".join(f"{p} {n}" for p, n in by_phase.most_common(8))
          + "; by group: " + ", ".join(f"{p} {n}" for p, n in by_group.most_common(6)) + ".\n")

    # reflection coverage
    refl = json.load(open(os.path.join(ROOT, "logs", "decomp", "reflection.json")))
    fielded = {x["name"] for x in refl if x.get("fields")}
    named = {x["name"] for x in refl if x["kind"] in ("class", "class29")}
    c = collections.Counter()
    for r in rows:
        sig = r[7]
        if sig.startswith(("class:", "vtable:")):
            cls = sig.split(":", 1)[1].split("+")[0]
            c["fields" if cls in fielded else "named" if cls in named else "other"] += 1
    print(f"Reflection coverage: {c['fields']} hit functions ({100 * c['fields'] / tot_n:.1f}%) belong to a reflected "
          f"class with a full field layout, {c['named']} ({100 * c['named'] / tot_n:.1f}%) to a runtime class known by "
          f"name, size and super class only.")


if __name__ == "__main__":
    main()
