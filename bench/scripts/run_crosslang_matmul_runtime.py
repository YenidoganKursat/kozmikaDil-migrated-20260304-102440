#!/usr/bin/env python3
"""Runtime-only cross-language matrix-matmul benchmark.

Build/compile steps are executed before timing and never included in measured
runtime results.
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
from typing import Dict, List


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
RESULT_DIR = REPO_ROOT / "bench" / "results"


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


def run_checked(cmd: List[str], *, cwd: pathlib.Path, env: Dict[str, str] | None = None) -> subprocess.CompletedProcess:
    merged_env = os.environ.copy()
    if env:
        merged_env.update(env)
    return subprocess.run(cmd, cwd=str(cwd), env=merged_env, capture_output=True, text=True, check=True)


def parse_checksum(stdout: str) -> tuple[str, float]:
    line = stdout.strip().splitlines()[-1].strip()
    normalized = line.replace(",", ".")
    return line, float(normalized)


def measure_case(name: str, cmd: List[str], cwd: pathlib.Path, env: Dict[str, str], warmup: int, runs: int) -> BenchmarkResult:
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

    return BenchmarkResult(
        name=name,
        median_sec=statistics.median(times),
        min_sec=min(times),
        max_sec=max(times),
        runs=runs,
        checksum_raw=checksum_raw,
        checksum=checksum,
    )


def write_sources(workdir: pathlib.Path, n: int, repeats: int) -> Dict[str, pathlib.Path]:
    spark_src = workdir / "spark_matmul.k"
    spark_src.write_text(
        "\n".join(
            [
                f"n = {n}",
                f"repeats = {repeats}",
                "",
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
        ),
        encoding="utf-8",
    )

    c_src = workdir / "matmul.c"
    c_src.write_text(
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

    py_src = workdir / "matmul.py"
    py_src.write_text(
        "\n".join(
            [
                f"n = {n}",
                f"repeats = {repeats}",
                "",
                "a = [[((i * 17 + j * 13) % 97) / 97.0 for j in range(n)] for i in range(n)]",
                "b = [[((i * 7 + j * 11) % 89) / 89.0 for j in range(n)] for i in range(n)]",
                "",
                "total = 0.0",
                "for _ in range(repeats):",
                "    for i in range(n):",
                "        ai = a[i]",
                "        for j in range(n):",
                "            acc = 0.0",
                "            for k in range(n):",
                "                acc += ai[k] * b[k][j]",
                "            total += acc",
                "",
                "print(f\"{total:.17f}\")",
                "",
            ]
        ),
        encoding="utf-8",
    )

    np_src = workdir / "matmul_numpy.py"
    np_src.write_text(
        "\n".join(
            [
                "import os",
                "os.environ.setdefault('OPENBLAS_NUM_THREADS', '1')",
                "os.environ.setdefault('OMP_NUM_THREADS', '1')",
                "os.environ.setdefault('MKL_NUM_THREADS', '1')",
                "os.environ.setdefault('VECLIB_MAXIMUM_THREADS', '1')",
                "import numpy as np",
                "",
                f"n = {n}",
                f"repeats = {repeats}",
                "i = np.arange(n, dtype=np.float64)[:, None]",
                "j = np.arange(n, dtype=np.float64)[None, :]",
                "a = np.ascontiguousarray(((i * 17.0 + j * 13.0) % 97.0) / 97.0, dtype=np.float64)",
                "b = np.ascontiguousarray(((i * 7.0 + j * 11.0) % 89.0) / 89.0, dtype=np.float64)",
                "",
                "total = 0.0",
                "for _ in range(repeats):",
                "    c = a @ b",
                "    total += float(c.sum(dtype=np.float64))",
                "",
                "print(f\"{total:.17f}\")",
                "",
            ]
        ),
        encoding="utf-8",
    )

    java_src = workdir / "MatrixMatmul.java"
    java_src.write_text(
        f"""public class MatrixMatmul {{
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

    return {
        "spark_src": spark_src,
        "c_src": c_src,
        "py_src": py_src,
        "np_src": np_src,
        "java_src": java_src,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Cross-language runtime-only matmul benchmark")
    parser.add_argument("--n", type=int, default=100, help="Matrix dimension N (NxN)")
    parser.add_argument("--repeats", type=int, default=100, help="Repeat count")
    parser.add_argument("--runs", type=int, default=5, help="Measured runs")
    parser.add_argument("--warmup", type=int, default=1, help="Warmup runs")
    parser.add_argument("--spark-threads", type=int, default=1, help="SPARK_MATMUL_OWN_THREADS")
    args = parser.parse_args()

    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="spark-crosslang-") as tmp:
        workdir = pathlib.Path(tmp)
        src = write_sources(workdir, args.n, args.repeats)

        c_bin = workdir / "matmul_c_bin"
        run_checked(["clang", "-O3", "-DNDEBUG", str(src["c_src"]), "-o", str(c_bin)], cwd=REPO_ROOT)
        run_checked(["javac", str(src["java_src"])], cwd=REPO_ROOT)

        common_thread_env = {
            "OPENBLAS_NUM_THREADS": "1",
            "OMP_NUM_THREADS": "1",
            "MKL_NUM_THREADS": "1",
            "VECLIB_MAXIMUM_THREADS": "1",
            "LC_ALL": "C",
        }

        cases = [
            (
                "Spark interpret (own)",
                ["./k", "run", "--interpret", str(src["spark_src"])],
                {**common_thread_env, "SPARK_MATMUL_BACKEND": "own", "SPARK_MATMUL_OWN_THREADS": str(args.spark_threads)},
            ),
            (
                "Spark interpret (blas)",
                ["./k", "run", "--interpret", str(src["spark_src"])],
                {**common_thread_env, "SPARK_MATMUL_BACKEND": "blas", "SPARK_MATMUL_OWN_THREADS": str(args.spark_threads)},
            ),
            (
                "Spark interpret (auto)",
                ["./k", "run", "--interpret", str(src["spark_src"])],
                {**common_thread_env, "SPARK_MATMUL_BACKEND": "auto", "SPARK_MATMUL_OWN_THREADS": str(args.spark_threads)},
            ),
            ("C (clang -O3)", [str(c_bin)], common_thread_env),
            ("Python (naive loops)", ["python3", str(src["py_src"])], common_thread_env),
            ("Python+NumPy", ["python3", str(src["np_src"])], common_thread_env),
            (
                "Java (naive loops)",
                ["java", "-Duser.language=en", "-Duser.region=US", "-cp", str(workdir), "MatrixMatmul"],
                common_thread_env,
            ),
        ]

        results: List[BenchmarkResult] = []
        for name, cmd, env in cases:
            results.append(measure_case(name, cmd, REPO_ROOT, env, args.warmup, args.runs))

        c_case = next((r for r in results if r.name.startswith("C ")), None)
        if c_case is None:
            raise RuntimeError("C baseline result missing")
        for row in results:
            row.vs_c = c_case.median_sec / row.median_sec if row.median_sec > 0 else None

        for row in results:
            print(
                f"{row.name}: median={row.median_sec:.6f}s min={row.min_sec:.6f}s "
                f"max={row.max_sec:.6f}s C/this={row.vs_c:.3f}x checksum={row.checksum_raw}"
            )

        stem = f"crosslang_matmul_runtime_{args.n}x{args.n}_r{args.repeats}"
        json_path = RESULT_DIR / f"{stem}.json"
        csv_path = RESULT_DIR / f"{stem}.csv"

        payload = {
            "config": {
                "n": args.n,
                "repeats": args.repeats,
                "runs": args.runs,
                "warmup": args.warmup,
                "spark_threads": args.spark_threads,
                "runtime_only": True,
            },
            "results": [asdict(r) for r in results],
            "checksum_reference": c_case.checksum,
        }
        json_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")

        with csv_path.open("w", newline="", encoding="utf-8") as f:
            writer = csv.writer(f)
            writer.writerow(["name", "median_sec", "min_sec", "max_sec", "runs", "checksum_raw", "checksum", "vs_c"])
            for row in results:
                writer.writerow(
                    [
                        row.name,
                        f"{row.median_sec:.9f}",
                        f"{row.min_sec:.9f}",
                        f"{row.max_sec:.9f}",
                        row.runs,
                        row.checksum_raw,
                        f"{row.checksum:.17g}",
                        f"{row.vs_c:.9f}" if row.vs_c is not None else "",
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
