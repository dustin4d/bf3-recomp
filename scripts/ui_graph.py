#!/usr/bin/env python3
"""Follow a Frostbite UI flow graph (an ebx_dump.py text dump) from a named output port.
Usage: scripts/ui_graph.py <dump.txt> <port name> [depth]"""
import re, sys
from collections import defaultdict

text = open(sys.argv[1]).read()
start, depth_max = sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 8
inst = {}
for block in re.split(r"\n(?=\[\d+\] )", text):
    m = re.match(r"\[(\d+)\] (\w+)", block)
    if m:
        inst[int(m.group(1))] = (m.group(2), block)

def field(i, name):
    m = re.search(rf"\n  {name} = (.*)", inst[i][1])
    return m.group(1) if m else None

def ref(v):
    m = re.match(r"-> \[(\d+)\]", v or "")
    return int(m.group(1)) if m else None

def label(i):
    t, b = inst[i]
    name = field(i, "Name") or ""
    extra = ""
    for k in ("ActionKey", "Id", "Param", "DataKey"):
        v = field(i, k)
        if v and v not in ("0", "''"):
            extra += f" {k}={v}"
    m = re.search(r"\n  Screen = (.*)", b)
    return f"[{i}] {t} {name}{extra}"

conns = defaultdict(list)
for i, (t, b) in inst.items():
    if t == "UINodeConnection":
        s, tn, sp, tp = (ref(field(i, k)) for k in ("SourceNode", "TargetNode", "SourcePort", "TargetPort"))
        conns[sp].append((tn, tp, s))

ports = [i for i, (t, b) in inst.items() if t == "UINodePort" and field(i, "Name") == repr(start)]
seen = set()

def node_out_ports(n):
    # ports owned by a node: any port used as SourcePort in a connection whose SourceNode is n
    return {sp for sp, lst in conns.items() for (_, _, s) in lst if s == n}

def walk(port, depth, indent):
    for tn, tp, s in conns.get(port, []):
        key = (tn, tp)
        print("  " * indent + f"--{field(port, 'Name')}--> {label(tn)} . {field(tp, 'Name') if tp is not None else ''}")
        if key in seen or depth >= depth_max:
            continue
        seen.add(key)
        for op in sorted(node_out_ports(tn)):
            walk(op, depth + 1, indent + 1)

for p in ports:
    owner = {s for (_, _, s) in conns.get(p, [])}
    print(f"port [{p}] {start} on {[label(o) for o in owner]}")
    walk(p, 0, 1)
