#!/usr/bin/env python3
"""Disassemble code from an IOP module (IRX) as loaded by ps2xIOP.

    python port/tools/irxdis.py <file.irx> <load base hex> <address hex> [count]

The load base is printed by the runtime ("[IOP] loaded IRX ... base=0x...").
Needs capstone. Works on the user's own extracted modules; prints nothing
derived beyond the disassembly on screen.
"""
import struct
import sys

import capstone


def main():
    path, base, addr = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
    count = int(sys.argv[4]) if len(sys.argv) > 4 else 40
    data = open(path, 'rb').read()
    phoff = struct.unpack_from('<I', data, 0x1C)[0]
    phnum = struct.unpack_from('<H', data, 0x2C)[0]
    phentsize = struct.unpack_from('<H', data, 0x2A)[0]
    offset = None
    for i in range(phnum):
        ptype, poff, pvaddr, _, filesz = struct.unpack_from('<IIIII', data, phoff + i * phentsize)
        if ptype == 1:  # PT_LOAD
            offset = poff + (addr - base - pvaddr)
            break
    if offset is None:
        raise SystemExit('no PT_LOAD segment')
    md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32 + capstone.CS_MODE_LITTLE_ENDIAN)
    code = data[offset:offset + count * 4]
    for ins in md.disasm(code, addr):
        word = struct.unpack_from('<I', code, ins.address - addr)[0]
        print(f'{ins.address:06x}  {word:08x}  {ins.mnemonic} {ins.op_str}')


if __name__ == '__main__':
    main()
