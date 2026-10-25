#!/usr/bin/env python3
"""Minimal AVM1 (ActionScript 2) disassembler for a byte range that starts with ActionConstantPool.
Usage: scripts/avm1_dis.py <file> <hex offset of the constant pool action> [count]"""
import struct, sys

NAMES = {0x0A: "Add", 0x0B: "Subtract", 0x0C: "Multiply", 0x0D: "Divide", 0x0E: "Equals", 0x0F: "Less",
         0x10: "And", 0x11: "Or", 0x12: "Not", 0x13: "StringEquals", 0x17: "Pop", 0x1C: "GetVariable",
         0x1D: "SetVariable", 0x3C: "DefineLocal", 0x3D: "CallFunction", 0x3E: "Return", 0x40: "NewObject",
         0x41: "DefineLocal2", 0x43: "InitObject", 0x47: "Add2", 0x48: "Less2", 0x49: "Equals2", 0x4C: "PushDuplicate",
         0x4E: "GetMember", 0x4F: "SetMember", 0x52: "CallMethod", 0x53: "NewMethod", 0x66: "StrictEquals",
         0x67: "Greater", 0x88: "ConstantPool", 0x96: "Push", 0x99: "Jump", 0x9D: "If", 0x9B: "DefineFunction",
         0x8E: "DefineFunction2", 0x87: "StoreRegister", 0x94: "With", 0x9A: "GetURL2", 0x8B: "SetTarget"}

def main():
    d = open(sys.argv[1], "rb").read()
    p = int(sys.argv[2], 16)
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 200
    pool = []
    for _ in range(count):
        if p >= len(d): break
        op = d[p]; p += 1
        ln = 0
        if op >= 0x80:
            ln = struct.unpack_from("<H", d, p)[0]; p += 2
        body = d[p:p + ln]; p += ln
        name = NAMES.get(op, f"op{op:02X}")
        info = ""
        if op == 0x88:
            n = struct.unpack_from("<H", body, 0)[0]; q = 2
            for _ in range(n):
                e = body.index(b"\0", q); pool.append(body[q:e].decode("latin1")); q = e + 1
            info = f"{n} entries"
        elif op == 0x96:
            q, vals = 0, []
            while q < len(body):
                t = body[q]; q += 1
                if t == 0: e = body.index(b"\0", q); vals.append(repr(body[q:e].decode("latin1"))); q = e + 1
                elif t == 1: vals.append(str(struct.unpack_from("<f", body, q)[0])); q += 4
                elif t in (2, 3): vals.append("null" if t == 2 else "undefined")
                elif t == 4: vals.append(f"r{body[q]}"); q += 1
                elif t == 5: vals.append(str(bool(body[q]))); q += 1
                elif t == 6: vals.append(str(struct.unpack_from("<d", body[q:q+8][4:] + body[q:q+8][:4])[0])); q += 8
                elif t == 7: vals.append(str(struct.unpack_from("<i", body, q)[0])); q += 4
                elif t == 8: vals.append(repr(pool[body[q]]) if body[q] < len(pool) else f"c{body[q]}"); q += 1
                elif t == 9: i = struct.unpack_from("<H", body, q)[0]; vals.append(repr(pool[i]) if i < len(pool) else f"c{i}"); q += 2
                else: vals.append(f"?{t}"); break
            info = ", ".join(vals)
        elif op == 0x8E:
            e = body.index(b"\0"); fname = body[:e].decode("latin1")
            n, regs, flags = struct.unpack_from("<HBH", body, e + 1)
            q = e + 6
            params = []
            for _ in range(n):
                r = body[q]; e2 = body.index(b"\0", q + 1); params.append(f"r{r}:{body[q+1:e2].decode('latin1')}"); q = e2 + 1
            size = struct.unpack_from("<H", body, q)[0]
            info = f"'{fname}'({', '.join(params)}) regs {regs} code {size} bytes"
        elif op in (0x99, 0x9D):
            info = str(struct.unpack_from("<h", body, 0)[0])
        elif op == 0x87:
            info = f"r{body[0]}"
        print(f"{p - ln - (3 if op >= 0x80 else 1):#08x} {name} {info}")

main()
