#!/usr/bin/env python3
"""List or extract files from Xenosaga's XENOSAGA.xx disc archives.

    python port/tools/xsarc.py <iso> list [pattern]
    python port/tools/xsarc.py <iso> extract <path-substring> <out-dir>

The table of contents is XENOSAGA.00 (data in XENOSAGA.01) and XENOSAGA.10
(data in XENOSAGA.11, continuing in .12/.13). Record format from the game's
xglCdGetFilePosSub: flags byte (0x80 directory push, 0x40 packed, low 6 bits
record length), name, 3-byte sector offset, 4-byte stored size, +3 bytes if
packed. Extracted files are game data: never commit them.
"""
import struct
import sys
from pathlib import Path


def iso_files(f):
    f.seek(16 * 2048)
    pvd = f.read(2048)
    root = pvd[156:190]
    lba, size = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
    f.seek(lba * 2048)
    d = f.read(size)
    out = {}
    i = 0
    while i < len(d):
        n = d[i]
        if n == 0:
            i = (i // 2048 + 1) * 2048
            continue
        name = d[i + 33:i + 33 + d[i + 32]].decode("ascii", "replace").split(";")[0]
        out[name] = (struct.unpack_from("<I", d, i + 2)[0], struct.unpack_from("<I", d, i + 10)[0])
        i += n
    return out


def walk_toc(toc):
    files = []
    dirs = [""]
    p = 1
    while p < len(toc):
        fl = toc[p]
        if fl == 0:
            break
        packed = bool(fl & 0x40)
        length = fl & 0x3F
        if fl & 0x80:
            back = toc[p + 1]
            name = toc[p + 2:p + length].decode("latin1")
            if back:
                dirs = dirs[:len(dirs) - back]
            dirs.append(dirs[-1] + name + "\\")
            p += length
        else:
            name = toc[p + 1:p + length].decode("latin1")
            r = p + length
            off = toc[r] | (toc[r + 1] << 8) | (toc[r + 2] << 16)
            size = struct.unpack_from("<I", toc, r + 3)[0]
            files.append((dirs[-1] + name, off, size, packed))
            p = r + 7 + (3 if packed else 0)
    return files


def main():
    iso, cmd = sys.argv[1], sys.argv[2]
    f = open(iso, "rb")
    ext = iso_files(f)
    entries = []
    for toc_name, data_name in (("XENOSAGA.00", "XENOSAGA.01"), ("XENOSAGA.10", "XENOSAGA.11")):
        if toc_name not in ext or data_name not in ext:
            continue
        lba, size = ext[toc_name]
        f.seek(lba * 2048)
        base = lba  # offsets count from the start of the table file (data follows it)
        for name, off, sz, packed in walk_toc(f.read(size)):
            entries.append((name, base + off, sz, packed))
    if cmd == "list":
        pat = sys.argv[3].lower() if len(sys.argv) > 3 else ""
        for name, lba, sz, packed in entries:
            if pat in name.lower():
                print(f"{name}  lba={lba} size={sz}{' packed' if packed else ''}")
    elif cmd == "extract":
        pat = sys.argv[3].lower()
        out = Path(sys.argv[4])
        out.mkdir(parents=True, exist_ok=True)
        for name, lba, sz, packed in entries:
            if pat in name.lower():
                f.seek(lba * 2048)
                data = f.read(sz)
                dest = out / name.replace("\\", "_")
                dest.write_bytes(data)
                print(f"wrote {dest} ({len(data)} bytes{', packed' if packed else ''})")


if __name__ == "__main__":
    main()
