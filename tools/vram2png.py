#!/usr/bin/env python3
"""Turn a GS local-memory dump (4 MB, e.g. xs_vram.bin or GS_REPLAY_DUMP) into
a PNG of one PSMCT32 buffer.

    python port/tools/vram2png.py vram.bin out.png --fbp 0x70 [--fbw 8] [--w 512] [--h 448]

fbp is in pages (2048 words, as in DISPFB/FRAME), fbw in 64-pixel units.
"""
import argparse
import struct

from PIL import Image

BLOCK32 = [[0, 1, 4, 5, 16, 17, 20, 21],
           [2, 3, 6, 7, 18, 19, 22, 23],
           [8, 9, 12, 13, 24, 25, 28, 29],
           [10, 11, 14, 15, 26, 27, 30, 31]]
COLUMN32 = [[0, 1, 4, 5, 8, 9, 12, 13],
            [2, 3, 6, 7, 10, 11, 14, 15],
            [16, 17, 20, 21, 24, 25, 28, 29],
            [18, 19, 22, 23, 26, 27, 30, 31],
            [32, 33, 36, 37, 40, 41, 44, 45],
            [34, 35, 38, 39, 42, 43, 46, 47],
            [48, 49, 52, 53, 56, 57, 60, 61],
            [50, 51, 54, 55, 58, 59, 62, 63]]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("vram")
    ap.add_argument("out")
    ap.add_argument("--fbp", type=lambda v: int(v, 0), required=True)
    ap.add_argument("--fbw", type=int, default=8)
    ap.add_argument("--w", type=int, default=512)
    ap.add_argument("--h", type=int, default=448)
    a = ap.parse_args()
    data = open(a.vram, "rb").read()
    words = struct.unpack(f"<{len(data) // 4}I", data)
    img = Image.new("RGB", (a.w, a.h))
    px = img.load()
    for y in range(a.h):
        for x in range(a.w):
            page = a.fbp + (y // 32) * a.fbw + x // 64
            addr = page * 2048 + BLOCK32[(y % 32) // 8][(x % 64) // 8] * 64 + COLUMN32[y % 8][x % 8]
            v = words[addr % len(words)]
            px[x, y] = (v & 255, (v >> 8) & 255, (v >> 16) & 255)
    img.save(a.out)


if __name__ == "__main__":
    main()
