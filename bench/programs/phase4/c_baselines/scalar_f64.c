#include <stdio.h>

int main(void) {
  long long i = 0;
  double acc = 0.0;
  while (i < 50000) {
    acc = acc + i + 0.75;
    i = i + 1;
  }
  printf("%.15g\n", acc);
  return 0;
}
