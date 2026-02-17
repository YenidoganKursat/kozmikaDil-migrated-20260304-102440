#include <stdio.h>

int main(void) {
  long long acc = 0;
  long long i;
  for (i = 0; i < 10; ++i) {
    if (i < 3) {
      acc = acc + 1;
    } else if (i < 6) {
      acc = acc + 2;
    } else {
      acc = acc + 3;
    }
  }
  printf("%lld\n", acc);
  return 0;
}
