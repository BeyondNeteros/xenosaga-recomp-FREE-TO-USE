#!/usr/bin/env python3
"""Seal an existing VU1 holdout split and validate it with vu1_replay."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
import os
from pathlib import Path
import re
from statistics import median
import subprocess
import sys

ROLES = ("training", "held-out")
MODES = {"interpreter": ("0", "0"), "blocks": ("1", "0"), "programs": ("1", "1")}
COMPARE = set("calls differences cycle_differences cycle_delta_min cycle_delta_max programs completed "
              "block_calls blocks block_pairs gif_packets gif_bytes".split())
TIMING = {"calls", "timed", "ns"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def entry(kind, pc, code):
    result = f"{kind}:{pc}:{code}"
    require(re.fullmatch(r"[01]:[0-9a-f]{4}:[0-9a-f]{16}", result), f"Invalid entry {result}")
    return result


def seal(args):
    # These modules are needed only when preparing the suite, not on Windows.
    from vucap import records
    from vu1prog import code_hash

    suite = args.suite
    files = []
    for role in ROLES:
        for path in sorted((suite / role).glob("vu1_capture*.bin")):
            counts = Counter()
            for r in records(path):
                pc = r["pc"] if r["kind"] else r["start"]
                counts[entry(r["kind"], f"{pc:04x}", f"{code_hash(r['code']):016x}")] += 1
            files.append(dict(role=role, name=path.name, sha256=digest(path), entries=dict(counts)))
    manifest = dict(registrations=dict(programs=len(args.target), blocks=args.blocks),
                    targets=args.target, files=files)
    check_suite(suite, manifest)
    with (suite / "suite.json").open("x", encoding="utf-8") as stream:
        json.dump(manifest, stream, indent=2)
        stream.write("\n")


def check_suite(suite, manifest):
    seen = set()
    counts = {role: Counter() for role in ROLES}
    targets = manifest["targets"]
    reg = manifest["registrations"]
    require(reg["programs"] == len(set(targets)) == len(targets) and reg["blocks"] > 0,
            "Missing or inconsistent registration expectations")
    for item in manifest["files"]:
        role, name = item["role"], item["name"]
        require(role in ROLES and re.fullmatch(r"vu1_capture(?:_[0-9]+)?\.bin", name)
                and (role, name) not in seen, f"Unexpected or duplicate capture {role}/{name}")
        seen.add((role, name))
        path = suite / role / name
        require(digest(path) == item["sha256"], f"Capture checksum mismatch: {path}")
        entries = item["entries"]
        require(entries and all(type(n) is int and n > 0 for n in entries.values()),
                f"Missing capture counts: {path}")
        for key in entries:
            entry(*key.split(":"))
        counts[role].update(entries)
    for role in ROLES:
        actual = {p.name for p in (suite / role).glob("vu1_capture*.bin")}
        require(counts[role] and actual == {name for r, name in seen if r == role},
                f"Missing or unsealed captures in {role}")
    for target in targets:
        entry(*target.split(":"))
        for role in ROLES:
            require(counts[role][target] > 0, f"Missing {target} in {role}")


def parse_rows(output, prefix, fields, expected):
    rows = {}
    for line in output.splitlines():
        if not line.startswith(prefix + " "):
            continue
        parts = [part.split("=", 1) for part in line.split()[1:]]
        fields_read = dict(parts)
        key = entry(*(fields_read.pop(k) for k in ("kind", "pc", "code")))
        require(len(parts) == len(fields) + 3 and fields_read.keys() == fields and key not in rows,
                f"Incomplete or duplicate {prefix} evidence for {key}")
        rows[key] = {k: int(v) for k, v in fields_read.items()}
    require({key: row["calls"] for key, row in rows.items()} == expected,
            f"{prefix} entry counts do not match the suite manifest")
    return rows


def replay(exe, capture, mode, repeat, compare, log, registrations):
    env = {k: v for k, v in os.environ.items()
           if not k.upper().startswith(("VU_REPLAY_", "PS2X_VU1_", "PS2X_VU_CAPTURE"))}
    blocks, programs = MODES[mode]
    env.update(VU_REPLAY_NOGS="1", PS2X_VU1_NATIVE=blocks, PS2X_VU1_PROGRAMS=programs)
    if compare:
        env["VU_REPLAY_NATIVE_CMP"] = "1"
    try:
        with log.open("wb") as stream:
            run = subprocess.run([str(exe), str(capture), str(repeat)], env=env, check=False,
                                 cwd=log.parent, stdout=stream, stderr=subprocess.STDOUT, timeout=600)
    except subprocess.TimeoutExpired as exc:
        raise ValueError(f"Replay timed out; see {log}") from exc
    output = log.read_text(encoding="utf-8", errors="replace")
    require(run.returncode == 0, f"Replay exited {run.returncode}; see {log}")
    check_registrations(output, registrations, log)
    return output


def check_registrations(output, registrations, log):
    expected = f"registered programs={registrations['programs']} blocks={registrations['blocks']}"
    require(re.findall(r"^registered .*?$", output, re.M) == [expected], f"Missing registrations in {log}")


def comparison_rows(output, expected, mode, targets, log):
    count = sum(expected.values())
    require(re.findall(r"^native compare: .*?$", output, re.M)
            == [f"native compare: 0 of {count} calls differ"], f"Missing successful comparison in {log}")
    rows = parse_rows(output, "compare", COMPARE, expected)
    for key, row in rows.items():
        n = row["calls"]
        require(row["differences"] == 0 and 0 <= row["cycle_differences"] <= n
                and 0 <= row["block_calls"] <= n and row["blocks"] >= row["block_calls"]
                and row["block_pairs"] >= row["blocks"] and row["gif_packets"] >= 0
                and row["gif_bytes"] >= row["gif_packets"], f"Invalid comparison row {key} in {log}")
        programs = n if mode == "programs" and key in targets else 0
        require(row["programs"] == row["completed"] == programs,
                f"Missing, partial or unexpected program dispatch for {key} in {log}")
        if mode == "interpreter":
            require(row["blocks"] == 0, f"Unexpected block dispatch for {key} in {log}")
        if mode == "blocks":
            require(row["block_calls"] == n, f"Incomplete native-block dispatch for {key} in {log}")
        d = row["cycle_differences"]
        lo, hi = row["cycle_delta_min"], row["cycle_delta_max"]
        require((d == 0 and lo == hi == 0) or (d > 0 and lo <= hi and (lo != 0 or hi != 0)),
                f"Invalid cycle-delta evidence for {key} in {log}")
    return rows


def compare_gif(baseline, rows, log):
    for key, row in rows.items():
        for field in ("gif_packets", "gif_bytes"):
            require(row[field] == baseline[key][field], f"Mismatched {field} for {key} in {log}")


def verify_blocks(args):
    """Re-evaluate the raw interpreter and block logs from a completed run."""
    manifest = json.loads((args.suite / "suite.json").read_text(encoding="utf-8"))
    check_suite(args.suite, manifest)
    require(json.loads((args.results / "suite.json").read_text(encoding="utf-8")) == manifest,
            "Recorded suite differs from the sealed suite")
    run = json.loads((args.results / "run.json").read_text(encoding="utf-8"))
    require(run["platform"] == args.platform and run["sha256"] == digest(args.exe),
            "Recorded platform or replay executable does not match")
    logs = {}
    gif = Counter()
    totals = Counter()
    targets = set(manifest["targets"])
    registration = manifest["registrations"]
    if run.get("gate") == "blocks":
        registration = dict(registration, programs=0)
    for item in manifest["files"]:
        role, name, expected = item["role"], item["name"], item["entries"]
        for mode in ("interpreter", "blocks"):
            log = args.results / f"compare-{mode}-{role}-{name}.txt"
            output = log.read_text(encoding="utf-8")
            check_registrations(output, registration, log)
            rows = comparison_rows(output, expected, mode, targets, log)
            require(all(row["cycle_differences"] == 0 for row in rows.values()),
                    f"VU cycle counts differ in {log}")
            logs[log.name] = digest(log)
            if mode == "interpreter":
                baseline = rows
            else:
                compare_gif(baseline, rows, log)
                for key, row in rows.items():
                    gif[role, key] += row["gif_packets"]
                    totals.update({field: row[field] for field in ("calls", "blocks", "block_pairs",
                                                                  "gif_packets", "gif_bytes")})
    require(all(gif[role, key] > 0 for role in ROLES for key in targets), "Missing target GIF evidence")
    with args.receipt.open("x", encoding="utf-8") as stream:
        json.dump(dict(platform=run["platform"], replay_sha256=run["sha256"],
                       suite_sha256=digest(args.suite / "suite.json"), registrations=registration,
                       totals=dict(totals), logs=logs), stream, indent=2)
        stream.write("\n")


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def run_suite(args):
    suite, exe, results = (p.resolve() for p in (args.suite, args.exe, args.results))
    require(args.trials >= 3 and args.trials % 2 == 1 and 2 <= args.repeat <= 100000,
            "Use an odd trial count of at least 3 and a repeat count between 2 and 100000")
    results.mkdir(parents=True, exist_ok=False)
    try:
        manifest = json.loads((suite / "suite.json").read_text(encoding="utf-8"))
        write_json(results / "suite.json", manifest)
        check_suite(suite, manifest)
        write_json(results / "run.json", dict(exe=str(exe), sha256=digest(exe), platform=sys.platform,
                                             trials=args.trials, repeat=args.repeat, gate=args.gate))
        validate(exe, suite, results, manifest, args)
    except (OSError, ValueError, KeyError) as exc:
        (results / "FAIL.txt").write_text(str(exc) + "\n", encoding="utf-8")
        raise


def validate(exe, suite, results, manifest, args):
    require(args.gate != "programs" or manifest["targets"], "The program gate requires registered targets")
    modes = tuple(MODES) if args.gate == "programs" else ("interpreter", "blocks")
    registration = manifest["registrations"]
    if args.gate == "blocks":
        registration = dict(registration, programs=0)
    targets = set(manifest["targets"])
    coverage = defaultdict(Counter)
    cycle_ranges = {}
    errors = []
    print(f"Comparing {len(manifest['files'])} capture slices in {len(modes)} dispatch modes.", flush=True)
    for item in manifest["files"]:
        role, name, expected = item["role"], item["name"], item["entries"]
        baseline = None
        for mode in modes:
            log = results / f"compare-{mode}-{role}-{name}.txt"
            try:
                output = replay(exe, suite / role / name, mode, 1, True, log, registration)
                rows = comparison_rows(output, expected, mode, targets, log)
                if mode == "interpreter":
                    baseline = rows
                elif baseline is not None:
                    compare_gif(baseline, rows, log)
                for key, row in rows.items():
                    d = row["cycle_differences"]
                    lo, hi = row["cycle_delta_min"], row["cycle_delta_max"]
                    group = (role, mode, key)
                    coverage[group].update({k: v for k, v in row.items() if not k.startswith("cycle_delta_")})
                    if d:
                        old = cycle_ranges.get(group, (lo, hi))
                        cycle_ranges[group] = min(old[0], lo), max(old[1], hi)
            except (OSError, ValueError, KeyError) as exc:
                errors.append(f"{log.name}: {exc}")

    all_rows = []
    for (role, mode, key), row in sorted(coverage.items()):
        lo, hi = cycle_ranges.get((role, mode, key), (0, 0))
        all_rows.append(dict(role=role, mode=mode, entry=key, **row, cycle_delta_min=lo, cycle_delta_max=hi))
    write_json(results / "comparison.json", all_rows)
    for role in ROLES:
        for key in targets:
            for mode in modes:
                actual = coverage[role, mode, key]
                if not actual["gif_packets"] or not actual["gif_bytes"]:
                    errors.append(f"Missing GIF evidence: {role} {mode} {key}")
                if mode == "blocks" and not actual["block_calls"]:
                    errors.append(f"Missing native-block dispatch: {role} {key}")
    require(not errors, "\n".join(errors))

    cycles = sum(row["cycle_differences"] for row in all_rows)
    if cycles:
        (results / "CYCLE_MISMATCH.txt").write_text(f"{cycles} comparisons differed in VU cycle count.\n",
                                                  encoding="utf-8")
    print("State, GIF and dispatch checks passed. Running separate timing trials.", flush=True)
    if cycles:
        print("Cycle counts differ. Timing results are diagnostic and the suite will exit with failure.", flush=True)
    benchmark(exe, suite, results, manifest, args, modes, registration)
    require(cycles == 0, f"{cycles} VU cycle counts differed. See comparison.json; gameplay staging is blocked.")
    (results / "PASS.txt").write_text("Replay correctness and benchmark evidence verified.\n", encoding="utf-8")


def benchmark(exe, suite, results, manifest, args, modes, registration):
    samples = defaultdict(list)
    for trial in range(args.trials):
        print(f"Timing trial {trial + 1}/{args.trials}", flush=True)
        totals = defaultdict(Counter)
        for mode in modes[trial % len(modes):] + modes[:trial % len(modes)]:
            for item in manifest["files"]:
                role, name, expected = item["role"], item["name"], item["entries"]
                log = results / f"timing-{trial + 1}-{mode}-{role}-{name}.txt"
                output = replay(exe, suite / role / name, mode, args.repeat, False, log, registration)
                for key, row in parse_rows(output, "timing", TIMING, expected).items():
                    require(row["timed"] == row["calls"] * (args.repeat - 1) and row["ns"] > 0,
                            f"Missing timed calls for {key} in {log}")
                    totals[role, key, mode].update(row)
        for group, times in totals.items():
            samples[group].append(times["ns"] / times["timed"] / 1000)
    output = []
    for role, key in sorted({(role, key) for role, key, _ in samples}):
        times = {mode: samples[role, key, mode] for mode in modes}
        medians = {mode: median(values) for mode, values in times.items()}
        output.append(dict(role=role, entry=key, us_per_call=times, median_us=medians,
                           median_ratio_to_interpreter={mode: medians["interpreter"] / medians[mode]
                                                        for mode in modes}))
    write_json(results / "benchmark.json", output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sealing = sub.add_parser("seal")
    sealing.add_argument("suite", type=Path)
    sealing.add_argument("--target", action="append", default=[], help="Experimental region entry KIND:PC:CODE")
    sealing.add_argument("--blocks", type=int, required=True)
    running = sub.add_parser("run")
    for name in ("exe", "suite", "results"):
        running.add_argument(name, type=Path)
    running.add_argument("--repeat", type=int, default=200)
    running.add_argument("--trials", type=int, default=5)
    running.add_argument("--gate", choices=("programs", "blocks"), default="blocks")
    verifying = sub.add_parser("verify-blocks")
    for name in ("exe", "suite", "results", "receipt"):
        verifying.add_argument(name, type=Path)
    verifying.add_argument("--platform", choices=("linux", "win32"), required=True)
    args = parser.parse_args()
    try:
        {"seal": seal, "run": run_suite, "verify-blocks": verify_blocks}[args.command](args)
    except (OSError, ValueError, KeyError) as exc:
        parser.exit(1, f"{exc}\n")


if __name__ == "__main__":
    main()
