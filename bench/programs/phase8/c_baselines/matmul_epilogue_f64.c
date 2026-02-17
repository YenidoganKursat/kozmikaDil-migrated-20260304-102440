#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void fill(double* a, double* b, double* bias, double* acc, int n) {
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      a[i * n + j] = (double)((i * 13 + j * 7) % 31) * 0.05;
      b[i * n + j] = (double)((i * 19 + j * 11) % 29) * 0.04;
      acc[i * n + j] = (double)((i + j) % 9) * 0.2;
    }
    bias[i] = (double)(i % 7) * 0.3;
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
  const int n = 96;
  const int repeats = 2;
  const int size = n * n;

  double* a = (double*)malloc((size_t)size * sizeof(double));
  double* b = (double*)malloc((size_t)size * sizeof(double));
  double* c = (double*)malloc((size_t)size * sizeof(double));
  double* bias = (double*)malloc((size_t)n * sizeof(double));
  double* acc = (double*)malloc((size_t)size * sizeof(double));
  if (!a || !b || !c || !bias || !acc) return 1;

  fill(a, b, bias, acc, n);

  const double base = checksum_ref(a, b, n);
  double bias_sum = 0.0;
  for (int i = 0; i < n; ++i) {
    bias_sum += bias[i];
  }
  double acc_sum = 0.0;
  for (int i = 0; i < size; ++i) {
    acc_sum += acc[i];
  }

  double total = 0.0;
  for (int r = 0; r < repeats; ++r) {
    mul(a, b, c, n);
    for (int i = 0; i < n; ++i) {
      for (int j = 0; j < n; ++j) {
        const double base_cell = c[i * n + j];
        const double c1 = base_cell + bias[j];
        const double c2 = 1.5 * base_cell + 0.25 * acc[i * n + j];
        total += c1 + c2;
      }
    }
  }

  const double c1_expected = base + (double)n * bias_sum;
  const double c2_expected = 1.5 * base + 0.25 * acc_sum;
  const double expected = (c1_expected + c2_expected) * (double)repeats;
  const double diff = fabs(total - expected);
  const int ok = diff < 1e-6;

  printf("total=%.12f\n", total);
  printf("expected=%.12f\n", expected);
  printf("diff=%.12f\n", diff);
  printf("pass=%s\n", ok ? "PASS" : "FAIL");

  free(a);
  free(b);
  free(c);
  free(bias);
  free(acc);
  return ok ? 0 : 2;
}
