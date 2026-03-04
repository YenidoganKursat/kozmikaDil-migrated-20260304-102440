#include <stdio.h>

int main(void) {
  long long x = 1;
  long long y = 2;
  long long i;
  for (i = 0; i < 4; ++i) {
    y = y + i;
  }
  if (x == 1) {
    y = y + 10;
  } else {
    y = y - 1;
  }
  printf("%lld\n", y);
  return 0;
}
