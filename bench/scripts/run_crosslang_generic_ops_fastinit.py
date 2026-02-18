#!/usr/bin/env python3
"""Cross-language generic operator benchmark with Kozmika fast-init builtins.

Measures list/matrix `+,-,*,/,%` chains in single-thread mode, runtime-only.
Kozmika uses `list_fill_affine` and `matrix_fill_affine` for initialization.
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
from dataclasses import asdict, dataclass
from typing import Dict, List


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
RESULT_DIR = REPO_ROOT / "bench" / "results"


@dataclass
class ResultRow:
    case: str
    name: str
    median_sec: float
    min_sec: float
    max_sec: float
    runs: int
    checksum_raw: str
    checksum: float
    vs_c: float | None = None


def run_checked(cmd: List[str], cwd: pathlib.Path, env: Dict[str, str]) -> subprocess.CompletedProcess:
    merged = os.environ.copy()
    merged.update(env)
    return subprocess.run(cmd, cwd=str(cwd), env=merged, capture_output=True, text=True, check=True)


def parse_checksum(stdout: str) -> tuple[str, float]:
    line = stdout.strip().splitlines()[-1].strip().replace(",", ".")
    return line, float(line)


def measure(cmd: List[str], env: Dict[str, str], warmup: int, runs: int) -> tuple[float, float, float, str, float]:
    for _ in range(warmup):
        run_checked(cmd, REPO_ROOT, env)
    times: List[float] = []
    checksum_raw = ""
    checksum = 0.0
    for _ in range(runs):
        t0 = time.perf_counter()
        proc = run_checked(cmd, REPO_ROOT, env)
        t1 = time.perf_counter()
        times.append(t1 - t0)
        checksum_raw, checksum = parse_checksum(proc.stdout)
    return statistics.median(times), min(times), max(times), checksum_raw, checksum


def write_sources(workdir: pathlib.Path, list_n: int, list_repeats: int,
                  matrix_n: int, matrix_repeats: int) -> Dict[str, pathlib.Path]:
    (workdir / "spark_list_ops.k").write_text(
        "\n".join(
            [
                f"N = {list_n}",
                f"repeats = {list_repeats}",
                "x = list_fill_affine(N, 17, 13, 97, 0.010309278350515464)",
                "acc = 0.0",
                "r = 0",
                "while r < repeats:",
                "  y = (((x + 1.25) * 1.5) - 0.75) / 3.5",
                "  z = y % 1.1",
                "  acc = acc + z.reduce_sum()",
                "  r = r + 1",
                "print(acc)",
                "",
            ]
        ),
        encoding="utf-8",
    )

    (workdir / "spark_matrix_ops.k").write_text(
        "\n".join(
            [
                f"n = {matrix_n}",
                f"repeats = {matrix_repeats}",
                "a = matrix_fill_affine(n, n, 31, 17, 101, 0.009900990099009901)",
                "b = matrix_fill_affine(n, n, 19, 7, 89, 0.011235955056179775)",
                "acc = 0.0",
                "k = 0",
                "while k < repeats:",
                "  m = (((a + b) * 1.75) - (a / 3.0)) % 5.0",
                "  acc = acc + m.reduce_sum()",
                "  k = k + 1",
                "print(acc)",
                "",
            ]
        ),
        encoding="utf-8",
    )

    (workdir / "c_list_ops.c").write_text(
        f"""#include <math.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) {{
  const int n = {list_n};
  const int repeats = {list_repeats};
  double* x = (double*)aligned_alloc(64, (size_t)n * sizeof(double));
  if (!x) return 2;
  for (int i = 0; i < n; ++i) x[i] = (double)((i * 17 + 13) % 97) / 97.0;
  double acc = 0.0;
  for (int r = 0; r < repeats; ++r) {{
    for (int i = 0; i < n; ++i) {{
      const double y = (((x[i] + 1.25) * 1.5) - 0.75) / 3.5;
      acc += fmod(y, 1.1);
    }}
  }}
  printf("%.17f\\n", acc);
  free(x);
  return 0;
}}
""",
        encoding="utf-8",
    )

    (workdir / "c_matrix_ops.c").write_text(
        f"""#include <math.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) {{
  const int n = {matrix_n};
  const int repeats = {matrix_repeats};
  const size_t nn = (size_t)n * (size_t)n;
  double* a = (double*)aligned_alloc(64, nn * sizeof(double));
  double* b = (double*)aligned_alloc(64, nn * sizeof(double));
  if (!a || !b) return 2;
  for (int r = 0; r < n; ++r) {{
    for (int c = 0; c < n; ++c) {{
      const size_t idx = (size_t)r * (size_t)n + (size_t)c;
      a[idx] = (double)((r * 31 + c * 17) % 101) / 101.0;
      b[idx] = (double)((r * 19 + c * 7) % 89) / 89.0;
    }}
  }}
  double acc = 0.0;
  for (int k = 0; k < repeats; ++k) {{
    for (size_t i = 0; i < nn; ++i) {{
      const double m = fmod((((a[i] + b[i]) * 1.75) - (a[i] / 3.0)), 5.0);
      acc += m;
    }}
  }}
  printf("%.17f\\n", acc);
  free(a);
  free(b);
  return 0;
}}
""",
        encoding="utf-8",
    )

    (workdir / "py_list_ops.py").write_text(
        "\n".join(
            [
                f"n = {list_n}",
                f"repeats = {list_repeats}",
                "x = [((i * 17 + 13) % 97) / 97.0 for i in range(n)]",
                "acc = 0.0",
                "for _ in range(repeats):",
                "    for v in x:",
                "        y = (((v + 1.25) * 1.5) - 0.75) / 3.5",
                "        acc += y % 1.1",
                "print(f\"{acc:.17f}\")",
            ]
        ),
        encoding="utf-8",
    )

    (workdir / "py_matrix_ops.py").write_text(
        "\n".join(
            [
                f"n = {matrix_n}",
                f"repeats = {matrix_repeats}",
                "a = [[((r * 31 + c * 17) % 101) / 101.0 for c in range(n)] for r in range(n)]",
                "b = [[((r * 19 + c * 7) % 89) / 89.0 for c in range(n)] for r in range(n)]",
                "acc = 0.0",
                "for _ in range(repeats):",
                "    for r in range(n):",
                "        ar = a[r]",
                "        br = b[r]",
                "        for c in range(n):",
                "            acc += (((ar[c] + br[c]) * 1.75) - (ar[c] / 3.0)) % 5.0",
                "print(f\"{acc:.17f}\")",
            ]
        ),
        encoding="utf-8",
    )

    (workdir / "np_list_ops.py").write_text(
        "\n".join(
            [
                "import os",
                "os.environ.setdefault('OPENBLAS_NUM_THREADS', '1')",
                "os.environ.setdefault('OMP_NUM_THREADS', '1')",
                "os.environ.setdefault('MKL_NUM_THREADS', '1')",
                "os.environ.setdefault('VECLIB_MAXIMUM_THREADS', '1')",
                "import numpy as np",
                f"n = {list_n}",
                f"repeats = {list_repeats}",
                "i = np.arange(n, dtype=np.float64)",
                "x = ((i * 17.0 + 13.0) % 97.0) / 97.0",
                "acc = 0.0",
                "for _ in range(repeats):",
                "    y = (((x + 1.25) * 1.5) - 0.75) / 3.5",
                "    acc += float(np.fmod(y, 1.1).sum(dtype=np.float64))",
                "print(f\"{acc:.17f}\")",
            ]
        ),
        encoding="utf-8",
    )

    (workdir / "np_matrix_ops.py").write_text(
        "\n".join(
            [
                "import os",
                "os.environ.setdefault('OPENBLAS_NUM_THREADS', '1')",
                "os.environ.setdefault('OMP_NUM_THREADS', '1')",
                "os.environ.setdefault('MKL_NUM_THREADS', '1')",
                "os.environ.setdefault('VECLIB_MAXIMUM_THREADS', '1')",
                "import numpy as np",
                f"n = {matrix_n}",
                f"repeats = {matrix_repeats}",
                "r = np.arange(n, dtype=np.float64)[:, None]",
                "c = np.arange(n, dtype=np.float64)[None, :]",
                "a = ((r * 31.0 + c * 17.0) % 101.0) / 101.0",
                "b = ((r * 19.0 + c * 7.0) % 89.0) / 89.0",
                "acc = 0.0",
                "for _ in range(repeats):",
                "    acc += float(np.fmod((((a + b) * 1.75) - (a / 3.0)), 5.0).sum(dtype=np.float64))",
                "print(f\"{acc:.17f}\")",
            ]
        ),
        encoding="utf-8",
    )

    (workdir / "ListOpsBench.java").write_text(
        f"""public class ListOpsBench {{
  public static void main(String[] args) {{
    final int n = {list_n};
    final int repeats = {list_repeats};
    double[] x = new double[n];
    for (int i = 0; i < n; i++) x[i] = ((i * 17 + 13) % 97) / 97.0;
    double acc = 0.0;
    for (int r = 0; r < repeats; r++) {{
      for (int i = 0; i < n; i++) {{
        double y = (((x[i] + 1.25) * 1.5) - 0.75) / 3.5;
        acc += y % 1.1;
      }}
    }}
    System.out.printf("%.17f%n", acc);
  }}
}}
""",
        encoding="utf-8",
    )

    (workdir / "MatrixOpsBench.java").write_text(
        f"""public class MatrixOpsBench {{
  public static void main(String[] args) {{
    final int n = {matrix_n};
    final int repeats = {matrix_repeats};
    double[][] a = new double[n][n];
    double[][] b = new double[n][n];
    for (int r = 0; r < n; r++) {{
      for (int c = 0; c < n; c++) {{
        a[r][c] = ((r * 31 + c * 17) % 101) / 101.0;
        b[r][c] = ((r * 19 + c * 7) % 89) / 89.0;
      }}
    }}
    double acc = 0.0;
    for (int k = 0; k < repeats; k++) {{
      for (int r = 0; r < n; r++) {{
        double[] ar = a[r];
        double[] br = b[r];
        for (int c = 0; c < n; c++) {{
          acc += (((ar[c] + br[c]) * 1.75) - (ar[c] / 3.0)) % 5.0;
        }}
      }}
    }}
    System.out.printf("%.17f%n", acc);
  }}
}}
""",
        encoding="utf-8",
    )

    return {
        "spark_list": workdir / "spark_list_ops.k",
        "spark_matrix": workdir / "spark_matrix_ops.k",
        "c_list": workdir / "c_list_ops.c",
        "c_matrix": workdir / "c_matrix_ops.c",
        "py_list": workdir / "py_list_ops.py",
        "py_matrix": workdir / "py_matrix_ops.py",
        "np_list": workdir / "np_list_ops.py",
        "np_matrix": workdir / "np_matrix_ops.py",
        "java_list": workdir / "ListOpsBench.java",
        "java_matrix": workdir / "MatrixOpsBench.java",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Cross-language generic ops benchmark (Kozmika fast-init)")
    parser.add_argument("--list-n", type=int, default=200000)
    parser.add_argument("--list-repeats", type=int, default=5)
    parser.add_argument("--matrix-n", type=int, default=256)
    parser.add_argument("--matrix-repeats", type=int, default=5)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--warmup", type=int, default=1)
    args = parser.parse_args()

    RESULT_DIR.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="spark-crosslang-generic-fastinit-") as tmp:
      workdir = pathlib.Path(tmp)
      src = write_sources(workdir, args.list_n, args.list_repeats, args.matrix_n, args.matrix_repeats)

      c_list_bin = workdir / "c_list_ops_bin"
      c_matrix_bin = workdir / "c_matrix_ops_bin"
      run_checked(["clang", "-O3", "-DNDEBUG", str(src["c_list"]), "-lm", "-o", str(c_list_bin)], REPO_ROOT, {})
      run_checked(["clang", "-O3", "-DNDEBUG", str(src["c_matrix"]), "-lm", "-o", str(c_matrix_bin)], REPO_ROOT, {})
      run_checked(["javac", str(src["java_list"]), str(src["java_matrix"])], REPO_ROOT, {})

      common_env = {
          "OPENBLAS_NUM_THREADS": "1",
          "OMP_NUM_THREADS": "1",
          "MKL_NUM_THREADS": "1",
          "VECLIB_MAXIMUM_THREADS": "1",
          "BLIS_NUM_THREADS": "1",
          "LC_ALL": "C",
      }

      cases = [
          ("list_ops", "Kozmika (fast-init)", ["./k", "run", "--interpret", str(src["spark_list"])],
           {**common_env, "SPARK_LIST_FILL_DENSE_ONLY": "1", "SPARK_LIST_OPS_DENSE_ONLY": "1"}),
          ("list_ops", "C (clang -O3)", [str(c_list_bin)], common_env),
          ("list_ops", "Python (naive)", ["python3", str(src["py_list"])], common_env),
          ("list_ops", "Python+NumPy", ["python3", str(src["np_list"])], common_env),
          ("list_ops", "Java (loops)", ["java", "-Duser.language=en", "-Duser.region=US", "-cp", str(workdir), "ListOpsBench"], common_env),
          ("matrix_ops", "Kozmika (fast-init)", ["./k", "run", "--interpret", str(src["spark_matrix"])],
           {**common_env, "SPARK_MATRIX_FILL_DENSE_ONLY": "1", "SPARK_MATRIX_OPS_DENSE_ONLY": "1"}),
          ("matrix_ops", "C (clang -O3)", [str(c_matrix_bin)], common_env),
          ("matrix_ops", "Python (naive)", ["python3", str(src["py_matrix"])], common_env),
          ("matrix_ops", "Python+NumPy", ["python3", str(src["np_matrix"])], common_env),
          ("matrix_ops", "Java (loops)", ["java", "-Duser.language=en", "-Duser.region=US", "-cp", str(workdir), "MatrixOpsBench"], common_env),
      ]

      results: List[ResultRow] = []
      for case, name, cmd, env in cases:
          median, min_v, max_v, checksum_raw, checksum = measure(cmd, env, args.warmup, args.runs)
          results.append(ResultRow(case=case, name=name, median_sec=median, min_sec=min_v, max_sec=max_v,
                                   runs=args.runs, checksum_raw=checksum_raw, checksum=checksum))

      for case_name in ("list_ops", "matrix_ops"):
          c_ref = next(r for r in results if r.case == case_name and r.name == "C (clang -O3)")
          for row in results:
              if row.case == case_name:
                  row.vs_c = c_ref.median_sec / row.median_sec if row.median_sec > 0 else None

      for row in results:
          ratio = f"{row.vs_c:.3f}x" if row.vs_c is not None else "n/a"
          print(
              f"{row.case} | {row.name}: median={row.median_sec:.6f}s "
              f"min={row.min_sec:.6f}s max={row.max_sec:.6f}s C/this={ratio} checksum={row.checksum_raw}"
          )

      stem = f"crosslang_generic_ops_fastinit_list{args.list_n}_mat{args.matrix_n}_r{args.runs}"
      json_path = RESULT_DIR / f"{stem}.json"
      csv_path = RESULT_DIR / f"{stem}.csv"
      payload = {
          "config": {
              "list_n": args.list_n,
              "list_repeats": args.list_repeats,
              "matrix_n": args.matrix_n,
              "matrix_repeats": args.matrix_repeats,
              "runs": args.runs,
              "warmup": args.warmup,
              "single_thread": True,
              "runtime_only": True,
              "spark_fast_init": True,
          },
          "results": [asdict(r) for r in results],
      }
      json_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")

      with csv_path.open("w", newline="", encoding="utf-8") as f:
          writer = csv.writer(f)
          writer.writerow([
              "case", "name", "median_sec", "min_sec", "max_sec", "runs",
              "checksum_raw", "checksum", "vs_c",
          ])
          for row in results:
              writer.writerow([
                  row.case,
                  row.name,
                  f"{row.median_sec:.9f}",
                  f"{row.min_sec:.9f}",
                  f"{row.max_sec:.9f}",
                  row.runs,
                  row.checksum_raw,
                  f"{row.checksum:.17g}",
                  f"{row.vs_c:.9f}" if row.vs_c is not None else "",
              ])

      print(f"results json: {json_path}")
      print(f"results csv: {csv_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
