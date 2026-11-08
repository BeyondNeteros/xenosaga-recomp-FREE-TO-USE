#!/usr/bin/env python3
"""Compile whole VU1 microprogram calls into native C++ (native programs).

vu1gen.py makes native versions of single static blocks, but the runtime still
does cycle-accurate bookkeeping between and around them (~30% of VU1 time),
and every instruction still updates the delayed-write pipelines. This tool
compiles a whole call path instead: from an entry pc (where the game starts or
resumes VU1, taken from VU1 captures) through every branch and subroutine to
the E-bit end, as one C++ function with gotos and *sequential* semantics:
every result lands at once and MAC/status/CLIP/Q/P commit immediately.

That equals the interpreter whenever every read sees the newest value. VF and
VI reads are interlocked on the VU (they stall), so they always do. For the
rest the tool proves it per read, along every path of the control-flow graph:

- Q (DIV 7, SQRT 7, RSQRT 13 cycles), P (EFU ops, 11-54), status/MAC flags
  (FMAC 4, FDIV D/I bits), CLIP (4): the newest writer before the reader must
  be at least its latency in pairs earlier (pairs issue at least one cycle
  apart, stalls only add distance). A writer in the reader's own pair is never
  seen: the lower half runs first when it reads flags.
- A branch reading a VI that the previous pair wrote with an integer op sees
  the old value (as the interpreter's readBranchVi): handled with a backup.
- XGKICK sends the whole packet at once: no store may follow it before the
  program ends.

Anything it cannot prove is reported and the program is not generated (fix the
analysis or leave that entry to the interpreter). Unknown jump targets hand
off to the interpreter at that pc. Check with tools/vu1_check.sh: the
captures replay through native programs and must match the reference.

    python port/tools/vu1prog.py runs/*/vu1_capture.bin [--min-calls N]

Writes recomp/xenosaga_vu1_programs.cpp and copies it into
PS2Recomp/ps2xRuntime/src/runner. The output is derived from game code: never
commit it.
"""
import argparse
import os
import shutil
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vucap  # noqa: E402
import vudis  # noqa: E402

ROOT = Path(os.environ.get("XS_ROOT", Path(__file__).resolve().parents[2]))
OUT = ROOT / "recomp" / "xenosaga_vu1_programs.cpp"
RUNNER = Path(os.environ.get("PS2RECOMP", ROOT / "PS2Recomp")) / "ps2xRuntime" / "src" / "runner"
MASK64 = (1 << 64) - 1
CODE_SIZE = 0x4000


def code_hash(code, ranges=None, h=0x243F6A8885A308D3):
    """vu1native::codeHash in ps2_vu1_fastops.h: the whole image, or only the
    given [start, end) byte ranges (the pairs a program was compiled from)."""
    for a, b in ranges or [(0, len(code))]:
        for (w,) in struct.iter_unpack("<Q", code[a:b]):
            h ^= w
            h = (((h << 29) | (h >> 35)) & MASK64) * 0x9E3779B97F4A7C15 & MASK64
    return h


def merge_ranges(pcs):
    out = []
    for pc in sorted(pcs):
        if out and out[-1][1] == pc:
            out[-1][1] = pc + 8
        else:
            out.append([pc, pc + 8])
    return [tuple(r) for r in out]


class GenError(Exception):
    pass


# ---------------------------------------------------------------- decoding

def imm11(i):
    v = i & 0x7FF
    return v - 0x800 if v & 0x400 else v


REV4 = [0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15]  # DEST bits -> component bits
FMAC_FLAG_OPS = set(range(0x00, 0x10)) | set(range(0x18, 0x1C)) | {0x1C, 0x1E} | set(range(0x20, 0x2B)) | {0x2C, 0x2D, 0x2E}
Q_READ_OPS = {0x1C, 0x20, 0x21, 0x24, 0x25}  # mulq addq maddq subq msubq (and the ACC forms)
EFU_LAT = {0x70: 11, 0x71: 18, 0x72: 18, 0x73: 24, 0x74: 54, 0x75: 54, 0x76: 12, 0x77: 18, 0x78: 12,
           0x79: 29, 0x7A: 12, 0x7C: 54, 0x7D: 44}
FDIV_LAT = {0x38: 7, 0x39: 7, 0x3A: 13}


class Upper:
    def __init__(self, w):
        self.w = w
        self.ibit = bool(w >> 31 & 1)
        self.ebit = bool(w >> 30 & 1)
        self.dbit = bool(w >> 28 & 1)
        self.tbit = bool(w >> 27 & 1)
        op = w & 0x3F
        d = (w >> 21) & 0xF
        ft, fs, fd = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
        self.nop = False
        self.write = None      # VF register written (or None)
        self.reads = set()     # VF registers read
        self.acc_write = False
        self.flags = False     # writes MAC/status
        self.clip = False
        self.reads_q = False
        if op >= 0x3C:
            s = (w & 3) | ((w >> 4) & 0x7C)
            if s in (0x2F, 0x30):
                self.nop = True
                return
            if 0x10 <= s <= 0x17 or s == 0x1D:  # ITOF/FTOI/ABS: ft <- fs
                self.reads = {fs}
                self.write = ft if d else None
                return
            if s == 0x1F:  # CLIP
                self.reads = {fs, ft}
                self.clip = True
                return
            self.reads = {fs, ft}
            self.acc_write = bool(d)
            self.flags = s in FMAC_FLAG_OPS and bool(d)
            self.reads_q = s in Q_READ_OPS
            if s not in FMAC_FLAG_OPS:
                raise GenError(f"upper special 0x{s:02x} not handled ({w:08x})")
            return
        self.reads = {fs, ft}
        self.write = fd if d else None
        self.flags = op in FMAC_FLAG_OPS and bool(d)
        self.reads_q = op in Q_READ_OPS
        if op in (0x30, 0x31, 0x32, 0x33):
            raise GenError(f"reserved upper {w:08x}")


class Lower:
    """Lower instruction: what it reads/writes, and its C++."""

    def __init__(self, w, pc):
        self.w, self.pc = w, pc
        self.kind = "op"
        self.vf_reads = set()
        self.vf_write = None
        self.vi_reads = set()
        self.vi_write = None
        self.delays_branch = False   # integer result the next pair's branch does not see yet
        self.store = False
        self.kick = False
        self.reads_flags = None      # 'status' / 'mac' / 'clip'
        self.writes = {}             # resource -> latency ('q', 'p', 'status', 'clip')
        self.reads_p = False
        self.waitq = self.waitp = False
        self.target = None           # branch target (pc) for b/bal/ibxx
        self.jump_reg = None         # jr/jalr register
        self.link = None             # bal/jalr link register
        self.cond = None
        if w in (0, 0x8000033C):
            self.kind = "nop"
            return
        op = (w >> 25) & 0x7F
        it, is_, id_ = (w >> 16) & 15, (w >> 11) & 15, (w >> 6) & 15
        ft, fs = (w >> 16) & 31, (w >> 11) & 31
        self.op = op
        if op == 0x00:
            self.vi_reads, self.vf_write = {is_}, ft
        elif op == 0x01:
            self.vi_reads, self.vf_reads, self.store = {it}, {fs}, True
        elif op == 0x04:
            self.vi_reads, self.vi_write = {is_}, it
        elif op == 0x05:
            self.vi_reads, self.store = {is_, it}, True
        elif op in (0x08, 0x09):
            self.vi_reads, self.vi_write, self.delays_branch = {is_}, it, True
        elif op in (0x10, 0x12, 0x13, 0x1C):
            self.reads_flags = "clip"
            self.vi_write = 1 if op != 0x1C else it
        elif op == 0x11:
            self.writes = {"clip": 4}
        elif op in (0x14, 0x16, 0x17):
            self.reads_flags = "status"
            self.vi_write = it
        elif op == 0x15:
            self.writes = {"status": 4}
        elif op in (0x18, 0x1A, 0x1B):
            self.reads_flags = "mac"
            self.vi_reads, self.vi_write = {is_}, it
        elif op in (0x20, 0x21):
            self.kind = "branch"
            self.target = (pc + 8 + imm11(w) * 8) & (CODE_SIZE - 1)
            self.cond = "true"
            if op == 0x21:
                self.link, self.vi_write = it, it
        elif op in (0x24, 0x25):
            self.kind = "jump"
            self.jump_reg = is_
            self.vi_reads = {is_}
            if op == 0x25:
                self.link, self.vi_write = it, it
        elif op in (0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F):
            self.kind = "branch"
            self.target = (pc + 8 + imm11(w) * 8) & (CODE_SIZE - 1)
            self.vi_reads = {is_, it} if op in (0x28, 0x29) else {is_}
        elif op == 0x40:
            f = w & 0x3F
            if f in (0x30, 0x31, 0x34, 0x35):
                self.vi_reads, self.vi_write, self.delays_branch = {is_, it}, id_, True
            elif f == 0x32:
                self.vi_reads, self.vi_write, self.delays_branch = {is_}, it, True
            elif f >= 0x3C:
                s = (w & 3) | ((w >> 4) & 0x7C)
                self.special = s
                if s in (0x30, 0x31):
                    self.vf_reads, self.vf_write = {fs}, ft
                elif s in (0x34, 0x36):
                    self.vi_reads, self.vi_write, self.vf_write, self.delays_branch = {is_}, is_, ft, True
                elif s in (0x35, 0x37):
                    self.vi_reads, self.vi_write, self.vf_reads, self.delays_branch = {it}, it, {fs}, True
                    self.store = True
                elif s in FDIV_LAT:
                    self.vf_reads = {fs, ft}
                    self.writes = {"q": FDIV_LAT[s], "status": FDIV_LAT[s]}
                elif s == 0x3B:
                    self.waitq = True
                elif s == 0x3C:
                    self.vf_reads, self.vi_write, self.delays_branch = {fs}, it, True
                elif s == 0x3D:
                    self.vi_reads, self.vf_write = {is_}, ft
                elif s == 0x3E:
                    self.vi_reads, self.vi_write = {is_}, it
                elif s == 0x3F:
                    self.vi_reads, self.store = {is_, it}, True
                elif s in (0x40, 0x41, 0x42, 0x43):
                    self.vf_reads = {fs}
                    self.vf_write = ft if s in (0x40, 0x41) else None
                elif s == 0x64:
                    self.reads_p, self.vf_write = True, ft
                elif s in (0x68, 0x69):
                    self.vi_write = it
                elif s == 0x6C:
                    self.vi_reads, self.kick = {is_}, True
                elif s in EFU_LAT:
                    self.vf_reads = {fs}
                    self.writes = {"p": EFU_LAT[s]}
                elif s == 0x7B:
                    self.waitp = True
                else:
                    raise GenError(f"lower special 0x{s:02x} not handled at 0x{pc:04x}")
            else:
                raise GenError(f"lower {w:08x} not handled at 0x{pc:04x}")
        else:
            raise GenError(f"lower {w:08x} not handled at 0x{pc:04x}")
        if self.vi_write == 0:
            self.vi_write = None
        if self.vf_write == 0:
            self.vf_write = None

    # C++ for the instruction (branches/jumps are emitted by the caller).
    def cpp(self, bk_regs=()):
        w = self.w
        if self.kind == "nop":
            return []
        op = self.op
        it, is_, id_ = (w >> 16) & 15, (w >> 11) & 15, (w >> 6) & 15
        ft, fs = (w >> 16) & 31, (w >> 11) & 31
        d = (w >> 21) & 0xF
        m = REV4[d]
        imm = imm11(w)
        if op == 0x00:
            return [f"F_lq(s.vf[{ft}], D, s.vi[{is_}] + ({imm}), {m});"] if ft else []
        if op == 0x01:
            return [f"F_sq(D, s.vi[{it}] + ({imm}), s.vf[{fs}], {m});"]
        if op == 0x04:
            comp = 0 if d & 8 else 1 if d & 4 else 2 if d & 2 else 3
            return [f"s.vi[{it}] = F_ilw(D, s.vi[{is_}] + ({imm}), {comp});"] if it else []
        if op == 0x05:
            return [f"F_isw(D, s.vi[{is_}] + ({imm}), s.vi[{it}], {m});"]
        if op in (0x08, 0x09):
            imm15 = (w & 0x7FF) | ((w >> 10) & 0x7800)
            sign = "+" if op == 0x08 else "-"
            return [f"s.vi[{it}] = static_cast<int16_t>(s.vi[{is_}] {sign} {imm15});"] if it else []
        if op in (0x10, 0x12, 0x13, 0x1C):
            imm24 = w & 0xFFFFFF
            if op == 0x10:
                return [f"s.vi[1] = (s.clip & 0xFFFFFFu) == 0x{imm24:x}u ? 1 : 0;"]
            if op == 0x12:
                return [f"s.vi[1] = (s.clip & 0x{imm24:x}u) != 0u ? 1 : 0;"]
            if op == 0x13:
                return [f"s.vi[1] = (s.clip | 0x{imm24:x}u) == 0xFFFFFFu ? 1 : 0;"]
            return [f"s.vi[{it}] = static_cast<int32_t>(s.clip & 0x0FFFu);"] if it else []
        if op in (0x14, 0x16, 0x17):
            imm12 = (((w >> 21) & 1) << 11) | (w & 0x7FF)
            if not it:
                return []
            if op == 0x14:
                return [f"s.vi[{it}] = (s.status & 0xFFFu) == 0x{imm12:x}u ? 1 : 0;"]
            if op == 0x16:
                return [f"s.vi[{it}] = static_cast<int32_t>(s.status & 0x{imm12:x}u);"]
            return [f"s.vi[{it}] = static_cast<int32_t>((s.status & 0xFFFu) | 0x{imm12:x}u);"]
        if op in (0x11, 0x15, 0x18, 0x1A, 0x1B):
            return [f"F::nLowerGeneric<0x{w:08x}u>(vu, D, ds);"]
        if op == 0x40:
            f = w & 0x3F
            if f in (0x30, 0x31):
                sign = "+" if f == 0x30 else "-"
                return [f"s.vi[{id_}] = static_cast<int16_t>(s.vi[{is_}] {sign} s.vi[{it}]);"] if id_ else []
            if f in (0x34, 0x35):
                o = "&" if f == 0x34 else "|"
                return [f"s.vi[{id_}] = s.vi[{is_}] {o} s.vi[{it}];"] if id_ else []
            if f == 0x32:
                v = (w >> 6) & 31
                v = v - 32 if v & 16 else v
                return [f"s.vi[{it}] = static_cast<int16_t>(s.vi[{is_}] + ({v}));"] if it else []
            s_ = self.special
            if s_ == 0x30:
                return [f"F_lanes(s.vf[{ft}], _mm_loadu_ps(s.vf[{fs}]), {m});"] if ft else []
            if s_ == 0x31:
                return [f"F_lanes(s.vf[{ft}], F_mr32(s.vf[{fs}]), {m});"] if ft else []
            if s_ == 0x34:
                out = [f"F_lq(s.vf[{ft}], D, static_cast<uint16_t>(s.vi[{is_}]), {m});"] if ft else []
                if is_:
                    out.append(f"s.vi[{is_}] = static_cast<int16_t>(s.vi[{is_}] + 1);")
                return out
            if s_ == 0x35:
                out = [f"F_sq(D, static_cast<uint16_t>(s.vi[{it}]), s.vf[{fs}], {m});"]
                if it:
                    out.append(f"s.vi[{it}] = static_cast<int16_t>(s.vi[{it}] + 1);")
                return out
            if s_ == 0x36:
                out = [f"s.vi[{is_}] = static_cast<int16_t>(s.vi[{is_}] - 1);"] if is_ else []
                if ft:
                    out.append(f"F_lq(s.vf[{ft}], D, static_cast<uint16_t>(s.vi[{is_}]), {m});")
                return out
            if s_ == 0x37:
                out = [f"s.vi[{it}] = static_cast<int16_t>(s.vi[{it}] - 1);"] if it else []
                out.append(f"F_sq(D, static_cast<uint16_t>(s.vi[{it}]), s.vf[{fs}], {m});")
                return out
            if s_ in (0x3B, 0x7B):
                return []
            if s_ == 0x3C:
                comp = (w >> 21) & 3
                return [f"s.vi[{it}] = F_mtir(s.vf[{fs}][{comp}]);"] if it else []
            if s_ == 0x3D:
                return [f"F_lanes(s.vf[{ft}], F_mfir(s.vi[{is_}]), {m});"] if ft else []
            if s_ == 0x64:
                return [f"F_lanes(s.vf[{ft}], _mm_set1_ps(s.p), {m});"] if ft else []
            if s_ == 0x68:
                return [f"s.vi[{it}] = static_cast<int32_t>(s.top & 0x3FFu);"] if it else []
            if s_ == 0x69:
                return [f"s.vi[{it}] = static_cast<int32_t>(s.itop & 0x3FFu);"] if it else []
            if s_ == 0x6C:
                return [f"F::nKick(vu, s.vi[{is_}]);"]
            # DIV/SQRT/RSQRT, EFU, ILWR/ISWR, R*: the interpreter's own code.
            return [f"F::nLowerGeneric<0x{w:08x}u>(vu, D, ds);"]
        raise GenError(f"no C++ for lower {w:08x}")


class Pair:
    def __init__(self, code, pc):
        self.pc = pc
        lo, up = struct.unpack_from("<II", code, pc)
        self.lo_w, self.up_w = lo, up
        self.upper = Upper(up)
        if self.upper.dbit or self.upper.tbit:
            raise GenError(f"D/T bit at 0x{pc:04x}")
        self.lower = None if self.upper.ibit else Lower(lo, pc)
        l = vudis.lower(lo, pc) if not self.upper.ibit else f"loi {struct.unpack('<f', struct.pack('<I', lo))[0]:.6g}"
        self.text = f"{vudis.upper(up)} | {l}"

    @property
    def is_branch(self):
        return self.lower is not None and self.lower.kind in ("branch", "jump")


# ---------------------------------------------------------------- program

class Program:
    def __init__(self, code, entry, reg_values):
        self.code = code
        self.entry = entry
        self.reg_values = reg_values  # VI register -> set of values seen at entry (captures)
        self.pairs = {}
        self.succ = defaultdict(set)   # pc -> next pcs (for timing analysis)
        self.pred = defaultdict(set)
        self.jump_targets = {}         # jump pc -> sorted candidate targets
        self.handoff = set()           # jump pcs whose unknown targets hand off
        self.labels = {entry}
        self.ends = set()              # pcs of the pair after an E-bit pair
        self.problems = []
        self.reachable = set()
        while True:
            self.pairs, self.succ, self.pred = {}, defaultdict(set), defaultdict(set)
            self.jump_targets, self.handoff, self.labels, self.ends = {}, set(), {entry}, set()
            self.delay_only = set()
            self.explore()
            now = set(self.pairs)
            if now <= self.reachable:
                break
            self.reachable |= now

    def pair(self, pc):
        if pc not in self.pairs:
            self.pairs[pc] = Pair(self.code, pc)
        return self.pairs[pc]

    def reg_candidates(self, reg):
        vals = set(v * 8 for v in self.reg_values.get(reg, ()))
        # Every constant or link the reachable code puts in that register
        # (explore() repeats until this set stops growing).
        for pc in sorted(self.reachable):
            lo, up = struct.unpack_from("<II", self.code, pc)
            if up >> 31:
                continue
            op = (lo >> 25) & 0x7F
            if op == 0x08 and (lo >> 16) & 15 == reg and (lo >> 11) & 15 == 0:
                vals.add(((lo & 0x7FF) | ((lo >> 10) & 0x7800)) * 8)
            if op in (0x21, 0x25) and (lo >> 16) & 15 == reg:
                vals.add(pc + 16)
        return sorted(v & (CODE_SIZE - 1) for v in vals if 0 <= v < CODE_SIZE)

    def local_constant(self, pc, reg):
        """[target] when the straight-line code just before pc (no branch in
        between) sets reg with iaddiu reg, vi0, imm; else None."""
        if not hasattr(self, "static_targets"):
            self.static_targets = set()
            for a in range(0, CODE_SIZE, 8):
                lo, up = struct.unpack_from("<II", self.code, a)
                if not up >> 31 and (lo >> 25) & 0x7F in (0x20, 0x21, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F):
                    self.static_targets.add((a + 8 + imm11(lo) * 8) & (CODE_SIZE - 1))
        q = pc
        for _ in range(64):
            if q in self.static_targets:
                return None  # another path joins after the definition
            q = (q - 8) % CODE_SIZE
            lo, up = struct.unpack_from("<II", self.code, q)
            if up >> 31:
                continue
            l = Lower(lo, q)
            if l.kind in ("branch", "jump") or (up >> 30) & 1:
                return None
            if l.vi_write == reg:
                if l.op == 0x08 and (lo >> 11) & 15 == 0:
                    return [(((lo & 0x7FF) | ((lo >> 10) & 0x7800)) * 8) & (CODE_SIZE - 1)]
                return None
        return None

    def explore(self):
        work = [self.entry]
        seen = set()
        while work:
            pc = work.pop()
            if pc in seen:
                continue
            seen.add(pc)
            p = self.pair(pc)
            nxt = []
            if p.upper.ebit:
                end = (pc + 8) % CODE_SIZE
                q = self.pair(end)
                if q.is_branch or q.upper.ebit:
                    raise GenError(f"branch/E-bit after E-bit at 0x{pc:04x}")
                self.ends.add(end)
                self.add_edge(pc, end)
                seen.add(end)
                if p.is_branch:
                    raise GenError(f"branch with E-bit at 0x{pc:04x}")
                continue
            if p.is_branch:
                ds = (pc + 8) % CODE_SIZE
                q = self.pair(ds)
                if q.is_branch or q.upper.ebit:
                    raise GenError(f"branch/E-bit in delay slot at 0x{ds:04x}")
                self.add_edge(pc, ds)
                targets = []
                if p.lower.kind == "branch":
                    targets.append(p.lower.target)
                    self.labels.add(p.lower.target)
                    if p.lower.cond != "true":
                        targets.append((pc + 16) % CODE_SIZE)
                else:
                    cands = self.local_constant(pc, p.lower.jump_reg)
                    if cands is None:
                        cands = self.reg_candidates(p.lower.jump_reg)
                    self.jump_targets[pc] = cands
                    self.handoff.add(pc)
                    targets += cands
                    self.labels.update(cands)
                for t in targets:
                    self.add_edge(ds, t)
                    nxt.append(t)
                # The delay slot itself may also be a jump target elsewhere;
                # then it falls through (seen via explore of that target).
                self.delay_only.add(ds)
            else:
                t = (pc + 8) % CODE_SIZE
                self.add_edge(pc, t)
                nxt.append(t)
            work.extend(nxt)
        # A delay slot that is also a label is emitted on its own as well.
        self.delay_only -= self.labels

    def add_edge(self, a, b):
        self.succ[a].add(b)
        self.pred[b].add(a)

    # Pairs executed before pc, up to depth back (distance, pair) along every
    # path; stops at the entry (empty pipelines).
    def back(self, pc, depth):
        out = []
        frontier = [(pc, 0)]
        seen = set()
        while frontier:
            q, dist = frontier.pop()
            for p in self.pred.get(q, ()):
                if dist + 1 > depth or (p, dist + 1) in seen:
                    continue
                seen.add((p, dist + 1))
                out.append((dist + 1, p))
                frontier.append((p, dist + 1))
        return out

    def writes(self, pc, res):
        p = self.pairs[pc]
        if res == "status":
            if p.upper.flags:
                return 4
        elif res == "mac":
            if p.upper.flags:
                return 4
        elif res == "clip":
            if p.upper.clip:
                return 4
        if p.lower is not None:
            if res == "mac":
                return None
            return p.lower.writes.get(res)
        return None

    def barrier(self, pc, res):
        p = self.pairs[pc]
        if p.lower is None:
            return False
        return (res == "q" and p.lower.waitq) or (res == "p" and p.lower.waitp)

    def check_read(self, pc, res, what):
        """The newest writer of res before pc must be visible on every path."""
        frontier = [(pc, 0)]
        seen = set()
        while frontier:
            q, dist = frontier.pop()
            for p in self.pred.get(q, ()):
                nd = dist + 1
                if (p, nd) in seen or nd > 60:
                    continue
                seen.add((p, nd))
                lat = self.writes(p, res)
                if lat is not None:
                    if nd < lat:
                        self.problems.append(f"0x{pc:04x} {what} reads {res} written {nd} pair(s) earlier at "
                                             f"0x{p:04x} (latency {lat})")
                    continue  # newest writer on this path found
                if self.barrier(p, res):
                    continue
                frontier.append((p, nd))

    def analyse(self):
        for pc, p in sorted(self.pairs.items()):
            lo = p.lower
            if p.upper.reads_q:
                self.check_read(pc, "q", "upper")
            if lo is not None:
                if lo.reads_flags:
                    self.check_read(pc, lo.reads_flags, "lower")
                    if lo.reads_flags in ("status", "mac") and lo.writes.get("status"):
                        pass
                if lo.reads_p:
                    self.check_read(pc, "p", "lower")
                if lo.kick:
                    self.check_kick(pc)
            # upper writing vf0 while the lower reads it: lower would see it
            if p.upper.write == 0 and lo is not None and 0 in lo.vf_reads:
                self.problems.append(f"0x{pc:04x} upper writes vf0, lower reads it")
        # branch-VI: previous pair's integer write is not seen by the branch
        self.bk = {}  # branch pc -> set of regs read from backup
        self.bk_save = defaultdict(set)  # pred pc -> regs to back up before its lower
        for pc, p in self.pairs.items():
            if not p.is_branch:
                continue
            for r in p.lower.vi_reads:
                if r == 0:
                    continue
                kinds = set()
                for q in self.pred.get(pc, ()):
                    ql = self.pairs[q].lower
                    kinds.add(ql is not None and ql.delays_branch and ql.vi_write == r)
                if kinds == {True}:
                    self.bk.setdefault(pc, set()).add(r)
                    for q in self.pred[pc]:
                        self.bk_save[q].add(r)
                elif kinds == {True, False}:
                    self.problems.append(f"0x{pc:04x} branch reads vi{r}: old value on some paths only")

    def check_kick(self, pc):
        # No store may run after the kick until the program ends.
        work = [pc]
        seen = set()
        while work:
            q = work.pop()
            for n in self.succ.get(q, ()):
                if n in seen:
                    continue
                seen.add(n)
                p = self.pairs[n]
                if p.lower is not None and p.lower.store:
                    self.problems.append(f"0x{pc:04x} xgkick followed by a store at 0x{n:04x}")
                    return
                if p.lower is not None and p.lower.kick:
                    continue  # the next kick waits for this one anyway
                work.append(n)

    # ------------------------------------------------------------ emission

    def pair_cpp(self, pc, indent):
        """C++ for one pair (not the branch transfer)."""
        p = self.pairs[pc]
        ind = " " * indent
        out = [f"{ind}// {pc:04x}: {p.text}", f"{ind}++n;"]
        for r in sorted(self.bk_save.get(pc, ())):
            out.append(f"{ind}bk[{r}] = s.vi[{r}];")
        up = [] if p.upper.nop else [f"F::nUpper<0x{p.up_w:08x}u>(vu);"]
        if p.upper.write == 0:
            up.append("F_vf0(s);")
        if p.upper.ibit:
            out += [ind + x for x in up]
            out.append(f"{ind}s.i = F_norm(0x{p.lo_w:08x}u);")
            return out
        lo = p.lower
        low = [] if lo.kind in ("branch", "jump") else lo.cpp()
        if lo.kind in ("branch", "jump") and lo.link:
            low = [f"s.vi[{lo.link}] = {(pc + 16) // 8};"]
        uw = p.upper.write
        if lo.reads_flags and (p.upper.flags or p.upper.clip):
            # The lower reads flags before this pair's upper result commits.
            if (lo.vf_write is not None and lo.vf_write in p.upper.reads) or (uw is not None and uw in lo.vf_reads):
                raise GenError(f"0x{pc:04x}: cannot order flag read before upper")
            out += [ind + x for x in low + up]
        elif uw is not None and uw != 0 and (uw in lo.vf_reads or lo.vf_write == uw):
            # Shadow: the lower sees the register before the upper's write;
            # the upper's write wins over the lower's.
            out.append(f"{ind}{{ const __m128 o_ = _mm_loadu_ps(s.vf[{uw}]);")
            out += [ind + "  " + x for x in up]
            out.append(f"{ind}  const __m128 u_ = _mm_loadu_ps(s.vf[{uw}]); _mm_storeu_ps(s.vf[{uw}], o_);")
            out += [ind + "  " + x for x in low]
            out.append(f"{ind}  _mm_storeu_ps(s.vf[{uw}], u_); }}")
        else:
            out += [ind + x for x in up + low]
        return out

    def vi_expr(self, pc, r):
        if r == 0:
            return "0"
        if r in self.bk.get(pc, ()):
            return f"static_cast<int16_t>(bk[{r}])"
        return f"static_cast<int16_t>(s.vi[{r}])"

    def goto(self, src, dst, ind):
        if dst <= src:  # backward: loop, check the budget
            return [f"{ind}if (n > maxPairs) return F::nHandoff(vu, 0x{dst:04x}u, n);", f"{ind}goto L_{dst:04x};"]
        return [f"{ind}goto L_{dst:04x};"]

    def emit(self, name):
        order = sorted(pc for pc in self.pairs if pc not in self.delay_only and pc not in self.ends)
        lines = [f"bool {name}(VU1Interpreter &vu, uint8_t *D, uint32_t ds, uint64_t maxPairs)", "{",
                 "    VU1State &s = F::st(vu);", "    uint64_t n = 0;", "    int32_t bk[16];", "    (void)bk;",
                 f"    if (ds != 0x4000u) return F::nHandoff(vu, 0x{self.entry:04x}u, 0);"]
        if order[0] != self.entry:
            lines.append(f"    goto L_{self.entry:04x};")
        gotos = set()
        body = []
        for idx, pc in enumerate(order):
            p = self.pairs[pc]
            nxt_emitted = order[idx + 1] if idx + 1 < len(order) else None
            body.append(f"L_{pc:04x}:")
            body += self.pair_cpp(pc, 4)
            if p.upper.ebit:
                end = (pc + 8) % CODE_SIZE
                body += self.pair_cpp(end, 4)
                body.append(f"    return F::nEnd(vu, 0x{(end + 8) % CODE_SIZE:04x}u, n);")
                continue
            if p.is_branch:
                lo = p.lower
                ds = (pc + 8) % CODE_SIZE
                op = lo.op
                body.append("    {")
                if lo.kind == "branch":
                    a, b = self.vi_expr(pc, (lo.w >> 11) & 15), self.vi_expr(pc, (lo.w >> 16) & 15)
                    cond = {0x20: "true", 0x21: "true", 0x28: f"{a} == {b}", 0x29: f"{a} != {b}", 0x2C: f"{a} < 0",
                            0x2D: f"{a} > 0", 0x2E: f"{a} <= 0", 0x2F: f"{a} >= 0"}[op]
                    body.append(f"        const bool t_ = {cond};")
                else:
                    r = lo.jump_reg
                    reg = self.vi_expr(pc, r).replace("int16_t", "uint16_t")
                    body.append(f"        const uint32_t j_ = (static_cast<uint32_t>({reg}) * 8u) & 0x3FFFu;")
                # The link (bal/jalr) was written by the branch pair above.
                body += self.pair_cpp(ds, 8)
                if lo.kind == "branch":
                    if lo.cond == "true":
                        body += self.goto(pc, lo.target, "        ")
                        gotos.add(lo.target)
                    else:
                        body.append("        if (t_) {")
                        body += self.goto(pc, lo.target, "            ")
                        body.append("        }")
                        gotos.add(lo.target)
                else:
                    body.append("        switch (j_) {")
                    for t in self.jump_targets[pc]:
                        body.append(f"        case 0x{t:04x}u:")
                        body += self.goto(pc, t, "            ")
                        gotos.add(t)
                    body.append("        default: return F::nHandoff(vu, j_, n);")
                    body.append("        }")
                body.append("    }")
                if lo.kind == "branch" and lo.cond != "true":
                    ft = (pc + 16) % CODE_SIZE
                    if ft != nxt_emitted:
                        body += self.goto(pc, ft, "    ")
                        gotos.add(ft)
                continue
            ft = (pc + 8) % CODE_SIZE
            if ft != nxt_emitted:
                body += self.goto(pc, ft, "    ")
                gotos.add(ft)
        missing = gotos - set(order)
        if missing:
            raise GenError("jump to pcs not emitted: " + ", ".join(f"0x{m:04x}" for m in sorted(missing)))
        used = gotos | {self.entry}
        for line in body:
            if line.startswith("L_") and int(line[2:6], 16) not in used:
                continue
            lines.append(line)
        lines += ["}", ""]
        return "\n".join(lines)


PRELUDE = r"""// Generated by port/tools/vu1prog.py from VU1 microcode the game ran.
// Derived from game code: never commit or distribute this file.
#include "runtime/ps2_vu1_fastops.h"

namespace
{
using F = VU1FastOps;
using vu1fast::storeLanes;

// VU data addresses as the interpreter forms them (VU1: 16 KB).
PS2X_VU1_INLINE uint8_t *F_qw(uint8_t *D, int32_t qword) { return D + ((static_cast<uint32_t>(qword) * 16u) & 0x3FF0u); }
PS2X_VU1_INLINE void F_lanes(float *dst, __m128 v, int mask)
{
    if (mask == 15)
        _mm_storeu_ps(dst, v);
    else
        storeLanes(dst, v, mask);
}
PS2X_VU1_INLINE void F_lq(float *dst, uint8_t *D, int32_t qword, int mask)
{
    F_lanes(dst, _mm_loadu_ps(reinterpret_cast<const float *>(F_qw(D, qword))), mask);
}
PS2X_VU1_INLINE void F_sq(uint8_t *D, int32_t qword, const float *src, int mask)
{
    F_lanes(reinterpret_cast<float *>(F_qw(D, qword)), _mm_loadu_ps(src), mask);
}
PS2X_VU1_INLINE int32_t F_ilw(uint8_t *D, int32_t qword, int comp)
{
    uint32_t v;
    std::memcpy(&v, F_qw(D, qword) + comp * 4, 4);
    return static_cast<int32_t>(static_cast<int16_t>(v & 0xFFFFu));
}
PS2X_VU1_INLINE void F_isw(uint8_t *D, int32_t qword, int32_t vi, int mask)
{
    const uint32_t v = static_cast<uint16_t>(vi & 0xFFFF);
    F_lanes(reinterpret_cast<float *>(F_qw(D, qword)), _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(v))), mask);
}
PS2X_VU1_INLINE __m128 F_mr32(const float *v)
{
    const __m128 x = _mm_loadu_ps(v);
    return _mm_shuffle_ps(x, x, _MM_SHUFFLE(0, 3, 2, 1));
}
PS2X_VU1_INLINE int32_t F_mtir(float f)
{
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return static_cast<int32_t>(static_cast<int16_t>(b & 0xFFFFu));
}
PS2X_VU1_INLINE __m128 F_mfir(int32_t vi)
{
    return _mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(static_cast<int16_t>(vi & 0xFFFF))));
}
PS2X_VU1_INLINE float F_norm(uint32_t bits) // normalizeOperand
{
    const uint32_t e = bits & 0x7F800000u;
    if (e == 0u)
        bits &= 0x80000000u;
    else if (e == 0x7F800000u)
        bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
PS2X_VU1_INLINE void F_vf0(VU1State &s)
{
    s.vf[0][0] = s.vf[0][1] = s.vf[0][2] = 0.0f;
    s.vf[0][3] = 1.0f;
}

"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("captures", nargs="+")
    ap.add_argument("--min-calls", type=int, default=200)
    ap.add_argument("--list", action="store_true", help="print the analysis, write nothing")
    args = ap.parse_args()

    calls = Counter()
    codes = {}
    regs = defaultdict(lambda: defaultdict(set))
    for path in args.captures:
        for r in vucap.records(path):
            pc = r["pc"] if r["kind"] == 1 else r["start"]
            h = code_hash(r["code"])
            codes.setdefault(h, r["code"])
            calls[(h, pc)] += 1
            vi = struct.unpack_from("<16i", r["state"], 512)
            for k in range(1, 16):
                if len(regs[(h, pc)][k]) < 64:
                    regs[(h, pc)][k].add(vi[k] & 0xFFFF)

    out = [PRELUDE]
    entries = []
    failed = 0
    for (h, pc), n in calls.most_common():
        if n < args.min_calls:
            continue
        name = f"p_{h & 0xFFFFFFFF:08x}_{pc:04x}"
        try:
            prog = Program(codes[h], pc, regs[(h, pc)])
            prog.analyse()
            if prog.problems:
                raise GenError("\n    ".join(prog.problems))
            text = prog.emit(name)
        except GenError as e:
            print(f"SKIP code {h:016x} pc 0x{pc:04x} ({n} calls):\n    {e}")
            failed += 1
            continue
        print(f"ok   code {h:016x} pc 0x{pc:04x} ({n} calls): {len(prog.pairs)} pairs, "
              f"{len(prog.jump_targets)} jumps, {sum(len(v) for v in prog.bk.values())} branch-VI backups")
        out.append(text)
        ranges = merge_ranges(prog.pairs)
        rh = code_hash(codes[h], ranges)
        out.append(f"const uint16_t {name}_ranges[] = {{" + ", ".join(f"0x{a:04x}, 0x{b:04x}" for a, b in ranges) + "};")
        out.append("")
        entries.append((rh, pc, name, len(ranges)))
    if args.list:
        return
    out.append("const vu1native::ProgramEntry kPrograms[] = {")
    seen = set()
    for rh, pc, name, nr in entries:
        if (rh, pc) in seen:
            continue  # same pairs in another image: one function is enough
        seen.add((rh, pc))
        out.append(f"    {{0x{rh:016x}ull, 0x{pc:04x}u, {name}_ranges, {nr}u, {name}}},")
    if not entries:
        out.append("    {0, 0, nullptr, 0, nullptr},")
    out += ["};", "",
            "struct Register",
            "{",
            "    Register() { vu1native::registerPrograms(kPrograms, sizeof(kPrograms) / sizeof(kPrograms[0])); }",
            "} g_register;",
            "}", ""]
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(out))
    if RUNNER.is_dir():
        shutil.copy2(OUT, RUNNER / OUT.name)
    print(f"{len(entries)} programs ({failed} skipped) -> {OUT}")


if __name__ == "__main__":
    main()
