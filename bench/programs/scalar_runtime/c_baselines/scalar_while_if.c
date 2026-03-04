#include <stdio.h>

int main(void) {
  long long x = 50000;
  long long acc = 0;
  while (x > 0) {
    if ((x % 2) == 0) {
      acc = acc + 1;
    }
    x = x - 1;
  }
  printf("%lld\n", acc);
  return 0;
}
