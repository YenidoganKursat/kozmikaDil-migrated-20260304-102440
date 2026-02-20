from __future__ import annotations

import pathlib
import sys
import os
import subprocess
from typing import Dict, List

from crosslang.benchmark_core import run_checked

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]

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

