#include "phase9_support.h"

int main() {
  phase9_test::run_phase9_async_task_tests();
  phase9_test::run_phase9_async_task_extreme_tests();
  phase9_test::run_phase9_channel_stream_tests();
  phase9_test::run_phase9_channel_stream_extreme_tests();
  phase9_test::run_phase9_parallel_tests();
  phase9_test::run_phase9_parallel_extreme_tests();
  phase9_test::run_phase9_safety_diagnostics_tests();
  return 0;
}
