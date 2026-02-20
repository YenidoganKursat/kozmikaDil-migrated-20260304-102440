#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>

#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {

namespace {

using I128 = __int128_t;
using U128 = __uint128_t;

std::string vr_i128_to_string(I128 value) {
  if (value == 0) {
    return "0";
  }
  const bool negative = value < 0;
  U128 magnitude = negative ? static_cast<U128>(-(value + 1)) + 1 : static_cast<U128>(value);
  std::string out;
  while (magnitude > 0) {
    out.push_back(static_cast<char>('0' + static_cast<unsigned>(magnitude % 10)));
    magnitude /= 10;
  }
  if (negative) {
    out.push_back('-');
  }
  std::reverse(out.begin(), out.end());
  return out;
}

std::string vr_trim_decimal_string(std::string value) {
  if (value.find('.') == std::string::npos) {
    return value;
  }
  while (!value.empty() && value.back() == '0') {
    value.pop_back();
  }
  if (!value.empty() && value.back() == '.') {
    value.pop_back();
  }
  if (value.empty() || value == "-0") {
    return "0";
  }
  return value;
}

std::string vr_long_double_to_string(long double value) {
  std::ostringstream stream;
  stream << std::setprecision(36) << value;
  return vr_trim_decimal_string(stream.str());
}

}  // namespace

std::string Value::to_string() const {
  switch (kind) {
    case Kind::Nil:
      return "nil";
    case Kind::Int:
      return std::to_string(int_value);
    case Kind::Double:
      return double_to_string(double_value);
    case Kind::String:
      return string_value;
    case Kind::Numeric:
      if (!numeric_value) {
        return "<invalid numeric>";
      }
      if (numeric_kind_is_high_precision_float(numeric_value->kind)) {
        return high_precision_numeric_to_string(*numeric_value);
      }
      if (!numeric_value->payload.empty()) {
        return numeric_value->payload;
      }
      if (numeric_kind_is_int(numeric_value->kind) && numeric_value->parsed_int_valid) {
        return vr_i128_to_string(numeric_value->parsed_int);
      }
      if (numeric_value->parsed_float_valid) {
        return vr_long_double_to_string(numeric_value->parsed_float);
      }
      return numeric_value->payload;
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
      const auto compared = eval_numeric_binary_value(BinaryOp::Eq, *this, other);
      return compared.kind == Value::Kind::Bool && compared.bool_value;
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
    case Kind::String:
      return string_value == other.string_value;
    case Kind::Numeric: {
      if (!numeric_value || !other.numeric_value) {
        return static_cast<bool>(numeric_value) == static_cast<bool>(other.numeric_value);
      }
      const auto compared = eval_numeric_binary_value(BinaryOp::Eq, *this, other);
      return compared.kind == Value::Kind::Bool && compared.bool_value;
    }
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
