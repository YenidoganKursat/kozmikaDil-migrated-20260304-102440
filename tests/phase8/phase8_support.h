#pragma once

#include <string_view>
#include <vector>

#include "spark/evaluator.h"
#include "spark/parser.h"
#include "spark/semantic.h"

namespace phase8_test {

spark::Value run_and_get(std::string_view source, std::string_view name);
double as_number(const spark::Value& value);
std::vector<double> as_number_list(const spark::Value& value);
std::vector<double> as_matrix_flat(const spark::Value& value);
std::string analyze_dump(std::string_view source, std::string_view which);

void run_phase8_matmul_tests();
void run_phase8_matmul_extreme_tests();
void run_phase8_analyze_tests();

}  // namespace phase8_test
