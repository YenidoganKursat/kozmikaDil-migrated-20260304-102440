#!/usr/bin/env python3
"""Run baseline/current cycle for all primitive scalar benchmarks."""

from __future__ import annotations

import argparse
import json
import pathlib
from dataclasses import asdict
from typing import Dict, List

from benchmark_primitive_scalar_core import PrimitiveBenchResult, run_benchmark

PRIMITIVES: List[str] = [
    "i8",
    "i16",
    "i32",
    "i64",
    "i128",
    "i256",
    "i512",
    "f8",
    "f16",
    "bf16",
    "f32",
    "f64",
    "f128",
    "f256",
    "f512",
]

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
RESULT_DIR = REPO_ROOT / "bench" / "results" / "primitives"


def read_json(path: pathlib.Path) -> Dict[str, object]:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: pathlib.Path, payload: Dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")


def baseline_path(primitive: str) -> pathlib.Path:
    return RESULT_DIR / f"primitive_{primitive}_baseline.json"


def latest_path(primitive: str) -> pathlib.Path:
    return RESULT_DIR / f"primitive_{primitive}_latest.json"


def to_latest(result: PrimitiveBenchResult, baseline_sec: float) -> Dict[str, object]:
    speedup = baseline_sec / result.median_sec if result.median_sec > 0 else 1.0
    payload = asdict(result)
    payload["baseline_1x_sec"] = baseline_sec
    payload["speedup_vs_1x"] = speedup
    return payload


def main() -> int:
    parser = argparse.ArgumentParser(description="Run baseline/current cycle for all primitives")
    parser.add_argument("--loops", type=int, default=200000)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--mode", choices=["auto", "interpret", "native"], default="auto")
    parser.add_argument("--reset-baseline", action="store_true")
    args = parser.parse_args()

    summary: Dict[str, Dict[str, object]] = {}
    for primitive in PRIMITIVES:
        result = run_benchmark(
            primitive=primitive,
            loops=args.loops,
            warmup=args.warmup,
            runs=args.runs,
            mode=args.mode,
        )
        baseline_file = baseline_path(primitive)
        if args.reset_baseline or not baseline_file.exists():
            write_json(baseline_file, asdict(result))
            baseline_sec = result.median_sec
        else:
            baseline_sec = float(read_json(baseline_file)["median_sec"])

        latest = to_latest(result, baseline_sec)
        write_json(latest_path(primitive), latest)
        summary[primitive] = {
            "mode": latest["mode"],
            "median_sec": latest["median_sec"],
            "baseline_1x_sec": latest["baseline_1x_sec"],
            "speedup_vs_1x": latest["speedup_vs_1x"],
            "checksum_raw": latest["checksum_raw"],
        }
        print(
            f"{primitive}: mode={latest['mode']} median={latest['median_sec']:.6f}s "
            f"baseline_1x={latest['baseline_1x_sec']:.6f}s "
            f"speedup={latest['speedup_vs_1x']:.3f}x"
        )

    write_json(RESULT_DIR / "primitive_cycle_summary.json", {"primitives": summary})
    print(f"summary_json: {RESULT_DIR / 'primitive_cycle_summary.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
