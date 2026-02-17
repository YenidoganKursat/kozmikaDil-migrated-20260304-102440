#!/usr/bin/env python3
import argparse
import csv
import json
from pathlib import Path
import sys


SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent.parent
K_BIN = ROOT_DIR / "k"
PROGRAM_DIR = ROOT_DIR / "bench" / "programs" / "phase6"
RESULTS_DIR = ROOT_DIR / "bench" / "results"

sys.path.insert(0, str(SCRIPT_DIR / "phase6"))
from definitions import phase6_benchmarks  # noqa: E402
from utils import (  # noqa: E402
    collect_timing_samples,
    parse_phase6_output,
    summarize_times,
    try_collect_perf,
    within_tolerance,
)


def run_benchmark(defn, args):
    program_path = PROGRAM_DIR / defn.source
    if not program_path.exists():
        return {
            "name": defn.name,
            "source": str(program_path),
            "group": defn.group,
            "mode": defn.mode,
            "pass": False,
            "error": "missing program file",
        }

    command = [str(K_BIN), "run", "--interpret", str(program_path)]
    statuses, first_output, samples = collect_timing_samples(
        command,
        args.runs,
        args.warmup_runs,
        args.sample_repeat,
    )
    status_ok = all(code == 0 for code in statuses)

    parsed = {}
    parse_error = ""
    if status_ok:
        try:
            parsed = parse_phase6_output(first_output)
        except ValueError as exc:
            parse_error = str(exc)
            status_ok = False

    timing = summarize_times(samples, args.drift_limit)
    effective_ops_per_sample = max(1, defn.ops_per_run * args.sample_repeat)
    unit_time_sec = timing["median_time_sec"] / effective_ops_per_sample

    total_ok = status_ok and within_tolerance(parsed.get("total", 0.0), defn.expected_total)
    plan_ok = status_ok and parsed.get("plan_used") == defn.expected_plan
    materialize_ok = status_ok and parsed.get("materialize_count", 0) >= defn.expected_materialize_min
    bytes_ok = status_ok and parsed.get("cache_bytes", 0) >= defn.expected_cache_bytes_min
    reproducible_ok = (not args.require_reproducible) or timing["reproducible"]

    perf = {"available": False, "reason": "disabled"}
    if args.with_perf and status_ok:
        perf = try_collect_perf(command, defn.ops_per_run)

    return {
        "name": defn.name,
        "source": str(program_path),
        "group": defn.group,
        "mode": defn.mode,
        "ops_per_run": defn.ops_per_run,
        "effective_ops_per_sample": effective_ops_per_sample,
        "status_ok": status_ok,
        "statuses": statuses,
        "first_output": first_output,
        "parse_error": parse_error,
        "expected_total": defn.expected_total,
        "expected_plan": defn.expected_plan,
        "expected_materialize_min": defn.expected_materialize_min,
        "expected_cache_bytes_min": defn.expected_cache_bytes_min,
        "total_ok": total_ok,
        "plan_ok": plan_ok,
        "materialize_ok": materialize_ok,
        "bytes_ok": bytes_ok,
        "reproducible_ok": reproducible_ok,
        "pass": bool(status_ok and total_ok and plan_ok and materialize_ok and bytes_ok and reproducible_ok),
        "parsed": parsed,
        "timing": timing,
        "unit_time_sec": unit_time_sec,
        "unit_time_ns": unit_time_sec * 1e9,
        "perf": perf,
    }


def build_comparisons(records, args):
    by_key = {(record["group"], record["mode"]): record for record in records}
    packed = by_key.get(("packed", "steady"))
    comparisons = []
    for group in ("promote", "chunk", "gather"):
        first = by_key.get((group, "first"))
        steady = by_key.get((group, "steady"))
        if not packed or not first or not steady:
            continue
        first_vs_steady = 0.0
        if steady["unit_time_sec"] > 0:
            first_vs_steady = first["unit_time_sec"] / steady["unit_time_sec"]
        steady_vs_packed = 0.0
        if packed["unit_time_sec"] > 0:
            steady_vs_packed = steady["unit_time_sec"] / packed["unit_time_sec"]

        speedup_ok = first_vs_steady >= args.first_vs_steady_min
        overhead_ok = steady_vs_packed <= args.steady_overhead_upper
        comparisons.append(
            {
                "group": group,
                "first_vs_steady": first_vs_steady,
                "steady_vs_packed": steady_vs_packed,
                "speedup_ok": speedup_ok,
                "overhead_ok": overhead_ok,
                "pass": bool(first["pass"] and steady["pass"] and packed["pass"] and speedup_ok and overhead_ok),
            }
        )
    return comparisons


def write_outputs(records, comparisons, config):
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    json_path = RESULTS_DIR / "phase6_benchmarks.json"
    csv_path = RESULTS_DIR / "phase6_benchmarks.csv"
    cmp_csv_path = RESULTS_DIR / "phase6_benchmarks_comparisons.csv"

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
                "group",
                "mode",
                "pass",
                "status_ok",
                "total_ok",
                "plan_ok",
                "materialize_ok",
                "bytes_ok",
                "reproducible",
                "median_time_sec",
                "unit_time_ns",
                "effective_ops_per_sample",
                "expected_total",
                "actual_total",
                "expected_plan",
                "actual_plan",
                "materialize_count",
                "cache_hit_count",
                "cache_bytes",
            ]
        )
        for record in records:
            parsed = record.get("parsed", {})
            timing = record.get("timing", {})
            writer.writerow(
                [
                    record["name"],
                    record["group"],
                    record["mode"],
                    "PASS" if record["pass"] else "FAIL",
                    int(record.get("status_ok", False)),
                    int(record.get("total_ok", False)),
                    int(record.get("plan_ok", False)),
                    int(record.get("materialize_ok", False)),
                    int(record.get("bytes_ok", False)),
                    int(timing.get("reproducible", False)),
                    timing.get("median_time_sec", 0.0),
                    record.get("unit_time_ns", 0.0),
                    record.get("effective_ops_per_sample", 0),
                    record.get("expected_total", 0.0),
                    parsed.get("total", 0.0),
                    record.get("expected_plan", -1),
                    parsed.get("plan_used", -1),
                    parsed.get("materialize_count", 0),
                    parsed.get("cache_hit_count", 0),
                    parsed.get("cache_bytes", 0),
                ]
            )

    with cmp_csv_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "group",
                "pass",
                "speedup_ok",
                "overhead_ok",
                "first_vs_steady",
                "steady_vs_packed",
            ]
        )
        for cmp_row in comparisons:
            writer.writerow(
                [
                    cmp_row["group"],
                    "PASS" if cmp_row["pass"] else "FAIL",
                    int(cmp_row["speedup_ok"]),
                    int(cmp_row["overhead_ok"]),
                    cmp_row["first_vs_steady"],
                    cmp_row["steady_vs_packed"],
                ]
            )

    return json_path, csv_path, cmp_csv_path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--warmup-runs", type=int, default=1)
    parser.add_argument("--drift-limit", type=float, default=3.0)
    parser.add_argument("--sample-repeat", type=int, default=3)
    parser.add_argument("--first-vs-steady-min", type=float, default=1.05)
    parser.add_argument("--steady-overhead-upper", type=float, default=1.8)
    parser.add_argument("--require-reproducible", action="store_true")
    parser.add_argument("--with-perf", action=argparse.BooleanOptionalAction, default=True)
    args = parser.parse_args()
    if args.sample_repeat <= 0:
        raise SystemExit("--sample-repeat must be >= 1")

    config = {
        "runs": args.runs,
        "warmup_runs": args.warmup_runs,
        "drift_limit": args.drift_limit,
        "sample_repeat": args.sample_repeat,
        "first_vs_steady_min": args.first_vs_steady_min,
        "steady_overhead_upper": args.steady_overhead_upper,
        "require_reproducible": args.require_reproducible,
        "with_perf": args.with_perf,
    }

    defs = phase6_benchmarks(ROOT_DIR)
    records = [run_benchmark(defn, args) for defn in defs]
    comparisons = build_comparisons(records, args)
    json_path, csv_path, cmp_csv_path = write_outputs(records, comparisons, config)

    passed = sum(1 for record in records if record["pass"])
    cmp_passed = sum(1 for cmp_row in comparisons if cmp_row["pass"])
    print(f"phase6 benchmarks: {passed}/{len(records)} passed")
    print(f"phase6 comparisons: {cmp_passed}/{len(comparisons)} passed")
    print(f"results json: {json_path}")
    print(f"results csv: {csv_path}")
    print(f"comparison csv: {cmp_csv_path}")


if __name__ == "__main__":
    main()
