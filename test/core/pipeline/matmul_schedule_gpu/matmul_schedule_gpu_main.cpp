#include "matmul_schedule_gpu_support.h"

int main() {
  matmul_schedule_gpu_test::run_matmul_schedule_gpu_matmul_tests();
  matmul_schedule_gpu_test::run_matmul_schedule_gpu_matmul_extreme_tests();
  matmul_schedule_gpu_test::run_matmul_schedule_gpu_analyze_tests();
  return 0;
}
