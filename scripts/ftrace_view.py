#!/usr/bin/env python3
"""Decode a GPU frame trace (rexgpu-xenos gpu_frame_trace_at/_dir): for every traced resolve, untile its dump and
print channel statistics; with an output directory, also write one PNG per resolve (floats auto-scaled).
Usage: scripts/ftrace_view.py <win-run.log> <dump dir> [png out dir]"""
import os, re, struct, sys, zlib
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x360_untile import tiled_offset, png

# xenos::ColorFormat -> (name, bytes per texel, numpy channel dtype, channels)
FORMATS = {
    2: ("8", 1, ">u1", 1), 4: ("565", 2, None, 1), 6: ("8888", 4, ">u1", 4), 7: ("2_10_10_10", 4, None, 1),
    10: ("8_8", 2, ">u1", 2), 16: ("10_11_11", 4, None, 1), 17: ("11_11_10", 4, None, 1),
    24: ("16", 2, ">u2", 1), 25: ("16_16", 4, ">u2", 2), 26: ("16_16_16_16", 8, ">u2", 4),
    30: ("16F", 2, ">f2", 1), 31: ("16_16F", 4, ">f2", 2), 32: ("16_16_16_16F", 8, ">f2", 4),
    36: ("32F", 4, ">f4", 1), 37: ("32_32F", 8, ">f4", 2), 38: ("32_32_32_32F", 16, ">f4", 4),
}

def swap_endian(raw, endian):
    a = np.frombuffer(raw, dtype=np.uint8)
    n = len(a) & ~3
    if endian == 1:  # 8in16
        return a[: n & ~1].reshape(-1, 2)[:, ::-1].tobytes()
    if endian == 2:  # 8in32
        return a[:n].reshape(-1, 4)[:, ::-1].tobytes()
    if endian == 3:  # 16in32
        return a[:n].reshape(-1, 2, 2)[:, ::-1, :].tobytes()
    return raw

DIGITS = ["111101101101111", "010110010010111", "111001111100111", "111001111001111", "101101111001001",
          "111100111001111", "111100111101111", "111001001001001", "111101111101111", "111101111001111"]

def label(img, text, scale=3):
    """Burn a number into the top-left corner of an HxWx4 uint8 image (3x5 pixel digits)."""
    x0 = 2
    img[: 5 * scale + 4, : len(text) * 4 * scale + 4] = (0, 0, 0, 255)
    for ch in text:
        bits = DIGITS[int(ch)]
        for i, b in enumerate(bits):
            if b == "1":
                y, x = 2 + (i // 3) * scale, x0 + (i % 3) * scale
                img[y:y + scale, x:x + scale] = (255, 255, 0, 255)
        x0 += 4 * scale

def thumb(rgba, tw=256, th=144):
    h, w = rgba.shape[:2]
    ys = (np.arange(th) * h // th).clip(0, h - 1)
    xs = (np.arange(tw) * w // tw).clip(0, w - 1)
    return rgba[ys][:, xs]

def main():
    log, dumps = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else None
    if out:
        os.makedirs(out, exist_ok=True)
    files = {}
    for f in os.listdir(dumps):
        m = re.match(r"s(\d+)_r(\d+)_([0-9A-F]{8})\.bin$", f)
        if m:
            files[(int(m[1]), int(m[2]))] = (int(m[3], 16), os.path.join(dumps, f))
    swap = None
    pending = None
    thumbs = []
    for line in open(log, errors="replace"):
        m = re.search(r"ftrace begin at swap (\d+)", line)
        if m:
            swap = int(m[1])
            continue
        m = re.search(r"ftrace swap (\d+)", line)
        if m:
            swap = int(m[1])
            continue
        m = re.search(r"ftrace R(\d+) ctl (\w+) destinfo (\w+) base (\w+) pitch (\w+)", line)
        if m:
            pending = [int(m[1])] + [int(m[i], 16) for i in range(2, 6)]
            continue
        if pending is None or "ftrace   ->" not in line:
            continue
        idx, ctl, info, base, pitch = pending
        pending = None
        fmt = (info >> 7) & 0x3F
        endian = info & 7
        w, h = pitch & 0x3FFF, (pitch >> 16) & 0x3FFF
        name, bpp, dt, ch = FORMATS.get(fmt, (f"fmt{fmt}", 4, None, 1))
        key = (swap, idx)
        desc = f"s{swap} R{idx:03d} {base:08X} {w}x{h} {name} endian {endian} ctl {ctl:08X}"
        if key not in files:
            print(desc, "(no dump)")
            continue
        addr, path = files[key]
        raw = open(path, "rb").read()  # guest memory: already big-endian per texel
        start = base - addr
        if not w or not h or start < 0:
            print(desc, "(bad geometry)")
            continue
        log_bpp = {1: 0, 2: 1, 4: 2, 8: 3, 16: 4}[bpp]
        ys, xs = np.mgrid[0:h, 0:w]
        offs = start + tiled_offset(xs, ys, w, log_bpp).astype(np.int64) * bpp
        if offs.max() + bpp > len(raw):  # the written extent can stop before the last tile rows
            raw = raw + bytes(int(offs.max()) + bpp - len(raw))
        buf = np.frombuffer(raw, dtype=np.uint8)
        texels = buf[(offs[..., None] + np.arange(bpp)).reshape(-1)].reshape(h, w, bpp)
        if dt is None:
            vals = texels.copy().view(">u4" if bpp == 4 else ">u2").reshape(h, w, 1).astype(np.float64)
        else:
            vals = np.ascontiguousarray(texels).view(dt).reshape(h, w, ch).astype(np.float64)
        vals = np.nan_to_num(vals, nan=0, posinf=65504, neginf=-65504)
        stats = " ".join(f"c{c}[{vals[..., c].min():.3g},{vals[..., c].mean():.3g},{vals[..., c].max():.3g}]"
                         for c in range(vals.shape[2]))
        print(desc, stats)
        if out:
            v = vals[..., :3] if vals.shape[2] >= 3 else np.repeat(vals[..., :1], 3, axis=2)
            if name == "8888":
                # big-endian ARGB words, after endian swap -> bytes A R G B
                v = vals[..., [1, 2, 3]]
            hi = np.percentile(v, 99.5) or 1.0
            lo = min(v.min(), 0)
            img = np.clip((v - lo) / (hi - lo) * 255, 0, 255).astype(np.uint8)
            rgba = np.concatenate([img, np.full((h, w, 1), 255, np.uint8)], axis=2)
            png(os.path.join(out, f"s{swap}_r{idx:03d}_{base:08X}_{name}.png"), w, h, rgba.tobytes())
            t = thumb(rgba.reshape(h, w, 4)).copy()
            label(t, str(idx))
            thumbs.append(t)
    if out and thumbs:
        cols = 8
        rows = (len(thumbs) + cols - 1) // cols
        sheet = np.zeros((rows * 146, cols * 258, 4), np.uint8)
        sheet[..., 3] = 255
        for i, t in enumerate(thumbs):
            y, x = (i // cols) * 146, (i % cols) * 258
            sheet[y:y + 144, x:x + 256] = t
        png(os.path.join(out, "sheet.png"), sheet.shape[1], sheet.shape[0], sheet.tobytes())

if __name__ == "__main__":
    main()
