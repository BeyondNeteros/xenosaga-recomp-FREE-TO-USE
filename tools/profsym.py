# Symbolize XS_PROFILE output (texe/chain lines) against the exe that ran:
#   python port/tools/profsym.py <exe> <xs_profile.txt> [thread id]
# Needs MSYS2 nm on PATH. Thread ids: busiest first from the "tmod"/"texe" lines.
import bisect, collections, subprocess, sys
exe, prof = sys.argv[1], sys.argv[2]
only = sys.argv[3] if len(sys.argv) > 3 else None
syms = []
for line in subprocess.run(["nm", "-C", "--defined-only", exe], capture_output=True, text=True).stdout.splitlines():
    p = line.split(" ", 2)
    if len(p) == 3 and p[1] in "tTwW":
        syms.append((int(p[0], 16) - 0x140000000, p[2]))
syms.sort()
keys = [s[0] for s in syms]
def name(off):
    i = bisect.bisect_right(keys, off) - 1
    return syms[i][1][:110] if i >= 0 else "?"
flat = collections.Counter(); chains = collections.Counter()
for line in open(prof):
    w = line.split()
    if w[0] == "texe" and (only is None or w[1] == only):
        flat[name(int(w[2],16))] += int(w[3])
    elif w[0] == "chain" and (only is None or w[1] == only):
        chains[" <- ".join(name(int(x,16))[:60] for x in w[3:])] += int(w[2])
tot = sum(flat.values())
print("flat (in exe):", tot)
for k, v in flat.most_common(30): print(f"{v:6d} {100*v/tot:5.1f}%  {k}")
print("\nchains:")
for k, v in chains.most_common(25): print(f"{v:6d}  {k}")
