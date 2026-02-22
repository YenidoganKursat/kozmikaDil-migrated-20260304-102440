#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

#include "phase5_support.h"

namespace phase5_test {

spark::Value run_and_get(std::string_view source, std::string_view name) {
  spark::Interpreter interpreter;
  spark::Parser parser{std::string(source)};
  auto program = parser.parse_program();
  interpreter.run(*program);
  return interpreter.global(std::string(name));
}

void expect_global_int(std::string_view source, std::string_view name, long long expected) {
  const auto actual = run_and_get(source, name);
  if (actual.kind != spark::Value::Kind::Int) {
    std::fprintf(stderr,
                 "phase5 int assert failed: name=%.*s expected=%lld kind=%d value=%s\\nsource:\\n%.*s\\n",
                 static_cast<int>(name.size()), name.data(), expected,
                 static_cast<int>(actual.kind), actual.to_string().c_str(),
                 static_cast<int>(source.size()), source.data());
  }
  assert(actual.kind == spark::Value::Kind::Int);
  assert(actual.int_value == expected);
}

void expect_global_double(std::string_view source, std::string_view name, double expected) {
  const auto actual = run_and_get(source, name);
  assert(actual.kind == spark::Value::Kind::Double);
  assert(actual.double_value == expected);
}

void expect_global_list(std::string_view source, std::string_view name,
                       const std::vector<long long>& expected) {
  const auto actual = run_and_get(source, name);
  assert(actual.kind == spark::Value::Kind::List);
  assert(actual.list_value.size() == expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    assert(actual.list_value[i].kind == spark::Value::Kind::Int);
    assert(actual.list_value[i].int_value == expected[i]);
  }
}

void expect_global_list_double(std::string_view source, std::string_view name,
                              const std::vector<double>& expected) {
  const auto actual = run_and_get(source, name);
  assert(actual.kind == spark::Value::Kind::List);
  assert(actual.list_value.size() == expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    assert(actual.list_value[i].kind == spark::Value::Kind::Double ||
           actual.list_value[i].kind == spark::Value::Kind::Int);
    const double value = (actual.list_value[i].kind == spark::Value::Kind::Int)
                           ? static_cast<double>(actual.list_value[i].int_value)
                           : actual.list_value[i].double_value;
    assert(std::fabs(value - expected[i]) < 1e-12);
  }
}

void expect_global_list_string(std::string_view source, std::string_view name,
                               const std::vector<std::string>& expected) {
  const auto actual = run_and_get(source, name);
  assert(actual.kind == spark::Value::Kind::List);
  assert(actual.list_value.size() == expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    assert(actual.list_value[i].kind == spark::Value::Kind::String);
    assert(actual.list_value[i].string_value == expected[i]);
  }
}

void expect_global_matrix(std::string_view source, std::string_view name,
                         std::size_t rows, std::size_t cols,
                         const std::vector<long long>& flat_values) {
  const auto actual = run_and_get(source, name);
  assert(actual.kind == spark::Value::Kind::Matrix);
  assert(actual.matrix_value != nullptr);
  assert(actual.matrix_value->rows == rows);
  assert(actual.matrix_value->cols == cols);
  assert(actual.matrix_value->data.size() == flat_values.size());
  for (std::size_t i = 0; i < flat_values.size(); ++i) {
    assert(actual.matrix_value->data[i].kind == spark::Value::Kind::Int);
    assert(actual.matrix_value->data[i].int_value == flat_values[i]);
  }
}

void expect_global_matrix_double(std::string_view source, std::string_view name,
                               std::size_t rows, std::size_t cols,
                               const std::vector<double>& flat_values) {
  const auto actual = run_and_get(source, name);
  assert(actual.kind == spark::Value::Kind::Matrix);
  assert(actual.matrix_value != nullptr);
  assert(actual.matrix_value->rows == rows);
  assert(actual.matrix_value->cols == cols);
  assert(actual.matrix_value->data.size() == flat_values.size());
  for (std::size_t i = 0; i < flat_values.size(); ++i) {
    assert(actual.matrix_value->data[i].kind == spark::Value::Kind::Double ||
           actual.matrix_value->data[i].kind == spark::Value::Kind::Int);
    const double expected = flat_values[i];
    const double actual_value = (actual.matrix_value->data[i].kind == spark::Value::Kind::Int)
                                   ? static_cast<double>(actual.matrix_value->data[i].int_value)
                                   : actual.matrix_value->data[i].double_value;
    assert(std::fabs(actual_value - expected) < 1e-12);
  }
}

void expect_global_matrix_string(std::string_view source, std::string_view name,
                                 std::size_t rows, std::size_t cols,
                                 const std::vector<std::string>& flat_values) {
  const auto actual = run_and_get(source, name);
  assert(actual.kind == spark::Value::Kind::Matrix);
  assert(actual.matrix_value != nullptr);
  assert(actual.matrix_value->rows == rows);
  assert(actual.matrix_value->cols == cols);
  assert(actual.matrix_value->data.size() == flat_values.size());
  for (std::size_t i = 0; i < flat_values.size(); ++i) {
    assert(actual.matrix_value->data[i].kind == spark::Value::Kind::String);
    assert(actual.matrix_value->data[i].string_value == flat_values[i]);
  }
}

void expect_type(std::string_view source, std::string_view name, const std::string& expected) {
  spark::Parser parser{std::string(source)};
  auto program = parser.parse_program();
  spark::TypeChecker checker;
  checker.check(*program);
  assert(!checker.has_errors());

  const spark::SymbolRecord* found = nullptr;
  for (const auto& symbol : checker.symbols()) {
    if (symbol.name == name) {
      found = &symbol;
      break;
    }
  }
  assert(found != nullptr);
  assert(found->type == expected);
}

}  // namespace phase5_test
