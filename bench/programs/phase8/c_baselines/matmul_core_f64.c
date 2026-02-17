#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void fill(double* a, double* b, int n) {
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      a[i * n + j] = (double)((i * 17 + j * 3) % 19) * 0.125;
      b[i * n + j] = (double)((i * 11 + j * 5) % 23) * 0.1;
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

int main(void) {
  const int n = 128;
  const int repeats = 3;
  const int size = n * n;

  double* a = (double*)malloc((size_t)size * sizeof(double));
  double* b = (double*)malloc((size_t)size * sizeof(double));
  double* c = (double*)malloc((size_t)size * sizeof(double));
  if (!a || !b || !c) return 1;

  fill(a, b, n);

  const double expected_single = checksum_ref(a, b, n);
  double total = 0.0;
  for (int r = 0; r < repeats; ++r) {
    mul(a, b, c, n);
    for (int i = 0; i < size; ++i) {
      total += c[i];
    }
  }

  const double expected = expected_single * (double)repeats;
  const double diff = fabs(total - expected);
  const int ok = diff < 1e-6;

  printf("total=%.12f\n", total);
  printf("expected=%.12f\n", expected);
  printf("diff=%.12f\n", diff);
  printf("pass=%s\n", ok ? "PASS" : "FAIL");

  free(a);
  free(b);
  free(c);
  return ok ? 0 : 2;
}
