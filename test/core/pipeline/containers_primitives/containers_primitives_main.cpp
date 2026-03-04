#include "containers_primitives_support.h"

namespace {

void verify_all() {
  containers_primitives_test::run_list_container_tests();
  containers_primitives_test::run_list_container_extreme_tests();
  containers_primitives_test::run_matrix_container_tests();
  containers_primitives_test::run_matrix_container_extreme_tests();
  containers_primitives_test::run_primitive_numeric_tests();
  containers_primitives_test::run_primitive_numeric_extreme_tests();
}

}  // namespace

int main() {
  verify_all();
  return 0;
}
