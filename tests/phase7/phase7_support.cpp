#include <cassert>
#include <string>

#include "phase7_support.h"

namespace phase7_test {

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

std::string analyze_dump(std::string_view source, std::string_view which) {
  spark::Parser parser{std::string(source)};
  auto program = parser.parse_program();
  spark::TypeChecker checker;
  checker.check(*program);

  if (which == "pipeline") {
    return checker.dump_pipeline_ir();
  }
  if (which == "fusion") {
    return checker.dump_fusion_plan();
  }
  if (which == "why") {
    return checker.dump_why_not_fused();
  }
  return "";
}

}  // namespace phase7_test
