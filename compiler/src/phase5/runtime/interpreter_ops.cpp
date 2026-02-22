#include "phase5/runtime/ops/runtime_ops.h"

#include <cmath>
#include <limits>
#include <optional>

#include "phase3/evaluator_parts/internal_helpers.h"

namespace spark {

namespace {

std::optional<long long> integral_pow_exponent(double value) {
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

double powi_double(double base, long long exponent) {
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

}  // namespace

// Keep Interpreter entry-points thin; container arithmetic lives in phase5/runtime/ops/*.
double Interpreter::to_number(const Value& value) {
  return runtime_ops::to_number(value);
}

bool Interpreter::truthy(const Value& value) {
  switch (value.kind) {
    case Value::Kind::Nil:
      return false;
    case Value::Kind::Bool:
      return value.bool_value;
    case Value::Kind::Int:
      return value.int_value != 0;
    case Value::Kind::Double:
      return value.double_value != 0.0;
    case Value::Kind::String:
      return !value.string_value.empty();
    case Value::Kind::Numeric:
      return !numeric_value_is_zero(value);
    case Value::Kind::List:
      if (!value.list_value.empty()) {
        return true;
      }
      if (value.list_cache.materialized_version == value.list_cache.version &&
          !value.list_cache.promoted_f64.empty()) {
        return true;
      }
      return false;
    default:
      return true;
  }
}

Value Interpreter::eval_unary(UnaryOp op, const Value& operand) const {
  switch (op) {
    case UnaryOp::Neg:
      if (!is_numeric_kind(operand)) {
        throw EvalException("unary - expects numeric value");
      }
      if (operand.kind == Value::Kind::Int) {
        return Value::int_value_of(-operand.int_value);
      }
      if (operand.kind == Value::Kind::Double) {
        return Value::double_value_of(-operand.double_value);
      }
      if (operand.kind == Value::Kind::Numeric && operand.numeric_value) {
        const auto zero = cast_numeric_to_kind(operand.numeric_value->kind, Value::int_value_of(0));
        return eval_numeric_binary_value(BinaryOp::Sub, zero, operand);
      }
      return Value::double_value_of(-to_number_for_compare(operand));
    case UnaryOp::Not:
      return Value::bool_value_of(!truthy(operand));
    case UnaryOp::Await:
      return await_task_value(operand);
  }

  return Value::nil();
}

Value Interpreter::eval_binary(BinaryOp op, const Value& left, const Value& right) const {
  if (op == BinaryOp::Or || op == BinaryOp::And) {
    return Value::bool_value_of(op == BinaryOp::And ? (truthy(left) && truthy(right)) :
                                           (truthy(left) || truthy(right)));
  }

  // Primitive numeric fast-path: avoid list/matrix/string dispatch when both
  // operands are explicit numeric primitives (f8..f512, i8..i512).
  if (left.kind == Value::Kind::Numeric && right.kind == Value::Kind::Numeric) {
    switch (op) {
      case BinaryOp::Add:
      case BinaryOp::Sub:
      case BinaryOp::Mul:
      case BinaryOp::Div:
      case BinaryOp::Mod:
      case BinaryOp::Pow:
      case BinaryOp::Eq:
      case BinaryOp::Ne:
      case BinaryOp::Lt:
      case BinaryOp::Lte:
      case BinaryOp::Gt:
      case BinaryOp::Gte:
        return eval_numeric_binary_value(op, left, right);
      default:
        break;
    }
  }

  if (is_numeric_kind(left) && is_numeric_kind(right) &&
      (op == BinaryOp::Eq || op == BinaryOp::Ne || op == BinaryOp::Lt || op == BinaryOp::Lte ||
       op == BinaryOp::Gt || op == BinaryOp::Gte)) {
    return eval_numeric_binary_value(op, left, right);
  }

  if (op == BinaryOp::Eq) {
    return Value::bool_value_of(left.equals(right));
  }
  if (op == BinaryOp::Ne) {
    return Value::bool_value_of(!left.equals(right));
  }

  if ((left.kind == Value::Kind::String || right.kind == Value::Kind::String) &&
      !runtime_ops::has_list_operand(left, right) &&
      !runtime_ops::has_matrix_operand(left, right)) {
    if (left.kind != Value::Kind::String || right.kind != Value::Kind::String) {
      throw EvalException("string arithmetic/comparison expects string operands");
    }
    if (op == BinaryOp::Add) {
      return Value::string_value_of(left.string_value + right.string_value);
    }
    if (op == BinaryOp::Lt) {
      return Value::bool_value_of(left.string_value < right.string_value);
    }
    if (op == BinaryOp::Lte) {
      return Value::bool_value_of(left.string_value <= right.string_value);
    }
    if (op == BinaryOp::Gt) {
      return Value::bool_value_of(left.string_value > right.string_value);
    }
    if (op == BinaryOp::Gte) {
      return Value::bool_value_of(left.string_value >= right.string_value);
    }
    throw EvalException("string supports only + and comparison operators");
  }

  if (op == BinaryOp::Add && left.kind == Value::Kind::List && right.kind == Value::Kind::List) {
    if (runtime_ops::env_bool_enabled("SPARK_LIST_ADD_ELEMENTWISE", false)) {
      return runtime_ops::apply_list_list_op(left, right, op);
    }
    std::vector<Value> result = left.list_value;
    result.insert(result.end(), right.list_value.begin(), right.list_value.end());
    return Value::list_value_of(std::move(result));
  }

  if (runtime_ops::has_list_operand(left, right)) {
    if (!runtime_ops::is_list_binary_op(op)) {
      throw EvalException("list arithmetic supports only +,-,*,/,%,^");
    }
    if (left.kind == Value::Kind::List && right.kind == Value::Kind::List) {
      return runtime_ops::apply_list_list_op(left, right, op);
    }
    const bool allow_hetero_scalar = (op == BinaryOp::Add || op == BinaryOp::Mul);
    if (left.kind == Value::Kind::List &&
        (is_numeric_kind(right) || (allow_hetero_scalar && right.kind != Value::Kind::List &&
                                    right.kind != Value::Kind::Matrix))) {
      return runtime_ops::apply_list_scalar_op(left, right, op, true);
    }
    if (right.kind == Value::Kind::List &&
        (is_numeric_kind(left) || (allow_hetero_scalar && left.kind != Value::Kind::List &&
                                   left.kind != Value::Kind::Matrix))) {
      return runtime_ops::apply_list_scalar_op(right, left, op, false);
    }
    throw EvalException("list arithmetic expects list/list or list/scalar operands");
  }

  if (runtime_ops::has_matrix_operand(left, right)) {
    if (!runtime_ops::is_matrix_binary_op(op)) {
      throw EvalException("binary arithmetic expects numeric values");
    }
    if (left.kind == Value::Kind::Matrix && right.kind == Value::Kind::Matrix) {
      return runtime_ops::apply_matrix_matrix_op(left, right, op);
    }
    const bool allow_hetero_scalar = (op == BinaryOp::Add || op == BinaryOp::Mul);
    if (left.kind == Value::Kind::Matrix &&
        (is_numeric_kind(right) || (allow_hetero_scalar && right.kind != Value::Kind::Matrix))) {
      return runtime_ops::apply_matrix_scalar_op(left, right, op, true);
    }
    if (right.kind == Value::Kind::Matrix &&
        (is_numeric_kind(left) || (allow_hetero_scalar && left.kind != Value::Kind::Matrix))) {
      return runtime_ops::apply_matrix_scalar_op(right, left, op, false);
    }
    throw EvalException("matrix arithmetic expects numeric matrix/scalar operands");
  }

  if (!is_numeric_kind(left) || !is_numeric_kind(right)) {
    throw EvalException("binary arithmetic expects numeric values");
  }

  if (left.kind == Value::Kind::Numeric || right.kind == Value::Kind::Numeric) {
    return eval_numeric_binary_value(op, left, right);
  }

  const bool is_double = left.kind == Value::Kind::Double || right.kind == Value::Kind::Double;
  const double lhs = to_number(left);
  const double rhs = to_number(right);

  switch (op) {
    case BinaryOp::Sub:
      if (is_double) return Value::double_value_of(lhs - rhs);
      return Value::int_value_of(left.int_value - right.int_value);
    case BinaryOp::Mul:
      if (is_double) return Value::double_value_of(lhs * rhs);
      return Value::int_value_of(left.int_value * right.int_value);
    case BinaryOp::Div:
      if (rhs == 0.0) {
        throw EvalException("division by zero");
      }
      return Value::double_value_of(lhs / rhs);
    case BinaryOp::Mod:
      if (left.kind != Value::Kind::Int || right.kind != Value::Kind::Int) {
        throw EvalException("modulo expects integer values");
      }
      if (right.int_value == 0) {
        throw EvalException("modulo by zero");
      }
      return Value::int_value_of(left.int_value % right.int_value);
    case BinaryOp::Pow:
      if (const auto integral_exp = integral_pow_exponent(rhs); integral_exp.has_value()) {
        return Value::double_value_of(powi_double(lhs, *integral_exp));
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
    case BinaryOp::Add:
      if (is_double) return Value::double_value_of(lhs + rhs);
      return Value::int_value_of(left.int_value + right.int_value);
    case BinaryOp::Eq:
    case BinaryOp::Ne:
    case BinaryOp::And:
    case BinaryOp::Or:
      break;
  }

  throw EvalException("unsupported binary operator");
}

}  // namespace spark
