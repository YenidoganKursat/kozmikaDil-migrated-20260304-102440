#include <stdio.h>

int main(void) {
  long long acc = 0;
  long long v;
  for (v = 0; v < 50000; ++v) {
    if ((v % 2) == 0) {
      acc = acc + v;
    } else {
      acc = acc + 2;
    }
  }
  printf("%lld\n", acc);
  return 0;
}
