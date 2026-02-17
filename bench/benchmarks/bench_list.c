#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  const int n = 5000000;
  double* a = (double*)malloc((size_t)n * sizeof(double));
  if (!a) return 1;

  for (int i = 0; i < n; ++i) {
    a[i] = (double)i;
  }

  double sum = 0.0;
  for (int i = 0; i < n; ++i) {
    sum += a[i] * 1.0000001; // deterministic arithmetic path
  }

  double expected = 0.0;
  for (int i = 0; i < n; ++i) {
    expected += ((double)i) * 1.0000001;
  }

  const double eps = 1e-6;
  int ok = fabs(sum - expected) < eps;

  printf("benchmark=list_iteration\n");
  printf("checksum=%.6f\n", sum);
  printf("expected=%.6f\n", expected);
  printf("pass=%s\n", ok ? "PASS" : "FAIL");

  free(a);
  return ok ? 0 : 2;
}
