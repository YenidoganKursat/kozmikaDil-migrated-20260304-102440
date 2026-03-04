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
#include <gmp.h>
#include <mpfr.h>
#endif

#include "semantic_runtime/evaluator_parts/internal_helpers.h"

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
  // Keep direct MPFR kernels on by default for arithmetic ops. We still keep
  // the env gate for bisecting/regression checks.
  static const bool enabled = parse_env_bool_numeric("SPARK_MPFR_DIRECT_KERNEL", true);
  return enabled;
}

bool mpfr_hybrid_adaptive_enabled() {
  // Adaptive strict routing:
  // - keeps arithmetic exactness (all paths are MPFR)
  // - chooses direct vs optimized kernels based on observed identity hit rate
  static const bool enabled = parse_env_bool_numeric("SPARK_MPFR_HYBRID_ADAPTIVE", true);
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

bool is_extended_int_kind_local(Value::NumericKind kind) {
  return kind == Value::NumericKind::I256 || kind == Value::NumericKind::I512;
}

int extended_int_bits_for_kind(Value::NumericKind kind) {
  switch (kind) {
    case Value::NumericKind::I256:
      return 256;
    case Value::NumericKind::I512:
      // I512 is treated as the unbounded big-int lane.
      // Fixed-width saturation is still available on narrower int kinds.
      return 0;
    default:
      return 128;
  }
}

#if defined(SPARK_HAS_MPFR)
mpfr_srcptr mpfr_cached_srcptr(const Value& value);
#endif

bool try_extract_i128_exact_from_value(const Value& value, I128& out) {
  if (value.kind == Value::Kind::Int) {
    out = static_cast<I128>(value.int_value);
    return true;
  }
  if (value.kind == Value::Kind::Double) {
    const long double v = static_cast<long double>(value.double_value);
    if (!std::isfinite(static_cast<double>(v)) || std::trunc(v) != v) {
      return false;
    }
    if (v < static_cast<long double>(i128_min()) ||
        v > static_cast<long double>(i128_max())) {
      return false;
    }
    out = static_cast<I128>(v);
    return true;
  }
  if (value.kind != Value::Kind::Numeric || !value.numeric_value) {
    return false;
  }
  const auto& numeric = *value.numeric_value;
  if (numeric.parsed_int_valid) {
    out = numeric.parsed_int;
    return true;
  }
  if (numeric.parsed_float_valid) {
    const long double v = numeric.parsed_float;
    if (!std::isfinite(static_cast<double>(v)) || std::trunc(v) != v) {
      return false;
    }
    if (v < static_cast<long double>(i128_min()) ||
        v > static_cast<long double>(i128_max())) {
      return false;
    }
    out = static_cast<I128>(v);
    return true;
  }
#if defined(SPARK_HAS_MPFR)
  if (numeric.kind == Value::NumericKind::F128 ||
      numeric.kind == Value::NumericKind::F256 ||
      numeric.kind == Value::NumericKind::F512) {
    if (const auto src = mpfr_cached_srcptr(value); src) {
      if (mpfr_integer_p(src) != 0) {
        if (mpfr_fits_slong_p(src, MPFR_RNDN) != 0) {
          out = static_cast<I128>(mpfr_get_si(src, MPFR_RNDN));
          return true;
        }
        if (mpfr_fits_ulong_p(src, MPFR_RNDN) != 0) {
          out = static_cast<I128>(mpfr_get_ui(src, MPFR_RNDN));
          return true;
        }
      }
    }
  }
#endif
  return false;
}

#if defined(SPARK_HAS_MPFR)
struct GmpInt {
  mpz_t value;
  GmpInt() {
    mpz_init(value);
  }
  explicit GmpInt(mp_bitcnt_t bits) {
    mpz_init2(value, bits);
  }
  ~GmpInt() {
    mpz_clear(value);
  }
  GmpInt(const GmpInt&) = delete;
  GmpInt& operator=(const GmpInt&) = delete;
};

void mpz_set_i128(mpz_t out, I128 value) {
  if (value == 0) {
    mpz_set_ui(out, 0U);
    return;
  }
  const bool negative = value < 0;
  const U128 magnitude = negative ? static_cast<U128>(-(value + 1)) + 1U
                                  : static_cast<U128>(value);
  const unsigned long long lo =
      static_cast<unsigned long long>(static_cast<U128>(magnitude));
  const unsigned long long hi =
      static_cast<unsigned long long>(static_cast<U128>(magnitude >> 64U));
  mpz_set_ui(out, static_cast<unsigned long>(hi));
  mpz_mul_2exp(out, out, 64U);
  mpz_add_ui(out, out, static_cast<unsigned long>(lo));
  if (negative) {
    mpz_neg(out, out);
  }
}

bool mpz_to_i128_exact(mpz_srcptr value, I128& out) {
  static thread_local GmpInt lo(160U);
  static thread_local GmpInt hi(160U);
  static thread_local bool initialized = false;
  if (!initialized) {
    mpz_set_i128(lo.value, i128_min());
    mpz_set_i128(hi.value, i128_max());
    initialized = true;
  }
  if (mpz_cmp(value, lo.value) < 0 || mpz_cmp(value, hi.value) > 0) {
    return false;
  }

  static thread_local GmpInt abs_value(160U);
  mpz_abs(abs_value.value, value);
  std::size_t limb_count = 0;
  unsigned long long limbs[2] = {0ULL, 0ULL};
  mpz_export(limbs, &limb_count, -1, sizeof(unsigned long long), 0, 0, abs_value.value);
  if (limb_count > 2U) {
    return false;
  }
  U128 magnitude = 0U;
  if (limb_count >= 1U) {
    magnitude |= static_cast<U128>(limbs[0]);
  }
  if (limb_count == 2U) {
    magnitude |= static_cast<U128>(limbs[1]) << 64U;
  }

  if (mpz_sgn(value) < 0) {
    const U128 min_mag = static_cast<U128>(i128_max()) + 1U;
    if (magnitude == min_mag) {
      out = i128_min();
      return true;
    }
    out = -static_cast<I128>(magnitude);
    return true;
  }
  out = static_cast<I128>(magnitude);
  return true;
}

I128 mpz_to_i128_clamped(mpz_srcptr value) {
  I128 out = 0;
  if (mpz_to_i128_exact(value, out)) {
    return out;
  }
  static thread_local GmpInt lo(160U);
  static thread_local GmpInt hi(160U);
  static thread_local bool initialized = false;
  if (!initialized) {
    mpz_set_i128(lo.value, i128_min());
    mpz_set_i128(hi.value, i128_max());
    initialized = true;
  }
  if (mpz_cmp(value, lo.value) < 0) {
    return i128_min();
  }
  return i128_max();
}

std::string mpz_to_decimal_string(mpz_srcptr value) {
  char* raw = mpz_get_str(nullptr, 10, value);
  if (!raw) {
    return "0";
  }
  std::string out(raw);
  void* (*alloc_fn)(size_t) = nullptr;
  void* (*realloc_fn)(void*, size_t, size_t) = nullptr;
  void (*free_fn)(void*, size_t) = nullptr;
  mp_get_memory_functions(&alloc_fn, &realloc_fn, &free_fn);
  if (free_fn) {
    free_fn(raw, std::strlen(raw) + 1U);
  }
  return out.empty() ? "0" : out;
}

bool mpz_set_from_decimal_text(mpz_t out, const std::string& text);

struct MpzNumericCache {
  explicit MpzNumericCache(int bits)
      : bit_width(bits) {
    mpz_init2(value, static_cast<mp_bitcnt_t>(bits + 64));
    mpz_set_ui(value, 0U);
  }
  ~MpzNumericCache() {
    mpz_clear(value);
  }

  int bit_width = 128;
  bool populated = false;
  std::uint64_t epoch = 1;
  mpz_t value;
};

using MpzNumericCachePtr = std::shared_ptr<MpzNumericCache>;

struct MpzCachePool {
  std::vector<MpzNumericCache*> p_unbounded;
  std::vector<MpzNumericCache*> p256;
  std::vector<MpzNumericCache*> p512;

  ~MpzCachePool() {
    auto release_all = [](std::vector<MpzNumericCache*>& free_list) {
      for (auto* entry : free_list) {
        delete entry;
      }
      free_list.clear();
    };
    release_all(p_unbounded);
    release_all(p256);
    release_all(p512);
  }
};

MpzCachePool& mpz_cache_pool() {
  thread_local MpzCachePool pool;
  return pool;
}

std::vector<MpzNumericCache*>& mpz_cache_freelist(int bits) {
  auto& pool = mpz_cache_pool();
  if (bits <= 0) {
    return pool.p_unbounded;
  }
  if (bits <= 256) {
    return pool.p256;
  }
  return pool.p512;
}

MpzNumericCachePtr acquire_mpz_cache(int bits) {
  auto& free_list = mpz_cache_freelist(bits);
  MpzNumericCache* entry = nullptr;
  if (!free_list.empty()) {
    entry = free_list.back();
    free_list.pop_back();
  } else {
    entry = new MpzNumericCache(bits);
  }
  entry->populated = false;
  entry->epoch = 1;

  return MpzNumericCachePtr(entry, [bits](MpzNumericCache* cache) {
    if (!cache) {
      return;
    }
    cache->populated = false;
    cache->epoch = 1;
    auto& release_list = mpz_cache_freelist(bits);
    release_list.push_back(cache);
  });
}

MpzNumericCachePtr mpz_cache_from_numeric(const Value::NumericValue& numeric) {
  if (!numeric.high_precision_cache || !is_extended_int_kind_local(numeric.kind)) {
    return nullptr;
  }
  return std::static_pointer_cast<MpzNumericCache>(numeric.high_precision_cache);
}

void set_numeric_cache(const Value::NumericValue& numeric, const MpzNumericCachePtr& cache) {
  numeric.high_precision_cache = std::static_pointer_cast<void>(cache);
}

MpzNumericCachePtr get_or_init_extended_int_cache(const Value::NumericValue& numeric) {
  if (!is_extended_int_kind_local(numeric.kind)) {
    return nullptr;
  }
  const int bits = extended_int_bits_for_kind(numeric.kind);
  auto cache = mpz_cache_from_numeric(numeric);
  if (!cache || cache->bit_width != bits) {
    cache = acquire_mpz_cache(bits);
    set_numeric_cache(numeric, cache);
  }
  if (!cache->populated) {
    if (numeric.parsed_int_valid) {
      mpz_set_i128(cache->value, numeric.parsed_int);
    } else if (!numeric.payload.empty()) {
      (void)mpz_set_from_decimal_text(cache->value, numeric.payload);
    } else {
      mpz_set_ui(cache->value, 0U);
    }
    cache->populated = true;
    ++cache->epoch;
  }
  return cache;
}

bool mpz_set_from_decimal_text(mpz_t out, const std::string& text) {
  std::size_t start = 0;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
    ++start;
  }
  std::size_t end = text.size();
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1U]))) {
    --end;
  }
  if (start >= end) {
    mpz_set_ui(out, 0U);
    return true;
  }
  std::string normalized = text.substr(start, end - start);
  if (!normalized.empty() && normalized.front() == '+') {
    normalized.erase(normalized.begin());
  }
  if (normalized.empty()) {
    mpz_set_ui(out, 0U);
    return true;
  }
  if (mpz_set_str(out, normalized.c_str(), 10) != 0) {
    mpz_set_ui(out, 0U);
    return false;
  }
  return true;
}

bool mpz_set_from_integer_value(mpz_t out, const Value& value) {
  if (value.kind == Value::Kind::Int) {
    mpz_set_si(out, static_cast<long>(value.int_value));
    return true;
  }
  if (value.kind != Value::Kind::Numeric || !value.numeric_value) {
    return false;
  }
  if (!numeric_kind_is_int(value.numeric_value->kind)) {
    return false;
  }
  const auto& numeric = *value.numeric_value;
  if (numeric.parsed_int_valid) {
    mpz_set_i128(out, numeric.parsed_int);
    return true;
  }
  if (is_extended_int_kind_local(numeric.kind)) {
    if (const auto cache = get_or_init_extended_int_cache(numeric); cache && cache->populated) {
      mpz_set(out, cache->value);
      return true;
    }
  }
  if (!numeric.payload.empty()) {
    return mpz_set_from_decimal_text(out, numeric.payload);
  }
  mpz_set_ui(out, 0U);
  return true;
}

void mpz_clamp_signed_bits(mpz_t value, int bits) {
  if (bits <= 0) {
    // Unbounded lane: do not clamp/saturate.
    return;
  }
  GmpInt lo(static_cast<mp_bitcnt_t>(bits) + 8U);
  GmpInt hi(static_cast<mp_bitcnt_t>(bits) + 8U);
  mpz_set_ui(hi.value, 1U);
  mpz_mul_2exp(hi.value, hi.value, static_cast<mp_bitcnt_t>(bits - 1));
  mpz_sub_ui(hi.value, hi.value, 1U);
  mpz_set_ui(lo.value, 1U);
  mpz_mul_2exp(lo.value, lo.value, static_cast<mp_bitcnt_t>(bits - 1));
  mpz_neg(lo.value, lo.value);
  if (mpz_cmp(value, hi.value) > 0) {
    mpz_set(value, hi.value);
  } else if (mpz_cmp(value, lo.value) < 0) {
    mpz_set(value, lo.value);
  }
}

Value big_int_value_from_mpz(Value::NumericKind kind, mpz_srcptr source) {
  GmpInt clamped(static_cast<mp_bitcnt_t>(extended_int_bits_for_kind(kind)) + 8U);
  mpz_set(clamped.value, source);
  mpz_clamp_signed_bits(clamped.value, extended_int_bits_for_kind(kind));

  Value value;
  value.kind = Value::Kind::Numeric;
  Value::NumericValue numeric;
  numeric.kind = kind;
  numeric.revision = 1;
  numeric.payload.clear();
  I128 compact = 0;
  if (mpz_to_i128_exact(clamped.value, compact)) {
    numeric.parsed_int_valid = true;
    numeric.parsed_int = compact;
    numeric.parsed_float_valid = true;
    numeric.parsed_float = static_cast<long double>(compact);
  } else {
    numeric.parsed_int_valid = false;
    numeric.parsed_int = 0;
    numeric.parsed_float_valid = false;
    numeric.parsed_float = 0.0L;
    // Keep wide-int source stable as canonical decimal payload.
    // This avoids cache-alias lifetime hazards on repeated constructor-heavy runs.
    numeric.payload = mpz_to_decimal_string(clamped.value);
  }
  value.numeric_value = std::move(numeric);
  return value;
}

bool eval_extended_int_binary_fast_i128(BinaryOp op, Value::NumericKind result_kind,
                                        const Value& left, const Value& right, Value& out) {
  (void)result_kind;
  I128 lhs = 0;
  I128 rhs = 0;
  if (!try_extract_i128_exact_from_value(left, lhs) ||
      !try_extract_i128_exact_from_value(right, rhs)) {
    return false;
  }

  I128 result = 0;
  switch (op) {
    case BinaryOp::Add:
      if (__builtin_add_overflow(lhs, rhs, &result)) {
        return false;
      }
      break;
    case BinaryOp::Sub:
      if (__builtin_sub_overflow(lhs, rhs, &result)) {
        return false;
      }
      break;
    case BinaryOp::Mul:
      if (__builtin_mul_overflow(lhs, rhs, &result)) {
        return false;
      }
      break;
    case BinaryOp::Mod:
      if (rhs == 0) {
        throw EvalException("modulo by zero");
      }
      result = lhs % rhs;
      break;
    default:
      return false;
  }

  out = Value::numeric_int_value_of(result_kind, result);
  return true;
}

bool eval_extended_int_binary(BinaryOp op, Value::NumericKind result_kind,
                              const Value& left, const Value& right, Value& out) {
  if (!is_extended_int_kind_local(result_kind)) {
    return false;
  }
  if (eval_extended_int_binary_fast_i128(op, result_kind, left, right, out)) {
    return true;
  }

  // Reuse wide temporaries to avoid repeated mpz init/clear churn on hot paths.
  static thread_local GmpInt lhs(640U);
  static thread_local GmpInt rhs(640U);
  static thread_local GmpInt result(640U);
  if (!mpz_set_from_integer_value(lhs.value, left) ||
      !mpz_set_from_integer_value(rhs.value, right)) {
    return false;
  }

  switch (op) {
    case BinaryOp::Add:
      mpz_add(result.value, lhs.value, rhs.value);
      break;
    case BinaryOp::Sub:
      mpz_sub(result.value, lhs.value, rhs.value);
      break;
    case BinaryOp::Mul:
      mpz_mul(result.value, lhs.value, rhs.value);
      break;
    case BinaryOp::Mod:
      if (mpz_sgn(rhs.value) == 0) {
        throw EvalException("modulo by zero");
      }
      mpz_tdiv_r(result.value, lhs.value, rhs.value);
      break;
    default:
      return false;
  }
  mpz_clamp_signed_bits(result.value, extended_int_bits_for_kind(result_kind));
  out = big_int_value_from_mpz(result_kind, result.value);
  return true;
}
#endif

std::string extended_int_numeric_to_string_impl(const Value::NumericValue& numeric) {
  if (numeric.parsed_int_valid) {
    return i128_to_string(numeric.parsed_int);
  }
  if (!numeric.payload.empty()) {
    return numeric.payload;
  }
#if defined(SPARK_HAS_MPFR)
  if (is_extended_int_kind_local(numeric.kind)) {
    if (const auto cache = get_or_init_extended_int_cache(numeric); cache && cache->populated) {
      return mpz_to_decimal_string(cache->value);
    }
  }
#endif
  return "0";
}

bool extended_int_numeric_to_i128_clamped_impl(const Value::NumericValue& numeric, I128& out) {
  if (numeric.parsed_int_valid) {
    out = numeric.parsed_int;
    return true;
  }
  if (!numeric.payload.empty()) {
    out = parse_i128_decimal(numeric.payload);
    return true;
  }
#if defined(SPARK_HAS_MPFR)
  if (is_extended_int_kind_local(numeric.kind)) {
    if (const auto cache = get_or_init_extended_int_cache(numeric); cache && cache->populated) {
      out = mpz_to_i128_clamped(cache->value);
      return true;
    }
  }
#endif
  out = 0;
  return true;
}

long double extended_int_numeric_to_long_double_impl(const Value::NumericValue& numeric) {
  if (numeric.parsed_int_valid) {
    return static_cast<long double>(numeric.parsed_int);
  }
  if (!numeric.payload.empty()) {
    return parse_long_double(numeric.payload);
  }
#if defined(SPARK_HAS_MPFR)
  if (is_extended_int_kind_local(numeric.kind)) {
    if (const auto cache = get_or_init_extended_int_cache(numeric); cache && cache->populated) {
      return static_cast<long double>(mpz_get_d(cache->value));
    }
  }
#endif
  return 0.0L;
}

}  // namespace

std::string extended_int_numeric_to_string(const Value::NumericValue& numeric) {
  return extended_int_numeric_to_string_impl(numeric);
}

bool extended_int_numeric_to_i128_clamped(const Value::NumericValue& numeric, __int128_t& out) {
  return extended_int_numeric_to_i128_clamped_impl(numeric, out);
}

long double extended_int_numeric_to_long_double(const Value::NumericValue& numeric) {
  return extended_int_numeric_to_long_double_impl(numeric);
}

namespace {

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

void mpfr_set_from_i128_exact(mpfr_t out, I128 value) {
  constexpr I128 long_min_v = static_cast<I128>(std::numeric_limits<long>::min());
  constexpr I128 long_max_v = static_cast<I128>(std::numeric_limits<long>::max());
  if (value >= long_min_v && value <= long_max_v) {
    mpfr_set_si(out, static_cast<long>(value), MPFR_RNDN);
    return;
  }
  if (value >= 0) {
    const U128 uv = static_cast<U128>(value);
    if (uv <= static_cast<U128>(std::numeric_limits<unsigned long>::max())) {
      mpfr_set_ui(out, static_cast<unsigned long>(uv), MPFR_RNDN);
      return;
    }
  }
  GmpInt wide(192U);
  mpz_set_i128(wide.value, value);
  mpfr_set_z(out, wide.value, MPFR_RNDN);
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
    } else if (numeric.parsed_int_valid) {
      mpfr_set_from_i128_exact(cache->value, static_cast<I128>(numeric.parsed_int));
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

void mpfr_populate_cache_from_numeric_if_needed(const Value::NumericValue& numeric,
                                                const MpfrNumericCachePtr& cache) {
  if (!cache || cache->populated) {
    return;
  }
  if (numeric.parsed_float_valid) {
    double narrowed = 0.0;
    if (long_double_exact_double(numeric.parsed_float, narrowed)) {
      mpfr_set_d(cache->value, narrowed, MPFR_RNDN);
    } else {
      mpfr_set_ld(cache->value, numeric.parsed_float, MPFR_RNDN);
    }
    cache->populated = true;
    return;
  }
  if (numeric.parsed_int_valid) {
    mpfr_set_from_i128_exact(cache->value, static_cast<I128>(numeric.parsed_int));
    cache->populated = true;
    return;
  }
  if (!numeric.payload.empty()) {
    if (mpfr_set_str(cache->value, numeric.payload.c_str(), 10, MPFR_RNDN) == 0) {
      cache->populated = true;
      return;
    }
  }
  mpfr_set_ui(cache->value, 0U, MPFR_RNDN);
  cache->populated = true;
}

bool mpfr_try_set_from_high_precision_cache(mpfr_t out, const Value& value) {
  if (value.kind != Value::Kind::Numeric || !value.numeric_value) {
    return false;
  }
  const auto& numeric = *value.numeric_value;
  if (!is_high_precision_float_kind_local(numeric.kind)) {
    return false;
  }
  const auto cache = get_or_init_high_precision_cache(numeric);
  if (!cache) {
    return false;
  }
  mpfr_populate_cache_from_numeric_if_needed(numeric, cache);
  if (!cache->populated) {
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
  const auto cache = get_or_init_high_precision_cache(numeric);
  if (!cache) {
    return nullptr;
  }
  mpfr_populate_cache_from_numeric_if_needed(numeric, cache);
  return cache->populated ? cache->value : nullptr;
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

Value high_precision_value_from_i128(Value::NumericKind kind, I128 value) {
  Value out;
  out.kind = Value::Kind::Numeric;
  Value::NumericValue numeric;
  numeric.kind = kind;
  numeric.payload.clear();
  numeric.parsed_int_valid = true;
  numeric.parsed_int = value;
  numeric.parsed_float_valid = false;
  numeric.parsed_float = 0.0L;
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
      const I128 iv = static_cast<I128>(numeric.parsed_int);
      if (iv >= static_cast<I128>(std::numeric_limits<long>::min()) &&
          iv <= static_cast<I128>(std::numeric_limits<long>::max())) {
        mpfr_set_si(out, static_cast<long>(iv), MPFR_RNDN);
      } else {
        static thread_local GmpInt wide(192U);
        mpz_set_i128(wide.value, iv);
        mpfr_set_z(out, wide.value, MPFR_RNDN);
      }
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
      const I128 iv = static_cast<I128>(numeric.parsed_int);
      if (iv >= static_cast<I128>(std::numeric_limits<long>::min()) &&
          iv <= static_cast<I128>(std::numeric_limits<long>::max())) {
        mpfr_set_si(out, static_cast<long>(iv), MPFR_RNDN);
      } else {
        static thread_local GmpInt wide(192U);
        mpz_set_i128(wide.value, iv);
        mpfr_set_z(out, wide.value, MPFR_RNDN);
      }
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

bool mpfr_try_read_slong_integer(mpfr_srcptr value, long& out) {
  if (mpfr_integer_p(value) == 0 || mpfr_fits_slong_p(value, MPFR_RNDN) == 0) {
    return false;
  }
  out = mpfr_get_si(value, MPFR_RNDN);
  return true;
}

void mpfr_binary_direct(BinaryOp op, mpfr_t out, mpfr_srcptr lhs, mpfr_srcptr rhs) {
  if ((op == BinaryOp::Div || op == BinaryOp::Mod) && mpfr_zero_p(rhs) != 0) {
    throw EvalException(op == BinaryOp::Div ? "division by zero" : "modulo by zero");
  }

  switch (op) {
    case BinaryOp::Add:
      {
        long rhs_int = 0;
        if (mpfr_try_read_slong_integer(rhs, rhs_int)) {
          mpfr_add_si(out, lhs, rhs_int, MPFR_RNDN);
          return;
        }
      }
      mpfr_add(out, lhs, rhs, MPFR_RNDN);
      return;
    case BinaryOp::Sub:
      {
        long rhs_int = 0;
        if (mpfr_try_read_slong_integer(rhs, rhs_int)) {
          mpfr_sub_si(out, lhs, rhs_int, MPFR_RNDN);
          return;
        }
      }
      mpfr_sub(out, lhs, rhs, MPFR_RNDN);
      return;
    case BinaryOp::Mul:
      {
        long rhs_int = 0;
        if (mpfr_try_read_slong_integer(rhs, rhs_int)) {
          mpfr_mul_si(out, lhs, rhs_int, MPFR_RNDN);
          return;
        }
      }
      mpfr_mul(out, lhs, rhs, MPFR_RNDN);
      return;
    case BinaryOp::Div:
      {
        long rhs_int = 0;
        if (mpfr_try_read_slong_integer(rhs, rhs_int) && rhs_int != 0L) {
          mpfr_div_si(out, lhs, rhs_int, MPFR_RNDN);
          return;
        }
      }
      mpfr_div(out, lhs, rhs, MPFR_RNDN);
      return;
    case BinaryOp::Mod:
      {
        long rhs_int = 0;
        if (mpfr_try_read_slong_integer(rhs, rhs_int) && rhs_int != 0L &&
            rhs_int != std::numeric_limits<long>::min()) {
          const unsigned long mag =
              static_cast<unsigned long>(rhs_int < 0L ? -rhs_int : rhs_int);
          if (mag != 0UL) {
            mpfr_fmod_ui(out, lhs, mag, MPFR_RNDN);
            return;
          }
        }
      }
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
  const auto pow_ui_small = [&](unsigned long n) -> bool {
    switch (n) {
      case 0UL:
        mpfr_set_ui(out, 1U, MPFR_RNDN);
        return true;
      case 1UL:
        mpfr_set(out, lhs, MPFR_RNDN);
        return true;
      case 2UL:
        mpfr_sqr(out, lhs, MPFR_RNDN);
        return true;
      case 3UL:
        mpfr_sqr(out, lhs, MPFR_RNDN);
        mpfr_mul(out, out, lhs, MPFR_RNDN);
        return true;
      case 4UL:
        mpfr_sqr(out, lhs, MPFR_RNDN);
        mpfr_sqr(out, out, MPFR_RNDN);
        return true;
      case 5UL:
        mpfr_sqr(out, lhs, MPFR_RNDN);
        mpfr_sqr(out, out, MPFR_RNDN);
        mpfr_mul(out, out, lhs, MPFR_RNDN);
        return true;
      case 6UL:
        mpfr_sqr(out, lhs, MPFR_RNDN);
        mpfr_mul(out, out, lhs, MPFR_RNDN);
        mpfr_sqr(out, out, MPFR_RNDN);
        return true;
      case 7UL:
        mpfr_sqr(out, lhs, MPFR_RNDN);
        mpfr_mul(out, out, lhs, MPFR_RNDN);
        mpfr_sqr(out, out, MPFR_RNDN);
        mpfr_mul(out, out, lhs, MPFR_RNDN);
        return true;
      default:
        return false;
    }
  };

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
      if (pow_ui_small(n)) {
        return;
      }
      mpfr_pow_ui(out, lhs, n, MPFR_RNDN);
      return;
    }
    if (mpfr_fits_slong_p(rhs, MPFR_RNDN) != 0) {
      const long n = mpfr_get_si(rhs, MPFR_RNDN);
      if (n == -1L) {
        mpfr_ui_div(out, 1UL, lhs, MPFR_RNDN);
        return;
      }
      if (n >= 0L) {
        if (pow_ui_small(static_cast<unsigned long>(n))) {
          return;
        }
      } else {
        const unsigned long mag = static_cast<unsigned long>(-(n + 1L)) + 1UL;
        if (pow_ui_small(mag)) {
          mpfr_ui_div(out, 1UL, out, MPFR_RNDN);
          return;
        }
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
      {
        long rhs_int = 0;
        if (mpfr_try_read_slong_integer(rhs, rhs_int)) {
          mpfr_add_si(out, lhs, rhs_int, MPFR_RNDN);
          return;
        }
      }
      mpfr_add(out, lhs, rhs, MPFR_RNDN);
      return;
    case BinaryOp::Sub:
      {
        long rhs_int = 0;
        if (mpfr_try_read_slong_integer(rhs, rhs_int)) {
          mpfr_sub_si(out, lhs, rhs_int, MPFR_RNDN);
          return;
        }
      }
      mpfr_sub(out, lhs, rhs, MPFR_RNDN);
      return;
    case BinaryOp::Mul:
      {
        long rhs_int = 0;
        if (mpfr_try_read_slong_integer(rhs, rhs_int)) {
          mpfr_mul_si(out, lhs, rhs_int, MPFR_RNDN);
          return;
        }
      }
      mpfr_mul(out, lhs, rhs, MPFR_RNDN);
      return;
    case BinaryOp::Div:
      {
        long rhs_int = 0;
        if (mpfr_try_read_slong_integer(rhs, rhs_int) && rhs_int != 0L) {
          mpfr_div_si(out, lhs, rhs_int, MPFR_RNDN);
          return;
        }
      }
      mpfr_div(out, lhs, rhs, MPFR_RNDN);
      return;
    case BinaryOp::Mod:
      // Keep remainder semantics aligned with codegen/bigdecimal checks.
      {
        long rhs_int = 0;
        if (mpfr_try_read_slong_integer(rhs, rhs_int) && rhs_int != 0L &&
            rhs_int != std::numeric_limits<long>::min()) {
          const unsigned long mag =
              static_cast<unsigned long>(rhs_int < 0L ? -rhs_int : rhs_int);
          if (mag != 0UL) {
            mpfr_fmod_ui(out, lhs, mag, MPFR_RNDN);
            return;
          }
        }
      }
      mpfr_fmod(out, lhs, rhs, MPFR_RNDN);
      return;
    case BinaryOp::Pow:
      mpfr_pow_optimized(out, lhs, rhs);
      return;
    default:
      throw EvalException("unsupported high-precision numeric operator");
  }
}

std::size_t high_precision_kind_index(Value::NumericKind kind) {
  switch (kind) {
    case Value::NumericKind::F128:
      return 0U;
    case Value::NumericKind::F256:
      return 1U;
    case Value::NumericKind::F512:
      return 2U;
    default:
      throw EvalException("unsupported high-precision numeric kind for adaptive routing");
  }
}

std::size_t arithmetic_binary_op_index(BinaryOp op) {
  switch (op) {
    case BinaryOp::Add:
      return 0U;
    case BinaryOp::Sub:
      return 1U;
    case BinaryOp::Mul:
      return 2U;
    case BinaryOp::Div:
      return 3U;
    case BinaryOp::Mod:
      return 4U;
    case BinaryOp::Pow:
      return 5U;
    default:
      throw EvalException("unsupported arithmetic operator for adaptive routing");
  }
}

bool is_arithmetic_binary_op(BinaryOp op) {
  switch (op) {
    case BinaryOp::Add:
    case BinaryOp::Sub:
    case BinaryOp::Mul:
    case BinaryOp::Div:
    case BinaryOp::Mod:
    case BinaryOp::Pow:
      return true;
    default:
      return false;
  }
}

bool mpfr_identity_shortcut_candidate(BinaryOp op, mpfr_srcptr lhs, mpfr_srcptr rhs) {
  // Only finite-number shortcuts. Non-finite behavior must stay canonical.
  if ((mpfr_number_p(lhs) == 0) || (mpfr_number_p(rhs) == 0)) {
    return false;
  }
  switch (op) {
    case BinaryOp::Add:
      return mpfr_zero_p(lhs) != 0 || mpfr_zero_p(rhs) != 0;
    case BinaryOp::Sub:
      return mpfr_zero_p(rhs) != 0 || mpfr_equal_p(lhs, rhs) != 0;
    case BinaryOp::Mul:
      return mpfr_zero_p(lhs) != 0 || mpfr_zero_p(rhs) != 0 ||
             mpfr_cmp_si(lhs, 1L) == 0 || mpfr_cmp_si(rhs, 1L) == 0 ||
             mpfr_cmp_si(lhs, -1L) == 0 || mpfr_cmp_si(rhs, -1L) == 0;
    case BinaryOp::Div:
      return mpfr_zero_p(lhs) != 0 || mpfr_cmp_si(rhs, 1L) == 0 || mpfr_cmp_si(rhs, -1L) == 0;
    case BinaryOp::Mod:
      return mpfr_zero_p(lhs) != 0 || mpfr_cmpabs(lhs, rhs) < 0;
    case BinaryOp::Pow:
    default:
      return false;
  }
}

struct MpfrHybridAdaptiveState {
  std::uint16_t window_samples = 0;
  std::uint16_t window_shortcuts = 0;
  std::uint16_t sample_tick = 0;
  bool prefer_optimized = false;
};

MpfrHybridAdaptiveState& mpfr_hybrid_adaptive_state(BinaryOp op, Value::NumericKind kind) {
  // 3 high-precision kinds x 6 arithmetic op slots.
  constexpr std::size_t kKinds = 3U;
  constexpr std::size_t kOps = 6U;
  static thread_local std::array<MpfrHybridAdaptiveState, kKinds * kOps> states{};
  const std::size_t idx = high_precision_kind_index(kind) * kOps + arithmetic_binary_op_index(op);
  return states[idx];
}

void mpfr_binary_hybrid_adaptive(BinaryOp op, Value::NumericKind kind, mpfr_t out, mpfr_srcptr lhs,
                                 mpfr_srcptr rhs) {
  if (!is_arithmetic_binary_op(op)) {
    throw EvalException("unsupported high-precision numeric operator");
  }
  if (!is_high_precision_float_kind_local(kind)) {
    // Safety fallback: if caller provides a non-high-precision kind, keep strict
    // MPFR semantics via canonical optimized kernel.
    mpfr_binary_optimized(op, out, lhs, rhs);
    return;
  }
  if (op == BinaryOp::Pow) {
    mpfr_binary_optimized(op, out, lhs, rhs);
    return;
  }
  if (op == BinaryOp::Div) {
    // Division identity shortcuts are relatively rare in random/generic workloads;
    // avoid adaptive policy overhead and use direct strict kernel.
    mpfr_binary_direct(op, out, lhs, rhs);
    return;
  }
  if (op == BinaryOp::Mod) {
    // Mod benefits most from optimized shortcut identities.
    mpfr_binary_optimized(op, out, lhs, rhs);
    return;
  }
  if (!mpfr_direct_kernel_enabled()) {
    mpfr_binary_optimized(op, out, lhs, rhs);
    return;
  }
  if (!mpfr_hybrid_adaptive_enabled()) {
    mpfr_binary_direct(op, out, lhs, rhs);
    return;
  }

  // Sampled adaptive policy: avoid paying shortcut-detection cost every call.
  constexpr std::uint16_t kSampleMask = 0x3FU;  // 1/64 calls sampled.
  constexpr std::uint16_t kWindow = 64U;
  constexpr std::uint16_t kShortcutThresholdPct = 18U;

  auto& state = mpfr_hybrid_adaptive_state(op, kind);
  const bool sample_now = (state.sample_tick++ & kSampleMask) == 0U;
  bool shortcut_hit = false;
  if (sample_now || state.prefer_optimized) {
    shortcut_hit = mpfr_identity_shortcut_candidate(op, lhs, rhs);
  }
  if (sample_now) {
    state.window_samples = static_cast<std::uint16_t>(state.window_samples + 1U);
    if (shortcut_hit) {
      state.window_shortcuts = static_cast<std::uint16_t>(state.window_shortcuts + 1U);
    }
  }

  if (sample_now && state.window_samples >= kWindow) {
    const std::uint32_t shortcut_pct =
        static_cast<std::uint32_t>(state.window_shortcuts) * 100U / state.window_samples;
    state.prefer_optimized = shortcut_pct >= kShortcutThresholdPct;
    state.window_samples = 0U;
    state.window_shortcuts = 0U;
  }

  // Immediate shortcut signals always take optimized route.
  // Otherwise use current adaptive preference.
  const bool use_optimized = shortcut_hit || state.prefer_optimized;
  if (use_optimized) {
    mpfr_binary_optimized(op, out, lhs, rhs);
  } else {
    mpfr_binary_direct(op, out, lhs, rhs);
  }
}

Value cast_high_precision_float_kind(Value::NumericKind kind, const Value& input) {
  if (input.kind == Value::Kind::Numeric && input.numeric_value &&
      input.numeric_value->kind == kind) {
    return input;
  }

  I128 compact = 0;
  if (try_extract_i128_exact_from_value(input, compact)) {
    return high_precision_value_from_i128(kind, compact);
  }
  MpfrValue out(mpfr_precision_for_kind(kind));
  mpfr_set_from_value(out.value, input);
  return high_precision_value_from_mpfr(kind, out.value);
}
#endif
