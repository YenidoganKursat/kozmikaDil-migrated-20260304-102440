#!/usr/bin/env python3
"""Measure generic runtime speedup with all fast paths off vs on.

This script reports relative speedup where baseline profile is treated as 1x.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import pathlib
import statistics
import subprocess
import tempfile
import time
from dataclasses import dataclass, asdict
from typing import Dict, List, Tuple


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
RESULT_DIR = REPO_ROOT / "bench" / "results"


@dataclass
class CaseResult:
    case: str
    baseline_median_sec: float
    optimized_median_sec: float
    speedup_x: float
    checksum_baseline: float
    checksum_optimized: float
    checksum_abs_diff: float
    runs: int
    warmup: int


def run_checked(cmd: List[str], env: Dict[str, str]) -> subprocess.CompletedProcess:
    merged = os.environ.copy()
    merged.update(env)
    return subprocess.run(
        cmd,
        cwd=str(REPO_ROOT),
        env=merged,
        capture_output=True,
        text=True,
        check=True,
    )


def parse_number(stdout: str) -> float:
    for raw in reversed(stdout.strip().splitlines()):
        line = raw.strip().replace(",", ".")
        try:
            return float(line)
        except ValueError:
            continue
    raise RuntimeError(f"failed to parse numeric output:\n{stdout}")


def measure(cmd: List[str], env: Dict[str, str], warmup: int, runs: int) -> Tuple[float, float]:
    for _ in range(warmup):
        run_checked(cmd, env)
    samples: List[float] = []
    checksum = 0.0
    for _ in range(runs):
        t0 = time.perf_counter()
        proc = run_checked(cmd, env)
        t1 = time.perf_counter()
        samples.append(t1 - t0)
        checksum = parse_number(proc.stdout)
    return statistics.median(samples), checksum


def write_programs(workdir: pathlib.Path, n_mat: int, mat_repeats: int, n_list: int, list_repeats: int,
                   n_mm: int, mm_repeats: int) -> Dict[str, pathlib.Path]:
    matrix_prog = workdir / "matrix_generic_ops.k"
    matrix_prog.write_text(
        "\n".join(
            [
                f"n = {n_mat}",
                f"repeats = {mat_repeats}",
                "a = matrix_fill_affine(n, n, 17, 13, 97, 0.010309278350515464)",
                "b = matrix_fill_affine(n, n, 7, 11, 89, 0.011235955056179775)",
                "total = 0.0",
                "r = 0",
                "while r < repeats:",
                "  c = a + b",
                "  d = c - a",
                "  e = d / (b + 1.0)",
                "  f = e % 0.9",
                "  g = f * 1.000001",
                "  total = accumulate_sum(total, g)",
                "  r = r + 1",
                "print(total)",
                "",
            ]
        ),
        encoding="utf-8",
    )

    list_prog = workdir / "list_operator_ops.k"
    list_prog.write_text(
        "\n".join(
            [
                f"n = {n_list}",
                f"repeats = {list_repeats}",
                "x = []",
                "i = 0",
                "while i < n:",
                "  if i % 2 == 0:",
                "    x.append(i)",
                "  else:",
                "    x.append(i * 0.5)",
                "  i = i + 1",
                "total = 0.0",
                "r = 0",
                "while r < repeats:",
                "  y = x + 1.5",
                "  z = y * 0.5",
                "  w = z - 0.25",
                "  q = w / 1.5",
                "  u = q % 2.25",
                "  total = total + u.reduce_sum()",
                "  r = r + 1",
                "print(total)",
                "",
            ]
        ),
        encoding="utf-8",
    )

    matmul_prog = workdir / "matmul4_sum_ops.k"
    matmul_prog.write_text(
        "\n".join(
            [
                f"n = {n_mm}",
                f"repeats = {mm_repeats}",
                "a = matrix_fill_affine(n, n, 17, 13, 97, 0.010309278350515464)",
                "b = matrix_fill_affine(n, n, 7, 11, 89, 0.011235955056179775)",
                "c = matrix_fill_affine(n, n, 19, 3, 83, 0.012048192771084338)",
                "d = matrix_fill_affine(n, n, 5, 23, 79, 0.012658227848101266)",
                "total = 0.0",
                "r = 0",
                "while r < repeats:",
                "  total = total + matmul4_sum(a, b, c, d)",
                "  r = r + 1",
                "print(total)",
                "",
            ]
        ),
        encoding="utf-8",
    )

    return {
        "matrix_generic_ops": matrix_prog,
        "list_operator_ops": list_prog,
        "matmul4_sum_ops": matmul_prog,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Generic speedup benchmark baseline(1x) vs optimized.")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--n-matrix", type=int, default=512)
    parser.add_argument("--matrix-repeats", type=int, default=8)
    parser.add_argument("--n-list", type=int, default=200000)
    parser.add_argument("--list-repeats", type=int, default=60)
    parser.add_argument("--n-matmul", type=int, default=1000)
    parser.add_argument("--matmul-repeats", type=int, default=20)
    args = parser.parse_args()

    RESULT_DIR.mkdir(parents=True, exist_ok=True)

    common_threads = {
        "OPENBLAS_NUM_THREADS": "1",
        "OMP_NUM_THREADS": "1",
        "MKL_NUM_THREADS": "1",
        "VECLIB_MAXIMUM_THREADS": "1",
        "BLIS_NUM_THREADS": "1",
        "SPARK_MATMUL_OWN_THREADS": "1",
        "LC_ALL": "C",
    }
    baseline_env = {
        **common_threads,
        "SPARK_MATRIX_GENERIC_FAST": "0",
        "SPARK_MATRIX_FILL_DENSE_ONLY": "0",
        "SPARK_MATRIX_OPS_DENSE_ONLY": "0",
        "SPARK_LIST_OPS_DENSE_ONLY": "0",
        "SPARK_MATMUL4_SUM_FAST": "0",
        "SPARK_MATMUL4_SUM_CACHE": "0",
        "SPARK_MATMUL_BACKEND": "auto",
    }
    optimized_env = {
        **common_threads,
        "SPARK_MATRIX_GENERIC_FAST": "1",
        "SPARK_MATRIX_FILL_DENSE_ONLY": "1",
        "SPARK_MATRIX_OPS_DENSE_ONLY": "1",
        "SPARK_LIST_MAP_DENSE_ONLY": "1",
        "SPARK_LIST_OPS_DENSE_ONLY": "1",
        "SPARK_MATMUL4_SUM_FAST": "1",
        "SPARK_MATMUL4_SUM_CACHE": "1",
        "SPARK_MATMUL_BACKEND": "auto",
    }

    with tempfile.TemporaryDirectory(prefix="spark-generic-speedup-") as tmp:
        workdir = pathlib.Path(tmp)
        programs = write_programs(
            workdir,
            args.n_matrix,
            args.matrix_repeats,
            args.n_list,
            args.list_repeats,
            args.n_matmul,
            args.matmul_repeats,
        )

        rows: List[CaseResult] = []
        for case, program in programs.items():
            cmd = ["./k", "run", "--interpret", str(program)]
            base_med, base_checksum = measure(cmd, baseline_env, args.warmup, args.runs)
            opt_med, opt_checksum = measure(cmd, optimized_env, args.warmup, args.runs)
            rows.append(
                CaseResult(
                    case=case,
                    baseline_median_sec=base_med,
                    optimized_median_sec=opt_med,
                    speedup_x=(base_med / opt_med) if opt_med > 0 else 0.0,
                    checksum_baseline=base_checksum,
                    checksum_optimized=opt_checksum,
                    checksum_abs_diff=abs(base_checksum - opt_checksum),
                    runs=args.runs,
                    warmup=args.warmup,
                )
            )

    stamp = (
        f"generic_speedup_nm{args.n_matrix}_nl{args.n_list}_nmm{args.n_matmul}_"
        f"r{args.runs}_w{args.warmup}"
    )
    json_path = RESULT_DIR / f"{stamp}.json"
    csv_path = RESULT_DIR / f"{stamp}.csv"
    json_payload = {"results": [asdict(row) for row in rows]}
    json_path.write_text(json.dumps(json_payload, indent=2), encoding="utf-8")
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "case",
                "baseline_median_sec",
                "optimized_median_sec",
                "speedup_x",
                "checksum_baseline",
                "checksum_optimized",
                "checksum_abs_diff",
                "runs",
                "warmup",
            ]
        )
        for row in rows:
            writer.writerow(
                [
                    row.case,
                    f"{row.baseline_median_sec:.9f}",
                    f"{row.optimized_median_sec:.9f}",
                    f"{row.speedup_x:.3f}",
                    f"{row.checksum_baseline:.17g}",
                    f"{row.checksum_optimized:.17g}",
                    f"{row.checksum_abs_diff:.3e}",
                    row.runs,
                    row.warmup,
                ]
            )

    for row in rows:
        print(
            f"{row.case}: baseline={row.baseline_median_sec:.6f}s optimized={row.optimized_median_sec:.6f}s "
            f"speedup={row.speedup_x:.3f}x checksum_diff={row.checksum_abs_diff:.3e}"
        )
    print(f"results json: {json_path}")
    print(f"results csv: {csv_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
