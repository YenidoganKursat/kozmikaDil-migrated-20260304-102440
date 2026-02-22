#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "spark/evaluator.h"
#include "spark/parser.h"
#include "spark/semantic.h"

namespace phase5_test {

spark::Value run_and_get(std::string_view source, std::string_view name);

void expect_global_int(std::string_view source, std::string_view name, long long expected);
void expect_global_double(std::string_view source, std::string_view name, double expected);
void expect_global_list(std::string_view source, std::string_view name, const std::vector<long long>& expected);
void expect_global_list_double(std::string_view source, std::string_view name, const std::vector<double>& expected);
void expect_global_list_string(std::string_view source, std::string_view name,
                               const std::vector<std::string>& expected);
void expect_global_matrix(std::string_view source, std::string_view name,
                         std::size_t rows, std::size_t cols,
                         const std::vector<long long>& flat_values);
void expect_global_matrix_double(std::string_view source, std::string_view name,
                                std::size_t rows, std::size_t cols,
                                const std::vector<double>& flat_values);
void expect_global_matrix_string(std::string_view source, std::string_view name,
                                 std::size_t rows, std::size_t cols,
                                 const std::vector<std::string>& flat_values);
void expect_type(std::string_view source, std::string_view name, const std::string& expected);

void run_list_container_tests();
void run_list_container_extreme_tests();
void run_matrix_container_tests();
void run_matrix_container_extreme_tests();
void run_primitive_numeric_tests();
void run_primitive_numeric_extreme_tests();

}  // namespace phase5_test
