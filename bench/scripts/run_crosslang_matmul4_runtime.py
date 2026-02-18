#!/usr/bin/env python3
"""Cross-language runtime-only benchmark for chained 4-matrix multiplication.

This benchmark computes the checksum of ((A @ B) @ C) @ D on NxN matrices.
All measurements pin math libraries to single-thread mode for fairness.
Build/compile steps are executed before timing and are not included in results.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import pathlib
import re
import shutil
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
    float_line = None
    for raw in reversed(stdout.strip().splitlines()):
        line = raw.strip().replace(",", ".")
        if re.fullmatch(r"[+-]?\d+(\.\d+)?([eE][+-]?\d+)?", line):
            float_line = line
            break
    if float_line is None:
        raise RuntimeError(f"could not parse checksum from output:\n{stdout}")
    return float_line, float(float_line)


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


def write_sources(workdir: pathlib.Path, n: int, repeats: int, spark_compute_mode: str) -> Dict[str, pathlib.Path]:
    if spark_compute_mode not in {"materialize", "fused_sum"}:
        raise ValueError(f"unsupported spark_compute_mode: {spark_compute_mode}")

    spark_compute = workdir / "spark_matmul4_compute.k"
    spark_init = workdir / "spark_matmul4_init.k"

    spark_header = [
        f"n = {n}",
        f"repeats = {repeats}",
        "",
        "a = matrix_fill_affine(n, n, 17, 13, 97, 0.010309278350515464)",
        "b = matrix_fill_affine(n, n, 7, 11, 89, 0.011235955056179775)",
        "c = matrix_fill_affine(n, n, 19, 3, 83, 0.012048192771084338)",
        "d = matrix_fill_affine(n, n, 5, 23, 79, 0.012658227848101266)",
    ]

    if spark_compute_mode == "fused_sum":
        spark_loop = [
            "",
            "total = 0.0",
            "r = 0",
            "while r < repeats:",
            "  total = total + matmul4_sum(a, b, c, d)",
            "  r = r + 1",
            "print(total)",
            "",
        ]
    else:
        spark_loop = [
            "",
            "total = 0.0",
            "r = 0",
            "while r < repeats:",
            "  t1 = a * b",
            "  t2 = t1 * c",
            "  t3 = t2 * d",
            "  total = accumulate_sum(total, t3)",
            "  r = r + 1",
            "print(total)",
            "",
        ]

    spark_compute.write_text("\n".join(spark_header + spark_loop), encoding="utf-8")
    spark_init.write_text(
        "\n".join(
            [
                f"n = {n}",
                "a = matrix_fill_affine(n, n, 17, 13, 97, 0.010309278350515464)",
                "b = matrix_fill_affine(n, n, 7, 11, 89, 0.011235955056179775)",
                "c = matrix_fill_affine(n, n, 19, 3, 83, 0.012048192771084338)",
                "d = matrix_fill_affine(n, n, 5, 23, 79, 0.012658227848101266)",
                "total = 0.0",
                "total = accumulate_sum(total, a)",
                "total = accumulate_sum(total, b)",
                "total = accumulate_sum(total, c)",
                "total = accumulate_sum(total, d)",
                "print(total)",
                "",
            ]
        ),
        encoding="utf-8",
    )

    c_naive = workdir / "matmul4_c_naive.c"
    c_naive.write_text(
        f"""#include <stdio.h>

int main(void) {{
  const int n = {n};
  const int repeats = {repeats};
  static double a[{n}][{n}];
  static double b[{n}][{n}];
  static double c[{n}][{n}];
  static double d[{n}][{n}];
  static double t1[{n}][{n}];
  static double t2[{n}][{n}];
  static double out[{n}][{n}];

  for (int i = 0; i < n; ++i) {{
    for (int j = 0; j < n; ++j) {{
      a[i][j] = (double)((i * 17 + j * 13) % 97) / 97.0;
      b[i][j] = (double)((i * 7 + j * 11) % 89) / 89.0;
      c[i][j] = (double)((i * 19 + j * 3) % 83) / 83.0;
      d[i][j] = (double)((i * 5 + j * 23) % 79) / 79.0;
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
        t1[i][j] = acc;
      }}
    }}
    for (int i = 0; i < n; ++i) {{
      for (int j = 0; j < n; ++j) {{
        double acc = 0.0;
        for (int k = 0; k < n; ++k) {{
          acc += t1[i][k] * c[k][j];
        }}
        t2[i][j] = acc;
      }}
    }}
    for (int i = 0; i < n; ++i) {{
      for (int j = 0; j < n; ++j) {{
        double acc = 0.0;
        for (int k = 0; k < n; ++k) {{
          acc += t2[i][k] * d[k][j];
        }}
        out[i][j] = acc;
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

    c_blocked = workdir / "matmul4_c_blocked.c"
    c_blocked.write_text(
        f"""#include <stdio.h>

static void zero_matrix(int n, double m[n][n]) {{
  for (int i = 0; i < n; ++i) {{
    for (int j = 0; j < n; ++j) {{
      m[i][j] = 0.0;
    }}
  }}
}}

int main(void) {{
  const int n = {n};
  const int repeats = {repeats};
  static double a[{n}][{n}];
  static double b[{n}][{n}];
  static double c[{n}][{n}];
  static double d[{n}][{n}];
  static double t1[{n}][{n}];
  static double t2[{n}][{n}];
  static double out[{n}][{n}];

  for (int i = 0; i < n; ++i) {{
    for (int j = 0; j < n; ++j) {{
      a[i][j] = (double)((i * 17 + j * 13) % 97) / 97.0;
      b[i][j] = (double)((i * 7 + j * 11) % 89) / 89.0;
      c[i][j] = (double)((i * 19 + j * 3) % 83) / 83.0;
      d[i][j] = (double)((i * 5 + j * 23) % 79) / 79.0;
    }}
  }}

  double total = 0.0;
  for (int r = 0; r < repeats; ++r) {{
    zero_matrix(n, t1);
    zero_matrix(n, t2);
    zero_matrix(n, out);
    for (int i = 0; i < n; ++i) {{
      for (int k = 0; k < n; ++k) {{
        const double aik = a[i][k];
        for (int j = 0; j < n; ++j) {{
          t1[i][j] += aik * b[k][j];
        }}
      }}
    }}
    for (int i = 0; i < n; ++i) {{
      for (int k = 0; k < n; ++k) {{
        const double aik = t1[i][k];
        for (int j = 0; j < n; ++j) {{
          t2[i][j] += aik * c[k][j];
        }}
      }}
    }}
    for (int i = 0; i < n; ++i) {{
      for (int k = 0; k < n; ++k) {{
        const double aik = t2[i][k];
        for (int j = 0; j < n; ++j) {{
          out[i][j] += aik * d[k][j];
        }}
      }}
    }}
    for (int i = 0; i < n; ++i) {{
      for (int j = 0; j < n; ++j) {{
        total += out[i][j];
      }}
    }}
  }}

  printf("%.17f\\n", total);
  return 0;
}}
""",
        encoding="utf-8",
    )

    c_blas = workdir / "matmul4_c_blas.c"
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
  const size_t nn = (size_t)n * (size_t)n;
  double* a = (double*)aligned_alloc(64, nn * sizeof(double));
  double* b = (double*)aligned_alloc(64, nn * sizeof(double));
  double* c = (double*)aligned_alloc(64, nn * sizeof(double));
  double* d = (double*)aligned_alloc(64, nn * sizeof(double));
  double* t1 = (double*)aligned_alloc(64, nn * sizeof(double));
  double* t2 = (double*)aligned_alloc(64, nn * sizeof(double));
  double* out = (double*)aligned_alloc(64, nn * sizeof(double));
  if (!a || !b || !c || !d || !t1 || !t2 || !out) {{
    return 2;
  }}

  for (int i = 0; i < n; ++i) {{
    for (int j = 0; j < n; ++j) {{
      const size_t idx = (size_t)i * (size_t)n + (size_t)j;
      a[idx] = (double)((i * 17 + j * 13) % 97) / 97.0;
      b[idx] = (double)((i * 7 + j * 11) % 89) / 89.0;
      c[idx] = (double)((i * 19 + j * 3) % 83) / 83.0;
      d[idx] = (double)((i * 5 + j * 23) % 79) / 79.0;
      t1[idx] = 0.0;
      t2[idx] = 0.0;
      out[idx] = 0.0;
    }}
  }}

  double total = 0.0;
  for (int r = 0; r < repeats; ++r) {{
    cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, n, n, n, 1.0, a, n, b, n, 0.0, t1, n);
    cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, n, n, n, 1.0, t1, n, c, n, 0.0, t2, n);
    cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, n, n, n, 1.0, t2, n, d, n, 0.0, out, n);
    for (size_t i = 0; i < nn; ++i) {{
      total += out[i];
    }}
  }}

  printf("%.17f\\n", total);
  free(a);
  free(b);
  free(c);
  free(d);
  free(t1);
  free(t2);
  free(out);
  return 0;
}}
""",
        encoding="utf-8",
    )

    c_init = workdir / "matmul4_c_init.c"
    c_init.write_text(
        f"""#include <stdio.h>

int main(void) {{
  const int n = {n};
  static double a[{n}][{n}];
  static double b[{n}][{n}];
  static double c[{n}][{n}];
  static double d[{n}][{n}];
  double total = 0.0;
  for (int i = 0; i < n; ++i) {{
    for (int j = 0; j < n; ++j) {{
      a[i][j] = (double)((i * 17 + j * 13) % 97) / 97.0;
      b[i][j] = (double)((i * 7 + j * 11) % 89) / 89.0;
      c[i][j] = (double)((i * 19 + j * 3) % 83) / 83.0;
      d[i][j] = (double)((i * 5 + j * 23) % 79) / 79.0;
      total += a[i][j] + b[i][j] + c[i][j] + d[i][j];
    }}
  }}
  printf("%.17f\\n", total);
  return 0;
}}
""",
        encoding="utf-8",
    )

    np_compute = workdir / "matmul4_numpy_compute.py"
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
                "c = np.ascontiguousarray(((i * 19.0 + j * 3.0) % 83.0) / 83.0, dtype=np.float64)",
                "d = np.ascontiguousarray(((i * 5.0 + j * 23.0) % 79.0) / 79.0, dtype=np.float64)",
                "total = 0.0",
                "for _ in range(repeats):",
                "    x = np.linalg.multi_dot([a, b, c, d])",
                "    total += float(x.sum(dtype=np.float64))",
                "print(f\"{total:.17f}\")",
            ]
        ),
        encoding="utf-8",
    )

    np_init = workdir / "matmul4_numpy_init.py"
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
                "c = np.ascontiguousarray(((i * 19.0 + j * 3.0) % 83.0) / 83.0, dtype=np.float64)",
                "d = np.ascontiguousarray(((i * 5.0 + j * 23.0) % 79.0) / 79.0, dtype=np.float64)",
                "total = float(a.sum(dtype=np.float64) + b.sum(dtype=np.float64) + c.sum(dtype=np.float64) + d.sum(dtype=np.float64))",
                "print(f\"{total:.17f}\")",
            ]
        ),
        encoding="utf-8",
    )

    java_compute = workdir / "MatrixMatmul4Compute.java"
    java_compute.write_text(
        f"""public class MatrixMatmul4Compute {{
  private static double[][] multiply(double[][] x, double[][] y) {{
    int n = x.length;
    double[][] out = new double[n][n];
    for (int i = 0; i < n; i++) {{
      double[] xi = x[i];
      double[] outi = out[i];
      for (int j = 0; j < n; j++) {{
        double acc = 0.0;
        for (int k = 0; k < n; k++) {{
          acc += xi[k] * y[k][j];
        }}
        outi[j] = acc;
      }}
    }}
    return out;
  }}

  private static double sum(double[][] x) {{
    double t = 0.0;
    for (int i = 0; i < x.length; i++) {{
      for (int j = 0; j < x.length; j++) {{
        t += x[i][j];
      }}
    }}
    return t;
  }}

  public static void main(String[] args) {{
    final int n = {n};
    final int repeats = {repeats};
    double[][] a = new double[n][n];
    double[][] b = new double[n][n];
    double[][] c = new double[n][n];
    double[][] d = new double[n][n];
    for (int i = 0; i < n; i++) {{
      for (int j = 0; j < n; j++) {{
        a[i][j] = ((i * 17 + j * 13) % 97) / 97.0;
        b[i][j] = ((i * 7 + j * 11) % 89) / 89.0;
        c[i][j] = ((i * 19 + j * 3) % 83) / 83.0;
        d[i][j] = ((i * 5 + j * 23) % 79) / 79.0;
      }}
    }}
    double total = 0.0;
    for (int r = 0; r < repeats; r++) {{
      double[][] t1 = multiply(a, b);
      double[][] t2 = multiply(t1, c);
      double[][] t3 = multiply(t2, d);
      total += sum(t3);
    }}
    System.out.printf("%.17f%n", total);
  }}
}}
""",
        encoding="utf-8",
    )

    java_init = workdir / "MatrixMatmul4Init.java"
    java_init.write_text(
        f"""public class MatrixMatmul4Init {{
  public static void main(String[] args) {{
    final int n = {n};
    double[][] a = new double[n][n];
    double[][] b = new double[n][n];
    double[][] c = new double[n][n];
    double[][] d = new double[n][n];
    double total = 0.0;
    for (int i = 0; i < n; i++) {{
      for (int j = 0; j < n; j++) {{
        a[i][j] = ((i * 17 + j * 13) % 97) / 97.0;
        b[i][j] = ((i * 7 + j * 11) % 89) / 89.0;
        c[i][j] = ((i * 19 + j * 3) % 83) / 83.0;
        d[i][j] = ((i * 5 + j * 23) % 79) / 79.0;
        total += a[i][j] + b[i][j] + c[i][j] + d[i][j];
      }}
    }}
    System.out.printf("%.17f%n", total);
  }}
}}
""",
        encoding="utf-8",
    )

    matlab_compute = workdir / "matmul4_compute.m"
    matlab_compute.write_text(
        "\n".join(
            [
                "maxNumCompThreads(1);",
                f"n = {n};",
                f"repeats = {repeats};",
                "[I, J] = ndgrid(0:n-1, 0:n-1);",
                "a = mod(I * 17 + J * 13, 97) / 97;",
                "b = mod(I * 7 + J * 11, 89) / 89;",
                "c = mod(I * 19 + J * 3, 83) / 83;",
                "d = mod(I * 5 + J * 23, 79) / 79;",
                "total = 0.0;",
                "for r = 1:repeats",
                "  x = ((a * b) * c) * d;",
                "  total = total + sum(x, 'all');",
                "end",
                "fprintf('%.17f\\n', total);",
            ]
        ),
        encoding="utf-8",
    )

    matlab_init = workdir / "matmul4_init.m"
    matlab_init.write_text(
        "\n".join(
            [
                "maxNumCompThreads(1);",
                f"n = {n};",
                "[I, J] = ndgrid(0:n-1, 0:n-1);",
                "a = mod(I * 17 + J * 13, 97) / 97;",
                "b = mod(I * 7 + J * 11, 89) / 89;",
                "c = mod(I * 19 + J * 3, 83) / 83;",
                "d = mod(I * 5 + J * 23, 79) / 79;",
                "total = sum(a, 'all') + sum(b, 'all') + sum(c, 'all') + sum(d, 'all');",
                "fprintf('%.17f\\n', total);",
            ]
        ),
        encoding="utf-8",
    )

    return {
        "spark_compute": spark_compute,
        "spark_init": spark_init,
        "c_naive": c_naive,
        "c_blocked": c_blocked,
        "c_blas": c_blas,
        "c_init": c_init,
        "np_compute": np_compute,
        "np_init": np_init,
        "java_compute": java_compute,
        "java_init": java_init,
        "matlab_compute": matlab_compute,
        "matlab_init": matlab_init,
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


def matlab_available() -> bool:
    return shutil.which("matlab") is not None


def main() -> int:
    parser = argparse.ArgumentParser(description="Cross-language runtime benchmark for ((A@B)@C)@D")
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
    with tempfile.TemporaryDirectory(prefix="spark-crosslang4-") as tmp:
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
