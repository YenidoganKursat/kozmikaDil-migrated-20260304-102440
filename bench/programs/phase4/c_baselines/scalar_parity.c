#include <stdio.h>

int main(void) {
  long long x = 50001;
  long long y = 0;
  while (x > 0) {
    if ((x % 2) == 0) {
      y = y + 1;
    } else {
      y = y - 1;
    }
    x = x - 1;
  }
  printf("%lld\n", y);
  return 0;
}
