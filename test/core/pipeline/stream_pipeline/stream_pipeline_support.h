#pragma once

#include <string_view>
#include <vector>

#include "spark/evaluator.h"
#include "spark/parser.h"
#include "spark/semantic.h"

namespace stream_pipeline_test {

spark::Value run_and_get(std::string_view source, std::string_view name);
double as_number(const spark::Value& value);
std::vector<double> as_number_list(const spark::Value& value);
std::string analyze_dump(std::string_view source, std::string_view which);

void run_stream_pipeline_list_tests();
void run_stream_pipeline_list_extreme_tests();
void run_stream_pipeline_matrix_tests();
void run_stream_pipeline_matrix_extreme_tests();
void run_stream_pipeline_analyze_tests();

}  // namespace stream_pipeline_test
