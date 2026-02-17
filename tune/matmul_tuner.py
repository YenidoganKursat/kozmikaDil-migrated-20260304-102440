#!/usr/bin/env python3
import argparse
import itertools
import json
import os
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
K_BIN = ROOT / "k"
PROGRAM = ROOT / "bench" / "programs" / "phase8" / "matmul_core_f64.k"
OUT_FILE = ROOT / "bench" / "results" / "matmul_tuned_schedule.json"

sys.path.insert(0, str(ROOT / "bench" / "scripts" / "phase8"))
from utils import collect_timing_samples, parse_phase8_output, summarize_times  # noqa: E402


def evaluate_config(tile_m, tile_n, tile_k, unroll, vector_width, runs, warmup):
    env = dict(os.environ)
    env["SPARK_MATMUL_BACKEND"] = "own"
    env["SPARK_MATMUL_TILE_M"] = str(tile_m)
    env["SPARK_MATMUL_TILE_N"] = str(tile_n)
    env["SPARK_MATMUL_TILE_K"] = str(tile_k)
    env["SPARK_MATMUL_UNROLL"] = str(unroll)
    env["SPARK_MATMUL_VECTOR_WIDTH"] = str(vector_width)

    cmd = [str(K_BIN), "run", "--interpret", str(PROGRAM)]
    statuses, first_output, samples = collect_timing_samples(cmd, env, runs, warmup)
    status_ok = all(code == 0 for code in statuses)
    if not status_ok:
        return None

    try:
        parsed = parse_phase8_output(first_output)
    except ValueError:
        return None

    timing = summarize_times(samples, drift_limit_percent=5.0)
    if int(parsed.get("pass", 0.0)) != 1:
        return None

    return {
        "tile_m": tile_m,
        "tile_n": tile_n,
        "tile_k": tile_k,
        "unroll": unroll,
        "vector_width": vector_width,
        "median_time_sec": timing["median_time_sec"],
        "max_drift_percent": timing["max_drift_percent"],
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--warmup-runs", type=int, default=1)
    parser.add_argument("--tile-options", default="64,96")
    parser.add_argument("--unroll-options", default="4,8")
    parser.add_argument("--vector-options", default="4,8")
    args = parser.parse_args()

    tiles = [int(v) for v in args.tile_options.split(",") if v.strip()]
    unrolls = [int(v) for v in args.unroll_options.split(",") if v.strip()]
    vectors = [int(v) for v in args.vector_options.split(",") if v.strip()]

    candidates = itertools.product(tiles, tiles, tiles, unrolls, vectors)
    best = None
    trials = []

    for tile_m, tile_n, tile_k, unroll, vector_width in candidates:
        result = evaluate_config(
            tile_m=tile_m,
            tile_n=tile_n,
            tile_k=tile_k,
            unroll=unroll,
            vector_width=vector_width,
            runs=args.runs,
            warmup=args.warmup_runs,
        )
        if result is None:
            continue
        trials.append(result)
        if best is None or result["median_time_sec"] < best["median_time_sec"]:
            best = result

    if best is None:
        raise SystemExit("tuner: no valid schedule candidate produced passing output")

    OUT_FILE.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "backend": "own",
        "tile_m": best["tile_m"],
        "tile_n": best["tile_n"],
        "tile_k": best["tile_k"],
        "unroll": best["unroll"],
        "vector_width": best["vector_width"],
        "source": "phase8_tuner",
        "median_time_sec": best["median_time_sec"],
        "trials_considered": len(trials),
    }
    OUT_FILE.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    print(f"tuner best: {best}")
    print(f"tuner output: {OUT_FILE}")


if __name__ == "__main__":
    main()
