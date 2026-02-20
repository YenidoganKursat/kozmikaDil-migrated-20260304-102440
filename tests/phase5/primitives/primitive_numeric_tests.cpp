#include <cassert>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "phase5_support.h"

namespace phase5_test {
namespace {

using K = spark::Value::NumericKind;
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

bool is_int_kind(K kind) {
  switch (kind) {
    case K::I8:
    case K::I16:
    case K::I32:
    case K::I64:
    case K::I128:
    case K::I256:
    case K::I512:
      return true;
    default:
      return false;
  }
}

double rel_tol_for_kind(K kind) {
  switch (kind) {
    case K::F8:
      return 2.0e-1;
    case K::F16:
    case K::BF16:
      return 5.0e-3;
    case K::F32:
      return 2.0e-6;
    case K::F64:
      return 1.0e-12;
    case K::F128:
    case K::F256:
    case K::F512:
      return 1.0e-12;
    default:
      return 1.0e-6;
  }
}

spark::Value make_value(K kind, long long iv, long double fv) {
  if (is_int_kind(kind)) {
    return spark::Value::numeric_int_value_of(kind, static_cast<__int128_t>(iv));
  }
  return spark::Value::numeric_float_value_of(kind, fv);
}

double eval_number(spark::Interpreter& interpreter, Op op, const spark::Value& lhs, const spark::Value& rhs) {
  const auto out = interpreter.eval_binary(op, lhs, rhs);
  return spark::Interpreter::to_number(out);
}

void run_int_kind_tests(spark::Interpreter& interpreter, K kind, std::size_t samples) {
  std::mt19937_64 rng(0x5A17D3ULL + static_cast<unsigned>(kind));
  std::uniform_int_distribution<long long> dist(-1000, 1000);

  for (std::size_t i = 0; i < samples; ++i) {
    long long x = dist(rng);
    long long y = dist(rng);
    if (y == 0) {
      y = 1;
    }

    const auto vx = make_value(kind, x, static_cast<long double>(x));
    const auto vy = make_value(kind, y, static_cast<long double>(y));

    const double add = eval_number(interpreter, Op::Add, vx, vy);
    const double sub = eval_number(interpreter, Op::Sub, vx, vy);
    const double mul = eval_number(interpreter, Op::Mul, vx, vy);
    const double div = eval_number(interpreter, Op::Div, vx, vy);
    const double mod = eval_number(interpreter, Op::Mod, vx, vy);

    assert(std::isfinite(add));
    assert(std::isfinite(sub));
    assert(std::isfinite(mul));
    assert(std::isfinite(div));
    assert(std::isfinite(mod));

    // Integer modulo invariant: |x % y| < |y| (unless y == 0, already guarded).
    assert(std::fabs(mod) < std::fabs(static_cast<double>(y)) + 1e-12);
  }
}

void run_float_kind_tests(spark::Interpreter& interpreter, K kind, std::size_t samples) {
  std::mt19937_64 rng(0xC0FFEEULL + static_cast<unsigned>(kind));
  std::uniform_real_distribution<long double> dist(-100.0L, 100.0L);
  std::uniform_int_distribution<int> exp_dist(-4, 4);

  const double rel_tol = rel_tol_for_kind(kind);

  for (std::size_t i = 0; i < samples; ++i) {
    long double x = dist(rng);
    long double y = dist(rng);
    if (std::fabsl(y) < 1e-12L) {
      y = 0.5L;
    }
    const int p = exp_dist(rng);

    const auto vx = make_value(kind, static_cast<long long>(x), x);
    const auto vy = make_value(kind, static_cast<long long>(y), y);
    const auto vp = make_value(kind, p, static_cast<long double>(p));
    const auto one = make_value(kind, 1, 1.0L);

    const double add = eval_number(interpreter, Op::Add, vx, vy);
    const double sub = eval_number(interpreter, Op::Sub, vx, vy);
    const double mul = eval_number(interpreter, Op::Mul, vx, vy);
    const double div = eval_number(interpreter, Op::Div, vx, vy);
    const double mod = eval_number(interpreter, Op::Mod, vx, vy);
    const double pow = eval_number(interpreter, Op::Pow, vx, vp);
    const double self_mul = eval_number(interpreter, Op::Mul, vx, one);

    assert(std::isfinite(add));
    assert(std::isfinite(sub));
    assert(std::isfinite(mul));
    assert(std::isfinite(div));
    assert(std::isfinite(mod));
    // pow can overflow to +/-inf for low precision kinds on extreme samples;
    // NaN indicates an invalid arithmetic path for these generated cases.
    assert(!std::isnan(pow));

    // x * 1 == x (under target primitive rounding).
    const double nx = spark::Interpreter::to_number(vx);
    assert(std::fabs(self_mul - nx) <= rel_tol * std::max(1.0, std::fabs(nx)));

    // Modulo invariant for non-zero divisor.
    const double ny = spark::Interpreter::to_number(vy);
    assert(std::fabs(mod) < std::fabs(ny) + rel_tol * std::max(1.0, std::fabs(ny)));
  }
}

}  // namespace

void run_primitive_numeric_tests() {
  spark::Interpreter interpreter;
  const auto samples = sample_count_from_env();

  const std::vector<K> int_kinds = {K::I8, K::I16, K::I32, K::I64, K::I128, K::I256, K::I512};
  const std::vector<K> float_kinds = {K::F8, K::F16, K::BF16, K::F32, K::F64, K::F128, K::F256, K::F512};

  for (const auto kind : int_kinds) {
    run_int_kind_tests(interpreter, kind, samples);
  }
  for (const auto kind : float_kinds) {
    run_float_kind_tests(interpreter, kind, samples);
  }
}

}  // namespace phase5_test
