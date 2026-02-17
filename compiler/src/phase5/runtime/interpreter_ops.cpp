#include <vector>

#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {

namespace {

double to_number(const Value& value) {
  if (value.kind == Value::Kind::Int) {
    return static_cast<double>(value.int_value);
  }
  if (value.kind == Value::Kind::Double) {
    return value.double_value;
  }
  throw EvalException("expected numeric value");
}

bool is_matrix_binary_op(BinaryOp op) {
  return op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul || op == BinaryOp::Div;
}

bool has_matrix_operand(const Value& left, const Value& right) {
  return left.kind == Value::Kind::Matrix || right.kind == Value::Kind::Matrix;
}

double matrix_number(const Value& value) {
  if (value.kind == Value::Kind::Int) {
    return static_cast<double>(value.int_value);
  }
  if (value.kind == Value::Kind::Double) {
    return value.double_value;
  }
  throw EvalException("matrix arithmetic expects numeric operands");
}

bool should_return_double(const Value& left, const Value& right) {
  const auto matrix_has_double = [](const Value& value) {
    if (value.kind != Value::Kind::Matrix || !value.matrix_value) {
      return false;
    }
    for (const auto& cell : value.matrix_value->data) {
      if (cell.kind == Value::Kind::Double) {
        return true;
      }
    }
    return false;
  };
  return left.kind == Value::Kind::Double || right.kind == Value::Kind::Double ||
         matrix_has_double(left) || matrix_has_double(right);
}

Value matrix_as_double_cell(double value) {
  return Value::double_value_of(value);
}

Value matrix_as_int_or_double_cell(double value, bool emit_double) {
  if (emit_double) {
    return matrix_as_double_cell(value);
  }
  const auto int_value = static_cast<long long>(value);
  return Value::int_value_of(int_value);
}

Value apply_matrix_matrix_op(const Value& left, const Value& right, BinaryOp op) {
  if (!left.matrix_value || !right.matrix_value) {
    throw EvalException("matrix arithmetic expects matrix values");
  }
  if (left.matrix_value->rows != right.matrix_value->rows ||
      left.matrix_value->cols != right.matrix_value->cols) {
    throw EvalException("matrix shapes must match for elementwise binary ops");
  }

  const bool emit_double = should_return_double(left, right);
  const std::size_t total = left.matrix_value->data.size();
  std::vector<Value> out_data;
  out_data.reserve(total);

  for (std::size_t i = 0; i < total; ++i) {
    const double lhs = matrix_number(left.matrix_value->data[i]);
    const double rhs = matrix_number(right.matrix_value->data[i]);
    double value = 0.0;
    switch (op) {
      case BinaryOp::Add:
        value = lhs + rhs;
        break;
      case BinaryOp::Sub:
        value = lhs - rhs;
        break;
      case BinaryOp::Mul:
        value = lhs * rhs;
        break;
      case BinaryOp::Div:
        if (rhs == 0.0) {
          throw EvalException("division by zero");
        }
        value = lhs / rhs;
        break;
      default:
        throw EvalException("unsupported matrix arithmetic operator");
    }
    if (op == BinaryOp::Div) {
      out_data.push_back(matrix_as_int_or_double_cell(value, emit_double));
      continue;
    }
    out_data.push_back(matrix_as_int_or_double_cell(value, emit_double));
  }

  return Value::matrix_value_of(left.matrix_value->rows, left.matrix_value->cols, std::move(out_data));
}

Value apply_matrix_scalar_op(const Value& matrix, const Value& scalar, BinaryOp op, bool matrix_on_left) {
  if (!matrix.matrix_value) {
    throw EvalException("matrix arithmetic expects matrix value");
  }

  const bool emit_double = should_return_double(matrix, scalar);
  std::vector<Value> out_data;
  out_data.reserve(matrix.matrix_value->data.size());

  const double rhs = matrix_number(scalar);
  for (const auto& item : matrix.matrix_value->data) {
    const double lhs = matrix_number(item);
    double value = 0.0;
    switch (op) {
      case BinaryOp::Add:
        value = matrix_on_left ? lhs + rhs : rhs + lhs;
        break;
      case BinaryOp::Sub:
        value = matrix_on_left ? lhs - rhs : rhs - lhs;
        break;
      case BinaryOp::Mul:
        value = lhs * rhs;
        break;
      case BinaryOp::Div:
        if (matrix_on_left && rhs == 0.0) {
          throw EvalException("division by zero");
        }
        if (!matrix_on_left && lhs == 0.0) {
          // scalar / matrix has no stable runtime behavior with zero cells here.
          // Keep explicit hard error to avoid silently returning inf.
          throw EvalException("division by zero");
        }
        value = matrix_on_left ? lhs / rhs : rhs / lhs;
        break;
      default:
        throw EvalException("unsupported matrix arithmetic operator");
    }
    const bool as_double = (op == BinaryOp::Div) || emit_double;
    out_data.push_back(matrix_as_int_or_double_cell(value, as_double));
  }
  return Value::matrix_value_of(matrix.matrix_value->rows, matrix.matrix_value->cols, std::move(out_data));
}

}  // namespace

double Interpreter::to_number(const Value& value) {
  return ::spark::to_number(value);
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
    case Value::Kind::List:
      return !value.list_value.empty();
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
      return Value::double_value_of(-operand.double_value);
    case UnaryOp::Not:
      return Value::bool_value_of(!truthy(operand));
  }

  return Value::nil();
}

Value Interpreter::eval_binary(BinaryOp op, const Value& left, const Value& right) const {
  if (op == BinaryOp::Or || op == BinaryOp::And) {
    return Value::bool_value_of(op == BinaryOp::And ? (truthy(left) && truthy(right)) :
                                           (truthy(left) || truthy(right)));
  }

  if (op == BinaryOp::Eq) {
    return Value::bool_value_of(left.equals(right));
  }
  if (op == BinaryOp::Ne) {
    return Value::bool_value_of(!left.equals(right));
  }

  if (op == BinaryOp::Add && left.kind == Value::Kind::List && right.kind == Value::Kind::List) {
    std::vector<Value> result = left.list_value;
    result.insert(result.end(), right.list_value.begin(), right.list_value.end());
    return Value::list_value_of(std::move(result));
  }

  if (has_matrix_operand(left, right)) {
    if (!is_matrix_binary_op(op)) {
      throw EvalException("binary arithmetic expects numeric values");
    }
    if (left.kind == Value::Kind::Matrix && right.kind == Value::Kind::Matrix) {
      return apply_matrix_matrix_op(left, right, op);
    }
    if (left.kind == Value::Kind::Matrix && is_numeric_kind(right)) {
      return apply_matrix_scalar_op(left, right, op, true);
    }
    if (right.kind == Value::Kind::Matrix && is_numeric_kind(left)) {
      return apply_matrix_scalar_op(right, left, op, false);
    }
    throw EvalException("matrix arithmetic expects numeric matrix/scalar operands");
  }

  if (!is_numeric_kind(left) || !is_numeric_kind(right)) {
    throw EvalException("binary arithmetic expects numeric values");
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
