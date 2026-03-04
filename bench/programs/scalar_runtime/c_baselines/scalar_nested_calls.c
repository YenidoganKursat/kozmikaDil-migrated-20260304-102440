#include <stdio.h>

static long long plus(long long a, long long b) {
  return a + b;
}

static long long inc(long long v) {
  return plus(v, 1);
}

static long long repeat_fn(long long v) {
  return inc(inc(v));
}

int main(void) {
  printf("%lld\n", repeat_fn(6));
  return 0;
}
