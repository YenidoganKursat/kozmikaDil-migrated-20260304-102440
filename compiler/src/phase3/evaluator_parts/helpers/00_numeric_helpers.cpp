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
  if (std::floor(value) == value && value >= static_cast<double>(std::numeric_limits<long long>::min()) &&
      value <= static_cast<double>(std::numeric_limits<long long>::max())) {
    std::ostringstream stream;
    stream << static_cast<long long>(value);
    return stream.str();
  }

  std::ostringstream stream;
  stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
  std::string raw = stream.str();
  const auto exp_pos = raw.find_first_of("eE");
  std::string mantissa = exp_pos == std::string::npos ? raw : raw.substr(0, exp_pos);
  const std::string exponent = exp_pos == std::string::npos ? std::string() : raw.substr(exp_pos);

  while (!mantissa.empty() && mantissa.back() == '0') {
    mantissa.pop_back();
  }
  if (!mantissa.empty() && mantissa.back() == '.') {
    mantissa.pop_back();
  }
  if (mantissa.empty() || mantissa == "-0") {
    mantissa = "0";
  }
  return mantissa + exponent;
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
