// Phase 3 type-check test helpers.
// Keep shared helpers in one place so each test file stays small and manageable.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "spark/parser.h"
#include "spark/semantic.h"

namespace phase3_test {

void expect_no_type_errors(const std::string& source);
void expect_type_errors(const std::string& source, std::size_t min_error_count);
const spark::TierRecord* find_function_report(const spark::TypeChecker& checker, std::string_view name);
const spark::ShapeRecord* find_shape(const spark::TypeChecker& checker, std::string_view name);
const spark::SymbolRecord* find_symbol(const spark::TypeChecker& checker, std::string_view name,
                                      std::string_view owner = "global");
void expect_tier(const std::string& source, const std::string& function_name, spark::TierLevel expected_tier);
void expect_symbol_type(const std::string& source, std::string_view symbol_name, const std::string& expected_type,
                       std::string_view owner = "global");

void run_core_typecheck_tests();
void run_tier_classification_tests();
void run_inference_shape_tests();

}  // namespace phase3_test
