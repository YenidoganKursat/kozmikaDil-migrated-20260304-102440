#include <cassert>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "phase5_support.h"

namespace phase5_test {
namespace {

spark::Interpreter run_program(std::string_view source) {
  spark::Parser parser{std::string(source)};
  auto program = parser.parse_program();
  spark::Interpreter interpreter;
  interpreter.run(*program);
  return interpreter;
}

double as_number(const spark::Value& value) {
  if (value.kind == spark::Value::Kind::Int) {
    return static_cast<double>(value.int_value);
  }
  if (value.kind == spark::Value::Kind::Double) {
    return value.double_value;
  }
  if (value.kind == spark::Value::Kind::Numeric) {
    return spark::Interpreter::to_number(value);
  }
  assert(false && "expected numeric value");
  return 0.0;
}

bool as_bool(const spark::Value& value) {
  assert(value.kind == spark::Value::Kind::Bool);
  return value.bool_value;
}

void assert_close(double actual, double expected, double tol, std::string_view context) {
  if (!std::isfinite(actual) || std::fabs(actual - expected) > tol) {
    std::fprintf(stderr, "primitive_extreme_assert_close_failed context=%.*s actual=%.17g expected=%.17g tol=%.17g\n",
                 static_cast<int>(context.size()), context.data(), actual, expected, tol);
    assert(false);
  }
}

void test_integer_primitive_family_core_ops() {
  const std::vector<std::string> kinds = {"i8", "i16", "i32", "i64", "i128", "i256", "i512"};
  for (const auto& kind : kinds) {
    std::ostringstream source;
    source << "a = " << kind << "(12)\n";
    source << "b = " << kind << "(3)\n";
    source << "add = a + b\n";
    source << "sub = a - b\n";
    source << "mul = a * b\n";
    source << "divv = a / b\n";
    source << "modv = a % b\n";
    source << "powv = " << kind << "(2) ^ " << kind << "(3)\n";
    source << "ok = add == " << kind << "(15) and sub == " << kind << "(9) and "
           << "mul == " << kind << "(36) and modv == " << kind << "(0) and "
           << "powv == " << kind << "(8)\n";

    auto interpreter = run_program(source.str());
    assert(as_bool(interpreter.global("ok")));
    assert_close(as_number(interpreter.global("divv")), 4.0, 1e-12, "int/div");
  }
}

void test_float_primitive_family_core_ops() {
  const std::vector<std::pair<std::string, double>> kinds = {
      {"f8", 2.0},     {"f16", 5e-2},  {"f32", 1e-5}, {"f64", 1e-12},
      {"f128", 1e-12}, {"f256", 1e-12}, {"f512", 1e-12},
  };
  for (const auto& [kind, tol] : kinds) {
    std::ostringstream source;
    source << "a = " << kind << "(1.5)\n";
    source << "b = " << kind << "(0.5)\n";
    source << "add = a + b\n";
    source << "sub = a - b\n";
    source << "mul = a * b\n";
    source << "divv = a / b\n";
    source << "modv = a % b\n";
    source << "powv = " << kind << "(2.0) ^ " << kind << "(3.0)\n";

    auto interpreter = run_program(source.str());
    const bool low_precision = (kind == "f8" || kind == "f16");
    const double add = as_number(interpreter.global("add"));
    const double sub = as_number(interpreter.global("sub"));
    const double mul = as_number(interpreter.global("mul"));
    const double divv = as_number(interpreter.global("divv"));
    const double modv = as_number(interpreter.global("modv"));
    const double powv = as_number(interpreter.global("powv"));

    if (low_precision) {
      assert(std::isfinite(add));
      assert(std::isfinite(sub));
      assert(std::isfinite(mul));
      assert(std::isfinite(divv));
      assert(std::isfinite(modv));
      assert(std::isfinite(powv));
      continue;
    }
    assert_close(add, 2.0, tol, "float/add");
    assert_close(sub, 1.0, tol, "float/sub");
    assert_close(mul, 0.75, tol * 2.0, "float/mul");
    assert_close(divv, 3.0, tol * 2.0, "float/div");
    assert_close(modv, 0.0, tol * 2.0, "float/mod");
    assert_close(powv, 8.0, tol * 4.0, "float/pow");
  }
}

void test_signed_mod_and_pow_edges_for_all_numeric_families() {
  const std::vector<std::pair<std::string, double>> kinds = {
      {"i8", 1e-12},  {"i16", 1e-12}, {"i32", 1e-12}, {"i64", 1e-12},
      {"i128", 1e-12}, {"i256", 1e-12}, {"i512", 1e-12}, {"f8", 4.0},
      {"f16", 5e-2},  {"f32", 1e-5}, {"f64", 1e-12}, {"f128", 1e-12},
      {"f256", 1e-12}, {"f512", 1e-12},
  };

  for (const auto& [kind, tol] : kinds) {
    std::ostringstream source;
    source << "a = " << kind << "(-9)\n";
    source << "b = " << kind << "(4)\n";
    source << "modv = a % b\n";
    source << "powv = b ^ " << kind << "(3)\n";
    source << "roundtrip = (a + b) - b\n";
    source << "divv = a / b\n";

    auto interpreter = run_program(source.str());
    const bool low_precision = (kind == "f8" || kind == "f16");
    const double roundtrip = as_number(interpreter.global("roundtrip"));
    const double modv = as_number(interpreter.global("modv"));
    const double powv = as_number(interpreter.global("powv"));
    const double divv = as_number(interpreter.global("divv"));
    const double b = as_number(interpreter.global("b"));

    if (low_precision) {
      assert(std::isfinite(roundtrip));
      assert(std::isfinite(modv));
      assert(std::isfinite(powv));
      assert(std::isfinite(divv));
      continue;
    }
    assert(std::isfinite(roundtrip));
    assert(std::isfinite(modv));
    assert(std::isfinite(powv));
    assert(std::isfinite(divv));
    assert(std::fabs(modv) < std::fabs(b) + tol * 16.0);
    assert(powv > 0.0);
    assert(std::isfinite(divv));
  }
}

void test_high_precision_delta_survival_vs_f64() {
  constexpr auto source = R"(
x64 = f64(1)
x512 = f512(1)
i = 0
while i < 220:
  x64 = x64 / f64(2)
  x512 = x512 / f512(2)
  i = i + 1
delta64 = (f64(1) + x64) - f64(1)
delta512 = (f512(1) + x512) - f512(1)
nz64 = delta64 != f64(0)
nz512 = delta512 != f512(0)
)";
  auto interpreter = run_program(source);
  assert(!as_bool(interpreter.global("nz64")));
  assert(as_bool(interpreter.global("nz512")));
}

}  // namespace

void run_primitive_numeric_extreme_tests() {
  test_integer_primitive_family_core_ops();
  test_float_primitive_family_core_ops();
  test_signed_mod_and_pow_edges_for_all_numeric_families();
  test_high_precision_delta_survival_vs_f64();
}

}  // namespace phase5_test
