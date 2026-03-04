#include <stdio.h>
#include <stdlib.h>

int main(void) {
  const int rows = 256;
  const int cols = 256;
  long long sum = 0;
  const size_t count = (size_t)rows * (size_t)cols;
  long long* mat_a = (long long*)malloc(count * sizeof(long long));
  long long* mat_b = (long long*)malloc(count * sizeof(long long));
  if (!mat_a || !mat_b) {
    free(mat_a);
    free(mat_b);
    return 1;
  }

  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      mat_a[(size_t)r * (size_t)cols + (size_t)c] = (long long)((r * 31 + c * 17) % 97);
      mat_b[(size_t)r * (size_t)cols + (size_t)c] = (long long)((r * 19 + c * 7) % 101);
    }
  }

  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      const long long a = mat_a[(size_t)r * (size_t)cols + (size_t)c];
      const long long b = mat_b[(size_t)r * (size_t)cols + (size_t)c];
      sum += a * b + 1;
    }
  }

  free(mat_a);
  free(mat_b);
  const long long expected = 157362600LL;
  printf("%lld\n", sum);
  return (sum == expected) ? 0 : 1;
}
