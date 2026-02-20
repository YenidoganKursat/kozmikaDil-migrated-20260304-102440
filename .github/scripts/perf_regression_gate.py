#!/usr/bin/env python3
"""CI performance regression gate for primitive scalar runtime benchmarks.

This gate is intentionally conservative:
- it verifies that selected primitives stay on native path
- it enforces upper-bound median runtime thresholds
"""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
from dataclasses import asdict, dataclass
from typing import Any

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
BENCH_SCRIPT = REPO_ROOT / "bench" / "scripts" / "primitives" / "benchmark_primitive_scalar_core.py"
BENCH_RESULT_DIR = REPO_ROOT / "bench" / "results" / "primitives"


@dataclass
class GateResult:
    check_id: str
    primitive: str
    loops: int
    warmup: int
    runs: int
    expected_mode: str
    actual_mode: str
    max_median_sec: float
    actual_median_sec: float
    passed_mode: bool
    passed_time: bool
    passed: bool


def run_checked(cmd: list[str], cwd: pathlib.Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, cwd=str(cwd), capture_output=True, text=True, check=True)


def load_json(path: pathlib.Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def run_one_check(entry: dict[str, Any]) -> GateResult:
    primitive = str(entry["primitive"])
    loops = int(entry.get("loops", 100000))
    warmup = int(entry.get("warmup", 0))
    runs = int(entry.get("runs", 1))
    expected_mode = str(entry.get("expected_mode", "native"))
    max_median_sec = float(entry["max_median_sec"])
    check_id = str(entry.get("id", primitive))

    cmd = [
        "python3",
        str(BENCH_SCRIPT),
        "--primitive",
        primitive,
        "--loops",
        str(loops),
        "--warmup",
        str(warmup),
        "--runs",
        str(runs),
    ]
    proc = run_checked(cmd, REPO_ROOT)
    sys.stdout.write(proc.stdout)
    if proc.stderr:
        sys.stderr.write(proc.stderr)

    out_json = BENCH_RESULT_DIR / f"primitive_{primitive}_baseline.json"
    payload = load_json(out_json)
    actual_mode = str(payload.get("mode", "unknown"))
    actual_median_sec = float(payload["median_sec"])

    passed_mode = actual_mode == expected_mode
    passed_time = actual_median_sec <= max_median_sec
    passed = passed_mode and passed_time

    return GateResult(
        check_id=check_id,
        primitive=primitive,
        loops=loops,
        warmup=warmup,
        runs=runs,
        expected_mode=expected_mode,
        actual_mode=actual_mode,
        max_median_sec=max_median_sec,
        actual_median_sec=actual_median_sec,
        passed_mode=passed_mode,
        passed_time=passed_time,
        passed=passed,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Performance regression gate for CI")
    parser.add_argument("--baseline", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()

    baseline = load_json(args.baseline)
    checks = baseline.get("checks", [])
    if not checks:
        raise SystemExit("perf baseline has no checks")

    results: list[GateResult] = []
    for entry in checks:
        result = run_one_check(entry)
        results.append(result)
        print(
            f"[perf] {result.check_id}: mode={result.actual_mode} "
            f"median={result.actual_median_sec:.6f}s "
            f"(limit={result.max_median_sec:.6f}s) pass={result.passed}"
        )

    failed = [result for result in results if not result.passed]
    payload = {
        "baseline": str(args.baseline),
        "results": [asdict(result) for result in results],
        "failed_count": len(failed),
        "passed": len(failed) == 0,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(f"perf_gate_report: {args.output}")

    if failed:
        print("perf_regression_failed:")
        for result in failed:
            print(
                f"  - {result.check_id}: mode={result.actual_mode}/{result.expected_mode}, "
                f"median={result.actual_median_sec:.6f}s limit={result.max_median_sec:.6f}s"
            )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
