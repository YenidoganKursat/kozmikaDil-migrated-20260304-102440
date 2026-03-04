#include "stream_pipeline_support.h"

int main() {
  stream_pipeline_test::run_stream_pipeline_list_tests();
  stream_pipeline_test::run_stream_pipeline_list_extreme_tests();
  stream_pipeline_test::run_stream_pipeline_matrix_tests();
  stream_pipeline_test::run_stream_pipeline_matrix_extreme_tests();
  stream_pipeline_test::run_stream_pipeline_analyze_tests();
  return 0;
}
