#!/usr/bin/env python3
"""Read-only XDVDFS (Xbox 360 game partition) reader for full disc images.

  xdvdfs.py list    <iso> [--dirs-only] [--depth N]
  xdvdfs.py extract <iso> <out_dir> [path ...]   # no paths = everything

The game partition offset is auto-detected (XGD3 0x2080000, XGD2 0xFD90000, XGD1 0x18300000, plain 0).
"""
import argparse
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
PARTITIONS = {"XGD3": 0x2080000, "XGD2": 0xFD90000, "XGD1": 0x18300000, "plain": 0}
ATTR_DIR = 0x10


class Xdvdfs:
    def __init__(self, path):
        self.f = open(path, "rb")
        for name, off in PARTITIONS.items():
            self.f.seek(off + 32 * SECTOR)
            vd = self.f.read(0x800)
            if vd[:20] == MAGIC and vd[0x7EC:0x800] == MAGIC:
                self.kind, self.base = name, off
                self.root_sector, self.root_size = struct.unpack_from("<II", vd, 20)
                return
        raise ValueError(f"{path}: no XDVDFS volume descriptor found")

    def read(self, sector, size):
        self.f.seek(self.base + sector * SECTOR)
        return self.f.read(size)

    def entries(self, sector, size):
        """Yield (name, attr, sector, size) for one directory, walking its binary tree."""
        if size == 0:
            return
        data = self.read(sector, size)
        stack, seen = [0], set()
        while stack:
            off = stack.pop()
            if off in seen or off + 14 > len(data):
                continue
            seen.add(off)
            left, right, start, length, attr, nlen = struct.unpack_from("<HHIIBB", data, off)
            if left == 0xFFFF and right == 0xFFFF:
                continue
            name = data[off + 14:off + 14 + nlen].decode("latin-1")
            yield name, attr, start, length
            for child in (left, right):
                if child:
                    stack.append(child * 4)

    def walk(self, sector=None, size=None, prefix=""):
        if sector is None:
            sector, size = self.root_sector, self.root_size
        for name, attr, start, length in sorted(self.entries(sector, size), key=lambda e: e[0].lower()):
            path = f"{prefix}{name}"
            is_dir = bool(attr & ATTR_DIR)
            yield path, is_dir, start, length
            if is_dir:
                yield from self.walk(start, length, path + "/")

    def extract_file(self, start, length, dest):
        os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
        self.f.seek(self.base + start * SECTOR)
        with open(dest, "wb") as out:
            remaining = length
            while remaining:
                chunk = self.f.read(min(remaining, 16 << 20))
                if not chunk:
                    raise IOError(f"short read extracting {dest}")
                out.write(chunk)
                remaining -= len(chunk)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    ls = sub.add_parser("list")
    ls.add_argument("iso")
    ls.add_argument("--dirs-only", action="store_true")
    ls.add_argument("--depth", type=int, default=-1)
    ex = sub.add_parser("extract")
    ex.add_argument("iso")
    ex.add_argument("out_dir")
    ex.add_argument("paths", nargs="*")
    a = ap.parse_args()

    fs = Xdvdfs(a.iso)
    print(f"# {fs.kind} partition at {fs.base:#x}", file=sys.stderr)
    if a.cmd == "list":
        files = total = 0
        for path, is_dir, _, length in fs.walk():
            depth = path.count("/")
            if not is_dir:
                files += 1
                total += length
            if (a.depth < 0 or depth <= a.depth) and (is_dir or not a.dirs_only):
                print(f"{'<DIR>' if is_dir else length:>12}  {path}{'/' if is_dir else ''}")
        print(f"# {files} files, {total} bytes", file=sys.stderr)
    else:
        wanted = {p.replace("\\", "/").lower() for p in a.paths}
        for path, is_dir, start, length in fs.walk():
            if is_dir or (wanted and path.lower() not in wanted):
                continue
            fs.extract_file(start, length, os.path.join(a.out_dir, path))
            print(f"{length:>12}  {path}")


if __name__ == "__main__":
    main()
