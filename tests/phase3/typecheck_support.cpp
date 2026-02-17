// Shared helper routines used by all phase3 typecheck tests.
// Keeping these helpers centralized avoids duplicated parser/checker setup per file.

#include <cassert>

#include "phase3/typecheck_support.h"

namespace phase3_test {

void expect_no_type_errors(const std::string& source) {
  spark::Parser parser(source);
  auto program = parser.parse_program();
  spark::TypeChecker checker;
  checker.check(*program);
  assert(!checker.has_errors());
}

void expect_type_errors(const std::string& source, std::size_t min_error_count) {
  spark::Parser parser(source);
  auto program = parser.parse_program();
  spark::TypeChecker checker;
  checker.check(*program);
  assert(checker.has_errors());
  assert(!checker.diagnostics().empty());
  assert(checker.diagnostics().size() >= min_error_count);
}

const spark::TierRecord* find_function_report(const spark::TypeChecker& checker, std::string_view name) {
  for (const auto& fn : checker.function_reports()) {
    if (fn.name == name && fn.kind == "function") {
      return &fn;
    }
  }
  return nullptr;
}

const spark::ShapeRecord* find_shape(const spark::TypeChecker& checker, std::string_view name) {
  for (const auto& shape : checker.shapes()) {
    if (shape.name == name) {
      return &shape;
    }
  }
  return nullptr;
}

const spark::SymbolRecord* find_symbol(const spark::TypeChecker& checker, std::string_view name,
                                      std::string_view owner) {
  for (const auto& symbol : checker.symbols()) {
    if (symbol.name == name && symbol.owner == owner) {
      return &symbol;
    }
  }
  return nullptr;
}

void expect_tier(const std::string& source, const std::string& function_name, spark::TierLevel expected_tier) {
  spark::Parser parser(source);
  auto program = parser.parse_program();
  spark::TypeChecker checker;
  checker.check(*program);
  assert(!checker.has_errors());
  const auto* rec = find_function_report(checker, function_name);
  assert(rec != nullptr);
  assert(rec->tier == expected_tier);
}

void expect_symbol_type(const std::string& source, std::string_view symbol_name, const std::string& expected_type,
                       std::string_view owner) {
  spark::Parser parser(source);
  auto program = parser.parse_program();
  spark::TypeChecker checker;
  checker.check(*program);
  assert(!checker.has_errors());
  const auto* record = find_symbol(checker, symbol_name, owner);
  assert(record != nullptr);
  assert(record->type == expected_type);
}

}  // namespace phase3_test
