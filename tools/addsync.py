#!/usr/bin/env python3
"""Add load sync points to an input script from a run's log.

    python port/tools/addsync.py script.txt xenosaga.log [-o out.txt]

The log must come from a run that followed the script correctly (it ended in
the right place) and have landmark lines with pad frames:
    [landmark] 12.3s f1234 file data\\scene\\ST0241.evt
Each becomes '@ 1234 file data\\scene\\ST0241.evt' in the script. On replay
ps2_pad.cpp waits at such a frame until the game reaches that landmark, or
jumps ahead when it comes early, so host speed (load times) can't shift the
inputs. New recordings get these lines on their own. Existing '@' lines are
replaced.
"""
import argparse
import re
import sys

LANDMARK = re.compile(r"\[landmark\] (?:[0-9.]+s )?f(\d+) (\S+) ?(.*)$")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("script")
    ap.add_argument("log")
    ap.add_argument("-o", "--out")
    args = ap.parse_args()

    syncs = []
    with open(args.log, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LANDMARK.search(line.rstrip("\r\n"))
            if m:
                key = (m.group(2) + " " + m.group(3)).rstrip()
                syncs.append((int(m.group(1)), key))
    if not syncs:
        sys.exit("no '[landmark] ... fN ...' lines in the log (needs a build that prints pad frames)")

    inputs = []
    header = []
    with open(args.script, encoding="utf-8") as f:
        for line in f:
            s = line.rstrip("\r\n")
            if s.startswith("@ "):
                continue
            words = s.split("#", 1)[0].split()
            if not words:
                if not inputs:
                    header.append(s)
                continue
            inputs.append((int(words[0], 0), s))

    last_input = inputs[-1][0] if inputs else 0
    syncs = [s for s in syncs if s[0] <= last_input]
    # Merge by frame; a sync goes before the input lines of the same frame
    # (the landmark came before that pad read).
    merged = [(fr, 0, "@ %d %s" % (fr, key)) for fr, key in syncs]
    merged += [(fr, 1, text) for fr, text in inputs]
    merged.sort(key=lambda t: (t[0], t[1]))
    out = header + [t[2] for t in merged]
    text = "\n".join(out) + "\n"
    if args.out:
        with open(args.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    print("%d sync points, %d input lines" % (len(syncs), len(inputs)), file=sys.stderr)


if __name__ == "__main__":
    main()
