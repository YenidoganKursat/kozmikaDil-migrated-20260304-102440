#include <algorithm>
#include <cctype>
#include <exception>
#include <string>
#include <vector>

#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {

namespace {

using I128 = __int128_t;
using U128 = __uint128_t;

I128 vc_i128_max() {
  return static_cast<I128>((~U128{0}) >> 1);
}

I128 vc_i128_min() {
  return -vc_i128_max() - 1;
}

std::string vc_i128_to_string(I128 value) {
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

I128 vc_parse_i128_decimal(const std::string& text) {
  std::size_t idx = 0;
  while (idx < text.size() && std::isspace(static_cast<unsigned char>(text[idx]))) {
    ++idx;
  }
  bool negative = false;
  if (idx < text.size() && (text[idx] == '-' || text[idx] == '+')) {
    negative = text[idx] == '-';
    ++idx;
  }

  const U128 max_positive = static_cast<U128>(vc_i128_max());
  const U128 max_negative_mag = max_positive + 1;
  const U128 limit = negative ? max_negative_mag : max_positive;
  U128 acc = 0;
  bool saw_digit = false;
  while (idx < text.size() && std::isdigit(static_cast<unsigned char>(text[idx]))) {
    saw_digit = true;
    const auto digit = static_cast<unsigned>(text[idx] - '0');
    if (acc > (limit - digit) / 10) {
      acc = limit;
    } else {
      acc = acc * 10 + digit;
    }
    ++idx;
  }
  if (!saw_digit) {
    return 0;
  }
  if (negative) {
    if (acc >= max_negative_mag) {
      return vc_i128_min();
    }
    return -static_cast<I128>(acc);
  }
  if (acc > max_positive) {
    return vc_i128_max();
  }
  return static_cast<I128>(acc);
}

long double vc_parse_long_double(const std::string& text) {
  try {
    return std::stold(text);
  } catch (const std::exception&) {
    return 0.0L;
  }
}

Value::NumericValue make_numeric_payload(
    Value::NumericKind kind, std::string payload, bool parsed_int_valid,
    __int128_t parsed_int, bool parsed_float_valid, long double parsed_float) {
  Value::NumericValue numeric;
  numeric.kind = kind;
  numeric.payload = std::move(payload);
  numeric.parsed_int_valid = parsed_int_valid;
  numeric.parsed_int = parsed_int;
  numeric.parsed_float_valid = parsed_float_valid;
  numeric.parsed_float = parsed_float;
  return numeric;
}

}  // namespace

Value Value::nil() {
  Value value;
  value.kind = Kind::Nil;
  return value;
}

Value Value::int_value_of(long long v) {
  Value value;
  value.kind = Kind::Int;
  value.int_value = v;
  return value;
}

Value Value::double_value_of(double v) {
  Value value;
  value.kind = Kind::Double;
  value.double_value = v;
  return value;
}

Value Value::string_value_of(std::string v) {
  Value value;
  value.kind = Kind::String;
  value.string_value = std::move(v);
  return value;
}

Value Value::numeric_value_of(NumericKind kind, std::string payload) {
  Value value;
  value.kind = Kind::Numeric;
  if (numeric_kind_is_int(kind)) {
    const auto parsed = vc_parse_i128_decimal(payload);
    value.numeric_value = make_numeric_payload(
        kind, std::move(payload), true, parsed, true, static_cast<long double>(parsed));
  } else {
    const bool high_precision_kind = kind == NumericKind::F128 || kind == NumericKind::F256 ||
                                     kind == NumericKind::F512;
    if (high_precision_kind) {
      value.numeric_value =
          make_numeric_payload(kind, std::move(payload), false, 0, false, 0.0L);
    } else {
      const auto parsed = vc_parse_long_double(payload);
      value.numeric_value =
          make_numeric_payload(kind, std::move(payload), false, 0, true, parsed);
    }
  }
  return value;
}

Value Value::numeric_int_value_of(NumericKind kind, __int128_t v) {
  Value value;
  value.kind = Kind::Numeric;
  value.numeric_value = make_numeric_payload(kind, "", true, v, true, static_cast<long double>(v));
  return value;
}

Value Value::numeric_float_value_of(NumericKind kind, long double v) {
  Value value;
  value.kind = Kind::Numeric;
  value.numeric_value = make_numeric_payload(kind, "", false, 0, true, v);
  return value;
}

Value Value::bool_value_of(bool v) {
  Value value;
  value.kind = Kind::Bool;
  value.bool_value = v;
  return value;
}

Value Value::list_value_of(std::vector<Value> values) {
  Value value;
  value.kind = Kind::List;
  value.list_value = std::move(values);
  value.list_cache = ListCache{};
  return value;
}

Value Value::function(std::shared_ptr<Function> fn) {
  Value value;
  value.kind = Kind::Function;
  value.function_value = std::move(fn);
  return value;
}

Value Value::builtin(std::string name, std::function<Value(const std::vector<Value>&)> impl) {
  Value value;
  value.kind = Kind::Builtin;
  value.builtin_value = std::make_shared<Builtin>(Builtin{std::move(name), std::move(impl)});
  return value;
}

Value Value::task_value_of(std::shared_ptr<TaskHandle> task) {
  Value value;
  value.kind = Kind::Task;
  value.task_value = std::move(task);
  return value;
}

Value Value::task_group_value_of(std::shared_ptr<TaskGroupHandle> task_group) {
  Value value;
  value.kind = Kind::TaskGroup;
  value.task_group_value = std::move(task_group);
  return value;
}

Value Value::channel_value_of(std::shared_ptr<ChannelHandle> channel) {
  Value value;
  value.kind = Kind::Channel;
  value.channel_value = std::move(channel);
  return value;
}

}  // namespace spark
