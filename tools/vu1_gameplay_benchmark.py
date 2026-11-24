#!/usr/bin/env python3
"""Compare wall time at matching gameplay landmarks in a private input replay."""

import argparse
import csv
import re
import sys
from pathlib import Path

LANDMARK = re.compile(r"\[landmark\] (?:(\d+\.\d)s )?f(\d+) ([^\n\[]+)")
LOADED = re.compile(r"\[pad-replay\] loaded (\d+) lines and (\d+) sync points ")
FINISH = re.compile(r"\[pad-replay\] script finished at frame (\d+)")


def read_route(path):
    inputs = 0
    events = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("@ "):
            frame, event = line[2:].split(maxsplit=1)
            events.append((int(frame), event))
        elif line.split()[0].isdecimal():
            inputs += 1
        else:
            raise ValueError(f"{path}:{number}: malformed route record")
    if not inputs or not events:
        raise ValueError(f"{path}: expected input and landmark records")
    return inputs, events


def read_trial(path, inputs, events, finish):
    log = path.read_text(encoding="utf-8", errors="replace")
    matches = LANDMARK.findall(log)
    if log.count("[landmark]") != len(events) or [(int(f), event.strip()) for _, f, event in matches] != events:
        raise ValueError(f"{path}: landmark order, event or frame mismatch")
    if LOADED.findall(log) != [(str(inputs), str(len(events)))]:
        raise ValueError(f"{path}: route load counts differ")
    if FINISH.findall(log) != [str(finish)] or "never came; going on" in log:
        raise ValueError(f"{path}: incomplete or skipped replay")
    times = [float(time) if time else None for time, _, _ in matches]
    timed = [t for t in times if t is not None]
    if not timed or timed != sorted(timed):
        raise ValueError(f"{path}: missing or non-monotone landmark times")
    return times


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("route", type=Path)
    parser.add_argument("control", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--finish-frame", type=int, required=True)
    args = parser.parse_args()
    try:
        count, events = read_route(args.route)
        control = read_trial(args.control, count, events, args.finish_frame)
        candidate = read_trial(args.candidate, count, events, args.finish_frame)
        if [t is None for t in control] != [t is None for t in candidate]:
            raise ValueError("The trials have timestamps for different landmarks")
        writer = csv.writer(sys.stdout, lineterminator="\n")
        writer.writerow(("frame", "event", "control_seconds", "candidate_seconds"))
        for (frame, event), a, b in zip(events, control, candidate):
            if a is not None and b is not None:
                writer.writerow((frame, event, f"{a:.1f}", f"{b:.1f}"))
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
