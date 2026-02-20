#!/usr/bin/env python3
"""Cross-language matrix-matmul benchmark with fair baseline controls.

Modes:
- full: process wall-time (init + compute).
- kernel-only: estimate compute by subtracting init runtime.
"""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
import subprocess
import sys
import tempfile
from dataclasses import asdict
from typing import List

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from crosslang.benchmark_core import BenchmarkResult, measure_samples, result_from_full, result_from_kernel_delta, run_checked
from crosslang.generate_matmul_pair_sources import compile_c_blas, write_sources

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
RESULT_DIR = REPO_ROOT / "bench" / "results"


def should_include_python_naive(n: int, max_n: int) -> bool:
    return n <= max_n


def main() -> int:
    parser = argparse.ArgumentParser(description="Cross-language runtime benchmark for A*B (matrix matmul)")
    parser.add_argument("--n", type=int, default=1000, help="Matrix dimension N (NxN)")
    parser.add_argument("--repeats", type=int, default=1, help="Repeat count for matrix multiply")
    parser.add_argument("--runs", type=int, default=3, help="Measured runs")
    parser.add_argument("--warmup", type=int, default=1, help="Warmup runs")
    parser.add_argument("--spark-threads", type=int, default=1, help="SPARK_MATMUL_OWN_THREADS")
    parser.add_argument("--mode", choices=["full", "kernel-only"], default="full")
    parser.add_argument(
        "--python-naive-max-n",
        type=int,
        default=350,
        help="Skip Python naive loops when n is above this threshold",
    )
    parser.add_argument(
        "--spark-init-mode",
        choices=["affine", "loops"],
        default="affine",
        help="Kozmika matrix initialization strategy in generated benchmark source",
    )
    parser.add_argument(
        "--spark-compute-mode",
        choices=["materialize", "fused_sum"],
        default="fused_sum",
        help="Kozmika matmul accumulation strategy (fused_sum avoids matrix materialization)",
    )
    args = parser.parse_args()

    if args.mode == "kernel-only" and args.runs < 3:
        print("kernel-only mode is sensitive to jitter; bumping --runs to 3")
        args.runs = 3

    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="kozmika-crosslang-") as tmp:
        workdir = pathlib.Path(tmp)
        src = write_sources(
            workdir,
            args.n,
            args.repeats,
            spark_init_mode=args.spark_init_mode,
            spark_compute_mode=args.spark_compute_mode,
        )

        c_naive_bin = workdir / "matmul_c_naive_bin"
        c_blocked_bin = workdir / "matmul_c_blocked_bin"
        c_blas_bin = workdir / "matmul_c_blas_bin"
        c_init_bin = workdir / "matmul_c_init_bin"

        run_checked(["clang", "-O3", "-DNDEBUG", str(src["c_naive"]), "-o", str(c_naive_bin)], cwd=REPO_ROOT)
        run_checked(["clang", "-O3", "-DNDEBUG", str(src["c_blocked"]), "-o", str(c_blocked_bin)], cwd=REPO_ROOT)
        run_checked(["clang", "-O3", "-DNDEBUG", str(src["c_init"]), "-o", str(c_init_bin)], cwd=REPO_ROOT)
        compile_c_blas(src["c_blas"], c_blas_bin)
        run_checked(["javac", str(src["java_compute"]), str(src["java_init"])], cwd=REPO_ROOT)

        common_thread_env = {
            "OPENBLAS_NUM_THREADS": "1",
            "OMP_NUM_THREADS": "1",
            "MKL_NUM_THREADS": "1",
            "VECLIB_MAXIMUM_THREADS": "1",
            "BLIS_NUM_THREADS": "1",
            "LC_ALL": "C",
        }

        cases = [
            {
                "name": "Kozmika interpret (own)",
                "compute_cmd": ["./k", "run", "--interpret", str(src["spark_compute"])],
                "init_cmd": ["./k", "run", "--interpret", str(src["spark_init"])],
                "env": {**common_thread_env, "SPARK_MATMUL_BACKEND": "own", "SPARK_MATMUL_OWN_THREADS": str(args.spark_threads)},
            },
            {
                "name": "Kozmika interpret (blas)",
                "compute_cmd": ["./k", "run", "--interpret", str(src["spark_compute"])],
                "init_cmd": ["./k", "run", "--interpret", str(src["spark_init"])],
                "env": {**common_thread_env, "SPARK_MATMUL_BACKEND": "blas", "SPARK_MATMUL_OWN_THREADS": str(args.spark_threads)},
            },
            {
                "name": "Kozmika interpret (auto)",
                "compute_cmd": ["./k", "run", "--interpret", str(src["spark_compute"])],
                "init_cmd": ["./k", "run", "--interpret", str(src["spark_init"])],
                "env": {**common_thread_env, "SPARK_MATMUL_BACKEND": "auto", "SPARK_MATMUL_OWN_THREADS": str(args.spark_threads)},
            },
            {
                "name": "C (clang -O3 naive)",
                "compute_cmd": [str(c_naive_bin)],
                "init_cmd": [str(c_init_bin)],
                "env": common_thread_env,
            },
            {
                "name": "C (clang -O3 blocked)",
                "compute_cmd": [str(c_blocked_bin)],
                "init_cmd": [str(c_init_bin)],
                "env": common_thread_env,
            },
            {
                "name": "C (BLAS dgemm)",
                "compute_cmd": [str(c_blas_bin)],
                "init_cmd": [str(c_init_bin)],
                "env": common_thread_env,
            },
            {
                "name": "Python+NumPy",
                "compute_cmd": ["python3", str(src["np_compute"])],
                "init_cmd": ["python3", str(src["np_init"])],
                "env": common_thread_env,
            },
            {
                "name": "Java (naive loops)",
                "compute_cmd": ["java", "-Duser.language=en", "-Duser.region=US", "-cp", str(workdir), "MatrixMatmulCompute"],
                "init_cmd": ["java", "-Duser.language=en", "-Duser.region=US", "-cp", str(workdir), "MatrixMatmulInit"],
                "env": common_thread_env,
            },
        ]

        if should_include_python_naive(args.n, args.python_naive_max_n):
            cases.append(
                {
                    "name": "Python (naive loops)",
                    "compute_cmd": ["python3", str(src["py_compute"])],
                    "init_cmd": ["python3", str(src["py_init"])],
                    "env": common_thread_env,
                }
            )
        else:
            print(f"skip: Python (naive loops) for n={args.n} (threshold={args.python_naive_max_n})")

        results: List[BenchmarkResult] = []
        for case in cases:
            if args.mode == "full":
                batch = measure_samples(case["compute_cmd"], REPO_ROOT, case["env"], args.warmup, args.runs)
                result = result_from_full(case["name"], batch, mode="full")
            else:
                compute = measure_samples(case["compute_cmd"], REPO_ROOT, case["env"], args.warmup, args.runs)
                init = measure_samples(case["init_cmd"], REPO_ROOT, case["env"], args.warmup, args.runs)
                result = result_from_kernel_delta(case["name"], compute, init)
            results.append(result)

        c_case = next((r for r in results if r.name == "C (clang -O3 naive)"), None)
        if c_case is None:
            raise RuntimeError("C naive baseline result missing")
        for row in results:
            row.vs_c = c_case.median_sec / row.median_sec if row.median_sec > 0 else None

        def fmt_ratio(value: float | None) -> str:
            return f"{value:.3f}x" if value is not None else "n/a"

        for row in results:
            summary = (
                f"{row.name}: median={row.median_sec:.6f}s min={row.min_sec:.6f}s "
                f"max={row.max_sec:.6f}s C_naive/this={fmt_ratio(row.vs_c)} checksum={row.checksum_raw}"
            )
            if row.mode == "kernel-only":
                summary += f" (compute_med={row.compute_median_sec:.6f}s init_med={row.init_median_sec:.6f}s)"
            print(summary)

        spark_blas = next((r for r in results if r.name == "Kozmika interpret (blas)"), None)
        reached_77x = bool(spark_blas and spark_blas.vs_c is not None and spark_blas.vs_c >= 77.0)
        if spark_blas and spark_blas.vs_c is not None:
            print(
                f"target_77x (Kozmika BLAS vs C naive): "
                f"{'REACHED' if reached_77x else 'NOT_REACHED'} ({spark_blas.vs_c:.3f}x)"
            )

        core_names = {
            "C (clang -O3 naive)",
            "C (clang -O3 blocked)",
            "C (BLAS dgemm)",
            "Kozmika interpret (auto)",
        }
        print("\nCore comparison (naive / blocked / BLAS / Kozmika auto):")
        print("| Name | Median (s) | C_naive/this |")
        print("|---|---:|---:|")
        for row in results:
            if row.name in core_names:
                print(f"| {row.name} | {row.median_sec:.6f} | {fmt_ratio(row.vs_c)} |")

        stem_prefix = "crosslang_matmul_runtime" if args.mode == "full" else "crosslang_matmul_kernel_only"
        stem = f"{stem_prefix}_{args.n}x{args.n}_r{args.repeats}"
        json_path = RESULT_DIR / f"{stem}.json"
        csv_path = RESULT_DIR / f"{stem}.csv"

        payload = {
            "config": {
                "n": args.n,
                "repeats": args.repeats,
                "runs": args.runs,
                "warmup": args.warmup,
                "spark_threads": args.spark_threads,
                "mode": args.mode,
                "runtime_only": args.mode == "full",
                "kernel_only_via_subtract_init": args.mode == "kernel-only",
                "python_naive_max_n": args.python_naive_max_n,
                "spark_init_mode": args.spark_init_mode,
                "spark_compute_mode": args.spark_compute_mode,
            },
            "results": [asdict(r) for r in results],
            "checksum_reference": c_case.checksum,
        }
        json_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")

        with csv_path.open("w", newline="", encoding="utf-8") as f:
            writer = csv.writer(f)
            writer.writerow(
                [
                    "name",
                    "mode",
                    "median_sec",
                    "min_sec",
                    "max_sec",
                    "runs",
                    "checksum_raw",
                    "checksum",
                    "vs_c_naive",
                    "compute_median_sec",
                    "init_median_sec",
                ]
            )
            for row in results:
                writer.writerow(
                    [
                        row.name,
                        row.mode,
                        f"{row.median_sec:.9f}",
                        f"{row.min_sec:.9f}",
                        f"{row.max_sec:.9f}",
                        row.runs,
                        row.checksum_raw,
                        f"{row.checksum:.17g}",
                        f"{row.vs_c:.9f}" if row.vs_c is not None else "",
                        f"{row.compute_median_sec:.9f}" if row.compute_median_sec is not None else "",
                        f"{row.init_median_sec:.9f}" if row.init_median_sec is not None else "",
                    ]
                )

        print(f"results json: {json_path}")
        print(f"results csv: {csv_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as exc:
        sys.stderr.write(exc.stderr or "")
        raise
