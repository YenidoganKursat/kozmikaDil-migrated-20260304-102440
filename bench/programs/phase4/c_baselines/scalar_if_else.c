#include <stdio.h>

int main(void) {
  long long z = 0;
  long long v = 0;
  while (v < 50000) {
    if (v > 1) {
      z = 10;
    } else {
      if (v == 1) {
        z = 7;
      } else {
        z = 99;
      }
    }
    v = v + 1;
  }
  printf("%lld\n", z);
  return 0;
}
