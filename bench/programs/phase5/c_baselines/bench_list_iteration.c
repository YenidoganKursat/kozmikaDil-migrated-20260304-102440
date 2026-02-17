#include <stdio.h>
#include <stdlib.h>

int main(void) {
  const int n = 400000;
  long long sum = 0;
  int capacity = n;
  int size = 0;
  long long* values = (long long*)malloc((size_t)capacity * sizeof(long long));
  if (!values) {
    return 1;
  }
  int i = 0;

  while (i < n) {
    values[size++] = i;
    ++i;
  }

  for (i = 0; i < size; ++i) {
    sum += values[i];
  }

  free(values);
  const long long expected = 79999800000LL;
  printf("%lld\n", sum);
  return (sum == expected) ? 0 : 1;
}
