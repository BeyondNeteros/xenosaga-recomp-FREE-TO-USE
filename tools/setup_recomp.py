#!/usr/bin/env python3
"""Generate the recompiled C++ for Xenosaga Episode I from your own game files.

Usage:
  python tools/setup_recomp.py <folder with SLUS_204.69 and OV*.OVL> [--bin <PS2Recomp build bin dir>]

Produces <XS_ROOT>/recomp/output (main executable) and recomp/ovNN/output for
each overlay. Nothing generated here may be committed or shared: it is derived
from the game.

The retail overlays are ELF files whose entry point is the main executable's
(0x200008). ps2_recomp needs an entry inside the overlay, so each copy gets its
entry set to its first function (the load address, 0xA00000).
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(os.environ.get("XS_ROOT", Path(__file__).resolve().parents[2]))
OVERLAYS = ["ov01", "ov02", "ov10", "ov11", "ov12"]


def find_tool(bin_dir: Path, name: str) -> str:
    for cand in (bin_dir / name, bin_dir / (name + ".exe")):
        if cand.exists():
            return str(cand)
    for cand in bin_dir.rglob(name + "*"):
        if cand.is_file() and cand.stem == name:
            return str(cand)
    sys.exit(f"could not find {name} under {bin_dir}")


def find_file(folder: Path, name: str) -> Path:
    for p in folder.iterdir():
        if p.name.lower() == name.lower():
            return p
    sys.exit(f"{name} not found in {folder}")


def run(cmd, cwd):
    print(">", " ".join(map(str, cmd)))
    subprocess.run(list(map(str, cmd)), cwd=cwd, check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game_files")
    ap.add_argument("--bin", default=str(Path(os.environ.get("PS2RECOMP", ROOT / "PS2Recomp")) / "out" / "build"))
    args = ap.parse_args()
    src = Path(args.game_files)
    bin_dir = Path(args.bin).resolve()  # absolute: tools run with cwd set to the output folder
    analyzer = find_tool(bin_dir, "ps2_analyzer")
    recomp = find_tool(bin_dir, "ps2_recomp")

    out = ROOT / "recomp"
    out.mkdir(parents=True, exist_ok=True)
    elf = out / "SLUS_204.69"
    shutil.copy2(find_file(src, "SLUS_204.69"), elf)
    run([analyzer, elf.name, "config.toml"], out)
    run([recomp, "config.toml"], out)

    for ov in OVERLAYS:
        d = out / ov
        d.mkdir(exist_ok=True)
        data = bytearray(find_file(src, ov + ".ovl").read_bytes())
        if data[:4] != b"\x7fELF":
            sys.exit(f"{ov}.ovl is not an ELF file")
        struct.pack_into("<I", data, 24, 0x00A00000)  # e_entry
        (d / f"{ov}.elf").write_bytes(data)
        run([analyzer, f"{ov}.elf", "config.toml"], d)
        run([recomp, "config.toml"], d)
    print("done; now run tools/assemble.py")


if __name__ == "__main__":
    main()
