#include <stdio.h>

int main(void) {
  long long y = 0;
  long long i = 0;
  while (i < 50000) {
    if ((i % 2 == 0) || (i % 3 == 0)) {
      y = y + 1;
    } else {
      y = y - 1;
    }
    i = i + 1;
  }
  printf("%lld\n", y);
  return 0;
}
