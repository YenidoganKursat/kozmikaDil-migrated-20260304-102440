#include <cassert>
#include <cmath>
#include <string>

#include "phase6_support.h"

namespace phase6_test {

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

bool as_bool(const spark::Value& value) {
  assert(value.kind == spark::Value::Kind::Bool);
  return value.bool_value;
}

std::vector<long long> as_int_list(const spark::Value& value) {
  assert(value.kind == spark::Value::Kind::List);
  std::vector<long long> out;
  out.reserve(value.list_value.size());
  for (const auto& item : value.list_value) {
    assert(item.kind == spark::Value::Kind::Int);
    out.push_back(item.int_value);
  }
  return out;
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

}  // namespace phase6_test
