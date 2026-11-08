#!/usr/bin/env python3
"""Read PS2X VU1 capture files (vu1_capture.bin) written by the runtime.

File = 16-byte header ('XVU1', 1, state size, 0), then records.
Record = u32 kind (0 MSCAL / 1 MSCNT), startPC, top, itop; VU1State (664
bytes); 16 KB microcode; 16 KB data. VU1State starts with vf[32][4] floats,
vi[16] i32, acc[4], q, p, i, r, pc, ...

    python port/tools/vucap.py survey runs/*/vu1_capture.bin
"""
import hashlib
import struct
import sys
from collections import Counter

SSZ = 664
REC = 16 + SSZ + 16384 + 16384


def records(path):
    d = open(path, 'rb').read()
    magic, _, ssz, _ = struct.unpack_from('<4I', d, 0)
    if magic != 0x31555658 or ssz != SSZ:
        return
    n = (len(d) - 16) // REC
    for k in range(n):
        o = 16 + k * REC
        kind, start, top, itop = struct.unpack_from('<4I', d, o)
        st = d[o + 16:o + 16 + SSZ]
        code = d[o + 16 + SSZ:o + 16 + SSZ + 16384]
        data = d[o + 16 + SSZ + 16384:o + REC]
        pc = struct.unpack_from('<I', st, 32 * 16 + 16 * 4 + 16 + 16)[0]
        yield dict(kind=kind, start=start, top=top, itop=itop, state=st, code=code, data=data, pc=pc)


def survey(paths):
    entries = Counter()
    codes = {}
    for p in paths:
        for r in records(p):
            pc = r['pc'] if r['kind'] == 1 else r['start']
            h = hashlib.sha1(r['code']).hexdigest()[:10]
            codes.setdefault(h, r['code'])
            entries[(r['kind'], pc, h)] += 1
    for (kind, pc, h), n in entries.most_common(40):
        print(f"{'MSCNT' if kind else 'MSCAL'} pc=0x{pc:04x} code={h} calls={n}")
    print(len(codes), 'distinct code images')


if __name__ == '__main__':
    if sys.argv[1] == 'survey':
        survey(sys.argv[2:])
