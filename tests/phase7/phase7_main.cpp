#include "phase7_support.h"

int main() {
  phase7_test::run_phase7_list_tests();
  phase7_test::run_phase7_list_extreme_tests();
  phase7_test::run_phase7_matrix_tests();
  phase7_test::run_phase7_matrix_extreme_tests();
  phase7_test::run_phase7_analyze_tests();
  return 0;
}
