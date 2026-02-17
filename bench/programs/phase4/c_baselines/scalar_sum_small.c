#include <stdio.h>

int main(void) {
  long long N = 50000;
  long long i = 0;
  long long total = 0;
  while (i < N) {
    total += i;
    i = i + 1;
  }
  printf("%lld\n", total);
  return 0;
}
