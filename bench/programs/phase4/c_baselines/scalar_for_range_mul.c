#include <stdio.h>

int main(void) {
  long long acc = 0;
  long long v;
  for (v = 0; v < 300; ++v) {
    acc = acc + v * 3;
  }
  printf("%lld\n", acc);
  return 0;
}
