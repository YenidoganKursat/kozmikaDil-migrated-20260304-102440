#include <cassert>
#include <string>

#include "phase8_support.h"

namespace phase8_test {

spark::Value run_and_get(std::string_view source, std::string_view name) {
  spark::Interpreter interpreter;
  spark::Parser parser{std::string(source)};
  auto program = parser.parse_program();
  interpreter.run(*program);
  return interpreter.global(std::string(name));
}

double as_number(const spark::Value& value) {
  if (value.kind == spark::Value::Kind::Int) {
    return static_cast<double>(value.int_value);
  }
  if (value.kind == spark::Value::Kind::Double) {
    return value.double_value;
  }
  assert(false && "expected numeric value");
  return 0.0;
}

std::vector<double> as_number_list(const spark::Value& value) {
  assert(value.kind == spark::Value::Kind::List);
  std::vector<double> out;
  out.reserve(value.list_value.size());
  for (const auto& item : value.list_value) {
    out.push_back(as_number(item));
  }
  return out;
}

std::vector<double> as_matrix_flat(const spark::Value& value) {
  assert(value.kind == spark::Value::Kind::Matrix);
  assert(value.matrix_value != nullptr);
  std::vector<double> out;
  out.reserve(value.matrix_value->data.size());
  for (const auto& item : value.matrix_value->data) {
    out.push_back(as_number(item));
  }
  return out;
}

std::string analyze_dump(std::string_view source, std::string_view which) {
  spark::Parser parser{std::string(source)};
  auto program = parser.parse_program();
  spark::TypeChecker checker;
  checker.check(*program);

  if (which == "types") {
    return checker.dump_types();
  }
  if (which == "tiers") {
    return checker.dump_tier_report();
  }
  return "";
}

}  // namespace phase8_test
