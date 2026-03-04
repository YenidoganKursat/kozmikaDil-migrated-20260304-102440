#include <algorithm>
#include <cctype>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "../../semantic_runtime/evaluator_parts/internal_helpers.h"

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

bool vc_parse_i128_decimal_exact(const std::string& text, I128& out) {
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
  bool overflow = false;
  while (idx < text.size() && std::isdigit(static_cast<unsigned char>(text[idx]))) {
    saw_digit = true;
    const auto digit = static_cast<unsigned>(text[idx] - '0');
    if (acc > (limit - digit) / 10) {
      overflow = true;
    } else {
      acc = acc * 10 + digit;
    }
    ++idx;
  }
  while (idx < text.size() && std::isspace(static_cast<unsigned char>(text[idx]))) {
    ++idx;
  }
  if (!saw_digit || overflow || idx != text.size()) {
    return false;
  }
  if (negative) {
    if (acc == max_negative_mag) {
      out = vc_i128_min();
      return true;
    }
    out = -static_cast<I128>(acc);
    return true;
  }
  out = static_cast<I128>(acc);
  return true;
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

bool classify_numeric_constructor_name(std::string_view name, Value::NumericKind& out_kind) {
  if (name == "i8") {
    out_kind = Value::NumericKind::I8;
    return true;
  }
  if (name == "i16") {
    out_kind = Value::NumericKind::I16;
    return true;
  }
  if (name == "i32") {
    out_kind = Value::NumericKind::I32;
    return true;
  }
  if (name == "i64") {
    out_kind = Value::NumericKind::I64;
    return true;
  }
  if (name == "i128") {
    out_kind = Value::NumericKind::I128;
    return true;
  }
  if (name == "i256") {
    out_kind = Value::NumericKind::I256;
    return true;
  }
  if (name == "i512" || name == "int" || name == "integer" || name == "ibig" ||
      name == "bigint") {
    out_kind = Value::NumericKind::I512;
    return true;
  }
  if (name == "f8") {
    out_kind = Value::NumericKind::F8;
    return true;
  }
  if (name == "f16") {
    out_kind = Value::NumericKind::F16;
    return true;
  }
  if (name == "bf16") {
    out_kind = Value::NumericKind::BF16;
    return true;
  }
  if (name == "f32") {
    out_kind = Value::NumericKind::F32;
    return true;
  }
  if (name == "f64") {
    out_kind = Value::NumericKind::F64;
    return true;
  }
  if (name == "f128") {
    out_kind = Value::NumericKind::F128;
    return true;
  }
  if (name == "f256") {
    out_kind = Value::NumericKind::F256;
    return true;
  }
  if (name == "f512") {
    out_kind = Value::NumericKind::F512;
    return true;
  }

  if (name.size() > 1 && name.front() == 'i') {
    std::uint64_t bits = 0;
    bool digits_only = true;
    for (std::size_t i = 1; i < name.size(); ++i) {
      const auto ch = static_cast<unsigned char>(name[i]);
      if (!std::isdigit(ch)) {
        digits_only = false;
        break;
      }
      const auto digit = static_cast<std::uint64_t>(name[i] - '0');
      if (bits > (std::numeric_limits<std::uint64_t>::max() - digit) / 10ULL) {
        bits = std::numeric_limits<std::uint64_t>::max();
      } else {
        bits = bits * 10ULL + digit;
      }
    }
    if (digits_only && bits > 0ULL) {
      if (bits <= 8ULL) {
        out_kind = Value::NumericKind::I8;
      } else if (bits <= 16ULL) {
        out_kind = Value::NumericKind::I16;
      } else if (bits <= 32ULL) {
        out_kind = Value::NumericKind::I32;
      } else if (bits <= 64ULL) {
        out_kind = Value::NumericKind::I64;
      } else if (bits <= 128ULL) {
        out_kind = Value::NumericKind::I128;
      } else if (bits <= 256ULL) {
        out_kind = Value::NumericKind::I256;
      } else {
        out_kind = Value::NumericKind::I512;
      }
      return true;
    }
  }

  return false;
}

Value::BuiltinTag classify_builtin_tag(const std::string& name, bool& out_numeric_ctor,
                                       Value::NumericKind& out_numeric_kind) {
  out_numeric_ctor = classify_numeric_constructor_name(name, out_numeric_kind);
  if (out_numeric_ctor) {
    return Value::BuiltinTag::NumericConstructor;
  }
  if (name == "bench_tick") {
    return Value::BuiltinTag::BenchTick;
  }
  if (name == "bench_tick_raw") {
    return Value::BuiltinTag::BenchTickRaw;
  }
  if (name == "bench_tick_scale_num") {
    return Value::BuiltinTag::BenchTickScaleNum;
  }
  if (name == "bench_tick_scale_den") {
    return Value::BuiltinTag::BenchTickScaleDen;
  }
  if (name == "string") {
    return Value::BuiltinTag::String;
  }
  if (name == "len") {
    return Value::BuiltinTag::Len;
  }
  if (name == "utf8_len") {
    return Value::BuiltinTag::Utf8Len;
  }
  if (name == "utf16_len") {
    return Value::BuiltinTag::Utf16Len;
  }
  return Value::BuiltinTag::Generic;
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
    if (kind == NumericKind::I256 || kind == NumericKind::I512) {
      if (payload.empty()) {
        value.numeric_value = make_numeric_payload(
            kind, "", true, 0, true, 0.0L);
      } else {
        I128 compact = 0;
        if (vc_parse_i128_decimal_exact(payload, compact)) {
          value.numeric_value = make_numeric_payload(
              kind, "", true, compact, true, static_cast<long double>(compact));
        } else {
          // Wide-int lanes keep canonical decimal payload as source of truth.
          value.numeric_value = make_numeric_payload(
              kind, std::move(payload), false, 0, false, 0.0L);
        }
      }
      return value;
    }
    const auto parsed = vc_parse_i128_decimal(payload);
    // Int numerics keep canonical parsed fields and avoid carrying textual
    // payload in runtime values to minimize copy/memory traffic in hot paths.
    value.numeric_value = make_numeric_payload(
        kind, "", true, parsed, true, static_cast<long double>(parsed));
  } else {
    const bool high_precision_kind = kind == NumericKind::F128 || kind == NumericKind::F256 ||
                                     kind == NumericKind::F512;
    if (high_precision_kind) {
      value.numeric_value =
          make_numeric_payload(kind, std::move(payload), false, 0, false, 0.0L);
      initialize_high_precision_numeric_cache(value);
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
  if (kind == NumericKind::I256 || kind == NumericKind::I512) {
    value.numeric_value = make_numeric_payload(
        kind, "", true, v, true, static_cast<long double>(v));
  } else {
    value.numeric_value = make_numeric_payload(kind, "", true, v, true, static_cast<long double>(v));
  }
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

Value Value::class_value_of(std::shared_ptr<ClassValue> cls) {
  Value value;
  value.kind = Kind::Class;
  value.class_value = std::move(cls);
  return value;
}

Value Value::object_value_of(std::shared_ptr<ObjectValue> obj) {
  Value value;
  value.kind = Kind::Object;
  value.object_value = std::move(obj);
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
  bool numeric_ctor = false;
  NumericKind numeric_ctor_kind = NumericKind::F64;
  const auto tag = classify_builtin_tag(name, numeric_ctor, numeric_ctor_kind);
  value.builtin_value = std::make_shared<Builtin>(Builtin{
      std::move(name),
      std::move(impl),
      tag,
      numeric_ctor,
      numeric_ctor_kind,
  });
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
