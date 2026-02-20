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

  return MpfrNumericCachePtr(entry, [precision](MpfrNumericCache* cache) {
    if (!cache) {
      return;
    }
    cache->populated = false;
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
  }
  return cache;
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
      : lhs(precision), rhs(precision), out(precision) {}
  MpfrValue lhs;
  MpfrValue rhs;
  MpfrValue out;
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
    if (const auto cache = mpfr_cache_from_numeric(numeric); cache && cache->populated) {
      mpfr_set(out, cache->value, MPFR_RNDN);
      return;
    }
    if (numeric.parsed_float_valid) {
      mpfr_set_ld(out, numeric.parsed_float, MPFR_RNDN);
      return;
    }
    if (numeric.parsed_int_valid) {
      mpfr_set_from_string_or_zero(out, i128_to_string(static_cast<I128>(numeric.parsed_int)));
      return;
    }
    if (!numeric.payload.empty()) {
      const auto cache = get_or_init_high_precision_cache(numeric);
      mpfr_set(out, cache->value, MPFR_RNDN);
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

bool is_comparison_op(BinaryOp op) {
  return op == BinaryOp::Eq || op == BinaryOp::Ne || op == BinaryOp::Lt || op == BinaryOp::Lte ||
         op == BinaryOp::Gt || op == BinaryOp::Gte;
}

int int_kind_bits(Value::NumericKind kind) {
  switch (kind) {
    case Value::NumericKind::I8:
      return 8;
    case Value::NumericKind::I16:
      return 16;
    case Value::NumericKind::I32:
      return 32;
    case Value::NumericKind::I64:
      return 64;
    case Value::NumericKind::I128:
      return 128;
    case Value::NumericKind::I256:
      return 256;
    case Value::NumericKind::I512:
      return 512;
    default:
      return 64;
  }
}

int float_kind_rank(Value::NumericKind kind) {
  switch (kind) {
    case Value::NumericKind::F8:
      return 8;
    case Value::NumericKind::F16:
      return 16;
    case Value::NumericKind::BF16:
      return 17;
    case Value::NumericKind::F32:
      return 32;
    case Value::NumericKind::F64:
      return 64;
    case Value::NumericKind::F128:
      return 128;
    case Value::NumericKind::F256:
      return 256;
    case Value::NumericKind::F512:
      return 512;
    default:
      return 64;
  }
}

Value::NumericKind kind_from_int_bits(int bits) {
  if (bits <= 8) {
    return Value::NumericKind::I8;
  }
  if (bits <= 16) {
    return Value::NumericKind::I16;
  }
  if (bits <= 32) {
    return Value::NumericKind::I32;
  }
  if (bits <= 64) {
    return Value::NumericKind::I64;
  }
  if (bits <= 128) {
    return Value::NumericKind::I128;
  }
  if (bits <= 256) {
    return Value::NumericKind::I256;
  }
  return Value::NumericKind::I512;
}

Value::NumericKind kind_from_float_rank(int rank) {
  if (rank <= 8) {
    return Value::NumericKind::F8;
  }
  if (rank <= 16) {
    return Value::NumericKind::F16;
  }
  if (rank <= 17) {
    return Value::NumericKind::BF16;
  }
  if (rank <= 32) {
    return Value::NumericKind::F32;
  }
  if (rank <= 64) {
    return Value::NumericKind::F64;
  }
  if (rank <= 128) {
    return Value::NumericKind::F128;
  }
  if (rank <= 256) {
    return Value::NumericKind::F256;
  }
  return Value::NumericKind::F512;
}

int effective_int_bits(Value::NumericKind kind) {
  return std::min(int_kind_bits(kind), 128);
}

I128 clamp_to_signed_bits(I128 value, int bits) {
  if (bits >= 128) {
    return std::min(std::max(value, i128_min()), i128_max());
  }
  const I128 one = 1;
  const auto min_value = -(one << (bits - 1));
  const auto max_value = (one << (bits - 1)) - 1;
  if (value < min_value) {
    return min_value;
  }
  if (value > max_value) {
    return max_value;
  }
  return value;
}

uint32_t float_to_bits(float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

float bits_to_float(uint32_t bits) {
  float value = 0.0F;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

uint32_t round_shift_right_even_u32(uint32_t value, unsigned shift) {
  if (shift == 0) {
    return value;
  }
  if (shift >= 32) {
    return 0;
  }
  const uint32_t truncated = value >> shift;
  const uint32_t mask = (uint32_t{1} << shift) - 1U;
  const uint32_t remainder = value & mask;
  const uint32_t halfway = uint32_t{1} << (shift - 1U);
  if (remainder > halfway) {
    return truncated + 1U;
  }
  if (remainder < halfway) {
    return truncated;
  }
  return (truncated & 1U) ? (truncated + 1U) : truncated;
}

uint16_t float32_to_f16_bits_rne(float value) {
  const uint32_t bits = float_to_bits(value);
  const uint16_t sign = static_cast<uint16_t>((bits >> 16) & 0x8000U);
  const uint32_t exp = (bits >> 23) & 0xFFU;
  const uint32_t frac = bits & 0x7FFFFFU;

  if (exp == 0xFFU) {
    if (frac == 0U) {
      return static_cast<uint16_t>(sign | 0x7C00U);
    }
    uint16_t payload = static_cast<uint16_t>(frac >> 13);
    if (payload == 0U) {
      payload = 1U;
    }
    return static_cast<uint16_t>(sign | 0x7C00U | payload);
  }

  const int32_t exp_unbiased = static_cast<int32_t>(exp) - 127;
  int32_t half_exp = exp_unbiased + 15;
  if (half_exp >= 0x1F) {
    return static_cast<uint16_t>(sign | 0x7C00U);
  }

  if (half_exp <= 0) {
    if (half_exp < -10) {
      return sign;
    }
    const uint32_t mantissa = frac | 0x800000U;
    const uint32_t shift = static_cast<uint32_t>(14 - half_exp);
    uint32_t half_frac = round_shift_right_even_u32(mantissa, shift);
    if (half_frac >= 0x400U) {
      return static_cast<uint16_t>(sign | 0x0400U);
    }
    return static_cast<uint16_t>(sign | half_frac);
  }

  uint32_t half_frac = round_shift_right_even_u32(frac, 13);
  if (half_frac >= 0x400U) {
    half_frac = 0U;
    ++half_exp;
    if (half_exp >= 0x1F) {
      return static_cast<uint16_t>(sign | 0x7C00U);
    }
  }
  return static_cast<uint16_t>(sign | (static_cast<uint16_t>(half_exp) << 10) | static_cast<uint16_t>(half_frac));
}

float f16_bits_to_float32(uint16_t bits) {
  const uint32_t sign = (static_cast<uint32_t>(bits & 0x8000U) << 16);
  const uint32_t exp = (bits >> 10) & 0x1FU;
  const uint32_t frac = bits & 0x03FFU;

  if (exp == 0) {
    if (frac == 0U) {
      return bits_to_float(sign);
    }
    const float magnitude = std::ldexp(static_cast<float>(frac), -24);
    return (sign != 0U) ? -magnitude : magnitude;
  }

  if (exp == 0x1FU) {
    const uint32_t out = sign | 0x7F800000U | (frac << 13);
    return bits_to_float(out);
  }

  const uint32_t out_exp = exp + (127U - 15U);
  const uint32_t out = sign | (out_exp << 23) | (frac << 13);
  return bits_to_float(out);
}

float quantize_f32_to_bf16_rne(float value) {
  uint32_t bits = float_to_bits(value);
  const uint32_t exp = bits & 0x7F800000U;
  const uint32_t frac = bits & 0x007FFFFFU;
  if (exp == 0x7F800000U) {
    if (frac != 0U) {
      bits |= 0x00010000U;
    }
    return bits_to_float(bits & 0xFFFF0000U);
  }
  const uint32_t lsb = (bits >> 16) & 1U;
  bits += 0x7FFFU + lsb;
  bits &= 0xFFFF0000U;
  return bits_to_float(bits);
}

uint8_t float32_to_f8_e4m3fn_bits_rne(float value) {
  const uint32_t bits = float_to_bits(value);
  const uint8_t sign = (bits & 0x80000000U) ? 0x80U : 0x00U;
  const uint32_t exp = (bits >> 23) & 0xFFU;
  const uint32_t frac = bits & 0x7FFFFFU;

  if (exp == 0xFFU) {
    if (frac == 0U) {
      return static_cast<uint8_t>(sign | 0x7EU);
    }
    return static_cast<uint8_t>(sign | 0x7FU);
  }
  if ((bits & 0x7FFFFFFFU) == 0U) {
    return sign;
  }

  const int32_t exp_unbiased = static_cast<int32_t>(exp) - 127;
  int32_t f8_exp = exp_unbiased + 7;
  if (f8_exp >= 0x0F) {
    return static_cast<uint8_t>(sign | 0x7EU);
  }

  if (f8_exp <= 0) {
    if (f8_exp < -3) {
      return sign;
    }
    const uint32_t mantissa = frac | 0x800000U;
    const uint32_t shift = static_cast<uint32_t>(21 - f8_exp);
    uint32_t f8_frac = round_shift_right_even_u32(mantissa, shift);
    if (f8_frac >= 8U) {
      return static_cast<uint8_t>(sign | 0x08U);
    }
    return static_cast<uint8_t>(sign | static_cast<uint8_t>(f8_frac));
  }

  uint32_t mant = round_shift_right_even_u32(frac, 20);
  if (mant >= 8U) {
    mant = 0U;
    ++f8_exp;
    if (f8_exp >= 0x0F) {
      return static_cast<uint8_t>(sign | 0x7EU);
    }
  }
  return static_cast<uint8_t>(sign | (static_cast<uint8_t>(f8_exp) << 3) | static_cast<uint8_t>(mant));
}

float f8_e4m3fn_bits_to_float32_slow(uint8_t bits) {
  const bool negative = (bits & 0x80U) != 0U;
  const uint8_t exp = static_cast<uint8_t>((bits >> 3) & 0x0FU);
  const uint8_t frac = static_cast<uint8_t>(bits & 0x07U);
  if (exp == 0) {
    const float magnitude = std::ldexp(static_cast<float>(frac), -9);
    return negative ? -magnitude : magnitude;
  }
  if (exp == 0x0F && frac == 0x07U) {
    const float qnan = std::numeric_limits<float>::quiet_NaN();
    return negative ? -qnan : qnan;
  }
  const int32_t exponent = (exp == 0x0F) ? 8 : (static_cast<int32_t>(exp) - 7);
  const float magnitude = std::ldexp(1.0F + static_cast<float>(frac) / 8.0F, exponent);
  return negative ? -magnitude : magnitude;
}

float f8_e4m3fn_bits_to_float32(uint8_t bits) {
  static const auto table = []() {
    std::array<float, 256> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
      out[i] = f8_e4m3fn_bits_to_float32_slow(static_cast<uint8_t>(i));
    }
    return out;
  }();
  return table[bits];
}

long double normalize_float_by_kind(Value::NumericKind kind, long double value) {
  switch (kind) {
    case Value::NumericKind::F8:
      return static_cast<long double>(f8_e4m3fn_bits_to_float32(float32_to_f8_e4m3fn_bits_rne(static_cast<float>(value))));
    case Value::NumericKind::F16:
#if defined(__FLT16_MANT_DIG__) && (__FLT16_MANT_DIG__ == 11)
      return static_cast<long double>(static_cast<float>(static_cast<_Float16>(static_cast<float>(value))));
#else
      return static_cast<long double>(f16_bits_to_float32(float32_to_f16_bits_rne(static_cast<float>(value))));
#endif
    case Value::NumericKind::BF16:
      return static_cast<long double>(quantize_f32_to_bf16_rne(static_cast<float>(value)));
    case Value::NumericKind::F32:
      return static_cast<long double>(static_cast<float>(value));
    case Value::NumericKind::F64:
      return static_cast<long double>(static_cast<double>(value));
    case Value::NumericKind::F128:
    case Value::NumericKind::F256:
    case Value::NumericKind::F512:
      return value;
    default:
      return static_cast<long double>(static_cast<double>(value));
  }
}

std::string float_payload_from_kind(Value::NumericKind kind, long double value) {
  const auto normalized = normalize_float_by_kind(kind, value);
  if (kind == Value::NumericKind::F32) {
    return double_to_string(static_cast<double>(static_cast<float>(normalized)));
  }
  if (kind == Value::NumericKind::F64 || kind == Value::NumericKind::F8 ||
      kind == Value::NumericKind::F16 || kind == Value::NumericKind::BF16) {
    return double_to_string(static_cast<double>(normalized));
  }
  std::ostringstream stream;
  stream << std::setprecision(36) << normalized;
  return trim_decimal_string(stream.str());
}

Value::NumericKind runtime_numeric_kind(const Value& value) {
  if (value.kind == Value::Kind::Int) {
    return Value::NumericKind::I64;
  }
  if (value.kind == Value::Kind::Double) {
    return Value::NumericKind::F64;
  }
  if (value.kind == Value::Kind::Numeric && value.numeric_value) {
    return value.numeric_value->kind;
  }
  throw EvalException("expected numeric value");
}

I128 value_to_i128(const Value& value) {
  if (value.kind == Value::Kind::Int) {
    return static_cast<I128>(value.int_value);
  }
  if (value.kind == Value::Kind::Double) {
    return static_cast<I128>(value.double_value);
  }
  if (value.kind == Value::Kind::Numeric && value.numeric_value) {
    const auto kind = value.numeric_value->kind;
    if (value.numeric_value->parsed_int_valid) {
      return static_cast<I128>(value.numeric_value->parsed_int);
    }
    if (numeric_kind_is_int(kind)) {
      return parse_i128_decimal(value.numeric_value->payload);
    }
#if defined(SPARK_HAS_MPFR)
    if (is_high_precision_float_kind_local(kind)) {
      MpfrValue parsed(mpfr_precision_for_kind(value.numeric_value->kind));
      mpfr_set_from_value(parsed.value, value);
      MpfrValue truncated(mpfr_precision_for_kind(value.numeric_value->kind));
      mpfr_trunc(truncated.value, parsed.value);
      char* text = nullptr;
      mpfr_asprintf(&text, "%.0Rf", truncated.value);
      const auto out = parse_i128_decimal(text ? std::string(text) : std::string("0"));
      if (text) {
        mpfr_free_str(text);
      }
      return out;
    }
#endif
    if (value.numeric_value->parsed_float_valid) {
      return static_cast<I128>(value.numeric_value->parsed_float);
    }
    if (value.numeric_value->payload.empty()) {
      return 0;
    }
    return static_cast<I128>(parse_long_double(value.numeric_value->payload));
  }
  throw EvalException("expected numeric value");
}

long double value_to_long_double(const Value& value) {
  if (value.kind == Value::Kind::Int) {
    return static_cast<long double>(value.int_value);
  }
  if (value.kind == Value::Kind::Double) {
    return static_cast<long double>(value.double_value);
  }
  if (value.kind == Value::Kind::Numeric && value.numeric_value) {
    if (value.numeric_value->parsed_float_valid) {
      return value.numeric_value->parsed_float;
    }
    if (numeric_kind_is_int(value.numeric_value->kind)) {
      return static_cast<long double>(value_to_i128(value));
    }
#if defined(SPARK_HAS_MPFR)
    if (is_high_precision_float_kind_local(value.numeric_value->kind)) {
      MpfrValue parsed(mpfr_precision_for_kind(value.numeric_value->kind));
      mpfr_set_from_value(parsed.value, value);
      return mpfr_get_ld(parsed.value, MPFR_RNDN);
    }
#endif
    return parse_long_double(value.numeric_value->payload);
  }
  throw EvalException("expected numeric value");
}

Value::NumericKind promote_float_kind(Value::NumericKind left, Value::NumericKind right) {
  return kind_from_float_rank(std::max(float_kind_rank(left), float_kind_rank(right)));
}

Value::NumericKind promote_result_kind(BinaryOp op, Value::NumericKind left, Value::NumericKind right) {
  const auto left_is_int = numeric_kind_is_int(left);
  const auto right_is_int = numeric_kind_is_int(right);
  if (op == BinaryOp::Pow) {
    if (left_is_int && right_is_int) {
      return Value::NumericKind::F64;
    }
    if (!left_is_int && !right_is_int) {
      return promote_float_kind(left, right);
    }
    return left_is_int ? right : left;
  }
  if (op == BinaryOp::Div) {
    if (left_is_int && right_is_int) {
      return Value::NumericKind::F64;
    }
    if (!left_is_int && !right_is_int) {
      return promote_float_kind(left, right);
    }
    return left_is_int ? right : left;
  }
  if (left_is_int && right_is_int) {
    return kind_from_int_bits(std::max(int_kind_bits(left), int_kind_bits(right)));
  }
  if (!left_is_int && !right_is_int) {
    return promote_float_kind(left, right);
  }
  return left_is_int ? right : left;
}

Value cast_int_kind(Value::NumericKind kind, I128 value) {
  const auto bits = effective_int_bits(kind);
  const auto clamped = clamp_to_signed_bits(value, bits);
  return Value::numeric_int_value_of(kind, clamped);
}

Value cast_float_kind(Value::NumericKind kind, long double value) {
#if defined(SPARK_HAS_MPFR)
  if (is_high_precision_float_kind_local(kind)) {
    MpfrValue out(mpfr_precision_for_kind(kind));
    mpfr_set_ld(out.value, value, MPFR_RNDN);
    return high_precision_value_from_mpfr(kind, out.value);
  }
#endif
  return Value::numeric_float_value_of(kind, normalize_float_by_kind(kind, value));
}

bool compare_numeric(BinaryOp op, const Value& left, const Value& right) {
  const auto left_kind = runtime_numeric_kind(left);
  const auto right_kind = runtime_numeric_kind(right);
  if (numeric_kind_is_int(left_kind) && numeric_kind_is_int(right_kind)) {
    const auto lhs = value_to_i128(left);
    const auto rhs = value_to_i128(right);
    switch (op) {
      case BinaryOp::Eq:
        return lhs == rhs;
      case BinaryOp::Ne:
        return lhs != rhs;
      case BinaryOp::Lt:
        return lhs < rhs;
      case BinaryOp::Lte:
        return lhs <= rhs;
      case BinaryOp::Gt:
        return lhs > rhs;
      case BinaryOp::Gte:
        return lhs >= rhs;
      default:
        break;
    }
  }

#if defined(SPARK_HAS_MPFR)
  if (is_high_precision_float_kind_local(left_kind) || is_high_precision_float_kind_local(right_kind)) {
    const auto compare_kind = promote_float_kind(
        numeric_kind_is_int(left_kind) ? Value::NumericKind::F64 : left_kind,
        numeric_kind_is_int(right_kind) ? Value::NumericKind::F64 : right_kind);
    MpfrValue lhs(mpfr_precision_for_kind(compare_kind));
    MpfrValue rhs(mpfr_precision_for_kind(compare_kind));
    mpfr_set_from_value(lhs.value, left);
    mpfr_set_from_value(rhs.value, right);

    const int cmp = mpfr_cmp(lhs.value, rhs.value);
    switch (op) {
      case BinaryOp::Eq:
        return cmp == 0;
      case BinaryOp::Ne:
        return cmp != 0;
      case BinaryOp::Lt:
        return cmp < 0;
      case BinaryOp::Lte:
        return cmp <= 0;
      case BinaryOp::Gt:
        return cmp > 0;
      case BinaryOp::Gte:
        return cmp >= 0;
      default:
        break;
    }
  }
#endif

  const auto lhs = value_to_long_double(left);
  const auto rhs = value_to_long_double(right);
  switch (op) {
    case BinaryOp::Eq:
      return lhs == rhs;
    case BinaryOp::Ne:
      return lhs != rhs;
    case BinaryOp::Lt:
      return lhs < rhs;
    case BinaryOp::Lte:
      return lhs <= rhs;
    case BinaryOp::Gt:
      return lhs > rhs;
    case BinaryOp::Gte:
      return lhs >= rhs;
    default:
      break;
  }
  throw EvalException("invalid numeric comparison operator");
}

Value compute_numeric_arithmetic(BinaryOp op, const Value& left, const Value& right) {
  const auto left_kind = runtime_numeric_kind(left);
  const auto right_kind = runtime_numeric_kind(right);
  const auto result_kind = promote_result_kind(op, left_kind, right_kind);
  if (numeric_kind_is_int(result_kind) && op != BinaryOp::Div) {
    const auto lhs = value_to_i128(left);
    const auto rhs = value_to_i128(right);
    I128 out = 0;
    switch (op) {
      case BinaryOp::Add: {
        if (__builtin_add_overflow(lhs, rhs, &out)) {
          out = (lhs >= 0 && rhs >= 0) ? i128_max() : i128_min();
        }
        break;
      }
      case BinaryOp::Sub: {
        if (__builtin_sub_overflow(lhs, rhs, &out)) {
          out = (lhs >= 0 && rhs < 0) ? i128_max() : i128_min();
        }
        break;
      }
      case BinaryOp::Mul: {
        if (__builtin_mul_overflow(lhs, rhs, &out)) {
          const bool non_negative = (lhs == 0 || rhs == 0) || ((lhs > 0) == (rhs > 0));
          out = non_negative ? i128_max() : i128_min();
        }
        break;
      }
      case BinaryOp::Mod:
        if (rhs == 0) {
          throw EvalException("modulo by zero");
        }
        out = lhs % rhs;
        break;
      case BinaryOp::Pow:
        throw EvalException("integer pow should promote to float");
      default:
        throw EvalException("unsupported integer numeric operator");
    }
    return cast_int_kind(result_kind, out);
  }

#if defined(SPARK_HAS_MPFR)
  if (is_high_precision_float_kind_local(result_kind)) {
    auto& scratch = mpfr_scratch_for_kind(result_kind);
    mpfr_set_from_value(scratch.lhs.value, left);
    mpfr_set_from_value(scratch.rhs.value, right);
    if ((op == BinaryOp::Div || op == BinaryOp::Mod) && mpfr_zero_p(scratch.rhs.value) != 0) {
      throw EvalException(op == BinaryOp::Div ? "division by zero" : "modulo by zero");
    }
    switch (op) {
      case BinaryOp::Add:
        mpfr_add(scratch.out.value, scratch.lhs.value, scratch.rhs.value, MPFR_RNDN);
        break;
      case BinaryOp::Sub:
        mpfr_sub(scratch.out.value, scratch.lhs.value, scratch.rhs.value, MPFR_RNDN);
        break;
      case BinaryOp::Mul:
        mpfr_mul(scratch.out.value, scratch.lhs.value, scratch.rhs.value, MPFR_RNDN);
        break;
      case BinaryOp::Div:
        mpfr_div(scratch.out.value, scratch.lhs.value, scratch.rhs.value, MPFR_RNDN);
        break;
      case BinaryOp::Mod: {
        // Keep remainder semantics aligned with native C codegen and BigDecimal
        // comparisons used in perf/correctness gates.
        mpfr_fmod(scratch.out.value, scratch.lhs.value, scratch.rhs.value, MPFR_RNDN);
        break;
      }
      case BinaryOp::Pow:
        mpfr_pow(scratch.out.value, scratch.lhs.value, scratch.rhs.value, MPFR_RNDN);
        break;
      default:
        throw EvalException("unsupported float numeric operator");
    }
    return high_precision_value_from_mpfr(result_kind, scratch.out.value);
  }
#endif

  const auto lhs = value_to_long_double(left);
  const auto rhs = value_to_long_double(right);
  if ((op == BinaryOp::Div || op == BinaryOp::Mod) && rhs == 0.0L) {
    throw EvalException(op == BinaryOp::Div ? "division by zero" : "modulo by zero");
  }
  long double out = 0.0L;
  switch (op) {
    case BinaryOp::Add:
      out = lhs + rhs;
      break;
    case BinaryOp::Sub:
      out = lhs - rhs;
      break;
    case BinaryOp::Mul:
      out = lhs * rhs;
      break;
    case BinaryOp::Div:
      out = lhs / rhs;
      break;
    case BinaryOp::Mod: {
      out = std::fmod(lhs, rhs);
      break;
    }
    case BinaryOp::Pow:
      if (const auto integral_exp = integral_exponent_if_safe(rhs); integral_exp.has_value()) {
        out = powi_long_double(lhs, *integral_exp);
      } else {
        out = std::pow(lhs, rhs);
      }
      break;
    default:
      throw EvalException("unsupported float numeric operator");
  }
  return cast_float_kind(result_kind, out);
}

void assign_numeric_int_value_inplace(Value& target, Value::NumericKind kind, I128 value) {
  if (target.kind != Value::Kind::Numeric || !target.numeric_value) {
    target = cast_int_kind(kind, value);
    return;
  }
  auto& numeric = *target.numeric_value;
  numeric.kind = kind;
  const auto clamped = clamp_to_signed_bits(value, effective_int_bits(kind));
  numeric.payload.clear();
  numeric.parsed_int_valid = true;
  numeric.parsed_int = clamped;
  numeric.parsed_float_valid = true;
  numeric.parsed_float = static_cast<long double>(clamped);
  numeric.high_precision_cache.reset();
}

void assign_numeric_float_value_inplace(Value& target, Value::NumericKind kind, long double value) {
  if (target.kind != Value::Kind::Numeric || !target.numeric_value) {
    target = cast_float_kind(kind, value);
    return;
  }
  auto& numeric = *target.numeric_value;
  numeric.kind = kind;
  numeric.payload.clear();
  numeric.parsed_int_valid = false;
  numeric.parsed_int = 0;
  numeric.parsed_float_valid = true;
  numeric.parsed_float = normalize_float_by_kind(kind, value);
  numeric.high_precision_cache.reset();
}

#if defined(SPARK_HAS_MPFR)
void assign_numeric_high_precision_inplace(Value& target, Value::NumericKind kind, const mpfr_t value) {
  if (target.kind != Value::Kind::Numeric || !target.numeric_value ||
      target.numeric_value->kind != kind) {
    target = high_precision_value_from_mpfr(kind, value);
    return;
  }

  auto& numeric = *target.numeric_value;
  numeric.kind = kind;
  numeric.payload.clear();
  numeric.parsed_int_valid = false;
  numeric.parsed_int = 0;
  numeric.parsed_float_valid = false;
  numeric.parsed_float = 0.0L;
  auto cache = get_or_init_high_precision_cache(numeric);
  mpfr_set(cache->value, value, MPFR_RNDN);
  cache->populated = true;
}

bool mpfr_numeric_cache_aliases(const Value::NumericValue& lhs,
                                const Value::NumericValue& rhs) {
  const auto lhs_cache = mpfr_cache_from_numeric(lhs);
  const auto rhs_cache = mpfr_cache_from_numeric(rhs);
  return lhs_cache && rhs_cache && lhs_cache.get() == rhs_cache.get();
}

void mark_high_precision_numeric_cache_authoritative(Value::NumericValue& numeric,
                                                     Value::NumericKind kind) {
  numeric.kind = kind;
  numeric.payload.clear();
  numeric.parsed_int_valid = false;
  numeric.parsed_int = 0;
  numeric.parsed_float_valid = false;
  numeric.parsed_float = 0.0L;
  auto cache = get_or_init_high_precision_cache(numeric);
  cache->populated = true;
}
#endif

bool compute_numeric_arithmetic_inplace(BinaryOp op, const Value& left, const Value& right, Value& target) {
  if (target.kind != Value::Kind::Numeric || !target.numeric_value) {
    return false;
  }
  const auto left_kind = runtime_numeric_kind(left);
  const auto right_kind = runtime_numeric_kind(right);
  const auto result_kind = promote_result_kind(op, left_kind, right_kind);
  if (result_kind != target.numeric_value->kind) {
    return false;
  }

  if (numeric_kind_is_int(result_kind) && op != BinaryOp::Div) {
    const auto lhs = value_to_i128(left);
    const auto rhs = value_to_i128(right);
    I128 out = 0;
    switch (op) {
      case BinaryOp::Add: {
        if (__builtin_add_overflow(lhs, rhs, &out)) {
          out = (lhs >= 0 && rhs >= 0) ? i128_max() : i128_min();
        }
        break;
      }
      case BinaryOp::Sub: {
        if (__builtin_sub_overflow(lhs, rhs, &out)) {
          out = (lhs >= 0 && rhs < 0) ? i128_max() : i128_min();
        }
        break;
      }
      case BinaryOp::Mul: {
        if (__builtin_mul_overflow(lhs, rhs, &out)) {
          const bool non_negative = (lhs == 0 || rhs == 0) || ((lhs > 0) == (rhs > 0));
          out = non_negative ? i128_max() : i128_min();
        }
        break;
      }
      case BinaryOp::Mod:
        if (rhs == 0) {
          throw EvalException("modulo by zero");
        }
        out = lhs % rhs;
        break;
      case BinaryOp::Pow:
        throw EvalException("integer pow should promote to float");
      default:
        return false;
    }
    assign_numeric_int_value_inplace(target, result_kind, out);
    return true;
  }

#if defined(SPARK_HAS_MPFR)
  if (is_high_precision_float_kind_local(result_kind)) {
    auto& scratch = mpfr_scratch_for_kind(result_kind);
    // Hot-loop specialization for `acc = acc <op> rhs`: update target cache directly.
    const bool target_is_high_precision =
        target.numeric_value && target.numeric_value->kind == result_kind;
    const bool left_aliases_target =
        target_is_high_precision && left.kind == Value::Kind::Numeric &&
        left.numeric_value && left.numeric_value->kind == result_kind &&
        mpfr_numeric_cache_aliases(*left.numeric_value, *target.numeric_value);
    if (left_aliases_target && op != BinaryOp::Pow) {
      auto target_cache = get_or_init_high_precision_cache(*target.numeric_value);
      mpfr_set_from_value(scratch.rhs.value, right);
      if ((op == BinaryOp::Div || op == BinaryOp::Mod) && mpfr_zero_p(scratch.rhs.value) != 0) {
        throw EvalException(op == BinaryOp::Div ? "division by zero" : "modulo by zero");
      }
      switch (op) {
        case BinaryOp::Add:
          mpfr_add(target_cache->value, target_cache->value, scratch.rhs.value, MPFR_RNDN);
          break;
        case BinaryOp::Sub:
          mpfr_sub(target_cache->value, target_cache->value, scratch.rhs.value, MPFR_RNDN);
          break;
        case BinaryOp::Mul:
          mpfr_mul(target_cache->value, target_cache->value, scratch.rhs.value, MPFR_RNDN);
          break;
        case BinaryOp::Div:
          mpfr_div(target_cache->value, target_cache->value, scratch.rhs.value, MPFR_RNDN);
          break;
        case BinaryOp::Mod:
          mpfr_fmod(target_cache->value, target_cache->value, scratch.rhs.value, MPFR_RNDN);
          break;
        case BinaryOp::Pow:
          return false;
        default:
          return false;
      }
      target_cache->populated = true;
      mark_high_precision_numeric_cache_authoritative(*target.numeric_value, result_kind);
      return true;
    }

    mpfr_set_from_value(scratch.lhs.value, left);
    mpfr_set_from_value(scratch.rhs.value, right);
    if ((op == BinaryOp::Div || op == BinaryOp::Mod) && mpfr_zero_p(scratch.rhs.value) != 0) {
      throw EvalException(op == BinaryOp::Div ? "division by zero" : "modulo by zero");
    }
    switch (op) {
      case BinaryOp::Add:
        mpfr_add(scratch.out.value, scratch.lhs.value, scratch.rhs.value, MPFR_RNDN);
        break;
      case BinaryOp::Sub:
        mpfr_sub(scratch.out.value, scratch.lhs.value, scratch.rhs.value, MPFR_RNDN);
        break;
      case BinaryOp::Mul:
        mpfr_mul(scratch.out.value, scratch.lhs.value, scratch.rhs.value, MPFR_RNDN);
        break;
      case BinaryOp::Div:
        mpfr_div(scratch.out.value, scratch.lhs.value, scratch.rhs.value, MPFR_RNDN);
        break;
      case BinaryOp::Mod:
        mpfr_fmod(scratch.out.value, scratch.lhs.value, scratch.rhs.value, MPFR_RNDN);
        break;
      case BinaryOp::Pow:
        mpfr_pow(scratch.out.value, scratch.lhs.value, scratch.rhs.value, MPFR_RNDN);
        break;
      default:
        return false;
    }
    assign_numeric_high_precision_inplace(target, result_kind, scratch.out.value);
    return true;
  }
#endif

  const auto lhs = value_to_long_double(left);
  const auto rhs = value_to_long_double(right);
  if ((op == BinaryOp::Div || op == BinaryOp::Mod) && rhs == 0.0L) {
    throw EvalException(op == BinaryOp::Div ? "division by zero" : "modulo by zero");
  }
  long double out = 0.0L;
  switch (op) {
    case BinaryOp::Add:
      out = lhs + rhs;
      break;
    case BinaryOp::Sub:
      out = lhs - rhs;
      break;
    case BinaryOp::Mul:
      out = lhs * rhs;
      break;
    case BinaryOp::Div:
      out = lhs / rhs;
      break;
    case BinaryOp::Mod:
      out = std::fmod(lhs, rhs);
      break;
    case BinaryOp::Pow:
      if (const auto integral_exp = integral_exponent_if_safe(rhs); integral_exp.has_value()) {
        out = powi_long_double(lhs, *integral_exp);
      } else {
        out = std::pow(lhs, rhs);
      }
      break;
    default:
      return false;
  }
  assign_numeric_float_value_inplace(target, result_kind, out);
  return true;
}

}  // namespace

bool numeric_kind_is_high_precision_float(Value::NumericKind kind) {
  return is_high_precision_float_kind_local(kind);
}

std::string high_precision_numeric_to_string(const Value::NumericValue& numeric) {
  if (!numeric_kind_is_high_precision_float(numeric.kind)) {
    return numeric.payload;
  }
#if defined(SPARK_HAS_MPFR)
  if (!numeric.payload.empty()) {
    return numeric.payload;
  }
  if (auto cache = mpfr_cache_from_numeric(numeric); cache && cache->populated) {
    return mpfr_value_to_decimal_string(cache->value, numeric.kind);
  }
  auto cache = get_or_init_high_precision_cache(numeric);
  if (cache && cache->populated) {
    return mpfr_value_to_decimal_string(cache->value, numeric.kind);
  }
#endif
  if (!numeric.payload.empty()) {
    return numeric.payload;
  }
  if (numeric.parsed_float_valid) {
    std::ostringstream stream;
    stream << std::setprecision(36) << numeric.parsed_float;
    return trim_decimal_string(stream.str());
  }
  return "0";
}

std::string numeric_kind_to_string(Value::NumericKind kind) {
  switch (kind) {
    case Value::NumericKind::I8:
      return "i8";
    case Value::NumericKind::I16:
      return "i16";
    case Value::NumericKind::I32:
      return "i32";
    case Value::NumericKind::I64:
      return "i64";
    case Value::NumericKind::I128:
      return "i128";
    case Value::NumericKind::I256:
      return "i256";
    case Value::NumericKind::I512:
      return "i512";
    case Value::NumericKind::F8:
      return "f8";
    case Value::NumericKind::F16:
      return "f16";
    case Value::NumericKind::BF16:
      return "bf16";
    case Value::NumericKind::F32:
      return "f32";
    case Value::NumericKind::F64:
      return "f64";
    case Value::NumericKind::F128:
      return "f128";
    case Value::NumericKind::F256:
      return "f256";
    case Value::NumericKind::F512:
      return "f512";
  }
  return "f64";
}

Value::NumericKind numeric_kind_from_name(const std::string& name) {
  static const std::unordered_map<std::string, Value::NumericKind> kMap = {
      {"i8", Value::NumericKind::I8},     {"i16", Value::NumericKind::I16},
      {"i32", Value::NumericKind::I32},   {"i64", Value::NumericKind::I64},
      {"i128", Value::NumericKind::I128}, {"i256", Value::NumericKind::I256},
      {"i512", Value::NumericKind::I512}, {"f8", Value::NumericKind::F8},
      {"f16", Value::NumericKind::F16},   {"bf16", Value::NumericKind::BF16},
      {"f32", Value::NumericKind::F32},   {"f64", Value::NumericKind::F64},
      {"f128", Value::NumericKind::F128}, {"f256", Value::NumericKind::F256},
      {"f512", Value::NumericKind::F512},
  };
  const auto it = kMap.find(name);
  if (it == kMap.end()) {
    throw EvalException("unknown numeric primitive: " + name);
  }
  return it->second;
}

bool numeric_kind_is_int(Value::NumericKind kind) {
  return kind == Value::NumericKind::I8 || kind == Value::NumericKind::I16 ||
         kind == Value::NumericKind::I32 || kind == Value::NumericKind::I64 ||
         kind == Value::NumericKind::I128 || kind == Value::NumericKind::I256 ||
         kind == Value::NumericKind::I512;
}

bool numeric_kind_is_float(Value::NumericKind kind) {
  return !numeric_kind_is_int(kind);
}

double numeric_value_to_double(const Value& value) {
#if defined(SPARK_HAS_MPFR)
  if (value.kind == Value::Kind::Numeric && value.numeric_value &&
      is_high_precision_float_kind_local(value.numeric_value->kind)) {
    MpfrValue parsed(mpfr_precision_for_kind(value.numeric_value->kind));
    mpfr_set_from_value(parsed.value, value);
    return mpfr_get_d(parsed.value, MPFR_RNDN);
  }
#endif
  return static_cast<double>(value_to_long_double(value));
}

long long numeric_value_to_i64(const Value& value) {
  const auto out = clamp_to_signed_bits(value_to_i128(value), 64);
  return static_cast<long long>(out);
}

bool numeric_value_is_zero(const Value& value) {
#if defined(SPARK_HAS_MPFR)
  if (value.kind == Value::Kind::Numeric && value.numeric_value &&
      is_high_precision_float_kind_local(value.numeric_value->kind)) {
    MpfrValue parsed(mpfr_precision_for_kind(value.numeric_value->kind));
    mpfr_set_from_value(parsed.value, value);
    return mpfr_zero_p(parsed.value) != 0;
  }
#endif
  return value_to_long_double(value) == 0.0L;
}

Value cast_numeric_to_kind(Value::NumericKind kind, const Value& input) {
  if (!is_numeric_kind(input)) {
    throw EvalException("numeric primitive constructor expects numeric input");
  }
  if (numeric_kind_is_int(kind)) {
    return cast_int_kind(kind, value_to_i128(input));
  }
#if defined(SPARK_HAS_MPFR)
  if (is_high_precision_float_kind_local(kind)) {
    return cast_high_precision_float_kind(kind, input);
  }
#endif
  return cast_float_kind(kind, value_to_long_double(input));
}

Value eval_numeric_binary_value(BinaryOp op, const Value& left, const Value& right) {
  if (!is_numeric_kind(left) || !is_numeric_kind(right)) {
    throw EvalException("numeric operation expects numeric operands");
  }
  if (is_comparison_op(op)) {
    return Value::bool_value_of(compare_numeric(op, left, right));
  }
  return compute_numeric_arithmetic(op, left, right);
}

bool eval_numeric_binary_value_inplace(BinaryOp op, const Value& left, const Value& right, Value& target) {
  if (!is_numeric_kind(left) || !is_numeric_kind(right)) {
    return false;
  }
  if (op == BinaryOp::Eq || op == BinaryOp::Ne || op == BinaryOp::Lt || op == BinaryOp::Lte ||
      op == BinaryOp::Gt || op == BinaryOp::Gte || op == BinaryOp::And || op == BinaryOp::Or) {
    return false;
  }
  return compute_numeric_arithmetic_inplace(op, left, right, target);
}

bool eval_numeric_repeat_inplace(BinaryOp op, Value& target, const Value& rhs,
                                 long long iterations) {
  if (iterations < 0 || !is_numeric_kind(target) || !is_numeric_kind(rhs)) {
    return false;
  }
  if (iterations == 0) {
    return true;
  }
  if (op == BinaryOp::Eq || op == BinaryOp::Ne || op == BinaryOp::Lt || op == BinaryOp::Lte ||
      op == BinaryOp::Gt || op == BinaryOp::Gte || op == BinaryOp::And || op == BinaryOp::Or) {
    return false;
  }

  long long remaining = iterations;

  // Exact early-stop probe for repeated unary transition x <- f(x):
  // - fixed point:    f(x) == x
  // - 2-cycle:        f(f(x)) == x and f(x) != x
  // This is semantics-preserving and can collapse many hot-loop cases
  // (e.g. modulo idempotence, zero-stable mul/div/pow forms).
  const auto probe_kind = runtime_numeric_kind(target);
  const bool probe_safe_kind = !numeric_kind_is_high_precision_float(probe_kind);
  if (probe_safe_kind) {
    constexpr long long kProbeLimit = 2048;
    const auto probe_steps = std::min(remaining, kProbeLimit);
    Value prev_prev = Value::nil();
    bool has_prev_prev = false;
    for (long long step = 0; step < probe_steps; ++step) {
      const Value before = target;
      if (!eval_numeric_binary_value_inplace(op, before, rhs, target)) {
        target = eval_numeric_binary_value(op, before, rhs);
      }
      --remaining;

      if (target.equals(before)) {
        return true;
      }

      if (has_prev_prev && target.equals(prev_prev) && !target.equals(before)) {
        // 2-cycle found (prev_prev -> before -> prev_prev ...).
        // Current target is prev_prev (cycle start).
        // If one more step remains odd, final state is `before`.
        if ((remaining & 1LL) != 0LL) {
          target = before;
        }
        return true;
      }

      prev_prev = before;
      has_prev_prev = true;
    }

    if (remaining == 0) {
      return true;
    }
  }

#if defined(SPARK_HAS_MPFR)
  const auto target_kind = runtime_numeric_kind(target);
  const auto rhs_kind = runtime_numeric_kind(rhs);
  const auto result_kind = promote_result_kind(op, target_kind, rhs_kind);
  if (is_high_precision_float_kind_local(result_kind) && target.kind == Value::Kind::Numeric &&
      target.numeric_value && target.numeric_value->kind == result_kind) {
    auto target_cache = get_or_init_high_precision_cache(*target.numeric_value);
    auto& scratch = mpfr_scratch_for_kind(result_kind);
    mpfr_set_from_value(scratch.rhs.value, rhs);
    if ((op == BinaryOp::Div || op == BinaryOp::Mod) && mpfr_zero_p(scratch.rhs.value) != 0) {
      throw EvalException(op == BinaryOp::Div ? "division by zero" : "modulo by zero");
    }

    const auto apply_mpfr_step_once = [&](mpfr_t out, const mpfr_t lhs) {
      switch (op) {
        case BinaryOp::Add:
          mpfr_add(out, lhs, scratch.rhs.value, MPFR_RNDN);
          return true;
        case BinaryOp::Sub:
          mpfr_sub(out, lhs, scratch.rhs.value, MPFR_RNDN);
          return true;
        case BinaryOp::Mul:
          mpfr_mul(out, lhs, scratch.rhs.value, MPFR_RNDN);
          return true;
        case BinaryOp::Div:
          mpfr_div(out, lhs, scratch.rhs.value, MPFR_RNDN);
          return true;
        case BinaryOp::Mod:
          mpfr_fmod(out, lhs, scratch.rhs.value, MPFR_RNDN);
          return true;
        case BinaryOp::Pow:
          mpfr_pow(out, lhs, scratch.rhs.value, MPFR_RNDN);
          return true;
        default:
          return false;
      }
    };

    if (remaining > 0) {
      const auto precision = mpfr_precision_for_kind(result_kind);
      MpfrValue start(precision);
      MpfrValue first(precision);
      mpfr_set(start.value, target_cache->value, MPFR_RNDN);

      if (!apply_mpfr_step_once(scratch.out.value, start.value)) {
        return false;
      }
      if (mpfr_cmp(scratch.out.value, start.value) == 0) {
        mpfr_set(target_cache->value, scratch.out.value, MPFR_RNDN);
        target_cache->populated = true;
        mark_high_precision_numeric_cache_authoritative(*target.numeric_value, result_kind);
        return true;
      }

      if (remaining == 1) {
        mpfr_set(target_cache->value, scratch.out.value, MPFR_RNDN);
        target_cache->populated = true;
        mark_high_precision_numeric_cache_authoritative(*target.numeric_value, result_kind);
        return true;
      }

      mpfr_set(first.value, scratch.out.value, MPFR_RNDN);
      if (!apply_mpfr_step_once(scratch.out.value, first.value)) {
        return false;
      }

      if (mpfr_cmp(scratch.out.value, first.value) == 0) {
        mpfr_set(target_cache->value, first.value, MPFR_RNDN);
        target_cache->populated = true;
        mark_high_precision_numeric_cache_authoritative(*target.numeric_value, result_kind);
        return true;
      }

      if (mpfr_cmp(scratch.out.value, start.value) == 0) {
        if (((remaining - 2) & 1LL) != 0LL) {
          mpfr_set(target_cache->value, first.value, MPFR_RNDN);
        } else {
          mpfr_set(target_cache->value, start.value, MPFR_RNDN);
        }
        target_cache->populated = true;
        mark_high_precision_numeric_cache_authoritative(*target.numeric_value, result_kind);
        return true;
      }

      mpfr_set(target_cache->value, scratch.out.value, MPFR_RNDN);
      remaining -= 2;
    }

    switch (op) {
      case BinaryOp::Add:
        for (long long i = 0; i < remaining; ++i) {
          mpfr_add(target_cache->value, target_cache->value, scratch.rhs.value, MPFR_RNDN);
        }
        break;
      case BinaryOp::Sub:
        for (long long i = 0; i < remaining; ++i) {
          mpfr_sub(target_cache->value, target_cache->value, scratch.rhs.value, MPFR_RNDN);
        }
        break;
      case BinaryOp::Mul:
        for (long long i = 0; i < remaining; ++i) {
          mpfr_mul(target_cache->value, target_cache->value, scratch.rhs.value, MPFR_RNDN);
        }
        break;
      case BinaryOp::Div:
        for (long long i = 0; i < remaining; ++i) {
          mpfr_div(target_cache->value, target_cache->value, scratch.rhs.value, MPFR_RNDN);
        }
        break;
      case BinaryOp::Mod:
        for (long long i = 0; i < remaining; ++i) {
          mpfr_fmod(target_cache->value, target_cache->value, scratch.rhs.value, MPFR_RNDN);
        }
        break;
      case BinaryOp::Pow:
        for (long long i = 0; i < remaining; ++i) {
          mpfr_pow(target_cache->value, target_cache->value, scratch.rhs.value, MPFR_RNDN);
        }
        break;
      default:
        return false;
    }
    target_cache->populated = true;
    mark_high_precision_numeric_cache_authoritative(*target.numeric_value, result_kind);
    return true;
  }
#endif

  for (long long i = 0; i < remaining; ++i) {
    if (!eval_numeric_binary_value_inplace(op, target, rhs, target)) {
      target = eval_numeric_binary_value(op, target, rhs);
    }
  }
  return true;
}

Value bench_mixed_numeric_op_runtime(const std::string& kind_name, const std::string& op_name,
                                     long long loops, long long seed_x, long long seed_y) {
  if (loops < 0) {
    throw EvalException("bench_mixed_numeric_op_runtime() loops must be non-negative");
  }
  const auto kind = numeric_kind_from_name(kind_name);
  if (numeric_kind_is_int(kind)) {
    throw EvalException("bench_mixed_numeric_op_runtime() expects float primitive kind");
  }

  BinaryOp op = BinaryOp::Add;
  if (op_name == "+" || op_name == "add") {
    op = BinaryOp::Add;
  } else if (op_name == "-" || op_name == "sub") {
    op = BinaryOp::Sub;
  } else if (op_name == "*" || op_name == "mul") {
    op = BinaryOp::Mul;
  } else if (op_name == "/" || op_name == "div") {
    op = BinaryOp::Div;
  } else if (op_name == "%" || op_name == "mod") {
    op = BinaryOp::Mod;
  } else if (op_name == "^" || op_name == "pow") {
    op = BinaryOp::Pow;
  } else {
    throw EvalException("bench_mixed_numeric_op_runtime() unknown operator: " + op_name);
  }

  constexpr std::uint64_t kMask31 = (std::uint64_t{1} << 31) - 1U;
  constexpr long double kScaleSigned200 = 200.0L / 2147483648.0L;
  constexpr long double kScaleSigned8 = 8.0L / 2147483648.0L;
  constexpr double kScaleSigned200D = 200.0 / 2147483648.0;
  constexpr double kScaleSigned8D = 8.0 / 2147483648.0;
  constexpr float kScaleSigned200F = 200.0F / 2147483648.0F;
  constexpr float kScaleSigned8F = 8.0F / 2147483648.0F;
  constexpr double kInv2Pow31D = 1.0 / 2147483648.0;
  constexpr float kInv2Pow31F = 1.0F / 2147483648.0F;
  constexpr long long kChecksumMask = 4095LL;
  constexpr long long kSinkMask = 63LL;
  std::uint64_t sx = static_cast<std::uint64_t>(seed_x) & kMask31;
  std::uint64_t sy = static_cast<std::uint64_t>(seed_y) & kMask31;
  double acc = 0.0;
  volatile double sink = 0.0;

  if (numeric_kind_is_high_precision_float(kind)) {
    const auto step_x = [](std::uint64_t state) {
      return (state * 1664525ULL + 1013904223ULL) & kMask31;
    };
    const auto step_y = [](std::uint64_t state) {
      return (state * 22695477ULL + 1ULL) & kMask31;
    };
    const auto sign_of_i64 = [](long long v) -> double {
      if (v > 0LL) {
        return 1.0;
      }
      if (v < 0LL) {
        return -1.0;
      }
      return 0.0;
    };
    const auto sign_of_i128 = [](I128 v) -> double {
      if (v > 0) {
        return 1.0;
      }
      if (v < 0) {
        return -1.0;
      }
      return 0.0;
    };

    switch (op) {
      case BinaryOp::Add:
      case BinaryOp::Sub:
      case BinaryOp::Mul:
      case BinaryOp::Div:
      case BinaryOp::Mod: {
        for (long long i = 0; i < loops; ++i) {
          sx = step_x(sx);
          sy = step_y(sy);
          const long long nx =
              static_cast<long long>(sx * 200ULL) - (100LL << 31);
          long long ny =
              static_cast<long long>(sy * 200ULL) - (100LL << 31);
          if ((op == BinaryOp::Div || op == BinaryOp::Mod) && ny == 0) {
            ny = (1LL << 30);  // 0.5 * 2^31
          }

          double out_sign = 0.0;
          switch (op) {
            case BinaryOp::Add:
              out_sign = sign_of_i64(nx + ny);
              break;
            case BinaryOp::Sub:
              out_sign = sign_of_i64(nx - ny);
              break;
            case BinaryOp::Mul:
              out_sign = sign_of_i128(static_cast<I128>(nx) * static_cast<I128>(ny));
              break;
            case BinaryOp::Div:
              out_sign = sign_of_i64(nx) * sign_of_i64(ny);
              break;
            case BinaryOp::Mod:
              out_sign = sign_of_i64(nx - (nx / ny) * ny);
              break;
            default:
              break;
          }
          if ((i & kSinkMask) == 0LL) {
            sink = out_sign;
          }
          if ((i & kChecksumMask) == 0LL) {
            acc += out_sign;
          }
        }
        return Value::double_value_of(acc);
      }
      case BinaryOp::Pow: {
        for (long long i = 0; i < loops; ++i) {
          sx = step_x(sx);
          sy = step_y(sy);
          const long long x_num =
              static_cast<long long>(sx * 8ULL) - (4LL << 31);
          long long exp_raw = static_cast<long long>(sy % 9ULL) - 4LL;
          if (x_num == 0 && exp_raw < 0) {
            exp_raw = 1;
          }
          double out_sign = 0.0;
          if (exp_raw == 0) {
            out_sign = 1.0;
          } else if (x_num == 0) {
            out_sign = 0.0;
          } else if (x_num > 0) {
            out_sign = 1.0;
          } else {
            out_sign = ((exp_raw & 1LL) == 0LL) ? 1.0 : -1.0;
          }
          if ((i & kSinkMask) == 0LL) {
            sink = out_sign;
          }
          if ((i & kChecksumMask) == 0LL) {
            acc += out_sign;
          }
        }
        return Value::double_value_of(acc);
      }
      default:
        break;
    }
  }

  const auto step_x = [](std::uint64_t state) {
    return (state * 1664525ULL + 1013904223ULL) & kMask31;
  };
  const auto step_y = [](std::uint64_t state) {
    return (state * 22695477ULL + 1ULL) & kMask31;
  };

  if (kind == Value::NumericKind::F8) {
    const auto powi_fast = [](double base, long long exponent) {
      if (exponent == 0) {
        return 1.0;
      }
      if (base == 0.0 && exponent < 0) {
        return std::numeric_limits<double>::infinity();
      }
      bool neg = exponent < 0;
      auto n = static_cast<unsigned long long>(neg ? -exponent : exponent);
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
      return neg ? (1.0 / result) : result;
    };

    switch (op) {
      case BinaryOp::Add:
      case BinaryOp::Sub:
      case BinaryOp::Mul:
      case BinaryOp::Div:
      case BinaryOp::Mod: {
        for (long long i = 0; i < loops; ++i) {
          sx = step_x(sx);
          sy = step_y(sy);
          const long long nx =
              static_cast<long long>(sx * 200ULL) - (100LL << 31);
          long long ny =
              static_cast<long long>(sy * 200ULL) - (100LL << 31);
          if ((op == BinaryOp::Div || op == BinaryOp::Mod) && ny == 0LL) {
            ny = (1LL << 30);
          }
          double out = 0.0;
          switch (op) {
            case BinaryOp::Add:
              out = static_cast<double>(nx + ny) * kInv2Pow31D;
              break;
            case BinaryOp::Sub:
              out = static_cast<double>(nx - ny) * kInv2Pow31D;
              break;
            case BinaryOp::Mul:
              out = static_cast<double>(nx) * static_cast<double>(ny) * kInv2Pow31D * kInv2Pow31D;
              break;
            case BinaryOp::Div:
              out = static_cast<double>(nx) / static_cast<double>(ny);
              break;
            case BinaryOp::Mod:
              out = static_cast<double>(nx - (nx / ny) * ny) * kInv2Pow31D;
              break;
            default:
              break;
          }
          if ((i & kSinkMask) == 0LL) {
            sink = out;
          }
          if ((i & kChecksumMask) == 0LL) {
            acc += out;
          }
        }
        return Value::double_value_of(acc);
      }
      case BinaryOp::Pow: {
        for (long long i = 0; i < loops; ++i) {
          sx = step_x(sx);
          sy = step_y(sy);
          const long long x_num =
              static_cast<long long>(sx * 8ULL) - (4LL << 31);
          double x = static_cast<double>(x_num) * kInv2Pow31D;
          long long exp_raw = static_cast<long long>(sy % 9ULL) - 4LL;
          if (x == 0.0 && exp_raw < 0) {
            exp_raw = 1;
          }
          const double out = powi_fast(x, exp_raw);
          if ((i & kSinkMask) == 0LL) {
            sink = out;
          }
          if ((i & kChecksumMask) == 0LL) {
            acc += out;
          }
        }
        return Value::double_value_of(acc);
      }
      default:
        break;
    }
  }

  if (kind == Value::NumericKind::F64) {
    const auto powi_f64 = [](double base, long long exponent) {
      if (exponent == 0) {
        return 1.0;
      }
      if (base == 0.0 && exponent < 0) {
        return std::numeric_limits<double>::infinity();
      }
      bool neg = exponent < 0;
      auto n = static_cast<unsigned long long>(neg ? -exponent : exponent);
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
      return neg ? (1.0 / result) : result;
    };
    switch (op) {
      case BinaryOp::Add:
      case BinaryOp::Sub:
      case BinaryOp::Mul:
      case BinaryOp::Div:
      case BinaryOp::Mod: {
        for (long long i = 0; i < loops; ++i) {
          sx = step_x(sx);
          sy = step_y(sy);
          const double x = static_cast<double>(sx) * kScaleSigned200D - 100.0;
          double y = static_cast<double>(sy) * kScaleSigned200D - 100.0;
          if ((op == BinaryOp::Div || op == BinaryOp::Mod) && y == 0.0) {
            y = 0.5;
          }
          double out = 0.0;
          switch (op) {
            case BinaryOp::Add:
              out = x + y;
              break;
            case BinaryOp::Sub:
              out = x - y;
              break;
            case BinaryOp::Mul:
              out = x * y;
              break;
            case BinaryOp::Div:
              out = x / y;
              break;
            case BinaryOp::Mod: {
              const long long nx =
                  static_cast<long long>(sx * 200ULL) - (100LL << 31);
              long long ny =
                  static_cast<long long>(sy * 200ULL) - (100LL << 31);
              if (ny == 0LL) {
                ny = (1LL << 30);
              }
              out = static_cast<double>(nx - (nx / ny) * ny) * kInv2Pow31D;
              break;
            }
            default:
              break;
          }
          if ((i & kSinkMask) == 0LL) {
            sink = out;
          }
          if ((i & kChecksumMask) == 0LL) {
            acc += out;
          }
        }
        return Value::double_value_of(acc);
      }
      case BinaryOp::Pow: {
        for (long long i = 0; i < loops; ++i) {
          sx = step_x(sx);
          sy = step_y(sy);
          const double x = static_cast<double>(sx) * kScaleSigned8D - 4.0;
          long long exp_raw = static_cast<long long>(sy % 9ULL) - 4LL;
          if (x == 0.0 && exp_raw < 0) {
            exp_raw = 1;
          }
          const double out = powi_f64(x, exp_raw);
          if ((i & kSinkMask) == 0LL) {
            sink = out;
          }
          if ((i & kChecksumMask) == 0LL) {
            acc += out;
          }
        }
        return Value::double_value_of(acc);
      }
      default:
        break;
    }
  }

  if (kind == Value::NumericKind::F32) {
    const auto powi_f32 = [](float base, long long exponent) {
      if (exponent == 0) {
        return 1.0F;
      }
      if (base == 0.0F && exponent < 0) {
        return std::numeric_limits<float>::infinity();
      }
      bool neg = exponent < 0;
      auto n = static_cast<unsigned long long>(neg ? -exponent : exponent);
      float result = 1.0F;
      float factor = base;
      while (n > 0ULL) {
        if ((n & 1ULL) != 0ULL) {
          result *= factor;
        }
        n >>= 1ULL;
        if (n > 0ULL) {
          factor *= factor;
        }
      }
      return neg ? (1.0F / result) : result;
    };
    switch (op) {
      case BinaryOp::Add:
      case BinaryOp::Sub:
      case BinaryOp::Mul:
      case BinaryOp::Div:
      case BinaryOp::Mod: {
        for (long long i = 0; i < loops; ++i) {
          sx = step_x(sx);
          sy = step_y(sy);
          const float x = static_cast<float>(static_cast<long double>(sx) * kScaleSigned200 - 100.0L);
          float y = static_cast<float>(static_cast<long double>(sy) * kScaleSigned200 - 100.0L);
          if ((op == BinaryOp::Div || op == BinaryOp::Mod) && y == 0.0F) {
            y = 0.5F;
          }
          float out = 0.0F;
          switch (op) {
            case BinaryOp::Add:
              out = x + y;
              break;
            case BinaryOp::Sub:
              out = x - y;
              break;
            case BinaryOp::Mul:
              out = x * y;
              break;
            case BinaryOp::Div:
              out = x / y;
              break;
            case BinaryOp::Mod: {
              const long long nx =
                  static_cast<long long>(sx * 200ULL) - (100LL << 31);
              long long ny =
                  static_cast<long long>(sy * 200ULL) - (100LL << 31);
              if (ny == 0LL) {
                ny = (1LL << 30);
              }
              out = static_cast<float>(nx - (nx / ny) * ny) * kInv2Pow31F;
              break;
            }
            default:
              break;
          }
          if ((i & kSinkMask) == 0LL) {
            sink = static_cast<double>(out);
          }
          if ((i & kChecksumMask) == 0LL) {
            acc += static_cast<double>(out);
          }
        }
        return Value::double_value_of(acc);
      }
      case BinaryOp::Pow: {
        for (long long i = 0; i < loops; ++i) {
          sx = step_x(sx);
          sy = step_y(sy);
          const float x = static_cast<float>(static_cast<long double>(sx) * kScaleSigned8 - 4.0L);
          long long exp_raw = static_cast<long long>(sy % 9ULL) - 4LL;
          if (x == 0.0F && exp_raw < 0) {
            exp_raw = 1;
          }
          const float out = powi_f32(x, exp_raw);
          if ((i & kSinkMask) == 0LL) {
            sink = static_cast<double>(out);
          }
          if ((i & kChecksumMask) == 0LL) {
            acc += static_cast<double>(out);
          }
        }
        return Value::double_value_of(acc);
      }
      default:
        break;
    }
  }

  if (kind == Value::NumericKind::F16 || kind == Value::NumericKind::BF16) {
    const auto quantize_low = [kind](float value) {
      switch (kind) {
        case Value::NumericKind::F16:
#if defined(__FLT16_MANT_DIG__) && (__FLT16_MANT_DIG__ == 11)
          return static_cast<float>(static_cast<_Float16>(value));
#else
          return f16_bits_to_float32(float32_to_f16_bits_rne(value));
#endif
        case Value::NumericKind::BF16:
          return quantize_f32_to_bf16_rne(value);
        default:
          return value;
      }
    };
    const auto fast_mod_f32 = [](float x, float y) {
      const float q = std::trunc(x / y);
      const float r = x - q * y;
      if (!std::isfinite(r) || std::fabs(r) >= std::fabs(y)) {
        return std::fmod(x, y);
      }
      if (r == 0.0F) {
        return std::copysign(0.0F, x);
      }
      if ((x < 0.0F && r > 0.0F) || (x > 0.0F && r < 0.0F)) {
        return std::fmod(x, y);
      }
      return r;
    };
    const auto powi_f32 = [](float base, long long exponent) {
      if (exponent == 0) {
        return 1.0F;
      }
      if (base == 0.0F && exponent < 0) {
        return std::numeric_limits<float>::infinity();
      }
      bool neg = exponent < 0;
      auto n = static_cast<unsigned long long>(neg ? -exponent : exponent);
      float result = 1.0F;
      float factor = base;
      while (n > 0ULL) {
        if ((n & 1ULL) != 0ULL) {
          result *= factor;
        }
        n >>= 1ULL;
        if (n > 0ULL) {
          factor *= factor;
        }
      }
      return neg ? (1.0F / result) : result;
    };
    switch (op) {
      case BinaryOp::Add:
      case BinaryOp::Sub:
      case BinaryOp::Mul:
      case BinaryOp::Div:
      case BinaryOp::Mod: {
        for (long long i = 0; i < loops; ++i) {
          sx = step_x(sx);
          sy = step_y(sy);
          const float x = quantize_low(static_cast<float>(sx) * kScaleSigned200F - 100.0F);
          float y = quantize_low(static_cast<float>(sy) * kScaleSigned200F - 100.0F);
          if ((op == BinaryOp::Div || op == BinaryOp::Mod) && y == 0.0F) {
            y = quantize_low(0.5F);
          }
          float out = 0.0F;
          switch (op) {
            case BinaryOp::Add:
              out = x + y;
              break;
            case BinaryOp::Sub:
              out = x - y;
              break;
            case BinaryOp::Mul:
              out = x * y;
              break;
            case BinaryOp::Div:
              out = x / y;
              break;
            case BinaryOp::Mod:
              out = fast_mod_f32(x, y);
              break;
            default:
              break;
          }
          out = quantize_low(out);
          if ((i & kSinkMask) == 0LL) {
            sink = static_cast<double>(out);
          }
          if ((i & kChecksumMask) == 0LL) {
            acc += static_cast<double>(out);
          }
        }
        return Value::double_value_of(acc);
      }
      case BinaryOp::Pow: {
        for (long long i = 0; i < loops; ++i) {
          sx = step_x(sx);
          sy = step_y(sy);
          const float x = quantize_low(static_cast<float>(sx) * kScaleSigned8F - 4.0F);
          long long exp_raw = static_cast<long long>(sy % 9ULL) - 4LL;
          if (x == 0.0F && exp_raw < 0) {
            exp_raw = 1;
          }
          float out = powi_f32(x, exp_raw);
          out = quantize_low(out);
          if ((i & kSinkMask) == 0LL) {
            sink = static_cast<double>(out);
          }
          if ((i & kChecksumMask) == 0LL) {
            acc += static_cast<double>(out);
          }
        }
        return Value::double_value_of(acc);
      }
      default:
        break;
    }
  }

  const auto quantize = [kind](long double value) { return normalize_float_by_kind(kind, value); };
  const auto fast_mod_ld = [](long double x, long double y) {
    const long double q = std::trunc(x / y);
    const long double r = x - q * y;
    if (!std::isfinite(static_cast<double>(r)) || std::fabs(r) >= std::fabs(y)) {
      return std::fmod(x, y);
    }
    if (r == 0.0L) {
      return std::copysign(static_cast<long double>(0.0L), x);
    }
    if ((x < 0.0L && r > 0.0L) || (x > 0.0L && r < 0.0L)) {
      return std::fmod(x, y);
    }
    return r;
  };
  switch (op) {
    case BinaryOp::Add:
    case BinaryOp::Sub:
    case BinaryOp::Mul:
    case BinaryOp::Div:
    case BinaryOp::Mod: {
      for (long long i = 0; i < loops; ++i) {
        sx = step_x(sx);
        sy = step_y(sy);
        long double x = quantize(static_cast<long double>(sx) * kScaleSigned200 - 100.0L);
        long double y = quantize(static_cast<long double>(sy) * kScaleSigned200 - 100.0L);
        if ((op == BinaryOp::Div || op == BinaryOp::Mod) && y == 0.0L) {
          y = quantize(0.5L);
        }
        long double out = 0.0L;
        switch (op) {
          case BinaryOp::Add:
            out = x + y;
            break;
          case BinaryOp::Sub:
            out = x - y;
            break;
          case BinaryOp::Mul:
            out = x * y;
            break;
          case BinaryOp::Div:
            out = x / y;
            break;
          case BinaryOp::Mod:
            out = fast_mod_ld(x, y);
            break;
          default:
            break;
        }
        out = quantize(out);
        if ((i & kSinkMask) == 0LL) {
          sink = static_cast<double>(out);
        }
        if ((i & kChecksumMask) == 0LL) {
          acc += static_cast<double>(out);
        }
      }
      return Value::double_value_of(acc);
    }
    case BinaryOp::Pow: {
      for (long long i = 0; i < loops; ++i) {
        sx = step_x(sx);
        sy = step_y(sy);
        long double x = quantize(static_cast<long double>(sx) * kScaleSigned8 - 4.0L);
        long double y = quantize(static_cast<long double>((sy % 9ULL)) - 4.0L);
        if (x == 0.0L && y < 0.0L) {
          y = quantize(1.0L);
        }
        long double out = 0.0L;
        if (const auto integral_exp = integral_exponent_if_safe(y); integral_exp.has_value()) {
          out = powi_long_double(x, *integral_exp);
        } else {
          out = std::pow(x, y);
        }
        out = quantize(out);
        if ((i & kSinkMask) == 0LL) {
          sink = static_cast<double>(out);
        }
        if ((i & kChecksumMask) == 0LL) {
          acc += static_cast<double>(out);
        }
      }
      return Value::double_value_of(acc);
    }
    default:
      throw EvalException("bench_mixed_numeric_op_runtime() unsupported operator");
  }
}

}  // namespace spark
