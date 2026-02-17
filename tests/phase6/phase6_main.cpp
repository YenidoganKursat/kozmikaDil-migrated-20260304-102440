#include "phase6_support.h"

namespace {

void verify_all() {
  phase6_test::run_list_phase6_tests();
  phase6_test::run_matrix_phase6_tests();
}

}  // namespace

int main() {
  verify_all();
  return 0;
}
