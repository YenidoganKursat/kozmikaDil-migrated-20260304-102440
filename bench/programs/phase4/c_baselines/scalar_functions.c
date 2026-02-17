#include <stdio.h>

static long long square(long long v) {
  return v * v;
}

static long long total_fn(long long n) {
  long long i = 0;
  long long acc = 0;
  while (i < n) {
    acc += square(i);
    i = i + 1;
  }
  return acc;
}

int main(void) {
  printf("%lld\n", total_fn(50000));
  return 0;
}
