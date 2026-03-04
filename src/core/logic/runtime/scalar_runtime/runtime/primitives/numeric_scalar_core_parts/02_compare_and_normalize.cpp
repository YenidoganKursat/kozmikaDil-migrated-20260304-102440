
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

bool i128_positive_pow2(I128 value) {
  if (value <= 0) {
    return false;
  }
  const auto bits = static_cast<U128>(value);
  return (bits & (bits - 1U)) == 0U;
}

unsigned i128_pow2_shift(I128 value) {
  auto bits = static_cast<U128>(value);
  unsigned shift = 0U;
  while (bits > 1U) {
    bits >>= 1U;
    ++shift;
  }
  return shift;
}

bool try_fast_i128_mod_pow2_nonneg(I128 lhs, I128 rhs, I128& out) {
  if (!i128_positive_pow2(rhs) || lhs < 0) {
    return false;
  }
  out = lhs & (rhs - 1);
  return true;
}

bool try_fast_i128_div_pow2_to_f64_nonneg_exact(I128 lhs, I128 rhs, double& out) {
  // Exact fast path for integer -> f64 division when lhs/rhs are non-negative
  // and rhs is a power of two. This preserves canonical f64 semantics while
  // avoiding an expensive hardware divide in hot integer-heavy loops.
  constexpr I128 kF64ExactIntMax = static_cast<I128>(9007199254740991LL);
  if (!i128_positive_pow2(rhs) || lhs < 0 || lhs > kF64ExactIntMax) {
    return false;
  }
  const auto shift = static_cast<int>(i128_pow2_shift(rhs));
  out = std::ldexp(static_cast<double>(static_cast<long long>(lhs)), -shift);
  return true;
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
      if (is_extended_int_kind_local(kind)) {
        I128 out = 0;
        if (extended_int_numeric_to_i128_clamped(*value.numeric_value, out)) {
          return out;
        }
      }
      return parse_i128_decimal(value.numeric_value->payload);
    }
#if defined(SPARK_HAS_MPFR)
    if (is_high_precision_float_kind_local(kind)) {
      MpfrValue parsed(mpfr_precision_for_kind(value.numeric_value->kind));
      if (const auto src = mpfr_cached_srcptr(value); src) {
        mpfr_set(parsed.value, src, MPFR_RNDN);
      } else {
        mpfr_set_from_value(parsed.value, value);
      }
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
      if (value.numeric_value->parsed_int_valid) {
        return static_cast<long double>(value.numeric_value->parsed_int);
      }
#if defined(SPARK_HAS_MPFR)
      if (is_extended_int_kind_local(value.numeric_value->kind)) {
        return extended_int_numeric_to_long_double(*value.numeric_value);
      }
#endif
      return static_cast<long double>(value_to_i128(value));
    }
#if defined(SPARK_HAS_MPFR)
    if (is_high_precision_float_kind_local(value.numeric_value->kind)) {
      if (const auto src = mpfr_cached_srcptr(value); src) {
        return mpfr_get_ld(src, MPFR_RNDN);
      }
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
  if (is_extended_int_kind_local(kind)) {
    // Compact lane: i256/i512 values that are exactly representable in i128
    // do not need decimal payload materialization.
    return Value::numeric_int_value_of(kind, value);
  }
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
#if defined(SPARK_HAS_MPFR)
    if (is_extended_int_kind_local(left_kind) || is_extended_int_kind_local(right_kind)) {
      I128 lhs_fast = 0;
      I128 rhs_fast = 0;
      if (try_extract_i128_exact_from_value(left, lhs_fast) &&
          try_extract_i128_exact_from_value(right, rhs_fast)) {
        switch (op) {
          case BinaryOp::Eq:
            return lhs_fast == rhs_fast;
          case BinaryOp::Ne:
            return lhs_fast != rhs_fast;
          case BinaryOp::Lt:
            return lhs_fast < rhs_fast;
          case BinaryOp::Lte:
            return lhs_fast <= rhs_fast;
          case BinaryOp::Gt:
            return lhs_fast > rhs_fast;
          case BinaryOp::Gte:
            return lhs_fast >= rhs_fast;
          default:
            break;
        }
      }

      GmpInt lhs(600U);
      GmpInt rhs(600U);
      if (!mpz_set_from_integer_value(lhs.value, left) ||
          !mpz_set_from_integer_value(rhs.value, right)) {
        throw EvalException("failed to materialize wide integer for comparison");
      }
      const int cmp = mpz_cmp(lhs.value, rhs.value);
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
    mpfr_srcptr lhs_src = mpfr_cached_srcptr(left);
    mpfr_srcptr rhs_src = mpfr_cached_srcptr(right);
    if (!lhs_src) {
      mpfr_set_from_value(lhs.value, left);
      lhs_src = lhs.value;
    }
    if (!rhs_src) {
      mpfr_set_from_value(rhs.value, right);
      rhs_src = rhs.value;
    }

    const int cmp = mpfr_cmp(lhs_src, rhs_src);
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

#if defined(SPARK_HAS_MPFR)
std::optional<Value> try_eval_high_precision_same_kind(BinaryOp op, const Value& left,
                                                       const Value& right);
#endif
