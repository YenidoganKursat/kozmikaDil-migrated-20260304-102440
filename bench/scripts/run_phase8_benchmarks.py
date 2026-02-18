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
PROGRAM_DIR = ROOT_DIR / "bench" / "programs" / "phase8"
RESULTS_DIR = ROOT_DIR / "bench" / "results"

sys.path.insert(0, str(SCRIPT_DIR / "phase8"))
from definitions import phase8_benchmarks  # noqa: E402
from utils import (  # noqa: E402
    collect_timing_samples_repeated,
    compile_c_baseline,
    parse_c_baseline_output,
    parse_phase8_output,
    summarize_times,
    within_tolerance,
)


def run_k_mode(defn, mode, args):
    source_path = PROGRAM_DIR / defn.source
    if not source_path.exists():
        return {
            "name": defn.name,
            "mode": mode,
            "pass": False,
            "error": f"missing source: {source_path}",
        }

    env = dict(os.environ)
    env["SPARK_MATMUL_BACKEND"] = mode
    # Keep benchmark runs reproducible by pinning math backends to single-thread.
    env.setdefault("OPENBLAS_NUM_THREADS", "1")
    env.setdefault("OMP_NUM_THREADS", "1")
    env.setdefault("MKL_NUM_THREADS", "1")
    env.setdefault("VECLIB_MAXIMUM_THREADS", "1")
    env.setdefault("BLIS_NUM_THREADS", "1")
    # For phase benchmarking we disable auto-learning by default to avoid
    # within-run backend flips from skewing mode-to-mode comparisons.
    env.setdefault("SPARK_MATMUL_AUTO_LEARN", "0")
    command = [str(K_BIN), "run", "--interpret", str(source_path)]
    statuses, first_output, samples = collect_timing_samples_repeated(
        command,
        env=env,
        runs=args.runs,
        warmup_runs=args.warmup_runs,
        sample_repeat=args.sample_repeat,
    )
    timing = summarize_times(samples, args.drift_limit)
    status_ok = all(code == 0 for code in statuses)

    parsed = {}
    parse_error = ""
    if status_ok:
        try:
            parsed = parse_phase8_output(first_output)
        except ValueError as exc:
            parse_error = str(exc)
            status_ok = False

    checksum_ok = status_ok and within_tolerance(parsed.get("total", 0.0), parsed.get("expected", 0.0))
    pass_flag_ok = status_ok and int(parsed.get("pass", 0.0)) == 1
    reproducible_ok = (not args.require_reproducible) or bool(timing.get("reproducible", 0.0))

    return {
        "name": defn.name,
        "source": str(source_path),
        "mode": mode,
        "pass": bool(status_ok and checksum_ok and pass_flag_ok and reproducible_ok),
        "status_ok": status_ok,
        "checksum_ok": checksum_ok,
        "pass_flag_ok": pass_flag_ok,
        "reproducible_ok": reproducible_ok,
        "statuses": statuses,
        "parse_error": parse_error,
        "output": first_output,
        "parsed": parsed,
        "timing": timing,
    }


def run_c_baseline(defn, args):
    source_path = PROGRAM_DIR / defn.baseline_c
    if not source_path.exists():
        return {
            "name": defn.name,
            "mode": "c_baseline",
            "pass": False,
            "error": f"missing baseline source: {source_path}",
        }

    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    out_bin = RESULTS_DIR / f"{defn.name}_baseline.out"
    ok, err = compile_c_baseline(source_path, out_bin, args.c_compiler, args.c_flags)
    if not ok:
        return {
            "name": defn.name,
            "mode": "c_baseline",
            "pass": False,
            "error": f"baseline compile failed: {err}",
        }

    env = dict(os.environ)
    env.setdefault("OPENBLAS_NUM_THREADS", "1")
    env.setdefault("OMP_NUM_THREADS", "1")
    env.setdefault("MKL_NUM_THREADS", "1")
    env.setdefault("VECLIB_MAXIMUM_THREADS", "1")
    env.setdefault("BLIS_NUM_THREADS", "1")

    command = [str(out_bin)]
    statuses, first_output, samples = collect_timing_samples_repeated(
        command,
        env=env,
        runs=args.runs,
        warmup_runs=args.warmup_runs,
        sample_repeat=args.sample_repeat,
    )
    timing = summarize_times(samples, args.drift_limit)
    status_ok = all(code == 0 for code in statuses)

    parsed = {}
    parse_error = ""
    if status_ok:
        try:
            parsed = parse_c_baseline_output(first_output)
        except ValueError as exc:
            parse_error = str(exc)
            status_ok = False

    checksum_ok = status_ok and within_tolerance(parsed.get("total", 0.0), parsed.get("expected", 0.0))
    pass_flag_ok = status_ok and int(parsed.get("pass", 0.0)) == 1
    reproducible_ok = (not args.require_reproducible) or bool(timing.get("reproducible", 0.0))

    return {
        "name": defn.name,
        "source": str(source_path),
        "mode": "c_baseline",
        "pass": bool(status_ok and checksum_ok and pass_flag_ok and reproducible_ok),
        "status_ok": status_ok,
        "checksum_ok": checksum_ok,
        "pass_flag_ok": pass_flag_ok,
        "reproducible_ok": reproducible_ok,
        "statuses": statuses,
        "parse_error": parse_error,
        "output": first_output,
        "parsed": parsed,
        "timing": timing,
    }


def compare_modes(defn, own_record, auto_record, blas_record, c_record):
    own_time = own_record.get("timing", {}).get("median_time_sec", 0.0)
    auto_time = auto_record.get("timing", {}).get("median_time_sec", 0.0)
    c_time = c_record.get("timing", {}).get("median_time_sec", 0.0)
    blas_time = blas_record.get("timing", {}).get("median_time_sec", 0.0)

    own_vs_c = 0.0
    if own_time > 0.0:
        own_vs_c = c_time / own_time

    blas_available = int(blas_record.get("parsed", {}).get("backend_id", 0.0)) == 1
    own_vs_blas = 0.0
    if blas_available and blas_time > 0.0:
        own_vs_blas = own_time / blas_time

    best_time = own_time
    if blas_available and blas_time > 0.0:
        best_time = min(best_time, blas_time)

    auto_backend_id = int(auto_record.get("parsed", {}).get("backend_id", 0.0))
    selected_backend = "own"
    selected_time = own_time
    if auto_backend_id == 1 and blas_available and blas_time > 0.0:
        selected_backend = "blas"
        selected_time = blas_time
    elif own_time <= 0.0 and best_time > 0.0:
        # Fallback if own sample is unavailable for any reason.
        selected_backend = "best_fallback"
        selected_time = best_time

    auto_vs_best = 0.0
    if selected_time > 0.0:
        auto_vs_best = auto_time / selected_time

    own_vs_c_ok = own_vs_c >= defn.min_own_vs_c
    own_vs_blas_ok = (not blas_available) or (own_vs_blas <= defn.max_own_vs_blas)
    auto_vs_best_ok = auto_vs_best <= defn.max_auto_vs_best

    passed = bool(
        own_record["pass"]
        and auto_record["pass"]
        and c_record["pass"]
        and (blas_record["pass"] or not blas_available)
        and own_vs_c_ok
        and own_vs_blas_ok
        and auto_vs_best_ok
    )

    return {
        "name": defn.name,
        "pass": passed,
        "blas_available": blas_available,
        "own_vs_c": own_vs_c,
        "own_vs_c_ok": own_vs_c_ok,
        "own_vs_blas": own_vs_blas,
        "own_vs_blas_ok": own_vs_blas_ok,
        "auto_vs_best": auto_vs_best,
        "auto_vs_best_ok": auto_vs_best_ok,
        "auto_selected_backend": selected_backend,
        "min_own_vs_c": defn.min_own_vs_c,
        "max_own_vs_blas": defn.max_own_vs_blas,
        "max_auto_vs_best": defn.max_auto_vs_best,
    }


def write_results(config, records, comparisons):
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    json_path = RESULTS_DIR / "phase8_benchmarks.json"
    csv_path = RESULTS_DIR / "phase8_benchmarks.csv"

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
                "pass_flag_ok",
                "reproducible_ok",
                "median_time_sec",
                "total",
                "expected",
                "diff",
                "calls",
                "own_calls",
                "blas_calls",
                "cache_hit_a",
                "cache_hit_b",
                "epilogue_fused",
                "backend_id",
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
                    int(record.get("pass_flag_ok", False)),
                    int(record.get("reproducible_ok", False)),
                    timing.get("median_time_sec", 0.0),
                    parsed.get("total", 0.0),
                    parsed.get("expected", 0.0),
                    parsed.get("diff", 0.0),
                    parsed.get("calls", 0.0),
                    parsed.get("own_calls", 0.0),
                    parsed.get("blas_calls", 0.0),
                    parsed.get("cache_hit_a", 0.0),
                    parsed.get("cache_hit_b", 0.0),
                    parsed.get("epilogue_fused", 0.0),
                    parsed.get("backend_id", 0.0),
                ]
            )

        writer.writerow([])
        writer.writerow(
            [
                "name",
                "pass",
                "blas_available",
                "auto_selected_backend",
                "own_vs_c",
                "own_vs_c_ok",
                "own_vs_blas",
                "own_vs_blas_ok",
                "auto_vs_best",
                "auto_vs_best_ok",
            ]
        )
        for cmp_row in comparisons:
            writer.writerow(
                [
                    cmp_row["name"],
                    "PASS" if cmp_row["pass"] else "FAIL",
                    int(cmp_row["blas_available"]),
                    cmp_row.get("auto_selected_backend", ""),
                    cmp_row["own_vs_c"],
                    int(cmp_row["own_vs_c_ok"]),
                    cmp_row["own_vs_blas"],
                    int(cmp_row["own_vs_blas_ok"]),
                    cmp_row["auto_vs_best"],
                    int(cmp_row["auto_vs_best_ok"]),
                ]
            )

    return json_path, csv_path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--warmup-runs", type=int, default=1)
    parser.add_argument("--sample-repeat", type=int, default=3)
    parser.add_argument("--drift-limit", type=float, default=3.0)
    parser.add_argument("--c-compiler", default="clang")
    parser.add_argument("--c-flags", default="-O3")
    parser.add_argument("--require-reproducible", action="store_true")
    args = parser.parse_args()
    if args.sample_repeat <= 0:
        raise SystemExit("--sample-repeat must be >= 1")

    config = {
        "runs": args.runs,
        "warmup_runs": args.warmup_runs,
        "sample_repeat": args.sample_repeat,
        "drift_limit": args.drift_limit,
        "c_compiler": args.c_compiler,
        "c_flags": args.c_flags,
        "require_reproducible": args.require_reproducible,
    }

    defs = phase8_benchmarks(ROOT_DIR)
    records = []
    comparisons = []
    for defn in defs:
        own_record = run_k_mode(defn, "own", args)
        auto_record = run_k_mode(defn, "auto", args)
        blas_record = run_k_mode(defn, "blas", args)
        c_record = run_c_baseline(defn, args)

        records.extend([own_record, auto_record, blas_record, c_record])
        comparisons.append(compare_modes(defn, own_record, auto_record, blas_record, c_record))

    json_path, csv_path = write_results(config, records, comparisons)
    passed = sum(1 for rec in records if rec["pass"])
    cmp_passed = sum(1 for cmp_row in comparisons if cmp_row["pass"])
    print(f"phase8 records: {passed}/{len(records)} passed")
    print(f"phase8 comparisons: {cmp_passed}/{len(comparisons)} passed")
    print(f"results json: {json_path}")
    print(f"results csv: {csv_path}")


if __name__ == "__main__":
    main()
