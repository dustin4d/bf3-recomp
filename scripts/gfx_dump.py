#!/usr/bin/env python3
"""Dump the tags of an uncompressed Scaleform GFX/SWF movie (header 'GFX'/'FWS'), with bitmap-fill matrices of
DefineShape records and symbol exports. Usage: scripts/gfx_dump.py <file>"""
import struct, sys

class Bits:
    def __init__(s, d, p): s.d, s.p, s.b = d, p, 0
    def u(s, n):
        v = 0
        for _ in range(n):
            v = v << 1 | (s.d[s.p] >> (7 - s.b)) & 1
            s.b += 1
            if s.b == 8: s.b, s.p = 0, s.p + 1
        return v
    def s(s_, n):
        v = s_.u(n)
        return v - (1 << n) if n and v >> (n - 1) else v
    def align(s):
        if s.b: s.b, s.p = 0, s.p + 1

def matrix(bt):
    sx = sy = 1.0; r0 = r1 = 0.0
    if bt.u(1):
        n = bt.u(5); sx = bt.s(n) / 65536; sy = bt.s(n) / 65536
    if bt.u(1):
        n = bt.u(5); r0 = bt.s(n) / 65536; r1 = bt.s(n) / 65536
    n = bt.u(5); tx = bt.s(n) / 20; ty = bt.s(n) / 20
    return sx, r0, r1, sy, tx, ty

def main():
    d = open(sys.argv[1], "rb").read()
    if d[:1] in (b"C",):  # CFX/CWS: zlib after the 8-byte header
        import zlib
        d = d[:8] + zlib.decompress(d[8:])
    print("magic", d[:3], "version", d[3], "length", struct.unpack_from("<I", d, 4)[0])
    bt = Bits(d, 8); n = bt.u(5); [bt.s(n) for _ in range(4)]; bt.align()
    p = bt.p + 4
    while p < len(d):
        code_len = struct.unpack_from("<H", d, p)[0]; p += 2
        code, ln = code_len >> 6, code_len & 0x3F
        if ln == 0x3F: ln = struct.unpack_from("<I", d, p)[0]; p += 4
        body = d[p:p + ln]
        info = ""
        if code in (2, 22, 32, 83):  # DefineShape 1-4
            sid = struct.unpack_from("<H", body, 0)[0]
            b2 = Bits(body, 2); n = b2.u(5); bounds = [b2.s(n) / 20 for _ in range(4)]; b2.align()
            q = b2.p
            if code == 83:
                b2 = Bits(body, q); n = b2.u(5); [b2.s(n) for _ in range(4)]; b2.align(); q = b2.p + 1
            cnt = body[q]; q += 1
            if cnt == 0xFF: cnt = struct.unpack_from("<H", body, q)[0]; q += 2
            fills = []
            for _ in range(cnt):
                ft = body[q]; q += 1
                if ft == 0:
                    q += 4 if code == 32 or code == 83 else 3
                elif ft in (0x40, 0x41, 0x42, 0x43):
                    bid = struct.unpack_from("<H", body, q)[0]; q += 2
                    b3 = Bits(body, q); m = matrix(b3); b3.align(); q = b3.p
                    fills.append(("bitmap", bid, tuple(round(v, 4) for v in m), q))
                else:
                    fills.append(("type", hex(ft))); break
            info = f"shape {sid} bounds {bounds} fills {fills}"
        elif code == 56:  # ExportAssets
            n = struct.unpack_from("<H", body, 0)[0]; q = 2; ex = []
            for _ in range(n):
                cid = struct.unpack_from("<H", body, q)[0]; q += 2
                e = body.index(b"\0", q); ex.append((cid, body[q:e].decode())); q = e + 1
            info = f"exports {ex}"
        elif code == 39:
            sid, frames = struct.unpack_from("<HH", body, 0); info = f"sprite {sid} frames {frames}"
        elif code in (1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009):
            info = body[:64].hex()
        print(f"@{p - 2:#06x} tag {code:4} len {ln:5} {info}")
        p += ln
        if code == 0: break

main()
