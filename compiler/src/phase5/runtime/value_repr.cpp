#include <string>

#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {

std::string Value::to_string() const {
  switch (kind) {
    case Kind::Nil:
      return "nil";
    case Kind::Int:
      return std::to_string(int_value);
    case Kind::Double:
      return double_to_string(double_value);
    case Kind::Bool:
      return bool_value ? "True" : "False";
    case Kind::List: {
      std::string out = "[";
      for (std::size_t i = 0; i < list_value.size(); ++i) {
        if (i > 0) {
          out += ", ";
        }
        out += list_value[i].to_string();
      }
      out += "]";
      return out;
    }
    case Kind::Function:
      return "<fn>";
    case Kind::Builtin:
      return "<builtin " + builtin_value->name + ">";
    case Kind::Task:
      return "<task>";
    case Kind::TaskGroup:
      return "<task_group>";
    case Kind::Channel:
      return "<channel>";
    case Kind::Matrix:
      if (!matrix_value) {
        return "<invalid matrix>";
      }
      return "<matrix " + std::to_string(matrix_value->rows) + "x" +
             std::to_string(matrix_value->cols) + ">";
  }

  return "nil";
}

bool Value::equals(const Value& other) const {
  if (kind != other.kind) {
    if (is_numeric_kind(*this) && is_numeric_kind(other)) {
      return to_number_for_compare(*this) == to_number_for_compare(other);
    }
    return false;
  }

  switch (kind) {
    case Kind::Nil:
      return true;
    case Kind::Int:
      return int_value == other.int_value;
    case Kind::Double:
      return double_value == other.double_value;
    case Kind::Bool:
      return bool_value == other.bool_value;
    case Kind::List:
      if (list_value.size() != other.list_value.size()) {
        return false;
      }
      for (std::size_t i = 0; i < list_value.size(); ++i) {
        if (!list_value[i].equals(other.list_value[i])) {
          return false;
        }
      }
      return true;
    case Kind::Function:
      return function_value == other.function_value;
    case Kind::Builtin:
      return builtin_value == other.builtin_value;
    case Kind::Task:
      return task_value == other.task_value;
    case Kind::TaskGroup:
      return task_group_value == other.task_group_value;
    case Kind::Channel:
      return channel_value == other.channel_value;
    case Kind::Matrix: {
      if (!matrix_value || !other.matrix_value) {
        return matrix_value == other.matrix_value;
      }
      if (matrix_value->rows != other.matrix_value->rows ||
          matrix_value->cols != other.matrix_value->cols ||
          matrix_value->data.size() != other.matrix_value->data.size()) {
        return false;
      }
      for (std::size_t i = 0; i < matrix_value->data.size(); ++i) {
        if (!matrix_value->data[i].equals(other.matrix_value->data[i])) {
          return false;
        }
      }
      return true;
    }
  }

  return false;
}

Value Value::matrix_value_of(std::size_t rows, std::size_t cols, std::vector<Value> values) {
  Value value;
  value.kind = Kind::Matrix;
  value.matrix_cache = MatrixCache{};
  value.matrix_value = std::make_shared<MatrixValue>();
  value.matrix_value->rows = rows;
  value.matrix_value->cols = cols;
  value.matrix_value->data = std::move(values);
  return value;
}

}  // namespace spark
