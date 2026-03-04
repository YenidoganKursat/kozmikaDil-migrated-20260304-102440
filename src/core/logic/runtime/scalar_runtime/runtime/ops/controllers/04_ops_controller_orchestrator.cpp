#include "scalar_runtime/runtime/ops/runtime_ops.h"

#include <optional>
#include <utility>

#include "semantic_runtime/evaluator_parts/internal_helpers.h"

namespace spark::runtime_ops::controllers {

std::optional<Value> try_eval_binary_container(BinaryOp op, const Value& left, const Value& right);
Value eval_binary_scalar_controller(BinaryOp op, const Value& left, const Value& right);

namespace {

bool is_numeric_compare_op_fast(const BinaryOp op) {
  return op == BinaryOp::Eq || op == BinaryOp::Ne || op == BinaryOp::Lt ||
         op == BinaryOp::Lte || op == BinaryOp::Gt || op == BinaryOp::Gte;
}

bool is_numeric_arith_or_compare_fast(const BinaryOp op) {
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
      return true;
    default:
      return false;
  }
}

bool is_scalar_int_or_double_fast(const Value& value) {
  return value.kind == Value::Kind::Int || value.kind == Value::Kind::Double;
}

}  // namespace

Value eval_unary_orchestrator(const UnaryOp op, const Value& operand) {
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
      return Value::bool_value_of(!Interpreter::truthy(operand));
    case UnaryOp::Await:
      return await_task_value(operand);
  }

  return Value::nil();
}

Value eval_binary_orchestrator(const BinaryOp op, const Value& left, const Value& right) {
  // Hot-path: fully typed numeric controller dispatch without optional/indirection.
  if (left.kind == Value::Kind::Numeric && right.kind == Value::Kind::Numeric &&
      is_numeric_arith_or_compare_fast(op)) {
    return eval_numeric_binary_value(op, left, right);
  }
  if (is_numeric_compare_op_fast(op) && is_numeric_kind(left) && is_numeric_kind(right)) {
    return eval_numeric_binary_value(op, left, right);
  }

  if (op == BinaryOp::Or || op == BinaryOp::And) {
    return Value::bool_value_of(op == BinaryOp::And ? (Interpreter::truthy(left) && Interpreter::truthy(right))
                                                    : (Interpreter::truthy(left) || Interpreter::truthy(right)));
  }
  if (op == BinaryOp::Eq) {
    return Value::bool_value_of(left.equals(right));
  }
  if (op == BinaryOp::Ne) {
    return Value::bool_value_of(!left.equals(right));
  }

  if (is_scalar_int_or_double_fast(left) && is_scalar_int_or_double_fast(right)) {
    return eval_binary_scalar_controller(op, left, right);
  }

  if (auto out = try_eval_binary_container(op, left, right); out.has_value()) {
    return std::move(*out);
  }

  if (!is_numeric_kind(left) || !is_numeric_kind(right)) {
    throw EvalException("binary arithmetic expects numeric values");
  }

  if (left.kind == Value::Kind::Numeric || right.kind == Value::Kind::Numeric) {
    return eval_numeric_binary_value(op, left, right);
  }

  return eval_binary_scalar_controller(op, left, right);
}

}  // namespace spark::runtime_ops::controllers
