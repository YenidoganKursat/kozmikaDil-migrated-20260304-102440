#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <memory>
#include <vector>

#if defined(SPARK_HAS_MPFR)
#include <mpfr.h>
#endif

#include "phase3/evaluator_parts/internal_helpers.h"

namespace spark {

namespace {

using I128 = __int128_t;
using U128 = __uint128_t;

I128 i128_max() {
  return static_cast<I128>((~U128{0}) >> 1);
}

I128 i128_min() {
  return -i128_max() - 1;
}

std::string trim_decimal_string(std::string value) {
  const auto exp_pos = value.find_first_of("eE");
  std::string mantissa = exp_pos == std::string::npos ? value : value.substr(0, exp_pos);
  const std::string exponent = exp_pos == std::string::npos ? std::string() : value.substr(exp_pos);
  if (mantissa.find('.') == std::string::npos) {
    return value;
  }
  while (!mantissa.empty() && mantissa.back() == '0') {
    mantissa.pop_back();
  }
  if (!mantissa.empty() && mantissa.back() == '.') {
    mantissa.pop_back();
  }
  if (mantissa.empty() || mantissa == "-0") {
    return "0";
  }
  return mantissa + exponent;
}

std::string i128_to_string(I128 value) {
  if (value == 0) {
    return "0";
  }
  bool negative = value < 0;
  U128 magnitude = negative ? static_cast<U128>(-(value + 1)) + 1 : static_cast<U128>(value);
  std::string out;
  while (magnitude > 0) {
    const auto digit = static_cast<unsigned>(magnitude % 10);
    out.push_back(static_cast<char>('0' + digit));
    magnitude /= 10;
  }
  if (negative) {
    out.push_back('-');
  }
  std::reverse(out.begin(), out.end());
  return out;
}

long double parse_long_double(const std::string& text) {
  try {
    return std::stold(text);
  } catch (const std::exception&) {
    return 0.0L;
  }
}

bool parse_env_bool_numeric(const char* name, bool fallback) {
  return env_flag_enabled(name, fallback);
}

bool long_double_exact_double(long double value, double& out) {
  const double narrowed = static_cast<double>(value);
  if (static_cast<long double>(narrowed) == value) {
    out = narrowed;
    return true;
  }
  return false;
}

bool same_kind_short_path_enabled() {
  static const bool enabled = parse_env_bool_numeric("SPARK_NUMERIC_SAME_KIND_FASTPATH", true);
  return enabled;
}

bool mpfr_direct_kernel_enabled() {
  static const bool enabled = parse_env_bool_numeric("SPARK_MPFR_DIRECT_KERNEL", false);
  return enabled;
}

I128 parse_i128_decimal(const std::string& text) {
  std::size_t idx = 0;
  while (idx < text.size() && std::isspace(static_cast<unsigned char>(text[idx]))) {
    ++idx;
  }
  bool negative = false;
  if (idx < text.size() && (text[idx] == '-' || text[idx] == '+')) {
    negative = text[idx] == '-';
    ++idx;
  }

  const U128 max_positive = static_cast<U128>(i128_max());
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
      return i128_min();
    }
    return -static_cast<I128>(acc);
  }
  if (acc > max_positive) {
    return i128_max();
  }
  return static_cast<I128>(acc);
}

int bit_width_u64(unsigned long long value) {
  int bits = 0;
  while (value != 0ULL) {
    ++bits;
    value >>= 1U;
  }
  return bits == 0 ? 1 : bits;
}

int bit_width_i128_signed(I128 value) {
  U128 magnitude = 0;
  if (value < 0) {
    magnitude = static_cast<U128>(-(value + 1)) + 1U;
  } else {
    magnitude = static_cast<U128>(value);
  }
  int bits = 0;
  while (magnitude != 0U) {
    ++bits;
    magnitude >>= 1U;
  }
  return bits == 0 ? 1 : bits;
}

bool is_high_precision_float_kind_local(Value::NumericKind kind) {
  return kind == Value::NumericKind::F128 || kind == Value::NumericKind::F256 ||
         kind == Value::NumericKind::F512;
}

std::optional<long long> integral_exponent_if_safe(long double exponent) {
  if (!std::isfinite(static_cast<double>(exponent))) {
    return std::nullopt;
  }
  const long double rounded = std::nearbyint(exponent);
  if (std::fabs(exponent - rounded) > 1e-12L) {
    return std::nullopt;
  }
  constexpr long double kMaxMagnitude = 1'000'000.0L;
  if (std::fabs(rounded) > kMaxMagnitude) {
    return std::nullopt;
  }
  return static_cast<long long>(rounded);
}

long double powi_long_double(long double base, long long exponent) {
  if (exponent == 0) {
    return 1.0L;
  }
  if (base == 0.0L && exponent < 0) {
    return std::numeric_limits<long double>::infinity();
  }
  bool negative_exp = exponent < 0;
  auto exp_mag = static_cast<unsigned long long>(negative_exp ? -exponent : exponent);
  long double result = 1.0L;
  long double factor = base;
  while (exp_mag > 0) {
    if ((exp_mag & 1ULL) != 0ULL) {
      result *= factor;
    }
    exp_mag >>= 1ULL;
    if (exp_mag > 0) {
      factor *= factor;
    }
  }
  if (negative_exp) {
    return 1.0L / result;
  }
  return result;
}

double powi_double_numeric_core(double base, long long exponent) {
  if (exponent == 0) {
    return 1.0;
  }
  if (base == 0.0 && exponent < 0) {
    return std::numeric_limits<double>::infinity();
  }
  bool negative_exp = exponent < 0;
  auto exp_mag = static_cast<unsigned long long>(negative_exp ? -exponent : exponent);
  double result = 1.0;
  double factor = base;
  while (exp_mag > 0) {
    if ((exp_mag & 1ULL) != 0ULL) {
      result *= factor;
    }
    exp_mag >>= 1ULL;
    if (exp_mag > 0) {
      factor *= factor;
    }
  }
  return negative_exp ? (1.0 / result) : result;
}

float powi_float_numeric_core(float base, long long exponent) {
  if (exponent == 0) {
    return 1.0F;
  }
  if (base == 0.0F && exponent < 0) {
    return std::numeric_limits<float>::infinity();
  }
  bool negative_exp = exponent < 0;
  auto exp_mag = static_cast<unsigned long long>(negative_exp ? -exponent : exponent);
  float result = 1.0F;
  float factor = base;
  while (exp_mag > 0) {
    if ((exp_mag & 1ULL) != 0ULL) {
      result *= factor;
    }
    exp_mag >>= 1ULL;
    if (exp_mag > 0) {
      factor *= factor;
    }
  }
  return negative_exp ? (1.0F / result) : result;
}

template <typename T>
T fast_fmod_scalar(T x, T y) {
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

template <typename T>
bool try_eval_float_binary_fast(BinaryOp op, T lhs, T rhs, T& out) {
  if ((op == BinaryOp::Div || op == BinaryOp::Mod) && rhs == static_cast<T>(0)) {
    throw EvalException(op == BinaryOp::Div ? "division by zero" : "modulo by zero");
  }

  const bool finite_numbers = std::isfinite(lhs) && std::isfinite(rhs);
  switch (op) {
    case BinaryOp::Add:
      if (finite_numbers) {
        if (rhs == static_cast<T>(0)) {
          out = lhs;
          return true;
        }
        if (lhs == static_cast<T>(0)) {
          out = rhs;
          return true;
        }
      }
      out = lhs + rhs;
      return true;
    case BinaryOp::Sub:
      if (finite_numbers) {
        if (rhs == static_cast<T>(0)) {
          out = lhs;
          return true;
        }
        if (lhs == rhs) {
          out = static_cast<T>(0);
          return true;
        }
      }
      out = lhs - rhs;
      return true;
    case BinaryOp::Mul:
      if (finite_numbers) {
        if (lhs == static_cast<T>(0) || rhs == static_cast<T>(0)) {
          out = static_cast<T>(0);
          return true;
        }
        if (rhs == static_cast<T>(1)) {
          out = lhs;
          return true;
        }
        if (lhs == static_cast<T>(1)) {
          out = rhs;
          return true;
        }
        if (rhs == static_cast<T>(-1)) {
          out = -lhs;
          return true;
        }
        if (lhs == static_cast<T>(-1)) {
          out = -rhs;
          return true;
        }
      }
      out = lhs * rhs;
      return true;
    case BinaryOp::Div:
      if (finite_numbers) {
        if (lhs == static_cast<T>(0)) {
          out = static_cast<T>(0);
          return true;
        }
        if (rhs == static_cast<T>(1)) {
          out = lhs;
          return true;
        }
        if (rhs == static_cast<T>(-1)) {
          out = -lhs;
          return true;
        }
      }
      out = lhs / rhs;
      return true;
    case BinaryOp::Mod:
      if (finite_numbers) {
        if (lhs == static_cast<T>(0)) {
          out = std::copysign(static_cast<T>(0), lhs);
          return true;
        }
        if (std::fabs(lhs) < std::fabs(rhs)) {
          out = lhs;
          return true;
        }
      }
      out = fast_fmod_scalar(lhs, rhs);
      return true;
    default:
      break;
  }
  return false;
}

#if defined(SPARK_HAS_MPFR)
mpfr_prec_t mpfr_precision_for_kind(Value::NumericKind kind) {
  switch (kind) {
    case Value::NumericKind::F128:
      return 113;
    case Value::NumericKind::F256:
      return 237;
    case Value::NumericKind::F512:
      return 493;
    default:
      return 64;
  }
}

int mpfr_decimal_digits_for_kind(Value::NumericKind kind) {
  const auto bits = static_cast<double>(mpfr_precision_for_kind(kind));
  const auto digits = static_cast<int>(std::ceil(bits * 0.3010299956639812));
  return std::max(20, digits + 2);
}

struct MpfrValue {
  explicit MpfrValue(mpfr_prec_t precision) {
    mpfr_init2(value, precision);
  }
  ~MpfrValue() {
    mpfr_clear(value);
  }
  mpfr_t value;
};

struct MpfrNumericCache {
  explicit MpfrNumericCache(mpfr_prec_t precision_bits)
      : precision(precision_bits) {
    mpfr_init2(value, precision);
    mpfr_set_ui(value, 0U, MPFR_RNDN);
  }
  ~MpfrNumericCache() {
    mpfr_clear(value);
  }

  mpfr_prec_t precision = 64;
  bool populated = false;
  std::uint64_t epoch = 1;
  mpfr_t value;
};

using MpfrNumericCachePtr = std::shared_ptr<MpfrNumericCache>;

struct MpfrCachePool {
  std::vector<MpfrNumericCache*> p113;
  std::vector<MpfrNumericCache*> p237;
  std::vector<MpfrNumericCache*> p493;

  ~MpfrCachePool() {
    auto release_all = [](std::vector<MpfrNumericCache*>& free_list) {
      for (auto* entry : free_list) {
        delete entry;
      }
      free_list.clear();
    };
    release_all(p113);
    release_all(p237);
    release_all(p493);
  }
};

MpfrCachePool& mpfr_cache_pool() {
  thread_local MpfrCachePool pool;
  return pool;
}

std::vector<MpfrNumericCache*>& mpfr_cache_freelist(mpfr_prec_t precision) {
  auto& pool = mpfr_cache_pool();
  if (precision <= 113) {
    return pool.p113;
  }
  if (precision <= 237) {
    return pool.p237;
  }
  return pool.p493;
}

MpfrNumericCachePtr acquire_mpfr_cache(mpfr_prec_t precision) {
  auto& free_list = mpfr_cache_freelist(precision);
  MpfrNumericCache* entry = nullptr;
  if (!free_list.empty()) {
    entry = free_list.back();
    free_list.pop_back();
  } else {
    entry = new MpfrNumericCache(precision);
  }
  entry->populated = false;
  entry->epoch = 1;

  return MpfrNumericCachePtr(entry, [precision](MpfrNumericCache* cache) {
    if (!cache) {
      return;
    }
    cache->populated = false;
    cache->epoch = 1;
    auto& release_list = mpfr_cache_freelist(precision);
    release_list.push_back(cache);
  });
}

std::string mpfr_value_to_decimal_string(const mpfr_t value, Value::NumericKind kind) {
  char* output = nullptr;
  const int digits = mpfr_decimal_digits_for_kind(kind);
  mpfr_asprintf(&output, "%.*Rg", digits, value);
  const std::string text = output ? std::string(output) : std::string("0");
  if (output) {
    mpfr_free_str(output);
  }
  return text;
}

void mpfr_set_from_string_or_zero(mpfr_t out, const std::string& text) {
  if (text.empty()) {
    mpfr_set_ui(out, 0U, MPFR_RNDN);
    return;
  }
  if (mpfr_set_str(out, text.c_str(), 10, MPFR_RNDN) == 0) {
    return;
  }
  try {
    const long double fallback = std::stold(text);
    mpfr_set_ld(out, fallback, MPFR_RNDN);
    return;
  } catch (const std::exception&) {
  }
  mpfr_set_ui(out, 0U, MPFR_RNDN);
}

MpfrNumericCachePtr mpfr_cache_from_numeric(const Value::NumericValue& numeric) {
  if (!numeric.high_precision_cache) {
    return nullptr;
  }
  return std::static_pointer_cast<MpfrNumericCache>(numeric.high_precision_cache);
}

void set_numeric_cache(const Value::NumericValue& numeric, const MpfrNumericCachePtr& cache) {
  numeric.high_precision_cache = std::static_pointer_cast<void>(cache);
}

MpfrNumericCachePtr get_or_init_high_precision_cache(const Value::NumericValue& numeric) {
  const auto precision = mpfr_precision_for_kind(numeric.kind);
  auto cache = mpfr_cache_from_numeric(numeric);
  if (!cache || cache->precision != precision) {
    cache = acquire_mpfr_cache(precision);
    set_numeric_cache(numeric, cache);
  }
  if (!cache->populated) {
    if (!numeric.payload.empty()) {
      mpfr_set_from_string_or_zero(cache->value, numeric.payload);
    } else if (numeric.parsed_float_valid) {
      mpfr_set_ld(cache->value, numeric.parsed_float, MPFR_RNDN);
    } else {
      mpfr_set_ui(cache->value, 0U, MPFR_RNDN);
    }
    cache->populated = true;
    ++cache->epoch;
  }
  return cache;
}

MpfrNumericCachePtr ensure_unique_high_precision_cache(Value::NumericValue& numeric) {
  auto cache = get_or_init_high_precision_cache(numeric);
  if (!cache || cache.use_count() <= 1) {
    return cache;
  }
  auto unique_cache = acquire_mpfr_cache(cache->precision);
  mpfr_set(unique_cache->value, cache->value, MPFR_RNDN);
  unique_cache->populated = cache->populated;
  unique_cache->epoch = cache->epoch;
  set_numeric_cache(numeric, unique_cache);
  return unique_cache;
}

bool mpfr_try_set_from_high_precision_cache(mpfr_t out, const Value& value) {
  if (value.kind != Value::Kind::Numeric || !value.numeric_value) {
    return false;
  }
  const auto& numeric = *value.numeric_value;
  if (!is_high_precision_float_kind_local(numeric.kind)) {
    return false;
  }
  // For transient constructor values (parsed_float-only) avoid allocating a
  // fresh MPFR cache per evaluation step. Callers can fall back to mpfr_set_ld.
  if (!numeric.high_precision_cache && numeric.parsed_float_valid &&
      numeric.payload.empty()) {
    return false;
  }
  const auto cache = get_or_init_high_precision_cache(numeric);
  if (!cache || !cache->populated) {
    return false;
  }
  mpfr_set(out, cache->value, MPFR_RNDN);
  return true;
}

mpfr_srcptr mpfr_cached_srcptr(const Value& value) {
  if (value.kind != Value::Kind::Numeric || !value.numeric_value) {
    return nullptr;
  }
  const auto& numeric = *value.numeric_value;
  if (!is_high_precision_float_kind_local(numeric.kind)) {
    return nullptr;
  }
  // Keep ephemeral parsed-float high values allocation-free on read-only paths.
  if (!numeric.high_precision_cache && numeric.parsed_float_valid &&
      numeric.payload.empty()) {
    return nullptr;
  }
  const auto cache = get_or_init_high_precision_cache(numeric);
  return (cache && cache->populated) ? cache->value : nullptr;
}

Value high_precision_value_from_mpfr(Value::NumericKind kind, const mpfr_t input) {
  Value out;
  out.kind = Value::Kind::Numeric;
  Value::NumericValue numeric;
  numeric.kind = kind;
  numeric.payload.clear();
  numeric.parsed_int_valid = false;
  numeric.parsed_int = 0;
  numeric.parsed_float_valid = false;
  numeric.parsed_float = 0.0L;
  auto cache = acquire_mpfr_cache(mpfr_precision_for_kind(kind));
  mpfr_set(cache->value, input, MPFR_RNDN);
  cache->populated = true;
  set_numeric_cache(numeric, cache);
  out.numeric_value = std::move(numeric);
  return out;
}

struct MpfrScratch {
  explicit MpfrScratch(mpfr_prec_t precision)
      : lhs(precision), rhs(precision), out(precision), tmp(precision) {}
  MpfrValue lhs;
  MpfrValue rhs;
  MpfrValue out;
  MpfrValue tmp;
};

MpfrScratch& mpfr_scratch_for_kind(Value::NumericKind kind) {
  switch (kind) {
    case Value::NumericKind::F128: {
      thread_local auto scratch = std::make_unique<MpfrScratch>(113);
      return *scratch;
    }
    case Value::NumericKind::F256: {
      thread_local auto scratch = std::make_unique<MpfrScratch>(237);
      return *scratch;
    }
    case Value::NumericKind::F512:
    default: {
      thread_local auto scratch = std::make_unique<MpfrScratch>(493);
      return *scratch;
    }
  }
}

void mpfr_set_from_value(mpfr_t out, const Value& value) {
  if (value.kind == Value::Kind::Int) {
    mpfr_set_si(out, static_cast<long>(value.int_value), MPFR_RNDN);
    return;
  }
  if (value.kind == Value::Kind::Double) {
    mpfr_set_d(out, value.double_value, MPFR_RNDN);
    return;
  }
  if (value.kind != Value::Kind::Numeric || !value.numeric_value) {
    mpfr_set_ui(out, 0U, MPFR_RNDN);
    return;
  }

  const auto& numeric = *value.numeric_value;
  if (numeric_kind_is_int(numeric.kind)) {
    if (numeric.parsed_int_valid) {
      mpfr_set_from_string_or_zero(out, i128_to_string(static_cast<I128>(numeric.parsed_int)));
      return;
    }
    mpfr_set_from_string_or_zero(out, numeric.payload);
    return;
  }
  if (is_high_precision_float_kind_local(numeric.kind)) {
    if (mpfr_try_set_from_high_precision_cache(out, value)) {
      return;
    }
    if (numeric.parsed_float_valid) {
      double narrowed = 0.0;
      if (long_double_exact_double(numeric.parsed_float, narrowed)) {
        mpfr_set_d(out, narrowed, MPFR_RNDN);
      } else {
        mpfr_set_ld(out, numeric.parsed_float, MPFR_RNDN);
      }
      return;
    }
    if (numeric.parsed_int_valid) {
      mpfr_set_from_string_or_zero(out, i128_to_string(static_cast<I128>(numeric.parsed_int)));
      return;
    }
    if (!numeric.payload.empty()) {
      mpfr_set_from_string_or_zero(out, numeric.payload);
      return;
    }
    mpfr_set_ui(out, 0U, MPFR_RNDN);
    return;
  }
  if (!numeric.payload.empty()) {
    mpfr_set_from_string_or_zero(out, numeric.payload);
    return;
  }
  if (numeric.parsed_float_valid) {
    mpfr_set_ld(out, numeric.parsed_float, MPFR_RNDN);
    return;
  }
  mpfr_set_ui(out, 0U, MPFR_RNDN);
}

void mpfr_pow_optimized(mpfr_t out, mpfr_srcptr lhs, mpfr_srcptr rhs);

void mpfr_binary_direct(BinaryOp op, mpfr_t out, mpfr_srcptr lhs, mpfr_srcptr rhs) {
  if ((op == BinaryOp::Div || op == BinaryOp::Mod) && mpfr_zero_p(rhs) != 0) {
    throw EvalException(op == BinaryOp::Div ? "division by zero" : "modulo by zero");
  }

  switch (op) {
    case BinaryOp::Add:
      mpfr_add(out, lhs, rhs, MPFR_RNDN);
      return;
    case BinaryOp::Sub:
      mpfr_sub(out, lhs, rhs, MPFR_RNDN);
      return;
    case BinaryOp::Mul:
      mpfr_mul(out, lhs, rhs, MPFR_RNDN);
      return;
    case BinaryOp::Div:
      mpfr_div(out, lhs, rhs, MPFR_RNDN);
      return;
    case BinaryOp::Mod:
      mpfr_fmod(out, lhs, rhs, MPFR_RNDN);
      return;
    case BinaryOp::Pow:
      mpfr_pow_optimized(out, lhs, rhs);
      return;
    default:
      throw EvalException("unsupported high-precision numeric operator");
  }
}

void mpfr_pow_optimized(mpfr_t out, mpfr_srcptr lhs, mpfr_srcptr rhs) {
  // Exact identity shortcuts first.
  if (mpfr_cmp_si(rhs, 0L) == 0) {
    mpfr_set_ui(out, 1U, MPFR_RNDN);
    return;
  }
  if (mpfr_cmp_si(rhs, 1L) == 0) {
    mpfr_set(out, lhs, MPFR_RNDN);
    return;
  }
  if (mpfr_cmp_si(lhs, 1L) == 0) {
    mpfr_set_ui(out, 1U, MPFR_RNDN);
    return;
  }
  if (mpfr_zero_p(lhs) != 0) {
    const int rhs_sign = mpfr_sgn(rhs);
    if (rhs_sign > 0) {
      mpfr_set_ui(out, 0U, MPFR_RNDN);
      return;
    }
    if (rhs_sign == 0) {
      mpfr_set_ui(out, 1U, MPFR_RNDN);
      return;
    }
    // rhs < 0 semantics (inf/nan) stay on canonical MPFR path below.
  }

  // Integer exponent fast-paths.
  if (mpfr_integer_p(rhs) != 0) {
    if (mpfr_fits_ulong_p(rhs, MPFR_RNDN) != 0) {
      const unsigned long n = mpfr_get_ui(rhs, MPFR_RNDN);
      if (n == 2UL) {
        mpfr_sqr(out, lhs, MPFR_RNDN);
        return;
      }
      mpfr_pow_ui(out, lhs, n, MPFR_RNDN);
      return;
    }
    if (mpfr_fits_slong_p(rhs, MPFR_RNDN) != 0) {
      const long n = mpfr_get_si(rhs, MPFR_RNDN);
      if (n == 2L) {
        mpfr_sqr(out, lhs, MPFR_RNDN);
        return;
      }
      if (n == -1L) {
        mpfr_ui_div(out, 1UL, lhs, MPFR_RNDN);
        return;
      }
      mpfr_pow_si(out, lhs, n, MPFR_RNDN);
      return;
    }
  }

  mpfr_pow(out, lhs, rhs, MPFR_RNDN);
}

struct MpfrPow2Factor {
  bool valid = false;
  bool negative = false;
  long shift = 0;
};

MpfrPow2Factor mpfr_try_integer_pow2_factor(mpfr_srcptr rhs) {
  MpfrPow2Factor result;
  if (mpfr_integer_p(rhs) == 0 || mpfr_fits_slong_p(rhs, MPFR_RNDN) == 0) {
    return result;
  }
  const long v = mpfr_get_si(rhs, MPFR_RNDN);
  if (v == 0L || v == 1L || v == -1L) {
    return result;
  }
  const bool negative = v < 0L;
  const unsigned long mag = static_cast<unsigned long>(negative ? -v : v);
  if ((mag & (mag - 1UL)) != 0UL) {
    return result;
  }
  long shift = 0;
  unsigned long tmp = mag;
  while (tmp > 1UL) {
    tmp >>= 1U;
    ++shift;
  }
  result.valid = true;
  result.negative = negative;
  result.shift = shift;
  return result;
}

void mpfr_binary_optimized(BinaryOp op, mpfr_t out, mpfr_srcptr lhs, mpfr_srcptr rhs) {
  if ((op == BinaryOp::Div || op == BinaryOp::Mod) && mpfr_zero_p(rhs) != 0) {
    throw EvalException(op == BinaryOp::Div ? "division by zero" : "modulo by zero");
  }

  // Keep NaN/Inf behavior on canonical MPFR ops; identity shortcuts are only
  // for finite-number operands.
  const bool finite_numbers = (mpfr_number_p(lhs) != 0) && (mpfr_number_p(rhs) != 0);
  if (finite_numbers) {
    switch (op) {
      case BinaryOp::Add:
        if (mpfr_zero_p(rhs) != 0) {
          mpfr_set(out, lhs, MPFR_RNDN);
          return;
        }
        if (mpfr_zero_p(lhs) != 0) {
          mpfr_set(out, rhs, MPFR_RNDN);
          return;
        }
        break;
      case BinaryOp::Sub:
        if (mpfr_zero_p(rhs) != 0) {
          mpfr_set(out, lhs, MPFR_RNDN);
          return;
        }
        if (mpfr_equal_p(lhs, rhs) != 0) {
          mpfr_set_ui(out, 0U, MPFR_RNDN);
          return;
        }
        break;
      case BinaryOp::Mul:
        if (mpfr_zero_p(lhs) != 0 || mpfr_zero_p(rhs) != 0) {
          mpfr_set_ui(out, 0U, MPFR_RNDN);
          return;
        }
        if (mpfr_cmp_si(rhs, 1L) == 0) {
          mpfr_set(out, lhs, MPFR_RNDN);
          return;
        }
        if (mpfr_cmp_si(lhs, 1L) == 0) {
          mpfr_set(out, rhs, MPFR_RNDN);
          return;
        }
        if (mpfr_cmp_si(rhs, -1L) == 0) {
          mpfr_neg(out, lhs, MPFR_RNDN);
          return;
        }
        if (mpfr_cmp_si(lhs, -1L) == 0) {
          mpfr_neg(out, rhs, MPFR_RNDN);
          return;
        }
        break;
      case BinaryOp::Div:
        if (mpfr_zero_p(lhs) != 0) {
          mpfr_set_ui(out, 0U, MPFR_RNDN);
          return;
        }
        if (mpfr_cmp_si(rhs, 1L) == 0) {
          mpfr_set(out, lhs, MPFR_RNDN);
          return;
        }
        if (mpfr_cmp_si(rhs, -1L) == 0) {
          mpfr_neg(out, lhs, MPFR_RNDN);
          return;
        }
        break;
      case BinaryOp::Mod:
        if (mpfr_zero_p(lhs) != 0) {
          mpfr_set_ui(out, 0U, MPFR_RNDN);
          return;
        }
        if (mpfr_cmpabs(lhs, rhs) < 0) {
          mpfr_set(out, lhs, MPFR_RNDN);
          return;
        }
        break;
      case BinaryOp::Pow:
      default:
        break;
    }
  }

  switch (op) {
    case BinaryOp::Add:
      mpfr_add(out, lhs, rhs, MPFR_RNDN);
      return;
    case BinaryOp::Sub:
      mpfr_sub(out, lhs, rhs, MPFR_RNDN);
      return;
    case BinaryOp::Mul:
      mpfr_mul(out, lhs, rhs, MPFR_RNDN);
      return;
    case BinaryOp::Div:
      mpfr_div(out, lhs, rhs, MPFR_RNDN);
      return;
    case BinaryOp::Mod:
      // Keep remainder semantics aligned with codegen/bigdecimal checks.
      mpfr_fmod(out, lhs, rhs, MPFR_RNDN);
      return;
    case BinaryOp::Pow:
      mpfr_pow_optimized(out, lhs, rhs);
      return;
    default:
      throw EvalException("unsupported high-precision numeric operator");
  }
}

Value cast_high_precision_float_kind(Value::NumericKind kind, const Value& input) {
  if (input.kind == Value::Kind::Numeric && input.numeric_value &&
      input.numeric_value->kind == kind) {
    return input;
  }
  if (input.kind == Value::Kind::Int) {
    return Value::numeric_float_value_of(kind, static_cast<long double>(input.int_value));
  }
  if (input.kind == Value::Kind::Double) {
    return Value::numeric_float_value_of(kind, static_cast<long double>(input.double_value));
  }
  if (input.kind == Value::Kind::Numeric && input.numeric_value &&
      !is_high_precision_float_kind_local(input.numeric_value->kind)) {
    const auto& numeric = *input.numeric_value;
    if (numeric.parsed_float_valid) {
      return Value::numeric_float_value_of(kind, numeric.parsed_float);
    }
    if (numeric.parsed_int_valid) {
      return Value::numeric_float_value_of(kind, static_cast<long double>(numeric.parsed_int));
    }
    return Value::numeric_float_value_of(kind, parse_long_double(numeric.payload));
  }
  MpfrValue out(mpfr_precision_for_kind(kind));
  mpfr_set_from_value(out.value, input);
  return high_precision_value_from_mpfr(kind, out.value);
}
#endif
