#!/usr/bin/env python3
import argparse
import json
import statistics
import subprocess
import time
from pathlib import Path


def run_once(command):
    start = time.perf_counter()
    proc = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False)
    elapsed = time.perf_counter() - start
    return proc.returncode, proc.stdout, elapsed


def summarize(samples):
    ordered = sorted(samples)
    trimmed = ordered[1:-1] if len(ordered) > 4 else ordered
    if not trimmed:
        trimmed = ordered
    median = statistics.median(trimmed) if trimmed else 0.0
    mean = statistics.fmean(trimmed) if trimmed else 0.0
    stdev = statistics.pstdev(trimmed) if len(trimmed) > 1 else 0.0
    drift = 0.0
    if median > 0.0 and trimmed:
        drift = max(abs((value - median) / median) * 100.0 for value in trimmed)
    return {
        "sample_count": len(samples),
        "median_sec": median,
        "mean_sec": mean,
        "stdev_sec": stdev,
        "max_drift_percent": drift,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--warmup-runs", type=int, default=1)
    parser.add_argument("--json-out", type=Path, default=None)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    if not args.command:
        raise SystemExit("missing command to execute")
    command = args.command
    if command[0] == "--":
        command = command[1:]

    for _ in range(max(0, args.warmup_runs)):
        run_once(command)

    samples = []
    statuses = []
    first_output = ""
    for i in range(max(1, args.runs)):
        code, output, elapsed = run_once(command)
        statuses.append(code)
        samples.append(elapsed)
        if i == 0:
            first_output = output

    summary = summarize(samples)
    payload = {
        "command": command,
        "statuses": statuses,
        "status_ok": all(code == 0 for code in statuses),
        "timing": summary,
        "first_output": first_output,
    }

    if args.json_out is not None:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    print(json.dumps(payload, indent=2))


if __name__ == "__main__":
    main()
