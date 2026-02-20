#include <cassert>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <random>

#include "phase5_support.h"

namespace phase5_test {
namespace {

using Op = spark::BinaryOp;

std::size_t sample_count_from_env() {
  if (const char* raw = std::getenv("SPARK_PRIMITIVE_FUZZ_SAMPLES"); raw != nullptr) {
    const long long parsed = std::atoll(raw);
    if (parsed > 0) {
      return static_cast<std::size_t>(parsed);
    }
  }
  return 10000;
}

double eval_number(spark::Interpreter& interpreter, Op op, const spark::Value& lhs, const spark::Value& rhs) {
  const auto out = interpreter.eval_binary(op, lhs, rhs);
  return spark::Interpreter::to_number(out);
}

void run_int_numeric_invariants(spark::Interpreter& interpreter, std::size_t samples) {
  std::mt19937_64 rng(0xA11CEB00ULL);
  std::uniform_int_distribution<long long> dist(-100000, 100000);

  for (std::size_t i = 0; i < samples; ++i) {
    const long long x = dist(rng);
    long long y = dist(rng);
    if (y == 0) {
      y = 1;
    }

    const auto vx = spark::Value::int_value_of(x);
    const auto vy = spark::Value::int_value_of(y);
    const auto vz = spark::Value::int_value_of(0);
    const auto v1 = spark::Value::int_value_of(1);

    const double add_xy = eval_number(interpreter, Op::Add, vx, vy);
    const double add_yx = eval_number(interpreter, Op::Add, vy, vx);
    const double mul_xy = eval_number(interpreter, Op::Mul, vx, vy);
    const double mul_yx = eval_number(interpreter, Op::Mul, vy, vx);
    const double sub_xy = eval_number(interpreter, Op::Sub, vx, vy);
    const double sub_yx = eval_number(interpreter, Op::Sub, vy, vx);
    const double div_xy = eval_number(interpreter, Op::Div, vx, vy);
    const double mod_xy = eval_number(interpreter, Op::Mod, vx, vy);
    const double mul_x1 = eval_number(interpreter, Op::Mul, vx, v1);
    const double add_x0 = eval_number(interpreter, Op::Add, vx, vz);

    assert(std::isfinite(add_xy));
    assert(std::isfinite(mul_xy));
    assert(std::isfinite(sub_xy));
    assert(std::isfinite(div_xy));
    assert(std::isfinite(mod_xy));

    // Algebraic identities/invariants.
    assert(std::fabs(add_xy - add_yx) <= 1e-12);
    assert(std::fabs(mul_xy - mul_yx) <= 1e-12);
    assert(std::fabs(sub_xy + sub_yx) <= 1e-12);
    assert(std::fabs(mul_x1 - static_cast<double>(x)) <= 1e-12);
    assert(std::fabs(add_x0 - static_cast<double>(x)) <= 1e-12);
    assert(std::fabs(mod_xy) < std::fabs(static_cast<double>(y)) + 1e-12);
  }
}

void run_double_numeric_invariants(spark::Interpreter& interpreter, std::size_t samples) {
  std::mt19937_64 rng(0xBEEF1234ULL);
  std::uniform_real_distribution<double> dist(-100.0, 100.0);

  for (std::size_t i = 0; i < samples; ++i) {
    const double x = dist(rng);
    double y = dist(rng);
    if (std::fabs(y) < 1e-9) {
      y = (y < 0.0) ? -0.5 : 0.5;
    }

    const auto vx = spark::Value::double_value_of(x);
    const auto vy = spark::Value::double_value_of(y);
    const auto vz = spark::Value::double_value_of(0.0);
    const auto v1 = spark::Value::double_value_of(1.0);

    const double add_xy = eval_number(interpreter, Op::Add, vx, vy);
    const double add_yx = eval_number(interpreter, Op::Add, vy, vx);
    const double mul_xy = eval_number(interpreter, Op::Mul, vx, vy);
    const double mul_yx = eval_number(interpreter, Op::Mul, vy, vx);
    const double sub_xy = eval_number(interpreter, Op::Sub, vx, vy);
    const double sub_yx = eval_number(interpreter, Op::Sub, vy, vx);
    const double div_xy = eval_number(interpreter, Op::Div, vx, vy);
    const double mul_x1 = eval_number(interpreter, Op::Mul, vx, v1);
    const double add_x0 = eval_number(interpreter, Op::Add, vx, vz);

    assert(std::isfinite(add_xy));
    assert(std::isfinite(mul_xy));
    assert(std::isfinite(sub_xy));
    assert(std::isfinite(div_xy));

    const double scale_add = std::max(1.0, std::fabs(add_xy));
    const double scale_mul = std::max(1.0, std::fabs(mul_xy));
    const double scale_sub = std::max(1.0, std::fabs(sub_xy));
    const double scale_x = std::max(1.0, std::fabs(x));

    assert(std::fabs(add_xy - add_yx) <= 1e-12 * scale_add);
    assert(std::fabs(mul_xy - mul_yx) <= 1e-12 * scale_mul);
    assert(std::fabs(sub_xy + sub_yx) <= 1e-12 * scale_sub);
    assert(std::fabs(mul_x1 - x) <= 1e-12 * scale_x);
    assert(std::fabs(add_x0 - x) <= 1e-12 * scale_x);
  }
}

void run_edge_case_sanity(spark::Interpreter& interpreter) {
  const auto i0 = spark::Value::int_value_of(0);
  const auto i1 = spark::Value::int_value_of(1);
  const auto im1 = spark::Value::int_value_of(-1);

  assert(eval_number(interpreter, Op::Add, i0, i0) == 0.0);
  assert(eval_number(interpreter, Op::Div, i1, i1) == 1.0);
  assert(eval_number(interpreter, Op::Mod, im1, i1) == 0.0);

  const auto d0 = spark::Value::double_value_of(0.0);
  const auto d1 = spark::Value::double_value_of(1.0);

  assert(eval_number(interpreter, Op::Add, d0, d0) == 0.0);
  assert(eval_number(interpreter, Op::Div, d1, d1) == 1.0);
}

}  // namespace

void run_primitive_numeric_tests() {
  spark::Interpreter interpreter;
  const auto samples = sample_count_from_env();
  run_edge_case_sanity(interpreter);
  run_int_numeric_invariants(interpreter, samples);
  run_double_numeric_invariants(interpreter, samples);
}

}  // namespace phase5_test
