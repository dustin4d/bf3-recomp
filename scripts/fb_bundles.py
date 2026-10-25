#!/usr/bin/env python3
"""List / extract entries of a Frostbite 2 console superbundle (.sb, binary bundle magic 0x970D1C13, big-endian).

  scripts/fb_bundles.py <file.sb> list [filter]
  scripts/fb_bundles.py <file.sb> extract <out_dir> [filter]

Bundle: u32 meta_size; meta {u32 magic, total, ebx_count, res_count, chunk_count, strings_off, chunk_meta_off,
chunk_meta_size; sha1[total]; {name_off, packed_size, size}[ebx + res]; res_type u32[res]; res_meta 16[res];
chunk {guid16, u32, packed_size, u32}[chunk] (payload: blocks); ...strings (offsets relative to the meta)}. Payload (ebx, res, chunks in
order, each 16-byte aligned) follows the meta: raw when packed_size == size, else blocks {u32 size, u16 type (0 = zlib), u16 packed}. Read-only; output goes where you point it (never commit it)."""
import os, re, struct, sys, zlib

def unpack(blob, size):
    if len(blob) == size:
        return blob
    out, p = bytearray(), 0
    while p + 8 <= len(blob) and (size is None or len(out) < size):
        usize, ctype, csize = struct.unpack_from(">IHH", blob, p)
        body = blob[p + 8:p + 8 + csize]
        try:
            out += zlib.decompress(body) if ctype == 0 and body[:1] == b"\x78" else body
        except zlib.error:
            out += body  # unknown packing: keep the raw block
        p += 8 + csize
    return bytes(out)

def bundles(d):
    p = 0
    while True:
        p = d.find(b"\x97\x0d\x1c\x13", p)
        if p < 4: return
        b = p - 4
        meta = struct.unpack_from(">I", d, b)[0]
        m = b + 4
        _, total, ne, nr, nc, soff, cmo, cms = struct.unpack_from(">8I", d, m)
        q = m + 32 + 20 * total
        ents = [struct.unpack_from(">3I", d, q + 12 * i) for i in range(ne + nr)]; q += 12 * (ne + nr)
        rtype = [struct.unpack_from(">I", d, q + 4 * i)[0] for i in range(nr)]; q += 4 * nr + 16 * nr
        chunks = [(d[q + 28 * i:q + 28 * i + 16].hex(),) + struct.unpack_from(">III", d, q + 28 * i + 16)
                  for i in range(nc)]
        def name(off):
            s = m + soff + off
            return d[s:d.index(b"\0", s)].decode("latin1")
        items = [("ebx" if i < ne else "res", name(o), packed, size, rtype[i - ne] if i >= ne else None)
                 for i, (o, packed, size) in enumerate(ents)]
        items += [("chunk", g, packed, None, None) for g, _, packed, _ in chunks]
        pos = m + meta
        for kind, n, packed, size, t in items:
            pos = (pos + 15) & ~15  # entries start 16-byte aligned
            blob = d[pos:pos + packed]
            pos += packed
            yield kind, n, size, t, (lambda blob=blob, size=size: unpack(blob, size))
        p = pos

def main():
    path, cmd = sys.argv[1], sys.argv[2]
    d = open(path, "rb").read()
    flt = re.compile(sys.argv[4] if cmd == "extract" and len(sys.argv) > 4 else (sys.argv[3] if cmd == "list" and len(sys.argv) > 3 else "."), re.I)
    out = sys.argv[3] if cmd == "extract" else None
    for kind, n, sz, t, load in bundles(d):
        if not flt.search(n): continue
        data = load()
        magic = data[:4].hex()
        if cmd == "list":
            print(f"{kind:5} {sz if sz is not None else len(data):9} {('%08X' % t) if t is not None else '':8} {magic} {n}")
        else:
            dst = os.path.join(out, kind, n.replace("/", "__") + (f".{t:08X}" if t is not None else ""))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, "wb").write(data)

if __name__ == "__main__":
    main()
