#include <algorithm>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

#include "spark/semantic.h"

namespace spark {

namespace {

TypePtr local_unknown_type() {
  return std::make_shared<Type>(Type{.kind = Type::Kind::Unknown});
}

TypePtr local_any_type() {
  return std::make_shared<Type>(Type{.kind = Type::Kind::Any});
}

TypePtr local_float_type(Type::FloatKind kind = Type::FloatKind::F64) {
  return std::make_shared<Type>(Type{.kind = Type::Kind::Float, .float_kind = kind});
}

TypePtr local_list_type(TypePtr element_type) {
  auto type = std::make_shared<Type>();
  type->kind = Type::Kind::List;
  type->list_element = std::move(element_type);
  return type;
}

TypePtr local_matrix_type(TypePtr element_type, std::size_t rows, std::size_t cols) {
  auto type = std::make_shared<Type>();
  type->kind = Type::Kind::Matrix;
  type->list_element = std::move(element_type);
  type->matrix_rows = rows;
  type->matrix_cols = cols;
  return type;
}

TypePtr local_normalize_list_elements(const TypePtr& current, const TypePtr& next) {
  if (!current || !next) {
    return local_any_type();
  }
  if (current->kind == Type::Kind::Unknown) {
    return next;
  }
  if (next->kind == Type::Kind::Unknown) {
    return current;
  }
  if (current->kind == Type::Kind::Any || next->kind == Type::Kind::Any) {
    return local_any_type();
  }

  const auto is_numeric = [](const Type& type) {
    return type.kind == Type::Kind::Int || type.kind == Type::Kind::Float;
  };

  if (is_numeric(*current) && is_numeric(*next)) {
    if (current->kind == Type::Kind::Int && next->kind == Type::Kind::Int) {
      return std::make_shared<Type>(Type{.kind = Type::Kind::Int});
    }
    const auto wide = (current->kind == Type::Kind::Float ? current->float_kind : Type::FloatKind::F64);
    const auto other = (next->kind == Type::Kind::Float ? next->float_kind : Type::FloatKind::F64);
    const auto wide_float = static_cast<int>(wide);
    const auto other_float = static_cast<int>(other);
    return local_float_type(static_cast<Type::FloatKind>(std::max(wide_float, other_float)));
  }

  if (current->kind == next->kind) {
    if (current->kind == Type::Kind::List && current->list_element && next->list_element) {
      return local_list_type(local_normalize_list_elements(current->list_element, next->list_element));
    }
    if (current->kind == Type::Kind::Matrix && current->list_element && next->list_element) {
      return local_matrix_type(
          local_normalize_list_elements(current->list_element, next->list_element),
          std::max(current->matrix_rows, next->matrix_rows),
          (current->matrix_cols == next->matrix_cols) ? current->matrix_cols : 0);
    }
    if (current->kind == Type::Kind::Class && current->class_name == next->class_name) {
      return current;
    }
    return current;
  }

  return local_any_type();
}

bool same_kind_or_unknown(const Type& a, const Type& b) {
  if (a.kind == Type::Kind::Unknown || b.kind == Type::Kind::Unknown) {
    return true;
  }
  return a.kind == b.kind;
}

std::string kind_to_string(Type::Kind kind) {
  switch (kind) {
    case Type::Kind::Unknown:
      return "Unknown";
    case Type::Kind::Nil:
      return "Nil";
    case Type::Kind::Int:
      return "Int";
    case Type::Kind::Float:
      return "Float";
    case Type::Kind::String:
      return "String";
    case Type::Kind::Bool:
      return "Bool";
    case Type::Kind::Any:
      return "Any";
    case Type::Kind::List:
      return "List";
    case Type::Kind::Matrix:
      return "Matrix";
    case Type::Kind::Task:
      return "Task";
    case Type::Kind::TaskGroup:
      return "TaskGroup";
    case Type::Kind::Channel:
      return "Channel";
    case Type::Kind::Function:
      return "Function";
    case Type::Kind::Class:
      return "Class";
    case Type::Kind::Builtin:
      return "Builtin";
    case Type::Kind::Error:
      return "Error";
  }
  return "Unknown";
}

std::string float_kind_to_string(Type::FloatKind kind) {
  switch (kind) {
    case Type::FloatKind::F8:
      return "f8";
    case Type::FloatKind::F16:
      return "f16";
    case Type::FloatKind::BF16:
      return "bf16";
    case Type::FloatKind::F32:
      return "f32";
    case Type::FloatKind::F64:
      return "f64";
    case Type::FloatKind::F128:
      return "f128";
    case Type::FloatKind::F256:
      return "f256";
    case Type::FloatKind::F512:
      return "f512";
  }
  return "f64";
}

std::string class_shape_status(const Type& type) {
  if (type.kind != Type::Kind::Class) {
    return "";
  }
  return type.class_open ? "open" : "slots";
}

const VariableExpr* as_variable_target(const Expr& expr) {
  if (expr.kind != Expr::Kind::Variable) {
    return nullptr;
  }
  return &static_cast<const VariableExpr&>(expr);
}

const VariableExpr* root_variable_expr(const Expr& expr) {
  if (expr.kind == Expr::Kind::Variable) {
    return &static_cast<const VariableExpr&>(expr);
  }
  if (expr.kind == Expr::Kind::Index) {
    return root_variable_expr(*static_cast<const IndexExpr&>(expr).target);
  }
  return nullptr;
}

std::string target_to_name_or_expr(const Expr& expr) {
  if (const auto* variable = root_variable_expr(expr)) {
    return variable->name;
  }
  return "<expr>";
}

Type::FloatKind widest_float(Type::FloatKind left, Type::FloatKind right) {
  return (static_cast<int>(left) < static_cast<int>(right)) ? right : left;
}

bool is_numeric_widen_candidate(const Type& left, const Type& right) {
  if (left.kind == Type::Kind::Int && right.kind == Type::Kind::Float) {
    return true;
  }
  if (left.kind == Type::Kind::Float && right.kind == Type::Kind::Int) {
    return true;
  }
  if (left.kind == Type::Kind::List && right.kind == Type::Kind::List &&
      left.list_element && right.list_element) {
    return is_numeric_widen_candidate(*left.list_element, *right.list_element);
  }
  if (left.kind == Type::Kind::Matrix && right.kind == Type::Kind::Matrix &&
      left.list_element && right.list_element) {
    return is_numeric_widen_candidate(*left.list_element, *right.list_element);
  }
  return false;
}

TypePtr normalize_matrix_matrix_elements(const TypePtr& first, const TypePtr& second) {
  if (!first || !second || !first->list_element || !second->list_element) {
    return local_unknown_type();
  }
  return local_normalize_list_elements(first->list_element, second->list_element);
}

bool has_matrix_shape_mismatch(const Type& left, const Type& right) {
  if (left.matrix_rows == 0 || right.matrix_rows == 0 || left.matrix_cols == 0 || right.matrix_cols == 0) {
    return false;
  }
  return left.matrix_rows != right.matrix_rows || left.matrix_cols != right.matrix_cols;
}

TypePtr matrix_result_from(const Type& lhs, const Type& rhs, bool prefer_rhs_shape = false) {
  if (lhs.kind != Type::Kind::Matrix && rhs.kind != Type::Kind::Matrix) {
    return local_unknown_type();
  }

  const Type& matrix = (lhs.kind == Type::Kind::Matrix) ? lhs : rhs;
  if ((lhs.kind == Type::Kind::Matrix && rhs.kind == Type::Kind::Matrix) &&
      has_matrix_shape_mismatch(lhs, rhs)) {
    const auto left_matrix = local_matrix_type(matrix.matrix_rows ? lhs.list_element : matrix.list_element,
                                               matrix.matrix_rows,
                                               matrix.matrix_cols);
    const auto right_matrix = local_matrix_type(rhs.list_element, rhs.matrix_rows, rhs.matrix_cols);
    return local_matrix_type(normalize_matrix_matrix_elements(left_matrix, right_matrix),
                            prefer_rhs_shape ? right_matrix->matrix_rows : left_matrix->matrix_rows,
                            prefer_rhs_shape ? right_matrix->matrix_cols : left_matrix->matrix_cols);
  }

  const auto first = local_matrix_type(matrix.list_element ? matrix.list_element : local_unknown_type(),
                                      matrix.matrix_rows,
                                      matrix.matrix_cols);
  if (rhs.kind != Type::Kind::Matrix && (rhs.kind == Type::Kind::Int || rhs.kind == Type::Kind::Float)) {
    auto merged = normalize_matrix_matrix_elements(first, local_unknown_type());
    if (merged->kind != Type::Kind::Unknown) {
      return local_matrix_type(merged, first->matrix_rows, first->matrix_cols);
    }
    return local_matrix_type(local_unknown_type(), first->matrix_rows, first->matrix_cols);
  }
  if (lhs.kind != Type::Kind::Matrix && (lhs.kind == Type::Kind::Int || lhs.kind == Type::Kind::Float)) {
    if (prefer_rhs_shape) {
      const auto rhs_matrix = local_matrix_type(rhs.list_element, rhs.matrix_rows, rhs.matrix_cols);
      auto merged = normalize_matrix_matrix_elements(rhs_matrix, local_unknown_type());
      if (merged->kind != Type::Kind::Unknown) {
        return local_matrix_type(merged, rhs_matrix->matrix_rows, rhs_matrix->matrix_cols);
      }
      return local_matrix_type(local_unknown_type(), rhs_matrix->matrix_rows, rhs_matrix->matrix_cols);
    }
  }

  return first;
}

}  // namespace
