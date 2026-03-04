#include "hetero_runtime_support.h"

namespace {

void verify_all() {
  hetero_runtime_test::run_list_hetero_runtime_tests();
  hetero_runtime_test::run_list_hetero_runtime_extreme_tests();
  hetero_runtime_test::run_matrix_hetero_runtime_tests();
  hetero_runtime_test::run_matrix_hetero_runtime_extreme_tests();
}

}  // namespace

int main() {
  verify_all();
  return 0;
}
