#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

#include "../internal_helpers.h"

namespace spark {

std::string double_to_string(double value) {
  if (std::isnan(value) || std::isinf(value)) {
    return std::to_string(value);
  }
  if (std::floor(value) == value) {
    std::ostringstream stream;
    stream << static_cast<long long>(value);
    return stream.str();
  }

  std::ostringstream stream;
  stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
  std::string out = stream.str();
  while (!out.empty() && out.back() == '0') {
    out.pop_back();
  }
  if (!out.empty() && out.back() == '.') {
    out.pop_back();
  }
  return out.empty() ? "0" : out;
}

bool is_numeric_kind(const Value& value) {
  return value.kind == Value::Kind::Int || value.kind == Value::Kind::Double ||
         value.kind == Value::Kind::Numeric;
}

double to_number_for_compare(const Value& value) {
  if (value.kind == Value::Kind::Int) {
    return static_cast<double>(value.int_value);
  }
  if (value.kind == Value::Kind::Double) {
    return value.double_value;
  }
  if (value.kind == Value::Kind::Numeric) {
    return numeric_value_to_double(value);
  }
  throw EvalException("expected numeric value");
}

long long value_to_int(const Value& value) {
  if (value.kind == Value::Kind::Int) {
    return value.int_value;
  }
  if (value.kind == Value::Kind::Double) {
    return static_cast<long long>(value.double_value);
  }
  if (value.kind == Value::Kind::Numeric) {
    return numeric_value_to_i64(value);
  }
  throw EvalException("expected integer value");
}

double matrix_element_to_double(const Value& value) {
  if (value.kind == Value::Kind::Int) {
    return static_cast<double>(value.int_value);
  }
  if (value.kind == Value::Kind::Double) {
    return value.double_value;
  }
  if (value.kind == Value::Kind::Numeric) {
    return numeric_value_to_double(value);
  }
  throw EvalException("matrix elements must be numeric");
}

bool matrix_element_wants_double(const Value& value) {
  return value.kind == Value::Kind::Double || value.kind == Value::Kind::Numeric;
}

}  // namespace spark
