#include "phase5_support.h"

namespace {

void verify_all() {
  phase5_test::run_list_container_tests();
  phase5_test::run_matrix_container_tests();
  phase5_test::run_primitive_numeric_tests();
}

}  // namespace

int main() {
  verify_all();
  return 0;
}
