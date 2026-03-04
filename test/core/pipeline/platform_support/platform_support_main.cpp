#include "platform_support_suite.h"

int main() {
  platform_support_test::run_platform_support_dispatch_tests();
  platform_support_test::run_platform_support_dispatch_extreme_tests();
  platform_support_test::run_platform_support_core_architecture_tests();
  platform_support_test::run_platform_support_platform_support_tests();
  return 0;
}
