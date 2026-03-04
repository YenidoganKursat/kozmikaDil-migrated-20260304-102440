#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../internal_helpers.h"

namespace spark {

namespace {

using I128 = spark_i128;
using U128 = spark_u128;

bool env_bool_enabled_binary_expr(const char* name, bool fallback) {
  return env_flag_enabled(name, fallback);
}

bool binary_pic_enabled() {
  static const bool enabled = env_bool_enabled_binary_expr("SPARK_BINARY_PIC", true);
  return enabled;
}

int binary_pic_warmup_threshold() {
  static const int warmup = [] {
    const char* raw = std::getenv("SPARK_BINARY_PIC_WARMUP");
    if (!raw || *raw == '\0') {
      return 8;
    }
    const int parsed = std::atoi(raw);
    if (parsed <= 0) {
      return 1;
    }
    return parsed;
  }();
  return warmup;
}

bool binary_pic_trace_enabled() {
  static const bool enabled = env_bool_enabled_binary_expr("SPARK_BINARY_PIC_TRACE", false);
  return enabled;
}

bool is_numeric_pic_operator(const BinaryOp op) {
  return op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul ||
         op == BinaryOp::Div || op == BinaryOp::Mod || op == BinaryOp::Pow ||
         op == BinaryOp::Eq || op == BinaryOp::Ne || op == BinaryOp::Lt ||
         op == BinaryOp::Lte || op == BinaryOp::Gt || op == BinaryOp::Gte;
}

bool is_numeric_pic_arith_operator(const BinaryOp op) {
  return op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul ||
         op == BinaryOp::Div || op == BinaryOp::Mod || op == BinaryOp::Pow;
}

bool is_int_or_double_value(const Value& value) {
  return value.kind == Value::Kind::Int || value.kind == Value::Kind::Double;
}

bool i64_positive_pow2(long long value) {
  if (value <= 0) {
    return false;
  }
  const auto bits = static_cast<unsigned long long>(value);
  return (bits & (bits - 1ULL)) == 0ULL;
}

unsigned i64_pow2_shift(long long value) {
  auto bits = static_cast<unsigned long long>(value);
  unsigned shift = 0U;
  while (bits > 1ULL) {
    bits >>= 1ULL;
    ++shift;
  }
  return shift;
}

bool try_fast_i64_mod_pow2(long long lhs, long long rhs, long long& out) {
  if (!i64_positive_pow2(rhs) || lhs < 0) {
    return false;
  }
  out = lhs & (rhs - 1LL);
  return true;
}

bool try_fast_i64_div_pow2(long long lhs, long long rhs, double& out) {
  // Preserves f64(lhs / rhs) for non-negative lhs in exact f64-int domain.
  constexpr long long kF64ExactIntMax = 9007199254740991LL;
  if (!i64_positive_pow2(rhs) || lhs < 0 || lhs > kF64ExactIntMax) {
    return false;
  }
  const auto shift = static_cast<int>(i64_pow2_shift(rhs));
  out = std::ldexp(static_cast<double>(lhs), -shift);
  return true;
}

bool is_low_float_numeric_kind(const Value::NumericKind kind) {
  return kind == Value::NumericKind::F8 || kind == Value::NumericKind::F16 ||
         kind == Value::NumericKind::BF16 || kind == Value::NumericKind::F32 ||
         kind == Value::NumericKind::F64;
}

bool is_extended_int_numeric_kind_pic(const Value::NumericKind kind) {
  return kind == Value::NumericKind::I256 || kind == Value::NumericKind::I512;
}

I128 i128_max_pic() {
  return static_cast<I128>((~U128{0}) >> 1);
}

I128 i128_min_pic() {
  return -i128_max_pic() - 1;
}

Value& numeric_pic_scratch(Value::NumericKind kind) {
  static thread_local Value i8_target = Value::numeric_value_of(Value::NumericKind::I8, "0");
  static thread_local Value i16_target = Value::numeric_value_of(Value::NumericKind::I16, "0");
  static thread_local Value i32_target = Value::numeric_value_of(Value::NumericKind::I32, "0");
  static thread_local Value i64_target = Value::numeric_value_of(Value::NumericKind::I64, "0");
  static thread_local Value i128_target = Value::numeric_value_of(Value::NumericKind::I128, "0");
  static thread_local Value i256_target = Value::numeric_value_of(Value::NumericKind::I256, "0");
  static thread_local Value i512_target = Value::numeric_value_of(Value::NumericKind::I512, "0");
  static thread_local Value f8_target = Value::numeric_value_of(Value::NumericKind::F8, "0");
  static thread_local Value f16_target = Value::numeric_value_of(Value::NumericKind::F16, "0");
  static thread_local Value bf16_target = Value::numeric_value_of(Value::NumericKind::BF16, "0");
  static thread_local Value f32_target = Value::numeric_value_of(Value::NumericKind::F32, "0");
  static thread_local Value f64_target = Value::numeric_value_of(Value::NumericKind::F64, "0");
  static thread_local Value f128_target = Value::numeric_value_of(Value::NumericKind::F128, "0");
  static thread_local Value f256_target = Value::numeric_value_of(Value::NumericKind::F256, "0");
  static thread_local Value f512_target = Value::numeric_value_of(Value::NumericKind::F512, "0");

  switch (kind) {
    case Value::NumericKind::I8:
      return i8_target;
    case Value::NumericKind::I16:
      return i16_target;
    case Value::NumericKind::I32:
      return i32_target;
    case Value::NumericKind::I64:
      return i64_target;
    case Value::NumericKind::I128:
      return i128_target;
    case Value::NumericKind::I256:
      return i256_target;
    case Value::NumericKind::I512:
      return i512_target;
    case Value::NumericKind::F8:
      return f8_target;
    case Value::NumericKind::F16:
      return f16_target;
    case Value::NumericKind::BF16:
      return bf16_target;
    case Value::NumericKind::F32:
      return f32_target;
    case Value::NumericKind::F64:
      return f64_target;
    case Value::NumericKind::F128:
      return f128_target;
    case Value::NumericKind::F256:
      return f256_target;
    case Value::NumericKind::F512:
      return f512_target;
  }
  return f64_target;
}

bool is_low_float_numeric_value(const Value& value, Value::NumericKind& out_kind) {
  if (value.kind != Value::Kind::Numeric || !value.numeric_value) {
    return false;
  }
  const auto kind = value.numeric_value->kind;
  if (!is_low_float_numeric_kind(kind) || numeric_kind_is_int(kind) ||
      numeric_kind_is_high_precision_float(kind)) {
    return false;
  }
  out_kind = kind;
  return true;
}

long double read_numeric_scalar_pic(const Value& value) {
  if (value.kind == Value::Kind::Numeric && value.numeric_value) {
    if (value.numeric_value->parsed_float_valid) {
      return value.numeric_value->parsed_float;
    }
    if (value.numeric_value->parsed_int_valid) {
      return static_cast<long double>(value.numeric_value->parsed_int);
    }
  }
  return static_cast<long double>(numeric_value_to_double(value));
}

Value make_low_float_numeric_value(const Value::NumericKind kind, long double out) {
  const auto normalized = normalize_numeric_float_value(kind, out);
  return Value::numeric_float_value_of(kind, normalized);
}

std::optional<long long> integral_pow_exponent_pic(const double value) {
  if (!std::isfinite(value)) {
    return std::nullopt;
  }
  const double rounded = std::nearbyint(value);
  if (std::fabs(value - rounded) > 1e-12) {
    return std::nullopt;
  }
  if (std::fabs(rounded) > 1'000'000.0) {
    return std::nullopt;
  }
  return static_cast<long long>(rounded);
}

double powi_double_pic(double base, long long exponent) {
  if (exponent == 0) {
    return 1.0;
  }
  if (base == 0.0 && exponent < 0) {
    return std::numeric_limits<double>::infinity();
  }
  const bool negative = exponent < 0;
  unsigned long long n = static_cast<unsigned long long>(negative ? -exponent : exponent);
  double result = 1.0;
  double factor = base;
  while (n > 0ULL) {
    if ((n & 1ULL) != 0ULL) {
      result *= factor;
    }
    n >>= 1ULL;
    if (n > 0ULL) {
      factor *= factor;
    }
  }
  return negative ? (1.0 / result) : result;
}

template <typename T>
T fast_mod_pic(T x, T y) {
  const T q = std::trunc(x / y);
  const T r = x - q * y;
  if (!std::isfinite(r) || std::fabs(r) >= std::fabs(y)) {
    return std::fmod(x, y);
  }
  if (r == static_cast<T>(0)) {
    return std::copysign(static_cast<T>(0), x);
  }
  if ((x < static_cast<T>(0) && r > static_cast<T>(0)) ||
      (x > static_cast<T>(0) && r < static_cast<T>(0))) {
    return std::fmod(x, y);
  }
  return r;
}

enum class BinaryPicRoute : std::uint8_t {
  None = 0,
  IntInt = 1,
  DoubleLike = 2,
  NumericInt = 3,
  NumericLowFloat = 4,
  NumericHighPrecision = 5,
};

struct BinaryPicEntry {
  const Expr* expr = nullptr;
  std::uint64_t env_id = 0;
  BinaryOp op = BinaryOp::Add;
  std::uint16_t lhs_sig = 0;
  std::uint16_t rhs_sig = 0;
  std::uint32_t hits = 0;
  BinaryPicRoute route = BinaryPicRoute::None;
  bool memo_valid = false;
  std::uint64_t lhs_stamp = 0;
  std::uint64_t rhs_stamp = 0;
  Value memo_value = Value::nil();
};

std::uint16_t value_kind_signature(const Value& value) {
  if (value.kind == Value::Kind::Numeric && value.numeric_value.has_value()) {
    return static_cast<std::uint16_t>(0x100U + static_cast<std::uint16_t>(value.numeric_value->kind));
  }
  return static_cast<std::uint16_t>(value.kind);
}

BinaryPicEntry& binary_pic_cache_slot(const Expr* expr, std::uint64_t env_id, BinaryOp op,
                                      std::uint16_t lhs_sig, std::uint16_t rhs_sig) {
  constexpr std::size_t kPicCacheSize = 4096;
  static thread_local std::array<BinaryPicEntry, kPicCacheSize> cache{};
  const auto key_a = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(expr));
  const auto key_b = static_cast<std::size_t>(env_id) * 11400714819323198485ull;
  const auto key_c = (static_cast<std::size_t>(op) << 17U) ^
                     (static_cast<std::size_t>(lhs_sig) << 7U) ^
                     static_cast<std::size_t>(rhs_sig);
  auto& slot = cache[(key_a ^ key_b ^ key_c) & (kPicCacheSize - 1)];
  return slot;
}

BinaryPicRoute choose_binary_pic_route(const BinaryOp op, const Value& lhs, const Value& rhs) {
  if (!is_numeric_pic_operator(op)) {
    return BinaryPicRoute::None;
  }

  if (is_int_or_double_value(lhs) && is_int_or_double_value(rhs)) {
    if (lhs.kind == Value::Kind::Int && rhs.kind == Value::Kind::Int) {
      return BinaryPicRoute::IntInt;
    }
    return BinaryPicRoute::DoubleLike;
  }

  Value::NumericKind lhs_numeric_kind = Value::NumericKind::F64;
  Value::NumericKind rhs_numeric_kind = Value::NumericKind::F64;
  if (lhs.kind == Value::Kind::Numeric && rhs.kind == Value::Kind::Numeric &&
      lhs.numeric_value && rhs.numeric_value &&
      lhs.numeric_value->kind == rhs.numeric_value->kind &&
      numeric_kind_is_int(lhs.numeric_value->kind)) {
    return BinaryPicRoute::NumericInt;
  }

  if (is_low_float_numeric_value(lhs, lhs_numeric_kind) &&
      is_low_float_numeric_value(rhs, rhs_numeric_kind) &&
      lhs_numeric_kind == rhs_numeric_kind) {
    return BinaryPicRoute::NumericLowFloat;
  }

  if (lhs.kind == Value::Kind::Numeric && rhs.kind == Value::Kind::Numeric &&
      lhs.numeric_value && rhs.numeric_value &&
      numeric_kind_is_high_precision_float(lhs.numeric_value->kind) &&
      lhs.numeric_value->kind == rhs.numeric_value->kind) {
    return BinaryPicRoute::NumericHighPrecision;
  }

  return BinaryPicRoute::None;
}

Value execute_binary_pic_route(const BinaryPicRoute route, const BinaryOp op,
                               const Value& lhs, const Value& rhs) {
  switch (route) {
    case BinaryPicRoute::IntInt: {
      switch (op) {
        case BinaryOp::Add:
          return Value::int_value_of(lhs.int_value + rhs.int_value);
        case BinaryOp::Sub:
          return Value::int_value_of(lhs.int_value - rhs.int_value);
        case BinaryOp::Mul:
          return Value::int_value_of(lhs.int_value * rhs.int_value);
        case BinaryOp::Div:
          if (rhs.int_value == 0) {
            throw EvalException("division by zero");
          }
          {
            double fast_div = 0.0;
            if (try_fast_i64_div_pow2(lhs.int_value, rhs.int_value, fast_div)) {
              return Value::double_value_of(fast_div);
            }
          }
          return Value::double_value_of(static_cast<double>(lhs.int_value) /
                                        static_cast<double>(rhs.int_value));
        case BinaryOp::Mod:
          if (rhs.int_value == 0) {
            throw EvalException("modulo by zero");
          }
          {
            long long fast_mod = 0;
            if (try_fast_i64_mod_pow2(lhs.int_value, rhs.int_value, fast_mod)) {
              return Value::int_value_of(fast_mod);
            }
          }
          return Value::int_value_of(lhs.int_value % rhs.int_value);
        case BinaryOp::Pow: {
          const auto lhs_double = static_cast<double>(lhs.int_value);
          const auto rhs_double = static_cast<double>(rhs.int_value);
          if (const auto integral_exp = integral_pow_exponent_pic(rhs_double); integral_exp.has_value()) {
            return Value::double_value_of(powi_double_pic(lhs_double, *integral_exp));
          }
          return Value::double_value_of(std::pow(lhs_double, rhs_double));
        }
        case BinaryOp::Eq:
          return Value::bool_value_of(lhs.int_value == rhs.int_value);
        case BinaryOp::Ne:
          return Value::bool_value_of(lhs.int_value != rhs.int_value);
        case BinaryOp::Lt:
          return Value::bool_value_of(lhs.int_value < rhs.int_value);
        case BinaryOp::Lte:
          return Value::bool_value_of(lhs.int_value <= rhs.int_value);
        case BinaryOp::Gt:
          return Value::bool_value_of(lhs.int_value > rhs.int_value);
        case BinaryOp::Gte:
          return Value::bool_value_of(lhs.int_value >= rhs.int_value);
        default:
          break;
      }
      break;
    }
    case BinaryPicRoute::DoubleLike: {
      const double lhs_double = (lhs.kind == Value::Kind::Int)
                                    ? static_cast<double>(lhs.int_value)
                                    : lhs.double_value;
      const double rhs_double = (rhs.kind == Value::Kind::Int)
                                    ? static_cast<double>(rhs.int_value)
                                    : rhs.double_value;
      switch (op) {
        case BinaryOp::Add:
          return Value::double_value_of(lhs_double + rhs_double);
        case BinaryOp::Sub:
          return Value::double_value_of(lhs_double - rhs_double);
        case BinaryOp::Mul:
          return Value::double_value_of(lhs_double * rhs_double);
        case BinaryOp::Div:
          if (rhs_double == 0.0) {
            throw EvalException("division by zero");
          }
          return Value::double_value_of(lhs_double / rhs_double);
        case BinaryOp::Mod:
          if (rhs_double == 0.0) {
            throw EvalException("modulo by zero");
          }
          return Value::double_value_of(fast_mod_pic(lhs_double, rhs_double));
        case BinaryOp::Pow:
          if (const auto integral_exp = integral_pow_exponent_pic(rhs_double); integral_exp.has_value()) {
            return Value::double_value_of(powi_double_pic(lhs_double, *integral_exp));
          }
          return Value::double_value_of(std::pow(lhs_double, rhs_double));
        case BinaryOp::Eq:
          return Value::bool_value_of(lhs_double == rhs_double);
        case BinaryOp::Ne:
          return Value::bool_value_of(lhs_double != rhs_double);
        case BinaryOp::Lt:
          return Value::bool_value_of(lhs_double < rhs_double);
        case BinaryOp::Lte:
          return Value::bool_value_of(lhs_double <= rhs_double);
        case BinaryOp::Gt:
          return Value::bool_value_of(lhs_double > rhs_double);
        case BinaryOp::Gte:
          return Value::bool_value_of(lhs_double >= rhs_double);
        default:
          break;
      }
      break;
    }
    case BinaryPicRoute::NumericInt: {
      if (lhs.kind != Value::Kind::Numeric || rhs.kind != Value::Kind::Numeric ||
          !lhs.numeric_value || !rhs.numeric_value) {
        break;
      }
      const auto kind = lhs.numeric_value->kind;
      if (kind != rhs.numeric_value->kind || !numeric_kind_is_int(kind)) {
        break;
      }

      if (is_numeric_pic_arith_operator(op) && op != BinaryOp::Div && op != BinaryOp::Pow) {
        auto& target = numeric_pic_scratch(kind);
        if (eval_numeric_binary_value_inplace(op, lhs, rhs, target)) {
          return target;
        }
      }

      // Preserve strictness for wide lanes:
      // if compact i128 metadata is unavailable, keep the canonical evaluator path.
      if (!lhs.numeric_value->parsed_int_valid || !rhs.numeric_value->parsed_int_valid) {
        return eval_numeric_binary_value(op, lhs, rhs);
      }

      const I128 lhs_i = static_cast<I128>(lhs.numeric_value->parsed_int);
      const I128 rhs_i = static_cast<I128>(rhs.numeric_value->parsed_int);
      const bool extended_kind = is_extended_int_numeric_kind_pic(kind);
      I128 out_i = 0;

      switch (op) {
        case BinaryOp::Add:
          if (__builtin_add_overflow(lhs_i, rhs_i, &out_i)) {
            if (extended_kind) {
              return eval_numeric_binary_value(op, lhs, rhs);
            }
            out_i = (lhs_i >= 0 && rhs_i >= 0) ? i128_max_pic() : i128_min_pic();
          }
          return cast_int_kind(kind, out_i);
        case BinaryOp::Sub:
          if (__builtin_sub_overflow(lhs_i, rhs_i, &out_i)) {
            if (extended_kind) {
              return eval_numeric_binary_value(op, lhs, rhs);
            }
            out_i = (lhs_i >= 0 && rhs_i < 0) ? i128_max_pic() : i128_min_pic();
          }
          return cast_int_kind(kind, out_i);
        case BinaryOp::Mul:
          if (__builtin_mul_overflow(lhs_i, rhs_i, &out_i)) {
            if (extended_kind) {
              return eval_numeric_binary_value(op, lhs, rhs);
            }
            const bool non_negative = (lhs_i == 0 || rhs_i == 0) || ((lhs_i > 0) == (rhs_i > 0));
            out_i = non_negative ? i128_max_pic() : i128_min_pic();
          }
          return cast_int_kind(kind, out_i);
        case BinaryOp::Mod:
          if (rhs_i == 0) {
            throw EvalException("modulo by zero");
          }
          out_i = lhs_i % rhs_i;
          return cast_int_kind(kind, out_i);
        case BinaryOp::Eq:
          return Value::bool_value_of(lhs_i == rhs_i);
        case BinaryOp::Ne:
          return Value::bool_value_of(lhs_i != rhs_i);
        case BinaryOp::Lt:
          return Value::bool_value_of(lhs_i < rhs_i);
        case BinaryOp::Lte:
          return Value::bool_value_of(lhs_i <= rhs_i);
        case BinaryOp::Gt:
          return Value::bool_value_of(lhs_i > rhs_i);
        case BinaryOp::Gte:
          return Value::bool_value_of(lhs_i >= rhs_i);
        case BinaryOp::Div:
        case BinaryOp::Pow:
        case BinaryOp::And:
        case BinaryOp::Or:
          // Keep canonical promotion/semantics paths.
          return eval_numeric_binary_value(op, lhs, rhs);
      }
      break;
    }
    case BinaryPicRoute::NumericLowFloat: {
      const auto kind = lhs.numeric_value->kind;
      if (is_numeric_pic_arith_operator(op)) {
        auto& target = numeric_pic_scratch(kind);
        if (eval_numeric_binary_value_inplace(op, lhs, rhs, target)) {
          return target;
        }
      }
      const auto lhs_scalar = read_numeric_scalar_pic(lhs);
      const auto rhs_scalar = read_numeric_scalar_pic(rhs);
      const bool as_f64 = kind == Value::NumericKind::F64;
      switch (op) {
        case BinaryOp::Add:
          return as_f64
                     ? make_low_float_numeric_value(
                           kind,
                           static_cast<long double>(static_cast<double>(lhs_scalar) +
                                                    static_cast<double>(rhs_scalar)))
                     : make_low_float_numeric_value(
                           kind,
                           static_cast<long double>(static_cast<float>(lhs_scalar) +
                                                    static_cast<float>(rhs_scalar)));
        case BinaryOp::Sub:
          return as_f64
                     ? make_low_float_numeric_value(
                           kind,
                           static_cast<long double>(static_cast<double>(lhs_scalar) -
                                                    static_cast<double>(rhs_scalar)))
                     : make_low_float_numeric_value(
                           kind,
                           static_cast<long double>(static_cast<float>(lhs_scalar) -
                                                    static_cast<float>(rhs_scalar)));
        case BinaryOp::Mul:
          if (lhs_scalar == 0.0L || rhs_scalar == 0.0L) {
            return make_low_float_numeric_value(kind, 0.0L);
          }
          return as_f64
                     ? make_low_float_numeric_value(
                           kind,
                           static_cast<long double>(static_cast<double>(lhs_scalar) *
                                                    static_cast<double>(rhs_scalar)))
                     : make_low_float_numeric_value(
                           kind,
                           static_cast<long double>(static_cast<float>(lhs_scalar) *
                                                    static_cast<float>(rhs_scalar)));
        case BinaryOp::Div:
          if (rhs_scalar == 0.0L) {
            throw EvalException("division by zero");
          }
          if (lhs_scalar == 0.0L) {
            return make_low_float_numeric_value(kind, 0.0L);
          }
          return as_f64
                     ? make_low_float_numeric_value(
                           kind,
                           static_cast<long double>(static_cast<double>(lhs_scalar) /
                                                    static_cast<double>(rhs_scalar)))
                     : make_low_float_numeric_value(
                           kind,
                           static_cast<long double>(static_cast<float>(lhs_scalar) /
                                                    static_cast<float>(rhs_scalar)));
        case BinaryOp::Mod:
          if (rhs_scalar == 0.0L) {
            throw EvalException("modulo by zero");
          }
          if (lhs_scalar == 0.0L) {
            return make_low_float_numeric_value(kind, 0.0L);
          }
          if (std::isfinite(lhs_scalar) && std::isfinite(rhs_scalar) &&
              std::fabs(lhs_scalar) < std::fabs(rhs_scalar)) {
            return make_low_float_numeric_value(kind, lhs_scalar);
          }
          return as_f64
                     ? make_low_float_numeric_value(
                           kind,
                           static_cast<long double>(
                               fast_mod_pic(static_cast<double>(lhs_scalar),
                                            static_cast<double>(rhs_scalar))))
                     : make_low_float_numeric_value(
                           kind,
                           static_cast<long double>(
                               fast_mod_pic(static_cast<float>(lhs_scalar),
                                            static_cast<float>(rhs_scalar))));
        case BinaryOp::Pow:
          return as_f64
                     ? make_low_float_numeric_value(
                           kind,
                           static_cast<long double>(
                               std::pow(static_cast<double>(lhs_scalar),
                                        static_cast<double>(rhs_scalar))))
                     : make_low_float_numeric_value(
                           kind,
                           static_cast<long double>(
                               std::pow(static_cast<float>(lhs_scalar),
                                        static_cast<float>(rhs_scalar))));
        case BinaryOp::Eq:
          return as_f64
                     ? Value::bool_value_of(static_cast<double>(lhs_scalar) ==
                                            static_cast<double>(rhs_scalar))
                     : Value::bool_value_of(static_cast<float>(lhs_scalar) ==
                                            static_cast<float>(rhs_scalar));
        case BinaryOp::Ne:
          return as_f64
                     ? Value::bool_value_of(static_cast<double>(lhs_scalar) !=
                                            static_cast<double>(rhs_scalar))
                     : Value::bool_value_of(static_cast<float>(lhs_scalar) !=
                                            static_cast<float>(rhs_scalar));
        case BinaryOp::Lt:
          return as_f64
                     ? Value::bool_value_of(static_cast<double>(lhs_scalar) <
                                            static_cast<double>(rhs_scalar))
                     : Value::bool_value_of(static_cast<float>(lhs_scalar) <
                                            static_cast<float>(rhs_scalar));
        case BinaryOp::Lte:
          return as_f64
                     ? Value::bool_value_of(static_cast<double>(lhs_scalar) <=
                                            static_cast<double>(rhs_scalar))
                     : Value::bool_value_of(static_cast<float>(lhs_scalar) <=
                                            static_cast<float>(rhs_scalar));
        case BinaryOp::Gt:
          return as_f64
                     ? Value::bool_value_of(static_cast<double>(lhs_scalar) >
                                            static_cast<double>(rhs_scalar))
                     : Value::bool_value_of(static_cast<float>(lhs_scalar) >
                                            static_cast<float>(rhs_scalar));
        case BinaryOp::Gte:
          return as_f64
                     ? Value::bool_value_of(static_cast<double>(lhs_scalar) >=
                                            static_cast<double>(rhs_scalar))
                     : Value::bool_value_of(static_cast<float>(lhs_scalar) >=
                                            static_cast<float>(rhs_scalar));
        default:
          break;
      }
      break;
    }
    case BinaryPicRoute::NumericHighPrecision: {
      if (!is_numeric_pic_operator(op)) {
        break;
      }
      if (op == BinaryOp::Eq || op == BinaryOp::Ne || op == BinaryOp::Lt ||
          op == BinaryOp::Lte || op == BinaryOp::Gt || op == BinaryOp::Gte) {
        return eval_numeric_binary_value(op, lhs, rhs);
      }
      auto& target = numeric_pic_scratch(lhs.numeric_value->kind);
      if (eval_numeric_binary_value_inplace(op, lhs, rhs, target)) {
        return target;
      }
      return eval_numeric_binary_value(op, lhs, rhs);
    }
    case BinaryPicRoute::None:
      break;
  }
  throw EvalException("unsupported binary PIC route");
}

std::uint64_t mix_u64(std::uint64_t x) {
  x ^= x >> 30U;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27U;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31U;
  return x;
}

std::uint64_t combine_hash_u64(std::uint64_t h, std::uint64_t v) {
  return mix_u64(h ^ (v + 0x9e3779b97f4a7c15ULL + (h << 6U) + (h >> 2U)));
}

std::uint64_t value_stamp_for_pic(const Value& value, const Value* identity_ptr) {
  std::uint64_t h = mix_u64(static_cast<std::uint64_t>(value.kind));
  if (identity_ptr) {
    h = combine_hash_u64(h, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(identity_ptr)));
  }
  switch (value.kind) {
    case Value::Kind::Int:
      h = combine_hash_u64(h, static_cast<std::uint64_t>(value.int_value));
      break;
    case Value::Kind::Double: {
      std::uint64_t bits = 0;
      std::memcpy(&bits, &value.double_value, sizeof(bits));
      h = combine_hash_u64(h, bits);
      break;
    }
    case Value::Kind::Numeric:
      if (value.numeric_value) {
        const auto& numeric = *value.numeric_value;
        h = combine_hash_u64(h, static_cast<std::uint64_t>(numeric.kind));
        h = combine_hash_u64(h, numeric.revision);
        if (!identity_ptr) {
          if (numeric.parsed_int_valid) {
            const auto int_low = static_cast<std::uint64_t>(numeric.parsed_int);
            const auto int_high = static_cast<std::uint64_t>(numeric.parsed_int >> 64U);
            h = combine_hash_u64(h, int_low);
            h = combine_hash_u64(h, int_high);
          } else if (numeric.parsed_float_valid) {
            const auto as_double = static_cast<double>(numeric.parsed_float);
            std::uint64_t bits = 0;
            std::memcpy(&bits, &as_double, sizeof(bits));
            h = combine_hash_u64(h, bits);
          } else if (!numeric.payload.empty()) {
            h = combine_hash_u64(h, static_cast<std::uint64_t>(std::hash<std::string>{}(numeric.payload)));
          }
        }
      }
      break;
    default:
      break;
  }
  return h;
}

bool try_eval_binary_pic(const BinaryExpr& binary, std::uint64_t env_id,
                         const Value& lhs, const Value& rhs, Value& out,
                         const Value* lhs_identity = nullptr,
                         const Value* rhs_identity = nullptr) {
  if (!binary_pic_enabled() || !is_numeric_pic_operator(binary.op)) {
    return false;
  }
  if (!is_int_or_double_value(lhs) || !is_int_or_double_value(rhs)) {
    const bool same_numeric_kind =
        lhs.kind == Value::Kind::Numeric && rhs.kind == Value::Kind::Numeric &&
        lhs.numeric_value && rhs.numeric_value &&
        lhs.numeric_value->kind == rhs.numeric_value->kind;
    if (!same_numeric_kind) {
      return false;
    }
    const auto kind = lhs.numeric_value->kind;
    if (!(numeric_kind_is_int(kind) || is_low_float_numeric_kind(kind) ||
          numeric_kind_is_high_precision_float(kind))) {
      return false;
    }
  }

  const auto lhs_sig = value_kind_signature(lhs);
  const auto rhs_sig = value_kind_signature(rhs);
  auto& slot = binary_pic_cache_slot(&binary, env_id, binary.op, lhs_sig, rhs_sig);

  const bool slot_match =
      slot.expr == &binary && slot.env_id == env_id && slot.op == binary.op &&
      slot.lhs_sig == lhs_sig && slot.rhs_sig == rhs_sig;
  if (!slot_match) {
    slot.expr = &binary;
    slot.env_id = env_id;
    slot.op = binary.op;
    slot.lhs_sig = lhs_sig;
    slot.rhs_sig = rhs_sig;
    slot.hits = 0;
    slot.route = BinaryPicRoute::None;
    slot.memo_valid = false;
  }

  if (slot.hits < std::numeric_limits<std::uint32_t>::max()) {
    ++slot.hits;
  }

  if (slot.route == BinaryPicRoute::None && slot.hits >= static_cast<std::uint32_t>(binary_pic_warmup_threshold())) {
    slot.route = choose_binary_pic_route(binary.op, lhs, rhs);
    if (binary_pic_trace_enabled() && slot.route != BinaryPicRoute::None) {
      std::fprintf(stderr,
                   "[spark-binary-pic] quickened expr=%p op=%d lhs_sig=%u rhs_sig=%u route=%u hits=%u\n",
                   static_cast<const void*>(&binary), static_cast<int>(binary.op),
                   static_cast<unsigned>(lhs_sig), static_cast<unsigned>(rhs_sig),
                   static_cast<unsigned>(slot.route), static_cast<unsigned>(slot.hits));
    }
  }

  if (slot.route == BinaryPicRoute::None) {
    return false;
  }

  // For cheap scalar routes, stamp/memo bookkeeping costs more than the op.
  // Keep memoization only for high-precision route where op cost dominates.
  if (slot.route != BinaryPicRoute::NumericHighPrecision) {
    out = execute_binary_pic_route(slot.route, binary.op, lhs, rhs);
    return true;
  }

  const auto lhs_stamp = value_stamp_for_pic(lhs, lhs_identity);
  const auto rhs_stamp = value_stamp_for_pic(rhs, rhs_identity);
  if (slot.memo_valid && slot.lhs_stamp == lhs_stamp && slot.rhs_stamp == rhs_stamp) {
    out = slot.memo_value;
    return true;
  }

  out = execute_binary_pic_route(slot.route, binary.op, lhs, rhs);
  slot.memo_valid = true;
  slot.lhs_stamp = lhs_stamp;
  slot.rhs_stamp = rhs_stamp;
  slot.memo_value = out;
  return true;
}

const Value* get_var_ref_cached(const Expr* expr, const std::shared_ptr<Environment>& env) {
  if (!expr || expr->kind != Expr::Kind::Variable) {
    return nullptr;
  }

  struct VarRefCacheEntry {
    const Expr* expr = nullptr;
    std::uint64_t env_id = 0;
    const Value* value = nullptr;
  };
  constexpr std::size_t kVarRefCacheSize = 1024;
  static thread_local std::array<VarRefCacheEntry, kVarRefCacheSize> kVarRefCache{};

  const auto key_expr = expr;
  const auto key_env = env->stable_id;
  const auto raw_hash =
      static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(key_expr)) ^
      (static_cast<std::size_t>(key_env) * 11400714819323198485ull);
  auto& slot = kVarRefCache[raw_hash & (kVarRefCacheSize - 1)];
  if (slot.expr == key_expr && slot.env_id == key_env && slot.value != nullptr) {
    return slot.value;
  }

  const auto& variable = static_cast<const VariableExpr&>(*expr);
  const auto* value = env->get_ptr(variable.name);
  if (value) {
    slot.expr = key_expr;
    slot.env_id = key_env;
    slot.value = value;
  }
  return value;
}

bool is_container_arith_op(const BinaryOp op) {
  return op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul ||
         op == BinaryOp::Div || op == BinaryOp::Mod || op == BinaryOp::Pow;
}

double apply_scalar_binary(const BinaryOp op, const double lhs, const double rhs) {
  switch (op) {
    case BinaryOp::Add:
      return lhs + rhs;
    case BinaryOp::Sub:
      return lhs - rhs;
    case BinaryOp::Mul:
      return lhs * rhs;
    case BinaryOp::Div:
      if (rhs == 0.0) {
        throw EvalException("division by zero");
      }
      return lhs / rhs;
    case BinaryOp::Mod:
      if (rhs == 0.0) {
        throw EvalException("modulo by zero");
      }
      return std::fmod(lhs, rhs);
    case BinaryOp::Pow:
      return std::pow(lhs, rhs);
    case BinaryOp::Eq:
    case BinaryOp::Ne:
    case BinaryOp::Lt:
    case BinaryOp::Lte:
    case BinaryOp::Gt:
    case BinaryOp::Gte:
    case BinaryOp::And:
    case BinaryOp::Or:
      break;
  }
  throw EvalException("unsupported fused arithmetic operator");
}

bool value_is_numeric_scalar(const Value& value) {
  return value.kind == Value::Kind::Int || value.kind == Value::Kind::Double ||
         value.kind == Value::Kind::Numeric;
}

double value_to_scalar_double(const Value& value) {
  if (value.kind == Value::Kind::Int) {
    return static_cast<double>(value.int_value);
  }
  if (value.kind == Value::Kind::Double) {
    return value.double_value;
  }
  if (value.kind == Value::Kind::Numeric) {
    return numeric_value_to_double(value);
  }
  throw EvalException("expected numeric scalar");
}

enum class DenseContainerKind { List, Matrix };

struct DenseContainer {
  const Value* value = nullptr;
  const std::vector<double>* dense = nullptr;
  std::vector<double> scratch;
};

struct FusedNode {
  enum class Kind { Constant, Container, Binary };

  Kind kind = Kind::Constant;
  double constant = 0.0;
  std::size_t container_index = 0;
  BinaryOp op = BinaryOp::Add;
  int left = -1;
  int right = -1;
};

struct BuildResult {
  bool ok = false;
  bool produces_container = false;
  int node_index = -1;
};

struct BuildContext {
  std::optional<DenseContainerKind> container_kind;
  std::vector<DenseContainer> containers;
  std::vector<FusedNode> nodes;
  std::unordered_map<std::string, std::size_t> container_index_by_name;
  bool list_add_elementwise = false;
};

int append_constant(BuildContext& context, const double value) {
  FusedNode node;
  node.kind = FusedNode::Kind::Constant;
  node.constant = value;
  context.nodes.push_back(node);
  return static_cast<int>(context.nodes.size() - 1);
}

int append_container(BuildContext& context, const std::size_t index) {
  FusedNode node;
  node.kind = FusedNode::Kind::Container;
  node.container_index = index;
  context.nodes.push_back(node);
  return static_cast<int>(context.nodes.size() - 1);
}

int append_binary(BuildContext& context, const BinaryOp op, const int left, const int right) {
  FusedNode node;
  node.kind = FusedNode::Kind::Binary;
  node.op = op;
  node.left = left;
  node.right = right;
  context.nodes.push_back(node);
  return static_cast<int>(context.nodes.size() - 1);
}

bool node_is_constant(const BuildContext& context, const int index, double& out) {
  if (index < 0 || static_cast<std::size_t>(index) >= context.nodes.size()) {
    return false;
  }
  const auto& node = context.nodes[static_cast<std::size_t>(index)];
  if (node.kind != FusedNode::Kind::Constant) {
    return false;
  }
  out = node.constant;
  return true;
}

BuildResult build_fused_tree(const Expr& expr, Interpreter& self,
                             const std::shared_ptr<Environment>& env,
                             BuildContext& context) {
  switch (expr.kind) {
    case Expr::Kind::Number: {
      const auto& number = static_cast<const NumberExpr&>(expr);
      return BuildResult{
          .ok = true,
          .produces_container = false,
          .node_index = append_constant(context, number.value),
      };
    }
    case Expr::Kind::Variable: {
      const auto& variable = static_cast<const VariableExpr&>(expr);
      const auto* value = env ? env->get_ptr(variable.name) : nullptr;
      if (!value) {
        throw EvalException("undefined variable: " + variable.name);
      }
      if (value_is_numeric_scalar(*value)) {
        return BuildResult{
            .ok = true,
            .produces_container = false,
            .node_index = append_constant(context, value_to_scalar_double(*value)),
        };
      }
      if (value->kind != Value::Kind::List && value->kind != Value::Kind::Matrix) {
        return BuildResult{};
      }

      const auto kind = (value->kind == Value::Kind::List) ? DenseContainerKind::List
                                                            : DenseContainerKind::Matrix;
      if (!context.container_kind.has_value()) {
        context.container_kind = kind;
      } else if (context.container_kind != kind) {
        return BuildResult{};
      }

      auto it = context.container_index_by_name.find(variable.name);
      std::size_t index = 0;
      if (it != context.container_index_by_name.end()) {
        index = it->second;
      } else {
        index = context.containers.size();
        context.containers.push_back(DenseContainer{.value = value});
        context.container_index_by_name.emplace(variable.name, index);
      }
      return BuildResult{
          .ok = true,
          .produces_container = true,
          .node_index = append_container(context, index),
      };
    }
    case Expr::Kind::Unary: {
      const auto& unary = static_cast<const UnaryExpr&>(expr);
      if (unary.op != UnaryOp::Neg) {
        return BuildResult{};
      }
      auto child = build_fused_tree(*unary.operand, self, env, context);
      if (!child.ok) {
        return BuildResult{};
      }
      if (!child.produces_container) {
        double value = 0.0;
        if (!node_is_constant(context, child.node_index, value)) {
          return BuildResult{};
        }
        return BuildResult{
            .ok = true,
            .produces_container = false,
            .node_index = append_constant(context, -value),
        };
      }

      const auto zero_index = append_constant(context, 0.0);
      return BuildResult{
          .ok = true,
          .produces_container = true,
          .node_index = append_binary(context, BinaryOp::Sub, zero_index, child.node_index),
      };
    }
    case Expr::Kind::Binary: {
      const auto& binary = static_cast<const BinaryExpr&>(expr);
      if (!is_container_arith_op(binary.op)) {
        return BuildResult{};
      }

      auto left = build_fused_tree(*binary.left, self, env, context);
      if (!left.ok) {
        return BuildResult{};
      }
      auto right = build_fused_tree(*binary.right, self, env, context);
      if (!right.ok) {
        return BuildResult{};
      }

      if (!left.produces_container && !right.produces_container) {
        double lhs = 0.0;
        double rhs = 0.0;
        if (!node_is_constant(context, left.node_index, lhs) ||
            !node_is_constant(context, right.node_index, rhs)) {
          return BuildResult{};
        }
        return BuildResult{
            .ok = true,
            .produces_container = false,
            .node_index = append_constant(context, apply_scalar_binary(binary.op, lhs, rhs)),
        };
      }

      if (!context.container_kind.has_value()) {
        return BuildResult{};
      }
      if (*context.container_kind == DenseContainerKind::List &&
          left.produces_container && right.produces_container &&
          binary.op == BinaryOp::Add && !context.list_add_elementwise) {
        return BuildResult{};
      }
      if (*context.container_kind == DenseContainerKind::Matrix &&
          left.produces_container && right.produces_container &&
          binary.op == BinaryOp::Mul) {
        // matrix * matrix means matmul in this language, not elementwise.
        return BuildResult{};
      }

      return BuildResult{
          .ok = true,
          .produces_container = true,
          .node_index = append_binary(context, binary.op, left.node_index, right.node_index),
      };
    }
    case Expr::Kind::String:
    case Expr::Kind::Bool:
    case Expr::Kind::List:
    case Expr::Kind::Attribute:
    case Expr::Kind::Call:
    case Expr::Kind::Index:
      break;
  }
  return BuildResult{};
}

std::size_t list_size_for_dense(const Value& value) {
  if (value.kind != Value::Kind::List) {
    return 0;
  }
  if (!value.list_value.empty()) {
    return value.list_value.size();
  }
  if (value.list_cache.materialized_version == value.list_cache.version &&
      !value.list_cache.promoted_f64.empty()) {
    return value.list_cache.promoted_f64.size();
  }
  return 0;
}

const std::vector<double>* dense_list_if_materialized(const Value& value, const std::size_t expected_size) {
  if (value.kind != Value::Kind::List) {
    return nullptr;
  }
  const auto& cache = value.list_cache;
  const auto cache_ready =
      cache.materialized_version == cache.version &&
      (cache.plan == Value::LayoutTag::PackedDouble || cache.plan == Value::LayoutTag::PromotedPackedDouble);
  if (!cache_ready) {
    return nullptr;
  }
  if (cache.promoted_f64.size() != expected_size) {
    return nullptr;
  }
  return &cache.promoted_f64;
}

const std::vector<double>* dense_matrix_if_materialized(const Value& value, const std::size_t expected_size) {
  if (value.kind != Value::Kind::Matrix || !value.matrix_value) {
    return nullptr;
  }
  const auto& cache = value.matrix_cache;
  const auto cache_ready =
      cache.materialized_version == cache.version &&
      cache.plan == Value::LayoutTag::PackedDouble;
  if (!cache_ready) {
    return nullptr;
  }
  if (cache.promoted_f64.size() != expected_size) {
    return nullptr;
  }
  return &cache.promoted_f64;
}

bool is_integer_like(const double value) {
  constexpr double kTol = 1e-12;
  const auto rounded = std::llround(value);
  return std::fabs(value - static_cast<double>(rounded)) <= kTol;
}

bool matrix_source_is_integral(const Value& value) {
  if (value.kind != Value::Kind::Matrix || !value.matrix_value) {
    return false;
  }
  const auto total = value.matrix_value->rows * value.matrix_value->cols;
  if (const auto* dense = dense_matrix_if_materialized(value, total)) {
    for (const auto entry : *dense) {
      if (!is_integer_like(entry)) {
        return false;
      }
    }
    return true;
  }
  const auto& data = value.matrix_value->data;
  if (data.size() != total) {
    return false;
  }
  for (const auto& cell : data) {
    if (cell.kind == Value::Kind::Int) {
      continue;
    }
    if (cell.kind == Value::Kind::Double && is_integer_like(cell.double_value)) {
      continue;
    }
    return false;
  }
  return true;
}

bool fused_matrix_prefers_int_output(const BuildContext& context) {
  for (const auto& node : context.nodes) {
    if (node.kind == FusedNode::Kind::Binary &&
        (node.op == BinaryOp::Div || node.op == BinaryOp::Mod || node.op == BinaryOp::Pow)) {
      return false;
    }
    if (node.kind == FusedNode::Kind::Constant && !is_integer_like(node.constant)) {
      return false;
    }
  }
  for (const auto& container : context.containers) {
    if (!container.value || !matrix_source_is_integral(*container.value)) {
      return false;
    }
  }
  return true;
}

Value make_list_from_dense(std::vector<double>&& dense, const std::optional<double> precomputed_sum) {
  const auto size = dense.size();
  const bool dense_only_enabled = env_bool_enabled_binary_expr("SPARK_LIST_OPS_DENSE_ONLY", false);
  std::size_t dense_only_min = 32u * 1024u;
  if (const auto* min_env = std::getenv("SPARK_LIST_OPS_DENSE_ONLY_MIN")) {
    const auto parsed = std::strtoull(min_env, nullptr, 10);
    if (parsed > 0) {
      dense_only_min = static_cast<std::size_t>(parsed);
    }
  }

  std::vector<Value> out_data;
  if (!(dense_only_enabled && size >= dense_only_min)) {
    out_data.resize(size);
    for (std::size_t i = 0; i < size; ++i) {
      out_data[i] = Value::double_value_of(dense[i]);
    }
  }

  auto out = Value::list_value_of(std::move(out_data));
  out.list_cache.live_plan = true;
  out.list_cache.plan = Value::LayoutTag::PackedDouble;
  out.list_cache.operation = "binary_fused";
  out.list_cache.analyzed_version = out.list_cache.version;
  out.list_cache.materialized_version = out.list_cache.version;
  out.list_cache.promoted_f64 = std::move(dense);
  if (precomputed_sum.has_value()) {
    out.list_cache.reduced_sum_version = out.list_cache.version;
    out.list_cache.reduced_sum_value = *precomputed_sum;
    out.list_cache.reduced_sum_is_int = false;
  } else {
    out.list_cache.reduced_sum_version = std::numeric_limits<std::uint64_t>::max();
    out.list_cache.reduced_sum_value = 0.0;
    out.list_cache.reduced_sum_is_int = false;
  }
  return out;
}

Value make_matrix_from_dense(std::size_t rows, std::size_t cols, std::vector<double>&& dense,
                             const std::optional<double> precomputed_sum) {
  const auto total = rows * cols;
  const bool dense_only_enabled = env_bool_enabled_binary_expr("SPARK_MATRIX_OPS_DENSE_ONLY", false);
  std::size_t dense_only_min = 16u * 1024u;
  if (const auto* min_env = std::getenv("SPARK_MATRIX_OPS_DENSE_ONLY_MIN")) {
    const auto parsed = std::strtoull(min_env, nullptr, 10);
    if (parsed > 0) {
      dense_only_min = static_cast<std::size_t>(parsed);
    }
  }

  std::vector<Value> out_data;
  if (!(dense_only_enabled && total >= dense_only_min)) {
    out_data.resize(total);
    for (std::size_t i = 0; i < total; ++i) {
      out_data[i] = Value::double_value_of(dense[i]);
    }
  }

  auto out = Value::matrix_value_of(rows, cols, std::move(out_data));
  out.matrix_cache.plan = Value::LayoutTag::PackedDouble;
  out.matrix_cache.live_plan = true;
  out.matrix_cache.operation = "binary_fused";
  out.matrix_cache.analyzed_version = out.matrix_cache.version;
  out.matrix_cache.materialized_version = out.matrix_cache.version;
  if (precomputed_sum.has_value()) {
    out.matrix_cache.reduced_sum_version = out.matrix_cache.version;
    out.matrix_cache.reduced_sum_value = *precomputed_sum;
    out.matrix_cache.reduced_sum_is_int = false;
  } else {
    out.matrix_cache.reduced_sum_version = std::numeric_limits<std::uint64_t>::max();
    out.matrix_cache.reduced_sum_value = 0.0;
    out.matrix_cache.reduced_sum_is_int = false;
  }
  out.matrix_cache.promoted_f64 = std::move(dense);
  return out;
}

std::optional<Value> try_fused_container_eval(const BinaryExpr& binary, Interpreter& self,
                                              const std::shared_ptr<Environment>& env) {
  BuildContext context;
  context.list_add_elementwise = env_bool_enabled_binary_expr("SPARK_LIST_ADD_ELEMENTWISE", false);
  auto root = build_fused_tree(binary, self, env, context);
  if (!root.ok || !root.produces_container || context.containers.empty() ||
      !context.container_kind.has_value()) {
    return std::nullopt;
  }

  std::size_t total = 0;
  std::size_t rows = 0;
  std::size_t cols = 0;

  if (*context.container_kind == DenseContainerKind::List) {
    const auto* base = context.containers.front().value;
    if (!base) {
      return std::nullopt;
    }
    total = list_size_for_dense(*base);
    for (auto& container : context.containers) {
      if (!container.value || container.value->kind != Value::Kind::List) {
        return std::nullopt;
      }
      const auto size = list_size_for_dense(*container.value);
      if (size != total) {
        throw EvalException("list elementwise arithmetic expects equal sizes");
      }
      if (const auto* dense = dense_list_if_materialized(*container.value, total)) {
        container.dense = dense;
        continue;
      }
      if (container.value->list_value.size() != total) {
        return std::nullopt;
      }
      container.scratch.resize(total);
      for (std::size_t i = 0; i < total; ++i) {
        const auto& item = container.value->list_value[i];
        if (!value_is_numeric_scalar(item)) {
          return std::nullopt;
        }
        container.scratch[i] = value_to_scalar_double(item);
      }
      container.dense = &container.scratch;
    }
  } else {
    const auto* base = context.containers.front().value;
    if (!base || !base->matrix_value) {
      return std::nullopt;
    }
    rows = base->matrix_value->rows;
    cols = base->matrix_value->cols;
    total = rows * cols;
    for (auto& container : context.containers) {
      if (!container.value || container.value->kind != Value::Kind::Matrix ||
          !container.value->matrix_value) {
        return std::nullopt;
      }
      if (container.value->matrix_value->rows != rows || container.value->matrix_value->cols != cols) {
        throw EvalException("matrix shapes must match for elementwise arithmetic");
      }
      if (const auto* dense = dense_matrix_if_materialized(*container.value, total)) {
        container.dense = dense;
        continue;
      }
      const auto& data = container.value->matrix_value->data;
      if (data.size() != total) {
        throw EvalException("matrix arithmetic requires materialized matrix payload");
      }
      container.scratch.resize(total);
      for (std::size_t i = 0; i < total; ++i) {
        if (!value_is_numeric_scalar(data[i])) {
          return std::nullopt;
        }
        container.scratch[i] = value_to_scalar_double(data[i]);
      }
      container.dense = &container.scratch;
    }
  }

  std::vector<double> stack(context.nodes.size(), 0.0);
  std::vector<int> dynamic_nodes;
  dynamic_nodes.reserve(context.nodes.size());
  for (std::size_t idx = 0; idx < context.nodes.size(); ++idx) {
    const auto& node = context.nodes[idx];
    if (node.kind == FusedNode::Kind::Constant) {
      stack[idx] = node.constant;
      continue;
    }
    dynamic_nodes.push_back(static_cast<int>(idx));
  }

  std::vector<double> out_dense(total, 0.0);
  double out_sum = 0.0;
  for (std::size_t i = 0; i < total; ++i) {
    for (const auto idx : dynamic_nodes) {
      const auto& node = context.nodes[static_cast<std::size_t>(idx)];
      if (node.kind == FusedNode::Kind::Container) {
        const auto* dense = context.containers[node.container_index].dense;
        if (!dense) {
          return std::nullopt;
        }
        stack[static_cast<std::size_t>(idx)] = (*dense)[i];
        continue;
      }
      const auto lhs = stack[static_cast<std::size_t>(node.left)];
      const auto rhs = stack[static_cast<std::size_t>(node.right)];
      stack[static_cast<std::size_t>(idx)] = apply_scalar_binary(node.op, lhs, rhs);
    }
    const auto value = stack[static_cast<std::size_t>(root.node_index)];
    out_dense[i] = value;
    out_sum += value;
  }

  if (*context.container_kind == DenseContainerKind::List) {
    return make_list_from_dense(std::move(out_dense), out_sum);
  }
  if (fused_matrix_prefers_int_output(context)) {
    std::vector<Value> out_data(total);
    for (std::size_t i = 0; i < total; ++i) {
      out_data[i] = Value::int_value_of(static_cast<long long>(std::llround(out_dense[i])));
    }
    return Value::matrix_value_of(rows, cols, std::move(out_data));
  }
  return make_matrix_from_dense(rows, cols, std::move(out_dense), out_sum);
}

}  // namespace

Value evaluate_case_binary(const BinaryExpr& binary, Interpreter& self,
                           const std::shared_ptr<Environment>& env) {
  if (env_bool_enabled_binary_expr("SPARK_BINARY_EXPR_FUSION", false) &&
      is_container_arith_op(binary.op)) {
    if (const auto fused = try_fused_container_eval(binary, self, env); fused.has_value()) {
      return *fused;
    }
  }

  const auto* lhs_ref = get_var_ref_cached(binary.left.get(), env);
  const auto* rhs_ref = get_var_ref_cached(binary.right.get(), env);
  const auto try_get_number_literal = [](const Expr* expr, Value& out) -> bool {
    if (!expr || expr->kind != Expr::Kind::Number) {
      return false;
    }
    const auto& number = static_cast<const NumberExpr&>(*expr);
    out = number.is_int ? Value::int_value_of(static_cast<long long>(number.value))
                        : Value::double_value_of(number.value);
    return true;
  };
  if (lhs_ref && rhs_ref) {
    if (lhs_ref->kind == Value::Kind::String && rhs_ref->kind == Value::Kind::String) {
      switch (binary.op) {
        case BinaryOp::Add: {
          std::string merged;
          merged.reserve(lhs_ref->string_value.size() + rhs_ref->string_value.size());
          merged.append(lhs_ref->string_value);
          merged.append(rhs_ref->string_value);
          return Value::string_value_of(std::move(merged));
        }
        case BinaryOp::Eq:
          return Value::bool_value_of(lhs_ref->string_value == rhs_ref->string_value);
        case BinaryOp::Ne:
          return Value::bool_value_of(lhs_ref->string_value != rhs_ref->string_value);
        case BinaryOp::Lt:
          return Value::bool_value_of(lhs_ref->string_value < rhs_ref->string_value);
        case BinaryOp::Lte:
          return Value::bool_value_of(lhs_ref->string_value <= rhs_ref->string_value);
        case BinaryOp::Gt:
          return Value::bool_value_of(lhs_ref->string_value > rhs_ref->string_value);
        case BinaryOp::Gte:
          return Value::bool_value_of(lhs_ref->string_value >= rhs_ref->string_value);
        default:
          break;
      }
    }

    if (lhs_ref->kind == Value::Kind::Numeric && rhs_ref->kind == Value::Kind::Numeric &&
        lhs_ref->numeric_value && rhs_ref->numeric_value &&
        lhs_ref->numeric_value->kind == rhs_ref->numeric_value->kind &&
        !numeric_kind_is_high_precision_float(lhs_ref->numeric_value->kind)) {
      switch (binary.op) {
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
          return eval_numeric_binary_value(binary.op, *lhs_ref, *rhs_ref);
        default:
          break;
      }
    }

    Value pic_value = Value::nil();
    if (try_eval_binary_pic(binary, env->stable_id, *lhs_ref, *rhs_ref, pic_value, lhs_ref, rhs_ref)) {
      return pic_value;
    }

    if (lhs_ref->kind == Value::Kind::Numeric && rhs_ref->kind == Value::Kind::Numeric) {
      switch (binary.op) {
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
          return eval_numeric_binary_value(binary.op, *lhs_ref, *rhs_ref);
        default:
          break;
      }
    }
    return self.eval_binary(binary.op, *lhs_ref, *rhs_ref);
  }

  if (lhs_ref) {
    Value rhs_number = Value::nil();
    if (try_get_number_literal(binary.right.get(), rhs_number)) {
      if (lhs_ref->kind == Value::Kind::Numeric && lhs_ref->numeric_value &&
          !numeric_kind_is_high_precision_float(lhs_ref->numeric_value->kind)) {
        switch (binary.op) {
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
            return eval_numeric_binary_value(binary.op, *lhs_ref, rhs_number);
          default:
            break;
        }
      }

      Value pic_value = Value::nil();
      if (try_eval_binary_pic(binary, env->stable_id, *lhs_ref, rhs_number, pic_value, lhs_ref, nullptr)) {
        return pic_value;
      }

      if (lhs_ref->kind == Value::Kind::Numeric) {
        switch (binary.op) {
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
            return eval_numeric_binary_value(binary.op, *lhs_ref, rhs_number);
          default:
            break;
        }
      }
      return self.eval_binary(binary.op, *lhs_ref, rhs_number);
    }
  }

  if (rhs_ref) {
    Value lhs_number = Value::nil();
    if (try_get_number_literal(binary.left.get(), lhs_number)) {
      if (rhs_ref->kind == Value::Kind::Numeric && rhs_ref->numeric_value &&
          !numeric_kind_is_high_precision_float(rhs_ref->numeric_value->kind)) {
        switch (binary.op) {
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
            return eval_numeric_binary_value(binary.op, lhs_number, *rhs_ref);
          default:
            break;
        }
      }

      Value pic_value = Value::nil();
      if (try_eval_binary_pic(binary, env->stable_id, lhs_number, *rhs_ref, pic_value, nullptr, rhs_ref)) {
        return pic_value;
      }

      if (rhs_ref->kind == Value::Kind::Numeric) {
        switch (binary.op) {
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
            return eval_numeric_binary_value(binary.op, lhs_number, *rhs_ref);
          default:
            break;
        }
      }
      return self.eval_binary(binary.op, lhs_number, *rhs_ref);
    }
  }

  auto lhs = self.evaluate(*binary.left, env);
  auto rhs = self.evaluate(*binary.right, env);

  if (lhs.kind == Value::Kind::Numeric && rhs.kind == Value::Kind::Numeric &&
      lhs.numeric_value && rhs.numeric_value &&
      lhs.numeric_value->kind == rhs.numeric_value->kind &&
      !numeric_kind_is_high_precision_float(lhs.numeric_value->kind)) {
    switch (binary.op) {
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
        return eval_numeric_binary_value(binary.op, lhs, rhs);
      default:
        break;
    }
  }

  Value pic_value = Value::nil();
  if (try_eval_binary_pic(binary, env->stable_id, lhs, rhs, pic_value)) {
    return pic_value;
  }

  return self.eval_binary(binary.op, lhs, rhs);
}

}  // namespace spark
