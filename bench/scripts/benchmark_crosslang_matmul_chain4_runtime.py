#!/usr/bin/env python3
"""Cross-language runtime-only benchmark for chained 4-matrix multiplication.

This benchmark computes the checksum of ((A * B) * C) * D on NxN matrices.
All measurements pin math libraries to single-thread mode for fairness.
Build/compile steps are executed before timing and are not included in results.
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
from crosslang.generate_matmul_chain4_sources import compile_c_blas, matlab_available, write_sources

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
RESULT_DIR = REPO_ROOT / "bench" / "results"


def main() -> int:
    parser = argparse.ArgumentParser(description="Cross-language runtime benchmark for ((A*B)*C)*D")
    parser.add_argument("--n", type=int, default=1000, help="Matrix dimension N (NxN)")
    parser.add_argument("--repeats", type=int, default=1, help="Repeat count for chained matmul")
    parser.add_argument("--runs", type=int, default=3, help="Measured runs")
    parser.add_argument("--warmup", type=int, default=1, help="Warmup runs")
    parser.add_argument("--spark-threads", type=int, default=1, help="SPARK_MATMUL_OWN_THREADS")
    parser.add_argument("--mode", choices=["full", "kernel-only"], default="full")
    parser.add_argument("--spark-compute-mode", choices=["materialize", "fused_sum"], default="fused_sum")
    parser.add_argument(
        "--case-set",
        choices=["core", "all"],
        default="core",
        help="core: Kozmika + C blocked/BLAS + NumPy (+MATLAB if available), all: add naive C/Java",
    )
    args = parser.parse_args()

    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="kozmika-crosslang4-") as tmp:
        workdir = pathlib.Path(tmp)
        src = write_sources(workdir, args.n, args.repeats, args.spark_compute_mode)

        c_naive_bin = workdir / "matmul4_c_naive_bin"
        c_blocked_bin = workdir / "matmul4_c_blocked_bin"
        c_blas_bin = workdir / "matmul4_c_blas_bin"
        c_init_bin = workdir / "matmul4_c_init_bin"
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
        spark_common_env = {
            **common_thread_env,
            "SPARK_MATRIX_FILL_DENSE_ONLY": "1",
            "SPARK_MATMUL4_SUM_FAST": "1",
            "SPARK_MATMUL4_SUM_CACHE": "1",
        }

        core_cases = [
            {
                "name": "Kozmika interpret (own)",
                "compute_cmd": ["./k", "run", "--interpret", str(src["spark_compute"])],
                "init_cmd": ["./k", "run", "--interpret", str(src["spark_init"])],
                "env": {**spark_common_env, "SPARK_MATMUL_BACKEND": "own", "SPARK_MATMUL_OWN_THREADS": str(args.spark_threads)},
            },
            {
                "name": "Kozmika interpret (blas)",
                "compute_cmd": ["./k", "run", "--interpret", str(src["spark_compute"])],
                "init_cmd": ["./k", "run", "--interpret", str(src["spark_init"])],
                "env": {**spark_common_env, "SPARK_MATMUL_BACKEND": "blas", "SPARK_MATMUL_OWN_THREADS": str(args.spark_threads)},
            },
            {
                "name": "Kozmika interpret (auto)",
                "compute_cmd": ["./k", "run", "--interpret", str(src["spark_compute"])],
                "init_cmd": ["./k", "run", "--interpret", str(src["spark_init"])],
                "env": {**spark_common_env, "SPARK_MATMUL_BACKEND": "auto", "SPARK_MATMUL_OWN_THREADS": str(args.spark_threads)},
            },
            {
                "name": "C (clang -O3 blocked)",
                "compute_cmd": [str(c_blocked_bin)],
                "init_cmd": [str(c_init_bin)],
                "env": common_thread_env,
            },
            {
                "name": "C (BLAS dgemm chain)",
                "compute_cmd": [str(c_blas_bin)],
                "init_cmd": [str(c_init_bin)],
                "env": common_thread_env,
            },
            {
                "name": "Python+NumPy multi_dot",
                "compute_cmd": ["python3", str(src["np_compute"])],
                "init_cmd": ["python3", str(src["np_init"])],
                "env": common_thread_env,
            },
        ]

        all_only_cases = [
            {
                "name": "C (clang -O3 naive)",
                "compute_cmd": [str(c_naive_bin)],
                "init_cmd": [str(c_init_bin)],
                "env": common_thread_env,
            },
            {
                "name": "Java (naive loops)",
                "compute_cmd": ["java", "-Duser.language=en", "-Duser.region=US", "-cp", str(workdir), "MatrixMatmul4Compute"],
                "init_cmd": ["java", "-Duser.language=en", "-Duser.region=US", "-cp", str(workdir), "MatrixMatmul4Init"],
                "env": common_thread_env,
            },
        ]

        cases = list(core_cases)
        if args.case_set == "all":
            cases.extend(all_only_cases)

        if matlab_available():
            cases.append(
                {
                    "name": "MATLAB (mtimes chain)",
                    "compute_cmd": ["matlab", "-batch", f"run('{src['matlab_compute']}')"],
                    "init_cmd": ["matlab", "-batch", f"run('{src['matlab_init']}')"],
                    "env": common_thread_env,
                }
            )

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

        baseline = next((r for r in results if r.name == "C (clang -O3 blocked)"), None)
        if baseline is None:
            raise RuntimeError("C blocked baseline missing")
        for row in results:
            row.vs_c = baseline.median_sec / row.median_sec if row.median_sec > 0 else None

        for row in results:
            suffix = ""
            if row.mode == "kernel-only":
                suffix = f" (compute={row.compute_median_sec:.6f}s init={row.init_median_sec:.6f}s)"
            ratio = "n/a" if row.vs_c is None else f"{row.vs_c:.3f}x"
            print(
                f"{row.name}: median={row.median_sec:.6f}s min={row.min_sec:.6f}s "
                f"max={row.max_sec:.6f}s C_blocked/this={ratio} checksum={row.checksum_raw}{suffix}"
            )

        spark_blas = next((r for r in results if r.name == "Kozmika interpret (blas)"), None)
        if spark_blas and spark_blas.vs_c is not None:
            print(
                f"target_1x_vs_c_blocked (Kozmika BLAS): "
                f"{'REACHED' if spark_blas.vs_c >= 1.0 else 'NOT_REACHED'} ({spark_blas.vs_c:.3f}x)"
            )

        stem_prefix = "crosslang_matmul4_runtime" if args.mode == "full" else "crosslang_matmul4_kernel_only"
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
                "spark_compute_mode": args.spark_compute_mode,
                "case_set": args.case_set,
                "baseline": "C (clang -O3 blocked)",
                "single_thread": True,
            },
            "results": [asdict(r) for r in results],
            "checksum_reference": baseline.checksum,
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
                    "vs_c_blocked",
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
