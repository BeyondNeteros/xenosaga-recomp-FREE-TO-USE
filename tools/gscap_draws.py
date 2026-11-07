#!/usr/bin/env python3
"""List what a GS capture draws (PS2X_GS_CAPTURE=seconds,frames -> gs_capture.bin).

    python port/tools/gscap_draws.py gs_capture.bin [--all]

Groups primitives by (frame buffer, PRIM type, texture, alpha/test state) and
prints count, colour range and screen bounding box (XYZ minus XYOFFSET), plus
image uploads (BITBLTBUF/TRXREG). --all prints every primitive instead.
The capture holds game data (VRAM): never commit it.
"""
import struct
import sys
from collections import OrderedDict

PRIM_NAMES = ["point", "line", "linestrip", "tri", "tristrip", "trifan", "sprite", "?"]


def read_capture(path):
    d = open(path, "rb").read()
    assert d[:4] == b"GSC1", "not a GS capture"
    vram_size, count = struct.unpack_from("<II", d, 4)
    pos = 12
    regs = {}
    for _ in range(count):
        r, v = struct.unpack_from("<BQ", d, pos)
        regs[r] = v
        pos += 9
    pos += vram_size
    records = []
    while pos < len(d):
        t = d[pos] & 0x7F
        pos += 1
        if t == 0:
            break
        if t == 7:
            records.append((7, b""))
            continue
        size, = struct.unpack_from("<I", d, pos)
        pos += 4
        records.append((t, d[pos:pos + size]))
        pos += size
    return regs, records


class GS:
    def __init__(self, regs, show_all):
        self.r = dict(regs)
        self.show_all = show_all
        self.groups = OrderedDict()
        self.verts = []
        self.ctx_q = 1.0
        self.uploads = []

    def ctx(self):
        prim = self.r.get(0x00, 0)
        prmode = self.r.get(0x1A, 0) if (self.r.get(0x1B, 0) & 1) == 0 else prim
        return (prmode >> 9) & 1

    def key(self):
        prim = self.r.get(0x00, 0)
        c = self.ctx()
        frame = self.r.get(0x4C + c, 0)
        tex0 = self.r.get(0x06 + c, 0)
        alpha = self.r.get(0x42 + c, 0)
        test = self.r.get(0x47 + c, 0)
        tme = (prim >> 4) & 1
        abe = (prim >> 6) & 1
        fbp = (frame & 0x1FF) * 32
        fbmsk = frame >> 32
        tbp = tex0 & 0x3FFF
        psm = (tex0 >> 20) & 0x3F
        tw = 1 << ((tex0 >> 26) & 0xF)
        th = 1 << ((tex0 >> 30) & 0xF)
        cbp = (tex0 >> 37) & 0x3FFF
        return (PRIM_NAMES[prim & 7], hex(fbp), hex(fbmsk), f"tex={hex(tbp)} psm={hex(psm)} {tw}x{th} cbp={hex(cbp)}" if tme else "untextured",
                f"abe={abe} alpha={hex(alpha)}", f"test={hex(test)}")

    def vertex(self, xyz, kick):
        c = self.ctx()
        ofs = self.r.get(0x18 + c, 0)
        ox, oy = (ofs & 0xFFFF) / 16.0, ((ofs >> 32) & 0xFFFF) / 16.0
        x = (xyz & 0xFFFF) / 16.0 - ox
        y = ((xyz >> 16) & 0xFFFF) / 16.0 - oy
        z = xyz >> 32
        rgba = self.r.get(0x01, 0) & 0xFFFFFFFF
        self.verts.append((x, y, z, rgba))
        prim = self.r.get(0x00, 0) & 7
        need = {0: 1, 1: 2, 2: 2, 3: 3, 4: 3, 5: 3, 6: 2}.get(prim, 1)
        if not kick or len(self.verts) < need:
            if prim in (3, 0, 1, 6) and len(self.verts) > need:
                self.verts = self.verts[-need:]
            return
        vs = self.verts[-need:]
        if prim in (3, 0, 1, 6):
            self.verts = []
        k = self.key()
        g = self.groups.setdefault(k, {"n": 0, "x0": 1e9, "y0": 1e9, "x1": -1e9, "y1": -1e9, "rgba": set(), "z": set()})
        g["n"] += 1
        for (x, y, z, col) in vs:
            g["x0"] = min(g["x0"], x); g["y0"] = min(g["y0"], y)
            g["x1"] = max(g["x1"], x); g["y1"] = max(g["y1"], y)
            if len(g["rgba"]) < 6:
                g["rgba"].add(hex(col))
            if len(g["z"]) < 3:
                g["z"].add(z)
        if self.show_all:
            print("  ", k[0], k[1], k[3], [(round(x), round(y), hex(c)) for x, y, z, c in vs])

    def write(self, reg, v):
        if reg == 0x00:  # PRIM resets the vertex queue
            self.verts = []
        if reg in (0x05, 0x0D, 0x04, 0x0C):  # XYZ2/XYZ3/XYZF2/XYZF3
            if reg in (0x04, 0x0C):
                v = (v & 0xFFFFFFFF) | (((v >> 32) & 0xFFFFFF) << 32)
            self.vertex(v, reg in (0x05, 0x04))
            return
        if reg == 0x53 and (v & 3) == 0:  # TRXDIR host->local
            b = self.r.get(0x50, 0)
            t = self.r.get(0x52, 0)
            p = self.r.get(0x51, 0)
            self.uploads.append(f"upload dbp={hex((b >> 32) & 0x3FFF)} dbw={(b >> 48) & 0x3F} dpsm={hex((b >> 56) & 0x3F)} "
                                f"{t & 0xFFF}x{(t >> 32) & 0xFFF} at ({(p >> 32) & 0x7FF},{(p >> 48) & 0x7FF})")
        if reg == 0x53 and (v & 3) == 2:
            b = self.r.get(0x50, 0)
            t = self.r.get(0x52, 0)
            self.uploads.append(f"COPY sbp={hex(b & 0x3FFF)} -> dbp={hex((b >> 32) & 0x3FFF)} {t & 0xFFF}x{(t >> 32) & 0xFFF}")
        self.r[reg] = v

    def packet(self, data):
        pos = 0
        n = len(data)
        while pos + 16 <= n:
            lo, hi = struct.unpack_from("<QQ", data, pos)
            pos += 16
            nloop = lo & 0x7FFF
            pre = (lo >> 46) & 1
            prim = (lo >> 47) & 0x7FF
            flg = (lo >> 58) & 3
            nreg = (lo >> 60) & 0xF or 16
            if pre:
                self.write(0x00, prim)
            regs = [(hi >> (4 * i)) & 0xF for i in range(nreg)]
            if flg == 0:
                for _ in range(nloop):
                    for rd in regs:
                        if pos + 16 > n:
                            return
                        a, b = struct.unpack_from("<QQ", data, pos)
                        pos += 16
                        self.packed(rd, a, b)
            elif flg == 1:
                words = nloop * nreg
                for i in range(words):
                    if pos + 8 > n:
                        return
                    v, = struct.unpack_from("<Q", data, pos)
                    pos += 8
                    self.write(regs[i % nreg], v)
                if words & 1:
                    pos += 8
            else:
                pos += nloop * 16

    def packed(self, rd, a, b):
        if rd == 0x0:  # PRIM
            self.write(0x00, a & 0x7FF)
        elif rd == 0x1:  # RGBAQ
            r, g, bl, al = a & 0xFF, (a >> 32) & 0xFF, b & 0xFF, (b >> 32) & 0xFF
            self.r[0x01] = r | (g << 8) | (bl << 16) | (al << 24)
        elif rd == 0x2:  # ST
            self.r[0x02] = a
        elif rd == 0x3:  # UV
            self.r[0x03] = (a & 0x3FFF) | (((a >> 32) & 0x3FFF) << 16)
        elif rd in (0x4, 0x5):  # XYZF2 / XYZ2
            x, y = a & 0xFFFF, (a >> 32) & 0xFFFF
            if rd == 0x5:
                z = b & 0xFFFFFFFF
                adc = (b >> 47) & 1
            else:
                z = (b >> 4) & 0xFFFFFF
                adc = (b >> 47) & 1
            v = x | (y << 16) | (z << 32)
            self.vertex(v, not adc)
        elif rd == 0xE:  # A+D
            self.write(b & 0xFF, a)
        elif rd == 0xA:  # FOG
            pass
        elif rd == 0xF:  # NOP
            pass
        else:
            self.write(rd, a)


def main():
    path = sys.argv[1]
    show_all = "--all" in sys.argv
    regs, records = read_capture(path)
    gs = GS(regs, show_all)
    frames = 0
    for t, data in records:
        if t == 7:
            frames += 1
        elif t == 1:
            gs.packet(data)
    print(f"{len(records)} records, {frames} frame marks, {len(gs.uploads)} transfers")
    for (prim, fbp, fbmsk, tex, ab, test), g in gs.groups.items():
        print(f"{g['n']:6d} {prim:9s} fb={fbp} msk={fbmsk} {tex} {ab} {test} "
              f"x {g['x0']:.0f}..{g['x1']:.0f} y {g['y0']:.0f}..{g['y1']:.0f} rgba {sorted(g['rgba'])} z {sorted(g['z'])}")
    seen = OrderedDict()
    for u in gs.uploads:
        seen[u] = seen.get(u, 0) + 1
    for u, c in list(seen.items())[:40]:
        print(f"{c:4d}x {u}")


if __name__ == "__main__":
    main()
