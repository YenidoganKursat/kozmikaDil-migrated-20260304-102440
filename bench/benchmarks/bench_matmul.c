#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void fill(double* m, int n, int seed) {
  for (int i = 0; i < n * n; ++i) {
    m[i] = (double)((seed + i * 13) % 17) * 0.25;
  }
}

static void mul(const double* a, const double* b, double* c, int n) {
  for (int i = 0; i < n * n; ++i) {
    c[i] = 0.0;
  }

  for (int i = 0; i < n; ++i) {
    for (int k = 0; k < n; ++k) {
      const double aik = a[i * n + k];
      for (int j = 0; j < n; ++j) {
        c[i * n + j] += aik * b[k * n + j];
      }
    }
  }
}

static double checksum_ref(const double* a, const double* b, int n) {
  double expected = 0.0;
  for (int k = 0; k < n; ++k) {
    double col_sum = 0.0;
    double row_sum = 0.0;
    for (int i = 0; i < n; ++i) {
      col_sum += a[i * n + k];
    }
    for (int j = 0; j < n; ++j) {
      row_sum += b[k * n + j];
    }
    expected += col_sum * row_sum;
  }
  return expected;
}

int main(void) {
  const int n = 320;
  const int repeats = 4;
  const int size = n * n;

  double* a = (double*)malloc((size_t)size * sizeof(double));
  double* b = (double*)malloc((size_t)size * sizeof(double));
  double* c = (double*)malloc((size_t)size * sizeof(double));
  if (!a || !b || !c) return 1;

  fill(a, n, 3);
  fill(b, n, 7);

  const double single_expected = checksum_ref(a, b, n);
  double checksum = 0.0;

  for (int r = 0; r < repeats; ++r) {
    mul(a, b, c, n);

    for (int i = 0; i < size; ++i) {
      checksum += c[i];
    }
  }

  const double expected = single_expected * (double)repeats;
  const double diff = fabs(checksum - expected);
  const int ok = diff < 1e-9;

  printf("benchmark=matrix_matmul\n");
  printf("checksum=%.12f\n", checksum);
  printf("expected=%.12f\n", expected);
  printf("diff=%.12f\n", diff);
  printf("pass=%s\n", ok ? "PASS" : "FAIL");

  free(a);
  free(b);
  free(c);
  return ok ? 0 : 2;
}
