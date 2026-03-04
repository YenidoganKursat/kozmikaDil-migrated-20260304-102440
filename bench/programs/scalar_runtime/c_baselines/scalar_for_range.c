#include <stdio.h>

int main(void) {
  long long v;
  long long total = 0;
  for (v = 0; v < 50000; ++v) {
    total += v * v;
  }
  printf("%lld\n", total);
  return 0;
}
