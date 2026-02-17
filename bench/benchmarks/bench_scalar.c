#include <math.h>
#include <stdio.h>

int main(void) {
  const int n = 80000000;
  double sum = 0.0;

  for (int i = 0; i < n; ++i) {
    sum += (double)i * 0.5;
  }

  const double expected = ((double)n * (double)(n - 1)) * 0.25;
  const double eps = 1e-6;
  const int ok = fabs(sum - expected) < eps;

  printf("benchmark=scalar\n");
  printf("checksum=%.12f\n", sum);
  printf("expected=%.12f\n", expected);
  printf("pass=%s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 2;
}
