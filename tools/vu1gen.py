#!/usr/bin/env python3
"""Generate native C++ for VU1 microcode blocks.

The runtime logs every static VU1 block it runs without a native version when
PS2X_VU1_BLOCKLOG=<file> is set (the game, or vu1_replay over captures). This
turns one or more such logs into C++: each block becomes a function that runs
its pairs with the instruction words as template constants, so the decode and
dispatch fold away (semantics come from include/runtime/ps2_vu1_fastops.h and
the interpreter, so results are identical; check with tools/vu1_check.sh).

    python port/tools/vu1gen.py runs/vu1blocks/*.vbl

Writes recomp/xenosaga_vu1_blocks.cpp and copies it into
PS2Recomp/ps2xRuntime/src/runner (assemble.py also copies it). The output is
derived from game code: never commit it.
"""
import shutil
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "recomp" / "xenosaga_vu1_blocks.cpp"
RUNNER = ROOT / "PS2Recomp" / "ps2xRuntime" / "src" / "runner"


def read_logs(paths):
    blocks = {}
    for path in paths:
        data = Path(path).read_bytes()
        i = 0
        while i + 20 <= len(data):
            if data[i:i + 4] != b"VBL1":
                raise SystemExit(f"{path}: bad record at {i}")
            key, start, count = struct.unpack_from("<QII", data, i + 4)
            i += 20
            pairs = []
            for _ in range(count):
                lower, upper, plain = struct.unpack_from("<IIB", data, i)
                pairs.append((lower, upper, plain))
                i += 9
            blocks.setdefault(key, (start, pairs))
    return blocks


# Upper ops with a fast FMAC path (vu1fast::kFastOps): they always write MAC
# and status when dest != 0, fast or not.
FMAC_OPS = set(range(0x00, 0x10)) | set(range(0x18, 0x1C)) | {0x1C, 0x1E} | set(range(0x20, 0x2B)) | {0x2C, 0x2D}


def writes_mac(upper):
    op = upper & 0x3F
    if op >= 0x3C:
        op = (upper & 3) | ((upper >> 4) & 0x7C)  # ACC forms
        if op in (0x2F, 0x30):
            return False  # NOP
    return op in FMAC_OPS and ((upper >> 21) & 0xF) != 0


def reads_flags(lower, upper):
    # FCEQ..FMOR (lower opcodes 0x10-0x1C); not when the lower word is an I-bit immediate.
    return not (upper & 0x80000000) and 0x10 <= ((lower >> 25) & 0x7F) <= 0x1C


def dead_flag_pairs(pairs):
    """Plain pairs whose FMAC flag entry nobody can observe. For op i take the
    next MAC/status writer j: if j is at least 4 pairs before the block's end
    (its entry commits before the block ends) and no flag read sits in pairs
    i+1 .. j+3 (a read in pair k sees entries ready by its cycle, and pair
    k >= j+4 is at least 4 cycles after j), i's MAC/status is replaced before
    anything can read it. Its sticky bits are folded into the next entry."""
    n = len(pairs)
    reads = [reads_flags(lo, up) for lo, up, _ in pairs]
    writers = [writes_mac(up) for _, up, _ in pairs]
    dead = set()
    for i, (lower, upper, plain) in enumerate(pairs):
        if not (plain and not plain & 8 and plain & 2 and writers[i]):
            continue
        j = next((k for k in range(i + 1, n) if writers[k]), None)
        if j is None or j > n - 5:
            continue
        if any(reads[k] for k in range(i + 1, min(n, j + 4))):
            continue
        dead.add(i)
    return dead


def emit_block(key, start, pairs):
    lines = [
        f"// start pair {start} (pc 0x{start * 8:04x}), {len(pairs)} pairs",
        f"bool b_{key:016x}(VU1Interpreter &vu, const VU1Interpreter::StaticBlock &b, uint32_t s, uint32_t cs,",
        "                       uint8_t *d, uint32_t ds, GS &gs, PS2Memory *m)",
        "{",
        "    const uint64_t e = F::cycle(vu);",
        "    bool ended = false;",
    ]
    dead = dead_flag_pairs(pairs)
    for i, (lower, upper, plain) in enumerate(pairs):
        if i > 0:
            lines.append(f"    F::step(vu, e + b.offsets[{i}]);")
        if plain == 0 or plain & 8:
            # Branch / E / D / T / I-bit / last pair (0), or a pair needing the
            # shadow or branch-VI handling (8): full pair logic, constant words.
            lines.append(f"    F::pair<0x{upper:08x}u, 0x{lower:08x}u>(vu, F::decoded(vu, s + {i}u), d, ds, gs, m);")
            if plain == 0:
                lines.append(f"    ended = F::finishPairC<0x{upper:08x}u, 0x{lower:08x}u>(vu, cs);")
            else:
                lines.append("    F::plainEndKeepBackup(vu);")
            continue
        parts = []
        if plain & 2:
            parts.append(f"F::upperC<0x{upper:08x}u{', true' if i in dead else ''}>(vu);")
        if plain & 4:
            parts.append(f"F::lowerC<0x{lower:08x}u, 0x{upper:08x}u>(vu, d, ds, gs, m);")
        parts.append("F::plainEnd(vu);")
        lines.append("    " + " ".join(parts))
    lines += ["    return F::finish(vu, b, e, ended);", "}", ""]
    return "\n".join(lines)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    blocks = read_logs(sys.argv[1:])
    keys = sorted(blocks)
    out = [
        "// Generated by port/tools/vu1gen.py from VU1 microcode the game ran.",
        "// Derived from game code: never commit or distribute this file.",
        '#include "runtime/ps2_vu1_fastops.h"',
        "",
        "namespace",
        "{",
        "using F = VU1FastOps;",
        "",
    ]
    for key in keys:
        start, pairs = blocks[key]
        out.append(emit_block(key, start, pairs))
    out.append("const VU1Interpreter::CompiledBlockEntry kBlocks[] = {")
    for key in keys:
        out.append(f"    {{0x{key:016x}ull, b_{key:016x}}},")
    out += [
        "};",
        "",
        "struct Register",
        "{",
        "    Register() { VU1Interpreter::registerCompiledBlocks(kBlocks, sizeof(kBlocks) / sizeof(kBlocks[0])); }",
        "} g_register;",
        "}",
        "",
    ]
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(out))
    if RUNNER.is_dir():
        shutil.copy2(OUT, RUNNER / OUT.name)
    total = sum(len(blocks[k][1]) for k in keys)
    print(f"{len(keys)} blocks, {total} pairs -> {OUT}")


if __name__ == "__main__":
    main()
