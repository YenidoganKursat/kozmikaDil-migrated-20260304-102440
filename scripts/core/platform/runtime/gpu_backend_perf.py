#!/usr/bin/env python3
"""GPU backend control-plane performance measurements.

This measures backend detection/probe overhead on the current host.
It is intentionally backend-agnostic and safe for generic CI runners.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path
from statistics import mean, median


SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from gpu_backend_catalog import (
    GPU_BACKENDS,
    detect_gpu_backend,
    known_backend_names,
    normalize_backend_list,
)


def parse_csv(value: str) -> list[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    idx = int(round((len(ordered) - 1) * p))
    idx = max(0, min(idx, len(ordered) - 1))
    return float(ordered[idx])


def timed_run(command: list[str], timeout_sec: float = 4.0) -> tuple[int, float]:
    start = time.perf_counter()
    proc = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
        timeout=timeout_sec,
    )
    elapsed_ms = (time.perf_counter() - start) * 1000.0
    return proc.returncode, elapsed_ms


def main() -> int:
    parser = argparse.ArgumentParser(description="GPU backend probe performance matrix")
    parser.add_argument("--backends", default="all", help="comma-separated backend names or 'all'")
    parser.add_argument("--include-planning", action="store_true")
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--json-out", default="bench/results/platform_support_gpu_perf.json")
    args = parser.parse_args()

    known = known_backend_names()
    if args.backends.strip().lower() == "all":
        selected = known
    else:
        selected = normalize_backend_list(parse_csv(args.backends))
    unknown = [item for item in selected if item not in known]
    if unknown:
        raise SystemExit(f"unknown backend(s): {', '.join(unknown)}")

    records = []
    for spec in GPU_BACKENDS:
        if spec.name not in selected:
            continue
        if spec.tier == "planning" and not args.include_planning:
            continue

        detect_samples = []
        detect_info = {}
        for _ in range(max(1, args.warmup)):
            detect_gpu_backend(spec)
        for _ in range(max(1, args.runs)):
            t0 = time.perf_counter()
            detect_info = detect_gpu_backend(spec)
            detect_samples.append((time.perf_counter() - t0) * 1000.0)

        command_stats = []
        if detect_info.get("available", False):
            for cmd in spec.probe_commands:
                if not cmd:
                    continue
                if shutil.which(cmd[0]) is None:
                    continue
                for _ in range(max(1, args.warmup)):
                    try:
                        timed_run(list(cmd))
                    except Exception:  # noqa: BLE001
                        pass
                samples = []
                success = 0
                for _ in range(max(1, args.runs)):
                    try:
                        rc, elapsed_ms = timed_run(list(cmd))
                    except Exception:  # noqa: BLE001
                        rc, elapsed_ms = 1, 0.0
                    if rc == 0:
                        success += 1
                    samples.append(elapsed_ms)
                command_stats.append(
                    {
                        "command": " ".join(cmd),
                        "success_runs": success,
                        "runs": max(1, args.runs),
                        "median_ms": round(median(samples), 4) if samples else 0.0,
                        "mean_ms": round(mean(samples), 4) if samples else 0.0,
                        "p95_ms": round(percentile(samples, 0.95), 4) if samples else 0.0,
                    }
                )

        record = {
            "name": spec.name,
            "api": spec.api,
            "tier": spec.tier,
            "available": bool(detect_info.get("available", False)),
            "reason": detect_info.get("reason", ""),
            "detect_runs": max(1, args.runs),
            "detect_median_ms": round(median(detect_samples), 4) if detect_samples else 0.0,
            "detect_mean_ms": round(mean(detect_samples), 4) if detect_samples else 0.0,
            "detect_p95_ms": round(percentile(detect_samples, 0.95), 4) if detect_samples else 0.0,
            "probe_commands": command_stats,
        }
        records.append(record)
        print(
            f"[gpu-perf] {record['name']}: available={record['available']} "
            f"detect_median={record['detect_median_ms']:.4f}ms "
            f"reason={record['reason']}"
        )

    payload = {
        "runs": max(1, args.runs),
        "warmup": max(1, args.warmup),
        "records": records,
    }

    root = SCRIPT_DIR.parents[3]
    out = root / args.json_out
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(f"gpu_perf json: {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
