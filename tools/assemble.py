#!/usr/bin/env python3
"""Assemble recompiled Xenosaga Episode I code into the PS2Recomp runtime build.

Inputs (produced by ps2_recomp):
  recomp/output/          main executable (SLUS_204.69)
  recomp/ovNN/output/     each overlay, recompiled on its own

Outputs:
  <runtime>/src/runner/   generated .cpp files + overlay registration + port code
  <runtime>/include/      generated headers

Overlays all load at the same address (0xA00000), so each overlay's functions
are renamed with an xs_ovNN_ prefix and registered into the shared function
table only while that overlay is loaded (see src/xenosaga_overlays.cpp).
"""
import re
import shutil
import sys
from pathlib import Path

import os

# Project layout (override with environment variables):
#   <XS_ROOT>/port/        this repository
#   <XS_ROOT>/recomp/      generated code (tools/setup_recomp.py)
#   <PS2RECOMP>/           PS2Recomp checkout (default <XS_ROOT>/PS2Recomp)
ROOT = Path(os.environ.get("XS_ROOT", Path(__file__).resolve().parents[2]))
RECOMP = ROOT / "recomp"
PORT_SRC = ROOT / "port" / "src"
RUNTIME = Path(os.environ.get("PS2RECOMP", ROOT / "PS2Recomp")) / "ps2xRuntime"
RUNNER = RUNTIME / "src" / "runner"
INCLUDE = RUNTIME / "include"

OVERLAYS = ["ov01", "ov02", "ov10", "ov11", "ov12"]
OVERLAY_REGION_START = 0x9FF000
TABLE_END = 0xA60000  # covers the largest overlay (ov01 ends at 0xA5B8F8)

ADDR_SUFFIX = re.compile(r"_0x([0-9a-f]+)\.cpp$")
DECL = re.compile(r"^void (\w+)\(uint8_t\* rdram", re.M)
TABLE_LINE = re.compile(r"g_ps2RecompiledFunctionTable\[\d+u?\] = (\w+); // 0x([0-9a-f]+)")


def file_addr(path: Path):
    m = ADDR_SUFFIX.search(path.name)
    return int(m.group(1), 16) if m else None


def reset_runner():
    keep = {"main.cpp"}
    for p in RUNNER.glob("*.cpp"):
        if p.name not in keep:
            p.unlink()
    for name in ["ps2_recompiled_functions.h", "ps2_recompiled_stubs.h"]:
        (INCLUDE / name).unlink(missing_ok=True)
    for p in INCLUDE.glob("ps2_recompiled_functions_ov*.h"):
        p.unlink()


def assemble_main():
    out = RECOMP / "output"
    copied = dropped = 0
    for p in out.glob("*.cpp"):
        if p.name == "register_functions.cpp":
            continue
        addr = file_addr(p)
        if addr is not None and addr >= OVERLAY_REGION_START:
            dropped += 1  # embedded copy of ov02; handled by the overlay system
            continue
        shutil.copy2(p, RUNNER / p.name)
        copied += 1

    reg = (out / "register_functions.cpp").read_text()
    base = int(re.search(r"TableBase = (0x[0-9a-f]+)u", reg).group(1), 16)
    slots = (TABLE_END - base) >> 2
    reg = re.sub(r"TableEnd = 0x[0-9a-f]+u", f"TableEnd = {TABLE_END:#x}u", reg)
    reg = re.sub(r"TableSlotCount = \d+u", f"TableSlotCount = {slots}u", reg)
    reg = re.sub(r"g_ps2RecompiledFunctionTable\[\d+u\] = \{\}",
                 f"g_ps2RecompiledFunctionTable[{slots}u] = {{}}", reg)
    lines = []
    for line in reg.splitlines():
        m = TABLE_LINE.search(line)
        if m and int(m.group(2), 16) >= OVERLAY_REGION_START:
            continue
        lines.append(line)
    (RUNNER / "register_functions.cpp").write_text("\n".join(lines) + "\n")

    for name in ["ps2_recompiled_functions.h", "ps2_recompiled_stubs.h"]:
        shutil.copy2(out / name, INCLUDE / name)
    print(f"main: {copied} functions, dropped {dropped} from overlay region, table base {base:#x} slots {slots}")
    return base


def assemble_overlay(ov: str, table_base: int):
    out = RECOMP / ov / "output"
    header = (out / "ps2_recompiled_functions.h").read_text()
    stubs = (out / "ps2_recompiled_stubs.h").read_text()
    names = set(DECL.findall(header)) | set(DECL.findall(stubs))
    prefix = f"xs_{ov}_"
    rename = re.compile(r"\b(" + "|".join(sorted(map(re.escape, names), key=len, reverse=True)) + r")\b")
    new_header = f"ps2_recompiled_functions_{ov}.h"

    def transform(text: str) -> str:
        text = text.replace("<ps2_recompiled_functions.h>", f"<{new_header}>")
        text = text.replace("<ps2_recompiled_stubs.h>", f"<{new_header}>")
        return rename.sub(lambda m: prefix + m.group(1), text)

    count = 0
    for p in out.glob("*.cpp"):
        if p.name == "register_functions.cpp":
            continue
        (RUNNER / f"{ov}_{p.name}").write_text(transform(p.read_text()))
        count += 1

    guard = f"PS2_RECOMPILED_FUNCTIONS_{ov.upper()}_H"
    decls = "\n".join(
        f"void {prefix}{n}(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime);" for n in sorted(names))
    (INCLUDE / new_header).write_text(
        f"#ifndef {guard}\n#define {guard}\n\n#include <cstdint>\n#include \"ps2_runtime.h\"\n"
        f"#include \"ps2_syscalls.h\"\n#include \"ps2_stubs.h\"\n\n{decls}\n\n#endif\n")

    entries = []
    for line in (out / "register_functions.cpp").read_text().splitlines():
        m = TABLE_LINE.search(line)
        if m:
            addr = int(m.group(2), 16)
            entries.append(f"    t[{(addr - table_base) >> 2}u] = {prefix}{m.group(1)}; // {addr:#x}")
    (RUNNER / f"xs_register_{ov}.cpp").write_text(
        f"#include \"ps2_runtime.h\"\n#include <{new_header}>\n\n"
        f"void xs_register_{ov}(PS2Runtime::RecompiledFunction *t)\n{{\n" + "\n".join(entries) + "\n}\n")
    print(f"{ov}: {count} functions, {len(entries)} table entries")


def main():
    reset_runner()
    base = assemble_main()
    for ov in OVERLAYS:
        assemble_overlay(ov, base)
    for p in PORT_SRC.glob("*.cpp"):
        shutil.copy2(p, RUNNER / p.name)
        print(f"port: {p.name}")
    for p in PORT_SRC.glob("*.h"):
        shutil.copy2(p, INCLUDE / p.name)
        print(f"port: {p.name}")
    # Native VU1 blocks from tools/vu1gen.py (generated from game code).
    vu1_blocks = RECOMP / "xenosaga_vu1_blocks.cpp"
    if vu1_blocks.exists():
        shutil.copy2(vu1_blocks, RUNNER / vu1_blocks.name)
        print(f"vu1 blocks: {vu1_blocks.name}")


if __name__ == "__main__":
    sys.exit(main())
