#include "phase8_support.h"

int main() {
  phase8_test::run_phase8_matmul_tests();
  phase8_test::run_phase8_matmul_extreme_tests();
  phase8_test::run_phase8_analyze_tests();
  return 0;
}
