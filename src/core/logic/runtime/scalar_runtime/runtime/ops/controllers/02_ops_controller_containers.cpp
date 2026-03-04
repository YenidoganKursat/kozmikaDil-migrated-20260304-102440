#include "scalar_runtime/runtime/ops/runtime_ops.h"

#include <optional>

#include "semantic_runtime/evaluator_parts/internal_helpers.h"

namespace spark::runtime_ops::controllers {

std::optional<Value> try_eval_binary_string(const BinaryOp op, const Value& left, const Value& right) {
  if ((left.kind != Value::Kind::String && right.kind != Value::Kind::String) ||
      runtime_ops::has_list_operand(left, right) ||
      runtime_ops::has_matrix_operand(left, right)) {
    return std::nullopt;
  }

  if (left.kind != Value::Kind::String || right.kind != Value::Kind::String) {
    throw EvalException("string arithmetic/comparison expects string operands");
  }
  if (op == BinaryOp::Add) {
    std::string out;
    out.reserve(left.string_value.size() + right.string_value.size());
    out.append(left.string_value);
    out.append(right.string_value);
    return Value::string_value_of(std::move(out));
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

std::optional<Value> try_eval_binary_list(const BinaryOp op, const Value& left, const Value& right) {
  if (op == BinaryOp::Add && left.kind == Value::Kind::List && right.kind == Value::Kind::List) {
    if (runtime_ops::env_bool_enabled("SPARK_LIST_ADD_ELEMENTWISE", false)) {
      return runtime_ops::apply_list_list_op(left, right, op);
    }
    std::vector<Value> result = left.list_value;
    result.insert(result.end(), right.list_value.begin(), right.list_value.end());
    return Value::list_value_of(std::move(result));
  }

  if (!runtime_ops::has_list_operand(left, right)) {
    return std::nullopt;
  }
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

std::optional<Value> try_eval_binary_matrix(const BinaryOp op, const Value& left, const Value& right) {
  if (!runtime_ops::has_matrix_operand(left, right)) {
    return std::nullopt;
  }
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

std::optional<Value> try_eval_binary_container(const BinaryOp op, const Value& left, const Value& right) {
  if (const auto out = try_eval_binary_string(op, left, right); out.has_value()) {
    return out;
  }
  if (const auto out = try_eval_binary_list(op, left, right); out.has_value()) {
    return out;
  }
  return try_eval_binary_matrix(op, left, right);
}

}  // namespace spark::runtime_ops::controllers
