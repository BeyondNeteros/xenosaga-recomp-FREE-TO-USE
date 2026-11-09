#!/usr/bin/env python3
"""Xenosaga ARX decompression (xglArxExtract in the decomp, main/xgl_timer0.c),
with MIPS shift semantics (variable shifts use the low 5 bits of the count).

    python port/tools/arx.py <file> <offset> [out.bin]

Decodes the ARX block at <offset> (hex ok) of <file> (e.g. the ISO) for
offline comparisons. Reads game data: never commit its output.
"""
import struct
import sys

ARX_MAGIC = 0x585241
M32 = 0xFFFFFFFF


def srl(v, s):
    return (v & M32) >> (s & 31)


def sll(v, s):
    return (v << (s & 31)) & M32


def extract(buf, off):
    w = lambda i: struct.unpack_from("<I", buf, off + 4 * i)[0]
    if w(0) != ARX_MAGIC or w(3) != 0:
        raise ValueError("not an ARX block")
    size = w(1)
    table = [w(4 + i) for i in range(30)]
    data = off + 34 * 4
    state = {"value": 0, "bits": 0, "pos": data}

    def word():
        v = struct.unpack_from("<I", buf, state["pos"])[0]
        state["pos"] += 4
        return v

    def getbit1():
        if state["bits"] == 0:
            state["value"] = word()
        state["bits"] = (state["bits"] - 1) & 0x1F
        bit = state["value"] >> 31
        state["value"] = sll(state["value"], 1)
        return bit

    def getbits(count):
        high = 0
        bits = state["bits"]
        if bits < count:
            high = srl(state["value"], 32 - bits)
            state["value"] = word()
            count -= bits
            state["bits"] = 32
            bits = 32
        result = (sll(high, count) | srl(state["value"], -count)) & M32
        state["bits"] = bits - count
        state["value"] = sll(state["value"], count)
        return result

    out = []
    while len(out) * 4 < size:
        if getbit1() == 0:
            out.append(word())
        else:
            length, index, unit = 1, 0, 1
            while getbit1() != 0:
                index += unit << length
                length += 1
            out.append(table[index + getbits(length)])
    return struct.pack(f"<{len(out)}I", *out)[:size]


if __name__ == "__main__":
    import mmap
    f = open(sys.argv[1], "rb")
    m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    data = extract(m, int(sys.argv[2], 0))
    if len(sys.argv) > 3:
        open(sys.argv[3], "wb").write(data)
    print(len(data), "bytes")
