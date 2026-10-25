#!/usr/bin/env python3
"""Print manifest function entries for the linker thunks in a module's .xidata section.

  python -I scripts/xidata_thunks.py "dump/Disc 1/Engine.BuildInfo.image.bin"

.xidata (executable) holds 16-byte trampolines `lis r11,hi; addi r11,r11,lo; mtctr r11; bctr` with no .pdata
entries, so codegen doesn't register them and every `b` into one is "target not in any function". The XEX
import stubs later in the section (`<lib/ordinal words>; mtctr r11; bctr`) are left alone: the import table
covers those. Output goes under the module's [modules.functions].
"""
import struct
import sys


def sections(image):
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    nsec = struct.unpack_from("<H", image, pe + 6)[0]
    opt = struct.unpack_from("<H", image, pe + 20)[0]
    base = struct.unpack_from("<I", image, pe + 0x34)[0]
    for i in range(nsec):
        sec = pe + 24 + opt + i * 40
        name = image[sec:sec + 8].rstrip(b"\0").decode()
        vsize, va = struct.unpack_from("<II", image, sec + 8)
        yield name, base, va, vsize


def main(path):
    image = open(path, "rb").read()
    for name, base, va, vsize in sections(image):
        if name != ".xidata":
            continue
        off = va
        while off + 16 <= va + vsize:
            w0, w1, w2, w3 = struct.unpack_from(">4I", image, off)
            if w0 >> 16 == 0x3D60 and w1 >> 16 == 0x396B and w2 == 0x7D6903A6 and w3 == 0x4E800420:
                target = (((w0 & 0xFFFF) << 16) + (((w1 & 0xFFFF) ^ 0x8000) - 0x8000)) & 0xFFFFFFFF
                print(f"0x{base + off:08X} = {{ size = 0x10 }}  # thunk -> 0x{target:08X}")
                off += 16
            else:
                off += 4


if __name__ == "__main__":
    main(sys.argv[1])
