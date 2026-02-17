#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  const int n = 1536;
  const int size = n * n;
  const int repeats = 4;

  double* a = (double*)malloc((size_t)size * sizeof(double));
  double* b = (double*)malloc((size_t)size * sizeof(double));
  double* c = (double*)malloc((size_t)size * sizeof(double));
  if (!a || !b || !c) return 1;

  for (int i = 0; i < size; ++i) {
    a[i] = (double)(i % 97);
    b[i] = (double)((i * 7) % 101);
  }

  double checksum = 0.0;
  for (int r = 0; r < repeats; ++r) {
    for (int i = 0; i < size; ++i) {
      c[i] = a[i] + b[i] * 0.5;
    }

    for (int i = 0; i < size; ++i) {
      checksum += c[i];
    }
  }

  double expected = 0.0;
  for (int i = 0; i < size; ++i) {
    expected += ((double)(i % 97)) + ((double)((i * 7) % 101)) * 0.5;
  }
  expected *= (double)repeats;

  const double diff = fabs(checksum - expected);
  const int ok = diff < 1e-6;

  printf("benchmark=matrix_elemwise\n");
  printf("checksum=%.6f\n", checksum);
  printf("expected=%.6f\n", expected);
  printf("diff=%.12f\n", diff);
  printf("pass=%s\n", ok ? "PASS" : "FAIL");

  free(a);
  free(b);
  free(c);
  return ok ? 0 : 2;
}
