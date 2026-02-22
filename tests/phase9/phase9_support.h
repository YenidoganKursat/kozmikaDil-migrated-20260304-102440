#pragma once

#include <string_view>
#include <vector>

#include "spark/evaluator.h"
#include "spark/parser.h"
#include "spark/semantic.h"

namespace phase9_test {

spark::Value run_and_get(std::string_view source, std::string_view name);
double as_number(const spark::Value& value);
std::vector<double> as_number_list(const spark::Value& value);
std::string analyze_dump(std::string_view source, std::string_view which);
std::vector<std::string> analyze_diagnostics(std::string_view source);

void run_phase9_async_task_tests();
void run_phase9_async_task_extreme_tests();
void run_phase9_channel_stream_tests();
void run_phase9_channel_stream_extreme_tests();
void run_phase9_parallel_tests();
void run_phase9_parallel_extreme_tests();
void run_phase9_safety_diagnostics_tests();

}  // namespace phase9_test
