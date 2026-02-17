#!/usr/bin/env python3
import argparse
import csv
import json
import os
from pathlib import Path
import sys


SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent.parent
K_BIN = ROOT_DIR / "k"
PROGRAM_DIR = ROOT_DIR / "bench" / "programs" / "phase7"
RESULTS_DIR = ROOT_DIR / "bench" / "results"

sys.path.insert(0, str(SCRIPT_DIR / "phase7"))
from definitions import phase7_benchmarks  # noqa: E402
from utils import (  # noqa: E402
    collect_timing_samples,
    parse_phase7_output,
    summarize_times,
    within_tolerance,
)


def run_mode(defn, mode_name, args):
    program_path = PROGRAM_DIR / defn.source
    if not program_path.exists():
        return {
            "name": defn.name,
            "mode": mode_name,
            "pass": False,
            "error": f"missing program: {program_path}",
        }

    env = dict(os.environ)
    env["SPARK_PIPELINE_FUSION"] = "1" if mode_name == "fused" else "0"
    command = [str(K_BIN), "run", "--interpret", str(program_path)]

    statuses, first_output, samples = collect_timing_samples(
        command,
        env,
        args.runs,
        args.warmup_runs,
        args.sample_repeat,
    )
    timing = summarize_times(samples, args.drift_limit)
    status_ok = all(code == 0 for code in statuses)

    parsed = {}
    parse_error = ""
    if status_ok:
        try:
            parsed = parse_phase7_output(first_output)
        except ValueError as exc:
            parse_error = str(exc)
            status_ok = False

    checksum_ok = status_ok and within_tolerance(parsed.get("checksum", 0.0), defn.expected_checksum)
    plan_ok = status_ok and int(parsed.get("plan_id", -1)) == defn.expected_plan
    fused_mode_ok = status_ok and (
        (mode_name == "fused" and parsed.get("fused_count", 0.0) >= 1.0)
        or (mode_name == "non_fused" and parsed.get("fallback_count", 0.0) >= 1.0)
    )

    reproducible_ok = (not args.require_reproducible) or bool(timing.get("reproducible", 0.0))

    passed = bool(status_ok and checksum_ok and plan_ok and fused_mode_ok and reproducible_ok)
    return {
        "name": defn.name,
        "source": str(program_path),
        "mode": mode_name,
        "pass": passed,
        "status_ok": status_ok,
        "checksum_ok": checksum_ok,
        "plan_ok": plan_ok,
        "mode_ok": fused_mode_ok,
        "reproducible_ok": reproducible_ok,
        "statuses": statuses,
        "parse_error": parse_error,
        "output": first_output,
        "parsed": parsed,
        "timing": timing,
        "expected_checksum": defn.expected_checksum,
        "expected_plan": defn.expected_plan,
        "min_speedup": defn.min_speedup,
    }


def compare_modes(defn, fused_record, non_fused_record):
    fused_time = fused_record.get("timing", {}).get("median_time_sec", 0.0)
    non_fused_time = non_fused_record.get("timing", {}).get("median_time_sec", 0.0)
    speedup = 0.0
    if fused_time > 0:
        speedup = non_fused_time / fused_time

    fused_alloc = fused_record.get("parsed", {}).get("last_allocations", 0.0)
    non_fused_alloc = non_fused_record.get("parsed", {}).get("last_allocations", 0.0)
    alloc_ratio = 0.0
    if fused_alloc > 0:
        alloc_ratio = non_fused_alloc / fused_alloc
    elif non_fused_alloc > 0:
        alloc_ratio = float("inf")

    speedup_ok = speedup >= defn.min_speedup
    passed = bool(fused_record["pass"] and non_fused_record["pass"] and speedup_ok)
    return {
        "name": defn.name,
        "pass": passed,
        "speedup": speedup,
        "speedup_ok": speedup_ok,
        "min_speedup": defn.min_speedup,
        "alloc_ratio": alloc_ratio,
    }


def write_results(config, records, comparisons):
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    json_path = RESULTS_DIR / "phase7_benchmarks.json"
    csv_path = RESULTS_DIR / "phase7_benchmarks.csv"

    payload = {
        "config": config,
        "benchmarks": records,
        "comparisons": comparisons,
    }
    json_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    with csv_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "name",
                "mode",
                "pass",
                "status_ok",
                "checksum_ok",
                "plan_ok",
                "mode_ok",
                "reproducible_ok",
                "median_time_sec",
                "checksum",
                "plan_id",
                "fused_count",
                "fallback_count",
                "last_allocations",
                "total_allocations",
            ]
        )
        for record in records:
            parsed = record.get("parsed", {})
            timing = record.get("timing", {})
            writer.writerow(
                [
                    record["name"],
                    record["mode"],
                    "PASS" if record["pass"] else "FAIL",
                    int(record.get("status_ok", False)),
                    int(record.get("checksum_ok", False)),
                    int(record.get("plan_ok", False)),
                    int(record.get("mode_ok", False)),
                    int(record.get("reproducible_ok", False)),
                    timing.get("median_time_sec", 0.0),
                    parsed.get("checksum", 0.0),
                    parsed.get("plan_id", -1),
                    parsed.get("fused_count", 0.0),
                    parsed.get("fallback_count", 0.0),
                    parsed.get("last_allocations", 0.0),
                    parsed.get("total_allocations", 0.0),
                ]
            )

        writer.writerow([])
        writer.writerow(["name", "pass", "speedup", "speedup_ok", "min_speedup", "alloc_ratio"])
        for cmp_row in comparisons:
            writer.writerow(
                [
                    cmp_row["name"],
                    "PASS" if cmp_row["pass"] else "FAIL",
                    cmp_row["speedup"],
                    int(cmp_row["speedup_ok"]),
                    cmp_row["min_speedup"],
                    cmp_row["alloc_ratio"],
                ]
            )

    return json_path, csv_path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--warmup-runs", type=int, default=1)
    parser.add_argument("--sample-repeat", type=int, default=3)
    parser.add_argument("--drift-limit", type=float, default=3.0)
    parser.add_argument("--require-reproducible", action="store_true")
    args = parser.parse_args()
    if args.sample_repeat <= 0:
        raise SystemExit("--sample-repeat must be >= 1")

    config = {
        "runs": args.runs,
        "warmup_runs": args.warmup_runs,
        "sample_repeat": args.sample_repeat,
        "drift_limit": args.drift_limit,
        "require_reproducible": args.require_reproducible,
    }

    defs = phase7_benchmarks(ROOT_DIR)
    records = []
    comparisons = []
    for defn in defs:
        fused = run_mode(defn, "fused", args)
        non_fused = run_mode(defn, "non_fused", args)
        records.append(fused)
        records.append(non_fused)
        comparisons.append(compare_modes(defn, fused, non_fused))

    json_path, csv_path = write_results(config, records, comparisons)
    passed = sum(1 for rec in records if rec["pass"])
    cmp_passed = sum(1 for row in comparisons if row["pass"])
    print(f"phase7 records: {passed}/{len(records)} passed")
    print(f"phase7 comparisons: {cmp_passed}/{len(comparisons)} passed")
    print(f"results json: {json_path}")
    print(f"results csv: {csv_path}")


if __name__ == "__main__":
    main()
