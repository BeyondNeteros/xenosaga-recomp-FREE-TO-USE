#!/usr/bin/env python3
"""Disassemble EE code from your own SLUS_204.69 (or an overlay ELF) by symbol or address.

Usage: python tools/eedis.py <elf> <symbol|0xaddr> [count]
       python tools/eedis.py <elf> --sym <substring>     (list matching symbols)
Needs: pip install capstone pyelftools
"""
import sys
from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS64, CS_MODE_LITTLE_ENDIAN
from elftools.elf.elffile import ELFFile


def load(path):
    f = open(path, "rb")
    elf = ELFFile(f)
    syms = {}
    st = elf.get_section_by_name(".symtab")
    if st:
        for s in st.iter_symbols():
            if s.name and s["st_value"]:
                syms[s.name] = (s["st_value"], s["st_size"])
    return elf, syms


def read(elf, addr, n):
    for seg in elf.iter_segments():
        if seg["p_type"] == "PT_LOAD" and seg["p_vaddr"] <= addr < seg["p_vaddr"] + seg["p_filesz"]:
            off = addr - seg["p_vaddr"]
            return seg.data()[off:off + n]
    raise SystemExit(f"0x{addr:x} not in a loaded segment")


def main():
    elf, syms = load(sys.argv[1])
    if sys.argv[2] == "--sym":
        for k, (v, sz) in sorted(syms.items(), key=lambda kv: kv[1]):
            if sys.argv[3].lower() in k.lower():
                print(f"0x{v:08x} {sz:6d} {k}")
        return
    rev = {}
    for k, (v, sz) in syms.items():
        rev.setdefault(v, k)
    t = sys.argv[2]
    if t.startswith("0x"):
        addr, size = int(t, 16), 0
    else:
        addr, size = syms[t]
    count = int(sys.argv[3]) if len(sys.argv) > 3 else (size // 4 or 64)
    md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS64 + CS_MODE_LITTLE_ENDIAN)
    data = read(elf, addr, count * 4)
    for i in range(0, len(data), 4):
        a = addr + i
        w = int.from_bytes(data[i:i + 4], "little")
        ins = list(md.disasm(data[i:i + 4], a))
        txt = f"{ins[0].mnemonic} {ins[0].op_str}" if ins else f".word 0x{w:08x}"
        if txt.startswith(("jal ", "j ")):
            try:
                tgt = int(txt.split()[1], 16)
                txt += f"   <{rev.get(tgt, '?')}>"
            except ValueError:
                pass
        lab = f"<{rev[a]}>:\n" if a in rev else ""
        print(f"{lab}{a:08x} +{a-addr:04x}  {w:08x}  {txt}")


if __name__ == "__main__":
    main()
