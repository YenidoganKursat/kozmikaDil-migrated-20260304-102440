#include <stdio.h>

static long long inc(long long v) {
  return v + 1;
}

static long long dbl(long long v) {
  return inc(v) * 2;
}

static long long quad(long long v) {
  return dbl(v) + dbl(v);
}

int main(void) {
  long long N = 50000;
  long long acc = 0;
  long long i = 0;
  while (i < N) {
    acc = acc + quad(i);
    i = i + 1;
  }
  printf("%lld\n", acc);
  return 0;
}
