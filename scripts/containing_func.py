#!/usr/bin/env python3
"""Print the recompiled function containing each guest address (from generated/*/bf3_init.cpp).
Usage: scripts/containing_func.py 0x831ccd04 [...]"""
import bisect, glob, os, re, sys
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
starts = sorted({int(m, 16) for f in glob.glob(f"{root}/project/generated/*/bf3_init.cpp")
                 for m in re.findall(r"\{ 0x([0-9A-F]{8}), sub_", open(f).read())})
for a in sys.argv[1:]:
    v = int(a, 16); i = bisect.bisect_right(starts, v) - 1
    print(f"{v:08X} -> sub_{starts[i]:08X} (+0x{v - starts[i]:X})")
