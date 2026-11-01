#!/usr/bin/env python3
"""Decode EE COP2 (VU0 macro) instructions in a function of your own ELF.

  python tools/cop2dis.py <elf> <symbol|0xaddr> [count]

Non-COP2 words are shown via eedis.py's capstone view. Needs capstone + pyelftools.
"""
import os, sys, struct
sys.path.insert(0, os.path.dirname(__file__))
import vudis
import eedis
from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS64, CS_MODE_LITTLE_ENDIAN

VF = lambda n: f"vf{n}"


def cop2(w):
    op = w >> 26
    if op == 0x36:
        return f"lqc2 vf{(w >> 16) & 31}, {struct.unpack('<h', struct.pack('<H', w & 0xffff))[0]}(r{(w >> 21) & 31})"
    if op == 0x3e:
        return f"sqc2 vf{(w >> 16) & 31}, {struct.unpack('<h', struct.pack('<H', w & 0xffff))[0]}(r{(w >> 21) & 31})"
    if op != 0x12:
        return None
    rs = (w >> 21) & 31
    if rs & 0x10:  # CO: macro operation
        f = w & 63
        if f < 0x3c:
            if f == 0x38: return f"vcallms 0x{((w >> 6) & 0x7fff) * 8:x}"
            if f == 0x39: return "vcallmsr (CMSAR0)"
            if f in (0x30, 0x31, 0x32, 0x34, 0x35):
                return "v" + vudis.lower((0x40 << 25) | (w & 0x01ffffff), 0)
            return "v" + vudis.upper(w & 0x01ffffff)
        s = (w & 3) | ((w >> 4) & 0x7c)
        if s < 0x30:
            return "v" + vudis.upper(w & 0x01ffffff)
        return "v" + vudis.lower((0x40 << 25) | (w & 0x01ffffff), 0)
    names = {1: "qmfc2", 2: "cfc2", 5: "qmtc2", 6: "ctc2", 8: "bc2"}
    i = "" if not (w & 1) else ".i"
    return f"{names.get(rs, f'cop2 rs={rs}')}{i} r{(w >> 16) & 31}, {'vi' if rs in (2, 6) else 'vf'}{(w >> 11) & 31}"


def main():
    elf, syms = eedis.load(sys.argv[1])
    t = sys.argv[2]
    addr, size = (int(t, 16), 0) if t.startswith("0x") else syms[t]
    count = int(sys.argv[3]) if len(sys.argv) > 3 else (size // 4 or 64)
    data = eedis.read(elf, addr, count * 4)
    md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS64 + CS_MODE_LITTLE_ENDIAN)
    for i in range(0, len(data), 4):
        w = int.from_bytes(data[i:i + 4], "little")
        txt = cop2(w)
        if txt is None:
            ins = list(md.disasm(data[i:i + 4], addr + i))
            txt = f"{ins[0].mnemonic} {ins[0].op_str}" if ins else f".word 0x{w:08x}"
        print(f"{addr + i:08x}  {w:08x}  {txt}")


if __name__ == "__main__":
    main()
