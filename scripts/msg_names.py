#!/usr/bin/env python3
"""Annotate Frostbite message hashes (djb2-xor of the class name, e.g. ServerLevelLoadedMessage -> 236C395A) in a log.
Usage: scripts/msg_names.py <log>    (reads names from the BF.Main image)"""
import os, re, sys
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
image = open(os.path.join(root, "dump/Disc 1/BF.Main.image.bin"), "rb").read()

def djbx(s):
    h = 5381
    for c in s.encode():
        h = ((h * 33) ^ c) & 0xFFFFFFFF
    return h

names = {}
for m in set(re.findall(rb"[A-Za-z_][A-Za-z0-9_]{2,80}", image)):
    n = m.decode()
    names.setdefault(djbx(n), n)
for line in open(sys.argv[1], errors="replace"):
    line = re.sub(r"\b([0-9A-F]{8})\b", lambda m: f"{m.group(1)}({names[int(m.group(1), 16)]})"
                  if int(m.group(1), 16) in names else m.group(1), line.rstrip())
    print(line)
