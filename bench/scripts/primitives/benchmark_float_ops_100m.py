#!/usr/bin/env python3
"""Benchmark float primitive operators (+,-,*,/,%,^) for f8..f512 at 100M iterations."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import statistics
import subprocess
import tempfile
import time
from dataclasses import asdict, dataclass
from typing import Dict, List

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
RESULT_DIR = REPO_ROOT / "bench" / "results" / "primitives"
PRIMITIVES = ["f8", "f16", "bf16", "f32", "f64", "f128", "f256", "f512"]
OPS = [
    ("add", "+"),
    ("sub", "-"),
    ("mul", "*"),
    ("div", "/"),
    ("mod", "%"),
    ("pow", "^"),
]


@dataclass
class OpBenchResult:
    primitive: str
    op_name: str
    operator: str
    loops: int
    runs: int
    warmup: int
    median_sec: float
    min_sec: float
    max_sec: float
    checksum_raw: str


def run_checked(cmd: List[str], env: Dict[str, str] | None = None) -> subprocess.CompletedProcess:
    merged = os.environ.copy()
    if env:
        merged.update(env)
    return subprocess.run(
        cmd,
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True,
        check=True,
        env=merged,
    )


def parse_last_line(stdout: str) -> str:
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    if not lines:
        raise RuntimeError("empty output")
    return lines[-1]


def make_program(path: pathlib.Path, primitive: str, operator: str, loops: int) -> None:
    source = "\n".join(
        [
            "seed = bench_tick()",
            f"a = {primitive}(((seed % 1000) / 100.0) + 1.25)",
            f"b = {primitive}(((seed % 900) / 100.0) + 2.5)",
            f"c = {primitive}(0)",
            "i = 0",
            f"while i < {loops}:",
            f"  c = c {operator} b",
            "  i = i + 1",
            "print(c)",
            "",
        ]
    )
    path.write_text(source, encoding="utf-8")


def benchmark_one(
    primitive: str,
    op_name: str,
    operator: str,
    loops: int,
    warmup: int,
    runs: int,
    perf_layer: str,
) -> OpBenchResult:
    with tempfile.TemporaryDirectory(prefix=f"kozmika-{primitive}-{op_name}-") as tmp:
        program = pathlib.Path(tmp) / f"{primitive}_{op_name}.k"
        binary = pathlib.Path(tmp) / f"{primitive}_{op_name}.bin"
        make_program(program, primitive, operator, loops)

        native_env = {
            "SPARK_CFLAGS": "-std=c11 -O3 -DNDEBUG -march=native -mtune=native "
                            "-fomit-frame-pointer -fstrict-aliasing -funroll-loops "
                            "-fvectorize -fslp-vectorize -ftree-vectorize",
            "SPARK_LTO": "full",
        }
        if perf_layer == "tier-max":
            # Layered performance mode: expensive build, faster repeat kernels.
            native_env["SPARK_OPT_PROFILE"] = "layered-max"
            native_env["SPARK_CFLAGS"] += " -DSPARK_REPEAT_AGGREGATE=1 -DSPARK_REPEAT_STABILITY_GUARD=1"
            native_env["SPARK_HP_REPEAT_AGGREGATE"] = "1"
        run_checked(["./k", "build", str(program), "-o", str(binary)], env=native_env)

        for _ in range(warmup):
            run_checked([str(binary)], env=native_env)

        samples: List[float] = []
        checksum = ""
        for _ in range(runs):
            t0 = time.perf_counter()
            proc = run_checked([str(binary)], env=native_env)
            t1 = time.perf_counter()
            samples.append(t1 - t0)
            checksum = parse_last_line(proc.stdout)

    return OpBenchResult(
        primitive=primitive,
        op_name=op_name,
        operator=operator,
        loops=loops,
        runs=runs,
        warmup=warmup,
        median_sec=statistics.median(samples),
        min_sec=min(samples),
        max_sec=max(samples),
        checksum_raw=checksum,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Benchmark float primitive ops (+,-,*,/,%,^)")
    parser.add_argument("--loops", type=int, default=100_000_000)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--perf-layer", choices=["strict", "tier-max"], default="strict")
    args = parser.parse_args()

    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    results: List[OpBenchResult] = []
    skipped: List[dict] = []
    for primitive in PRIMITIVES:
        for op_name, operator in OPS:
            try:
                result = benchmark_one(
                    primitive=primitive,
                    op_name=op_name,
                    operator=operator,
                    loops=args.loops,
                    warmup=args.warmup,
                    runs=args.runs,
                    perf_layer=args.perf_layer,
                )
            except subprocess.CalledProcessError as exc:
                skipped.append(
                    {
                        "primitive": primitive,
                        "op_name": op_name,
                        "operator": operator,
                        "reason": "build_or_run_failed",
                        "returncode": exc.returncode,
                    }
                )
                print(f"{primitive:<5} {operator:<1} skipped (build/run failed)")
                continue

            results.append(result)
            print(
                f"{primitive:<5} {operator:<1} median={result.median_sec:.6f}s "
                f"min={result.min_sec:.6f}s max={result.max_sec:.6f}s checksum={result.checksum_raw}"
            )

    payload = {
        "loops": args.loops,
        "runs": args.runs,
        "warmup": args.warmup,
        "perf_layer": args.perf_layer,
        "results": [asdict(r) for r in results],
        "skipped": skipped,
    }
    out_json = RESULT_DIR / "float_ops_100m.json"
    out_json.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(f"result_json: {out_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
