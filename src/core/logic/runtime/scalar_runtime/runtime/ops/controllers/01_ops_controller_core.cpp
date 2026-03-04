#include "scalar_runtime/runtime/ops/runtime_ops.h"

#include <optional>

#include "semantic_runtime/evaluator_parts/internal_helpers.h"

namespace spark::runtime_ops::controllers {

namespace {

bool is_numeric_compare_op(const BinaryOp op) {
  return op == BinaryOp::Eq || op == BinaryOp::Ne || op == BinaryOp::Lt ||
         op == BinaryOp::Lte || op == BinaryOp::Gt || op == BinaryOp::Gte;
}

}  // namespace

std::optional<Value> try_eval_binary_core(const BinaryOp op, const Value& left, const Value& right) {
  if (op == BinaryOp::Or || op == BinaryOp::And) {
    return Value::bool_value_of(op == BinaryOp::And ? (Interpreter::truthy(left) && Interpreter::truthy(right))
                                                    : (Interpreter::truthy(left) || Interpreter::truthy(right)));
  }

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

  if (is_numeric_kind(left) && is_numeric_kind(right) && is_numeric_compare_op(op)) {
    return eval_numeric_binary_value(op, left, right);
  }

  if (op == BinaryOp::Eq) {
    return Value::bool_value_of(left.equals(right));
  }
  if (op == BinaryOp::Ne) {
    return Value::bool_value_of(!left.equals(right));
  }

  return std::nullopt;
}

}  // namespace spark::runtime_ops::controllers
