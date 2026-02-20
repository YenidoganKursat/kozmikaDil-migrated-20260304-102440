from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
import os
from typing import Dict, List

from crosslang.benchmark_core import run_checked

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]


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
