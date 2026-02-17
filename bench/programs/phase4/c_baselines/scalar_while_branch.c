#include <stdio.h>

int main(void) {
  long long x = 0;
  long long acc = 0;
  while (x < 50000) {
    if (x < 7) {
      acc = acc + x;
    } else if (x < 14) {
      acc = acc - 2;
    } else {
      acc = acc + 3;
    }
    x = x + 1;
  }
  printf("%lld\n", acc);
  return 0;
}
