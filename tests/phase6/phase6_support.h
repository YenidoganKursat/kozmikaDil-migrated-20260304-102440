#pragma once

#include <string_view>
#include <vector>

#include "spark/evaluator.h"
#include "spark/parser.h"
#include "spark/semantic.h"

namespace phase6_test {

spark::Value run_and_get(std::string_view source, std::string_view name);
double as_number(const spark::Value& value);
bool as_bool(const spark::Value& value);
std::vector<long long> as_int_list(const spark::Value& value);
void expect_type(std::string_view source, std::string_view name, const std::string& expected);

void run_list_phase6_tests();
void run_list_phase6_extreme_tests();
void run_matrix_phase6_tests();
void run_matrix_phase6_extreme_tests();

}  // namespace phase6_test
