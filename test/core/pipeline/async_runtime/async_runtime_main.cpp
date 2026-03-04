#include "async_runtime_support.h"

int main() {
  async_runtime_test::run_async_runtime_async_task_tests();
  async_runtime_test::run_async_runtime_async_task_extreme_tests();
  async_runtime_test::run_async_runtime_channel_stream_tests();
  async_runtime_test::run_async_runtime_channel_stream_extreme_tests();
  async_runtime_test::run_async_runtime_parallel_tests();
  async_runtime_test::run_async_runtime_parallel_extreme_tests();
  async_runtime_test::run_async_runtime_safety_diagnostics_tests();
  return 0;
}
