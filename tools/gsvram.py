#!/usr/bin/env python3
"""Decode a region of a GS memory dump (xs_vram.bin, written by the heartbeat)
into a PNG, using the same swizzle as the runtime (runtime/gs/ps2_gs_psm*.h).

  python tools/gsvram.py xs_vram.bin out.png --tex0 0x20076e8629343800
  python tools/gsvram.py xs_vram.bin out.png --psm ct32 --tbp 0xe00 --tbw 8 --w 512 --h 448
  python tools/gsvram.py xs_vram.bin out.png --psm t8 --tbp 0x3800 --tbw 16 --w 1024 --h 256 \
                                              [--cbp 0x3b74] [--csa 0]

Without a CLUT, T8/T4 textures are written as greyscale indices, which is the
quickest way to see whether the texel data itself is sane. Needs Pillow.
Dumps are game data: never commit them.
"""
import argparse
import struct
from PIL import Image

BT32 = [[0, 1, 4, 5, 16, 17, 20, 21], [2, 3, 6, 7, 18, 19, 22, 23],
        [8, 9, 12, 13, 24, 25, 28, 29], [10, 11, 14, 15, 26, 27, 30, 31]]
CT32 = [[0, 1, 4, 5, 8, 9, 12, 13], [2, 3, 6, 7, 10, 11, 14, 15],
        [16, 17, 20, 21, 24, 25, 28, 29], [18, 19, 22, 23, 26, 27, 30, 31],
        [32, 33, 36, 37, 40, 41, 44, 45], [34, 35, 38, 39, 42, 43, 46, 47],
        [48, 49, 52, 53, 56, 57, 60, 61], [50, 51, 54, 55, 58, 59, 62, 63]]
BT8 = BT32
CT8 = [
    [0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54],
    [8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62],
    [33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23],
    [41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31],
    [96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86],
    [104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94],
    [65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119],
    [73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127],
    [128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182],
    [136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190],
    [161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151],
    [169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159],
    [224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214],
    [232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222],
    [193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247],
    [201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255]]
BT4 = [[0, 2, 8, 10], [1, 3, 9, 11], [4, 6, 12, 14], [5, 7, 13, 15],
       [16, 18, 24, 26], [17, 19, 25, 27], [20, 22, 28, 30], [21, 23, 29, 31]]


def addr32(bp, bw, x, y):
    page = (bp >> 5) + (y >> 5) * max(bw, 1) + (x >> 6)
    blk = (bp & 31) + BT32[(y >> 3) & 3][(x >> 3) & 7]
    return (page << 13) + ((blk >> 5) << 13) + (blk & 31) * 256 + CT32[y & 7][x & 7] * 4


def addr8(bp, bw, x, y):
    page = (bp >> 5) + (y >> 6) * max(bw >> 1, 1) + (x >> 7)
    blk = (bp & 31) + BT8[(y >> 4) & 3][(x >> 4) & 7]
    return (page << 13) + ((blk >> 5) << 13) + (blk & 31) * 256 + CT8[y & 15][x & 15]


def read32(v, bp, bw, x, y):
    return struct.unpack_from("<I", v, addr32(bp, bw, x, y) & 0x3FFFFF)[0]


def clut_ct32_csm1(v, cbp, csa=0):
    pal = []
    for i in range(256):
        # CSM1: entries 8-15 and 16-23 of every 32 are swapped in memory.
        j = (i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)
        j = (j + csa * 16) & 0xFF
        c = read32(v, cbp, 1, j & 15, j >> 4)
        pal.append((c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, min(255, ((c >> 24) & 0xFF) * 2)))
    return pal


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("vram")
    ap.add_argument("out")
    ap.add_argument("--tex0", type=lambda s: int(s, 0))
    ap.add_argument("--psm", choices=["ct32", "t8"], default="ct32")
    ap.add_argument("--tbp", type=lambda s: int(s, 0), default=0)
    ap.add_argument("--tbw", type=lambda s: int(s, 0), default=10)
    ap.add_argument("--w", type=int, default=640)
    ap.add_argument("--h", type=int, default=448)
    ap.add_argument("--cbp", type=lambda s: int(s, 0))
    ap.add_argument("--csa", type=int, default=0)
    a = ap.parse_args()
    if a.tex0 is not None:
        t = a.tex0
        a.tbp, a.tbw = t & 0x3FFF, (t >> 14) & 0x3F
        a.psm = {0: "ct32", 0x13: "t8"}.get((t >> 20) & 0x3F)
        if a.psm is None:
            raise SystemExit(f"PSM {(t >> 20) & 0x3F:#x} not supported yet")
        a.w, a.h = 1 << ((t >> 26) & 0xF), 1 << ((t >> 30) & 0xF)
        a.cbp, a.csa = (t >> 37) & 0x3FFF, (t >> 56) & 0x1F
        print(f"tbp={a.tbp:#x} tbw={a.tbw} psm={a.psm} {a.w}x{a.h} cbp={a.cbp:#x} cpsm={(t >> 51) & 0xF} "
              f"csm={(t >> 55) & 1} csa={a.csa} tcc={(t >> 34) & 1} tfx={(t >> 35) & 3}")
    v = open(a.vram, "rb").read()
    img = Image.new("RGBA", (a.w, a.h))
    px = img.load()
    if a.psm == "ct32":
        for y in range(a.h):
            for x in range(a.w):
                c = read32(v, a.tbp, a.tbw, x, y)
                px[x, y] = (c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, 255)
    else:
        pal = clut_ct32_csm1(v, a.cbp, a.csa) if a.cbp is not None else None
        for y in range(a.h):
            for x in range(a.w):
                i = v[addr8(a.tbp, a.tbw, x, y) & 0x3FFFFF]
                px[x, y] = (pal[i][0], pal[i][1], pal[i][2], 255) if pal else (i, i, i, 255)
    img.save(a.out)
    print("wrote", a.out)


if __name__ == "__main__":
    main()
