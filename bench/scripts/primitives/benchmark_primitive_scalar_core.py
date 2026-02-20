#!/usr/bin/env python3
"""Single-task benchmark core for one numeric primitive scalar loop."""

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
from typing import List

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
RESULT_DIR = REPO_ROOT / "bench" / "results" / "primitives"


@dataclass
class PrimitiveBenchResult:
    primitive: str
    mode: str
    loops: int
    runs: int
    warmup: int
    median_sec: float
    min_sec: float
    max_sec: float
    checksum_raw: str
    baseline_1x_sec: float
    speedup_vs_1x: float


def run_checked(cmd: List[str], env: dict | None = None) -> subprocess.CompletedProcess:
    merged_env = os.environ.copy()
    if env:
        merged_env.update(env)
    return subprocess.run(
        cmd,
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True,
        check=True,
        env=merged_env,
    )


def parse_last_line(stdout: str) -> str:
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    if not lines:
        raise RuntimeError("benchmark output is empty")
    return lines[-1]


def make_program(path: pathlib.Path, primitive: str, loops: int) -> None:
    if primitive.startswith("i"):
        body = [
            "seed = bench_tick()",
            f"x = {primitive}((seed % 31) + 11)",
            f"y = {primitive}((seed % 23) + 7)",
            f"modv = {primitive}((seed % 89) + 97)",
            f"acc = {primitive}(0)",
            "i = 0",
            f"while i < {loops}:",
            "  acc = acc + x",
            "  acc = acc * y",
            "  acc = acc % modv",
            "  acc = acc - x",
            "  i = i + 1",
            "print(acc)",
            "",
        ]
    else:
        body = [
            "seed = bench_tick()",
            f"x = {primitive}((seed % 19) + 1.25)",
            f"y = {primitive}((seed % 11) + 2.5)",
            f"acc = {primitive}(0)",
            "i = 0",
            f"while i < {loops}:",
            "  acc = acc + x",
            "  acc = acc * y",
            "  acc = acc / y",
            "  acc = acc - x",
            "  i = i + 1",
            "print(acc)",
            "",
        ]
    source = "\n".join(body)
    path.write_text(source, encoding="utf-8")


def run_benchmark(
    primitive: str,
    loops: int,
    warmup: int,
    runs: int,
    mode: str = "auto",
) -> PrimitiveBenchResult:
    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f"kozmika-prim-{primitive}-") as tmp:
        program = pathlib.Path(tmp) / f"{primitive}_scalar_loop.k"
        binary = pathlib.Path(tmp) / f"{primitive}_scalar_loop.bin"
        make_program(program, primitive, loops)

        resolved_mode = "interpret"
        native_env = {
            # Build-time is intentionally ignored in this benchmark; maximize runtime speed.
            "SPARK_CFLAGS": "-std=c11 -O3 -DNDEBUG -march=native -mtune=native "
                            "-fomit-frame-pointer -fstrict-aliasing -funroll-loops "
                            "-fvectorize -fslp-vectorize -ftree-vectorize",
            "SPARK_LTO": "full",
        }
        try:
            # Build once, run many: runtime-only measurement. Build-time intentionally excluded.
            if mode in ("auto", "native"):
                run_checked(["./k", "build", str(program), "-o", str(binary)], env=native_env)
                resolved_mode = "native"
            else:
                resolved_mode = "interpret"
        except subprocess.CalledProcessError:
            if mode == "native":
                raise
            resolved_mode = "interpret"

        for _ in range(warmup):
            if resolved_mode == "native":
                run_checked([str(binary)])
            else:
                run_checked(["./k", "run", "--interpret", str(program)])

        samples: List[float] = []
        checksum = ""
        for _ in range(runs):
            t0 = time.perf_counter()
            if resolved_mode == "native":
                proc = run_checked([str(binary)])
            else:
                proc = run_checked(["./k", "run", "--interpret", str(program)])
            t1 = time.perf_counter()
            samples.append(t1 - t0)
            checksum = parse_last_line(proc.stdout)

    median_sec = statistics.median(samples)
    return PrimitiveBenchResult(
        primitive=primitive,
        mode=resolved_mode,
        loops=loops,
        runs=runs,
        warmup=warmup,
        median_sec=median_sec,
        min_sec=min(samples),
        max_sec=max(samples),
        checksum_raw=checksum,
        baseline_1x_sec=median_sec,
        speedup_vs_1x=1.0,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Benchmark one numeric primitive scalar loop")
    parser.add_argument("--primitive", required=True)
    parser.add_argument("--loops", type=int, default=200000)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--runs", type=int, default=3)
    args = parser.parse_args()

    result = run_benchmark(args.primitive, args.loops, args.warmup, args.runs)
    out_json = RESULT_DIR / f"primitive_{args.primitive}_baseline.json"
    out_json.write_text(json.dumps(asdict(result), indent=2), encoding="utf-8")

    print(
        f"{result.primitive}: mode={result.mode} median={result.median_sec:.6f}s "
        f"baseline_1x={result.baseline_1x_sec:.6f}s speedup={result.speedup_vs_1x:.3f}x "
        f"checksum={result.checksum_raw}"
    )
    print(f"result_json: {out_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
