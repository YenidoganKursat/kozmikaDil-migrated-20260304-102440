#include "scalar_runtime/runtime/ops/runtime_ops.h"

#include <cmath>
#include <limits>
#include <optional>

namespace spark::runtime_ops::controllers {

namespace {

std::optional<long long> scalar_integral_pow_exponent_controller(const double value) {
  if (!std::isfinite(value)) {
    return std::nullopt;
  }
  const double rounded = std::nearbyint(value);
  if (std::fabs(value - rounded) > 1e-12) {
    return std::nullopt;
  }
  if (std::fabs(rounded) > 1'000'000.0) {
    return std::nullopt;
  }
  return static_cast<long long>(rounded);
}

double scalar_powi_double_controller(double base, long long exponent) {
  if (exponent == 0) {
    return 1.0;
  }
  if (base == 0.0 && exponent < 0) {
    return std::numeric_limits<double>::infinity();
  }
  const bool negative = exponent < 0;
  unsigned long long n = static_cast<unsigned long long>(negative ? -exponent : exponent);
  double result = 1.0;
  double factor = base;
  while (n > 0ULL) {
    if ((n & 1ULL) != 0ULL) {
      result *= factor;
    }
    n >>= 1ULL;
    if (n > 0ULL) {
      factor *= factor;
    }
  }
  return negative ? (1.0 / result) : result;
}

double scalar_as_double_fast_controller(const Value& value) {
  if (value.kind == Value::Kind::Double) {
    return value.double_value;
  }
  if (value.kind == Value::Kind::Int) {
    return static_cast<double>(value.int_value);
  }
  return runtime_ops::to_number(value);
}

}  // namespace

Value eval_binary_scalar_controller(const BinaryOp op, const Value& left, const Value& right) {
  const bool lhs_is_int = left.kind == Value::Kind::Int;
  const bool rhs_is_int = right.kind == Value::Kind::Int;

  if (lhs_is_int && rhs_is_int) {
    const long long lhs_i = left.int_value;
    const long long rhs_i = right.int_value;
    switch (op) {
      case BinaryOp::Add:
        return Value::int_value_of(lhs_i + rhs_i);
      case BinaryOp::Sub:
        return Value::int_value_of(lhs_i - rhs_i);
      case BinaryOp::Mul:
        return Value::int_value_of(lhs_i * rhs_i);
      case BinaryOp::Div:
        if (rhs_i == 0) {
          throw EvalException("division by zero");
        }
        return Value::double_value_of(static_cast<double>(lhs_i) / static_cast<double>(rhs_i));
      case BinaryOp::Mod:
        if (rhs_i == 0) {
          throw EvalException("modulo by zero");
        }
        return Value::int_value_of(lhs_i % rhs_i);
      case BinaryOp::Pow:
        return Value::double_value_of(
            scalar_powi_double_controller(static_cast<double>(lhs_i), rhs_i));
      case BinaryOp::Lt:
        return Value::bool_value_of(lhs_i < rhs_i);
      case BinaryOp::Lte:
        return Value::bool_value_of(lhs_i <= rhs_i);
      case BinaryOp::Gt:
        return Value::bool_value_of(lhs_i > rhs_i);
      case BinaryOp::Gte:
        return Value::bool_value_of(lhs_i >= rhs_i);
      case BinaryOp::Eq:
        return Value::bool_value_of(lhs_i == rhs_i);
      case BinaryOp::Ne:
        return Value::bool_value_of(lhs_i != rhs_i);
      case BinaryOp::And:
      case BinaryOp::Or:
        break;
    }
  }

  const double lhs = scalar_as_double_fast_controller(left);
  const double rhs = scalar_as_double_fast_controller(right);

  switch (op) {
    case BinaryOp::Add:
      return Value::double_value_of(lhs + rhs);
    case BinaryOp::Sub:
      return Value::double_value_of(lhs - rhs);
    case BinaryOp::Mul:
      return Value::double_value_of(lhs * rhs);
    case BinaryOp::Div:
      if (rhs == 0.0) {
        throw EvalException("division by zero");
      }
      return Value::double_value_of(lhs / rhs);
    case BinaryOp::Mod:
      throw EvalException("modulo expects integer values");
    case BinaryOp::Pow:
      if (const auto integral_exp = scalar_integral_pow_exponent_controller(rhs); integral_exp.has_value()) {
        return Value::double_value_of(scalar_powi_double_controller(lhs, *integral_exp));
      }
      return Value::double_value_of(std::pow(lhs, rhs));
    case BinaryOp::Lt:
      return Value::bool_value_of(lhs < rhs);
    case BinaryOp::Lte:
      return Value::bool_value_of(lhs <= rhs);
    case BinaryOp::Gt:
      return Value::bool_value_of(lhs > rhs);
    case BinaryOp::Gte:
      return Value::bool_value_of(lhs >= rhs);
    case BinaryOp::Eq:
      return Value::bool_value_of(lhs == rhs);
    case BinaryOp::Ne:
      return Value::bool_value_of(lhs != rhs);
    case BinaryOp::And:
    case BinaryOp::Or:
      break;
  }

  throw EvalException("unsupported binary operator");
}

}  // namespace spark::runtime_ops::controllers
