#!/usr/bin/env python3
"""Untile an Xbox 360 2D texture (32bpp) to PNG: scripts/x360_untile.py <raw> <width> <height> <out.png> [order]
order: channel order of each big-endian texel, default ARGB."""
import struct, sys, zlib

def tiled_offset(x, y, width, log_bpp):
    aligned = (width + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (log_bpp + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << log_bpp
    off = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((off & ~0x1FF) << 3) + ((y & 16) << 7) + ((off & 0x1C0) << 2) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (off & 0x3F)) >> log_bpp

def png(path, w, h, rgba):
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    def chunk(t, data):
        return struct.pack(">I", len(data)) + t + data + struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))

def main():
    src, w, h, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    order = sys.argv[5] if len(sys.argv) > 5 else "ARGB"
    d = open(src, "rb").read()
    rgba = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            t = tiled_offset(x, y, w, 2) * 4
            px = dict(zip(order, d[t:t + 4]))
            rgba[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes((px["R"], px["G"], px["B"], px["A"]))
    png(out, w, h, bytes(rgba))

if __name__ == "__main__":
    main()
