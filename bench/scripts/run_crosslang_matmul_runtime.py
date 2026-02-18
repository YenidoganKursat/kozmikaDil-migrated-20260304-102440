#!/usr/bin/env python3
"""Cross-language matrix-matmul benchmark with fair baseline controls.

Features:
- C baselines split into naive / blocked / BLAS.
- Two timing modes:
  - full: process wall-time for full program (init + compute).
  - kernel-only: estimated compute-only time via (compute_run - init_run).
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import pathlib
import statistics
import subprocess
import sys
import tempfile
import time
from dataclasses import asdict, dataclass
from typing import Dict, List, Tuple


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
RESULT_DIR = REPO_ROOT / "bench" / "results"


@dataclass
class SampleBatch:
    times: List[float]
    checksum_raw: str
    checksum: float


@dataclass
class BenchmarkResult:
    name: str
    median_sec: float
    min_sec: float
    max_sec: float
    runs: int
    checksum_raw: str
    checksum: float
    vs_c: float | None = None
    mode: str = "full"
    compute_median_sec: float | None = None
    init_median_sec: float | None = None


def run_checked(cmd: List[str], *, cwd: pathlib.Path, env: Dict[str, str] | None = None) -> subprocess.CompletedProcess:
    merged_env = os.environ.copy()
    if env:
        merged_env.update(env)
    return subprocess.run(cmd, cwd=str(cwd), env=merged_env, capture_output=True, text=True, check=True)


def parse_checksum(stdout: str) -> Tuple[str, float]:
    line = stdout.strip().splitlines()[-1].strip()
    normalized = line.replace(",", ".")
    return line, float(normalized)


def measure_samples(cmd: List[str], cwd: pathlib.Path, env: Dict[str, str], warmup: int, runs: int) -> SampleBatch:
    for _ in range(warmup):
        run_checked(cmd, cwd=cwd, env=env)

    times: List[float] = []
    checksum_raw = ""
    checksum = 0.0
    for _ in range(runs):
        t0 = time.perf_counter()
        proc = run_checked(cmd, cwd=cwd, env=env)
        t1 = time.perf_counter()
        times.append(t1 - t0)
        checksum_raw, checksum = parse_checksum(proc.stdout)
    return SampleBatch(times=times, checksum_raw=checksum_raw, checksum=checksum)


def result_from_full(name: str, batch: SampleBatch, mode: str) -> BenchmarkResult:
    return BenchmarkResult(
        name=name,
        median_sec=statistics.median(batch.times),
        min_sec=min(batch.times),
        max_sec=max(batch.times),
        runs=len(batch.times),
        checksum_raw=batch.checksum_raw,
        checksum=batch.checksum,
        mode=mode,
    )


def result_from_kernel_delta(name: str, compute: SampleBatch, init: SampleBatch) -> BenchmarkResult:
    pair_count = min(len(compute.times), len(init.times))
    if pair_count <= 0:
        raise RuntimeError(f"kernel-only delta requires non-empty samples for {name}")

    delta_times = [max(0.0, compute.times[i] - init.times[i]) for i in range(pair_count)]
    return BenchmarkResult(
        name=name,
        median_sec=statistics.median(delta_times),
        min_sec=min(delta_times),
        max_sec=max(delta_times),
        runs=pair_count,
        checksum_raw=compute.checksum_raw,
        checksum=compute.checksum,
        mode="kernel-only",
        compute_median_sec=statistics.median(compute.times),
        init_median_sec=statistics.median(init.times),
    )


def write_sources(workdir: pathlib.Path, n: int, repeats: int,
                  spark_init_mode: str = "affine",
                  spark_compute_mode: str = "fused_sum") -> Dict[str, pathlib.Path]:
    if spark_init_mode not in {"affine", "loops"}:
        raise ValueError(f"unsupported spark_init_mode: {spark_init_mode}")
    if spark_compute_mode not in {"materialize", "fused_sum"}:
        raise ValueError(f"unsupported spark_compute_mode: {spark_compute_mode}")

    if spark_init_mode == "affine":
        # 1/97 and 1/89 so values match the loop initialization exactly.
        spark_init_lines = [
            "a = matrix_fill_affine(n, n, 17, 13, 97, 0.010309278350515464)",
            "b = matrix_fill_affine(n, n, 7, 11, 89, 0.011235955056179775)",
        ]
        spark_init_only_lines = spark_init_lines + [
            "total = 0.0",
            "total = accumulate_sum(total, a)",
            "total = accumulate_sum(total, b)",
            "print(total)",
        ]
    else:
        spark_init_lines = [
            "a = matrix_f64(n, n)",
            "b = matrix_f64(n, n)",
            "",
            "i = 0",
            "while i < n:",
            "  j = 0",
            "  while j < n:",
            "    a[i][j] = ((i * 17 + j * 13) % 97) / 97.0",
            "    b[i][j] = ((i * 7 + j * 11) % 89) / 89.0",
            "    j = j + 1",
            "  i = i + 1",
        ]
        spark_init_only_lines = spark_init_lines + [
            "total = 0.0",
            "total = accumulate_sum(total, a)",
            "total = accumulate_sum(total, b)",
            "print(total)",
        ]

    spark_compute = workdir / "spark_matmul_compute.k"
    compute_lines = [
        f"n = {n}",
        f"repeats = {repeats}",
        "",
    ]
    compute_lines.extend(spark_init_lines)
    if spark_compute_mode == "fused_sum":
        compute_loop_lines = [
            "",
            "total = 0.0",
            "r = 0",
            "while r < repeats:",
            "  total = total + matmul_sum(a, b)",
            "  r = r + 1",
            "",
            "print(total)",
            "",
        ]
    else:
        compute_loop_lines = [
            "",
            "total = 0.0",
            "r = 0",
            "while r < repeats:",
            "  c = a * b",
            "  total = accumulate_sum(total, c)",
            "  r = r + 1",
            "",
            "print(total)",
            "",
        ]
    compute_lines.extend(compute_loop_lines)
    spark_compute.write_text("\n".join(compute_lines), encoding="utf-8")

    spark_init = workdir / "spark_matmul_init.k"
    spark_init_lines_final = [f"n = {n}"] + spark_init_only_lines + [""]
    spark_init.write_text("\n".join(spark_init_lines_final), encoding="utf-8")

    c_naive = workdir / "matmul_c_naive.c"
    c_naive.write_text(
        f"""#include <stdio.h>

int main(void) {{
  const int n = {n};
  const int repeats = {repeats};
  static double a[{n}][{n}];
  static double b[{n}][{n}];
  static double c[{n}][{n}];

  for (int i = 0; i < n; ++i) {{
    for (int j = 0; j < n; ++j) {{
      a[i][j] = (double)((i * 17 + j * 13) % 97) / 97.0;
      b[i][j] = (double)((i * 7 + j * 11) % 89) / 89.0;
      c[i][j] = 0.0;
    }}
  }}

  double total = 0.0;
  for (int r = 0; r < repeats; ++r) {{
    for (int i = 0; i < n; ++i) {{
      for (int j = 0; j < n; ++j) {{
        double acc = 0.0;
        for (int k = 0; k < n; ++k) {{
          acc += a[i][k] * b[k][j];
        }}
        c[i][j] = acc;
        total += acc;
      }}
    }}
  }}

  printf("%.17f\\n", total);
  return 0;
}}
""",
        encoding="utf-8",
    )

    c_blocked = workdir / "matmul_c_blocked.c"
    c_blocked.write_text(
        f"""#include <stdio.h>

int main(void) {{
  const int n = {n};
  const int repeats = {repeats};
  static double a[{n}][{n}];
  static double b[{n}][{n}];
  static double c[{n}][{n}];

  for (int i = 0; i < n; ++i) {{
    for (int j = 0; j < n; ++j) {{
      a[i][j] = (double)((i * 17 + j * 13) % 97) / 97.0;
      b[i][j] = (double)((i * 7 + j * 11) % 89) / 89.0;
      c[i][j] = 0.0;
    }}
  }}

  double total = 0.0;
  for (int r = 0; r < repeats; ++r) {{
    for (int i = 0; i < n; ++i) {{
      for (int k = 0; k < n; ++k) {{
        const double aik = a[i][k];
        for (int j = 0; j < n; ++j) {{
          c[i][j] += aik * b[k][j];
        }}
      }}
    }}
  }}
  for (int i = 0; i < n; ++i) {{
    for (int j = 0; j < n; ++j) {{
      total += c[i][j];
    }}
  }}

  printf("%.17f\\n", total);
  return 0;
}}
""",
        encoding="utf-8",
    )

    c_blas = workdir / "matmul_c_blas.c"
    c_blas.write_text(
        f"""#include <stdio.h>
#include <stdlib.h>
#if defined(__APPLE__)
#include <Accelerate/Accelerate.h>
#else
#include <cblas.h>
#endif

int main(void) {{
  const int n = {n};
  const int repeats = {repeats};
  double* a = (double*)aligned_alloc(64, (size_t)n * (size_t)n * sizeof(double));
  double* b = (double*)aligned_alloc(64, (size_t)n * (size_t)n * sizeof(double));
  double* c = (double*)aligned_alloc(64, (size_t)n * (size_t)n * sizeof(double));
  if (!a || !b || !c) {{
    return 2;
  }}

  for (int i = 0; i < n; ++i) {{
    for (int j = 0; j < n; ++j) {{
      const size_t idx = (size_t)i * (size_t)n + (size_t)j;
      a[idx] = (double)((i * 17 + j * 13) % 97) / 97.0;
      b[idx] = (double)((i * 7 + j * 11) % 89) / 89.0;
      c[idx] = 0.0;
    }}
  }}

  double total = 0.0;
  for (int r = 0; r < repeats; ++r) {{
    cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                n, n, n,
                1.0, a, n,
                b, n,
                0.0, c, n);
    for (size_t i = 0; i < (size_t)n * (size_t)n; ++i) {{
      total += c[i];
    }}
  }}

  printf("%.17f\\n", total);
  free(a);
  free(b);
  free(c);
  return 0;
}}
""",
        encoding="utf-8",
    )

    c_init = workdir / "matmul_c_init.c"
    c_init.write_text(
        f"""#include <stdio.h>

int main(void) {{
  const int n = {n};
  static double a[{n}][{n}];
  static double b[{n}][{n}];
  double total = 0.0;

  for (int i = 0; i < n; ++i) {{
    for (int j = 0; j < n; ++j) {{
      a[i][j] = (double)((i * 17 + j * 13) % 97) / 97.0;
      b[i][j] = (double)((i * 7 + j * 11) % 89) / 89.0;
      total += a[i][j] + b[i][j];
    }}
  }}

  printf("%.17f\\n", total);
  return 0;
}}
""",
        encoding="utf-8",
    )

    py_compute = workdir / "matmul_py_compute.py"
    py_compute.write_text(
        "\n".join(
            [
                f"n = {n}",
                f"repeats = {repeats}",
                "a = [[((i * 17 + j * 13) % 97) / 97.0 for j in range(n)] for i in range(n)]",
                "b = [[((i * 7 + j * 11) % 89) / 89.0 for j in range(n)] for i in range(n)]",
                "total = 0.0",
                "for _ in range(repeats):",
                "    for i in range(n):",
                "        ai = a[i]",
                "        for j in range(n):",
                "            acc = 0.0",
                "            for k in range(n):",
                "                acc += ai[k] * b[k][j]",
                "            total += acc",
                "print(f\"{total:.17f}\")",
            ]
        ),
        encoding="utf-8",
    )

    py_init = workdir / "matmul_py_init.py"
    py_init.write_text(
        "\n".join(
            [
                f"n = {n}",
                "a = [[((i * 17 + j * 13) % 97) / 97.0 for j in range(n)] for i in range(n)]",
                "b = [[((i * 7 + j * 11) % 89) / 89.0 for j in range(n)] for i in range(n)]",
                "total = 0.0",
                "for i in range(n):",
                "    total += sum(a[i]) + sum(b[i])",
                "print(f\"{total:.17f}\")",
            ]
        ),
        encoding="utf-8",
    )

    np_compute = workdir / "matmul_numpy_compute.py"
    np_compute.write_text(
        "\n".join(
            [
                "import os",
                "os.environ.setdefault('OPENBLAS_NUM_THREADS', '1')",
                "os.environ.setdefault('OMP_NUM_THREADS', '1')",
                "os.environ.setdefault('MKL_NUM_THREADS', '1')",
                "os.environ.setdefault('VECLIB_MAXIMUM_THREADS', '1')",
                "import numpy as np",
                f"n = {n}",
                f"repeats = {repeats}",
                "i = np.arange(n, dtype=np.float64)[:, None]",
                "j = np.arange(n, dtype=np.float64)[None, :]",
                "a = np.ascontiguousarray(((i * 17.0 + j * 13.0) % 97.0) / 97.0, dtype=np.float64)",
                "b = np.ascontiguousarray(((i * 7.0 + j * 11.0) % 89.0) / 89.0, dtype=np.float64)",
                "total = 0.0",
                "for _ in range(repeats):",
                "    c = a @ b",
                "    total += float(c.sum(dtype=np.float64))",
                "print(f\"{total:.17f}\")",
            ]
        ),
        encoding="utf-8",
    )

    np_init = workdir / "matmul_numpy_init.py"
    np_init.write_text(
        "\n".join(
            [
                "import os",
                "os.environ.setdefault('OPENBLAS_NUM_THREADS', '1')",
                "os.environ.setdefault('OMP_NUM_THREADS', '1')",
                "os.environ.setdefault('MKL_NUM_THREADS', '1')",
                "os.environ.setdefault('VECLIB_MAXIMUM_THREADS', '1')",
                "import numpy as np",
                f"n = {n}",
                "i = np.arange(n, dtype=np.float64)[:, None]",
                "j = np.arange(n, dtype=np.float64)[None, :]",
                "a = np.ascontiguousarray(((i * 17.0 + j * 13.0) % 97.0) / 97.0, dtype=np.float64)",
                "b = np.ascontiguousarray(((i * 7.0 + j * 11.0) % 89.0) / 89.0, dtype=np.float64)",
                "total = float(a.sum(dtype=np.float64) + b.sum(dtype=np.float64))",
                "print(f\"{total:.17f}\")",
            ]
        ),
        encoding="utf-8",
    )

    java_compute = workdir / "MatrixMatmulCompute.java"
    java_compute.write_text(
        f"""public class MatrixMatmulCompute {{
  public static void main(String[] args) {{
    final int n = {n};
    final int repeats = {repeats};
    double[][] a = new double[n][n];
    double[][] b = new double[n][n];
    for (int i = 0; i < n; i++) {{
      for (int j = 0; j < n; j++) {{
        a[i][j] = ((i * 17 + j * 13) % 97) / 97.0;
        b[i][j] = ((i * 7 + j * 11) % 89) / 89.0;
      }}
    }}
    double total = 0.0;
    for (int r = 0; r < repeats; r++) {{
      for (int i = 0; i < n; i++) {{
        double[] ai = a[i];
        for (int j = 0; j < n; j++) {{
          double acc = 0.0;
          for (int k = 0; k < n; k++) {{
            acc += ai[k] * b[k][j];
          }}
          total += acc;
        }}
      }}
    }}
    System.out.printf("%.17f%n", total);
  }}
}}
""",
        encoding="utf-8",
    )

    java_init = workdir / "MatrixMatmulInit.java"
    java_init.write_text(
        f"""public class MatrixMatmulInit {{
  public static void main(String[] args) {{
    final int n = {n};
    double[][] a = new double[n][n];
    double[][] b = new double[n][n];
    double total = 0.0;
    for (int i = 0; i < n; i++) {{
      for (int j = 0; j < n; j++) {{
        a[i][j] = ((i * 17 + j * 13) % 97) / 97.0;
        b[i][j] = ((i * 7 + j * 11) % 89) / 89.0;
        total += a[i][j] + b[i][j];
      }}
    }}
    System.out.printf("%.17f%n", total);
  }}
}}
""",
        encoding="utf-8",
    )

    return {
        "spark_compute": spark_compute,
        "spark_init": spark_init,
        "c_naive": c_naive,
        "c_blocked": c_blocked,
        "c_blas": c_blas,
        "c_init": c_init,
        "py_compute": py_compute,
        "py_init": py_init,
        "np_compute": np_compute,
        "np_init": np_init,
        "java_compute": java_compute,
        "java_init": java_init,
    }


def compile_c_blas(src: pathlib.Path, out_bin: pathlib.Path) -> None:
    env_flags = os.environ.get("SPARK_CBLAS_LINK_FLAGS", "").strip()
    if env_flags:
        cmd = ["clang", "-O3", "-DNDEBUG", str(src), "-o", str(out_bin)] + env_flags.split()
        run_checked(cmd, cwd=REPO_ROOT)
        return

    candidates: List[List[str]] = []
    if sys.platform == "darwin":
        candidates.append(["clang", "-O3", "-DNDEBUG", str(src), "-framework", "Accelerate", "-o", str(out_bin)])
    candidates.extend(
        [
            ["clang", "-O3", "-DNDEBUG", str(src), "-lopenblas", "-o", str(out_bin)],
            ["clang", "-O3", "-DNDEBUG", str(src), "-lblas", "-o", str(out_bin)],
        ]
    )

    errors: List[str] = []
    for cmd in candidates:
        try:
            run_checked(cmd, cwd=REPO_ROOT)
            return
        except subprocess.CalledProcessError as exc:
            errors.append((exc.stderr or "").strip())
    raise RuntimeError("failed to compile C BLAS baseline:\n" + "\n---\n".join(errors))


def should_include_python_naive(n: int, include_above: int) -> bool:
    return n <= include_above


def main() -> int:
    parser = argparse.ArgumentParser(description="Cross-language runtime-only matmul benchmark")
    parser.add_argument("--n", type=int, default=100, help="Matrix dimension N (NxN)")
    parser.add_argument("--repeats", type=int, default=100, help="Repeat count for matmul")
    parser.add_argument("--runs", type=int, default=5, help="Measured runs")
    parser.add_argument("--warmup", type=int, default=1, help="Warmup runs")
    parser.add_argument("--spark-threads", type=int, default=1, help="SPARK_MATMUL_OWN_THREADS")
    parser.add_argument(
        "--mode",
        choices=["full", "kernel-only"],
        default="full",
        help="full=init+compute wall-time, kernel-only=(compute-init) estimate",
    )
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
    with tempfile.TemporaryDirectory(prefix="spark-crosslang-") as tmp:
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
                summary += (
                    f" (compute_med={row.compute_median_sec:.6f}s init_med={row.init_median_sec:.6f}s)"
                )
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
