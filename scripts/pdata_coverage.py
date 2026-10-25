#!/usr/bin/env python3
"""Check that every .pdata function of each module is a function in a Ghidra export.

  python -I scripts/pdata_coverage.py logs/merged-functions.txt [--missing out.txt]

The export comes from scripts/ghidra/BF3ExportFunctions.java ("<space>:<addr> <size> <name>" lines).
Images are the rex_extract flat images in dump/Disc 1/. BF.Splash lives in overlay space BF_Splash.
"""
import argparse
import struct

MODULES = [  # (module, address-space prefix in the Ghidra export)
    ("Default", "ram:"),
    ("BF.Main", "ram:"),
    ("Engine.BuildInfo", "ram:"),
    ("BF.Splash", "BF_Splash::"),
]


def pdata_entries(image: bytes):
    """Yield (begin_address, length_in_bytes) for each .pdata record of a flat Xbox 360 PE image."""
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    nsec = struct.unpack_from("<H", image, pe + 6)[0]
    opt = struct.unpack_from("<H", image, pe + 20)[0]
    for i in range(nsec):
        sec = pe + 24 + opt + i * 40
        if image[sec:sec + 8].rstrip(b"\0") == b".pdata":
            vsize, va = struct.unpack_from("<II", image, sec + 8)
            break
    else:
        return
    for off in range(va, va + vsize, 8):
        begin, info = struct.unpack_from(">II", image, off)
        if begin:
            yield begin, ((info >> 8) & 0x3FFFFF) * 4


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("export")
    ap.add_argument("--missing", help="write missing entry addresses here")
    a = ap.parse_args()

    have = {line.split(" ", 1)[0] for line in open(a.export)}
    missing_out = open(a.missing, "w") if a.missing else None
    total = total_missing = 0
    for module, space in MODULES:
        image = open(f"dump/Disc 1/{module}.image.bin", "rb").read()
        entries = list(pdata_entries(image))
        missing = [b for b, _ in entries if f"{space}{b:08x}" not in have]
        total += len(entries)
        total_missing += len(missing)
        print(f"{module:17} .pdata {len(entries):6}  missing {len(missing):5}")
        if missing_out:
            missing_out.writelines(f"{space}{b:08x}\n" for b in missing)
    print(f"{'total':17} .pdata {total:6}  missing {total_missing:5}  ({len(have)} functions in export)")


if __name__ == "__main__":
    main()
