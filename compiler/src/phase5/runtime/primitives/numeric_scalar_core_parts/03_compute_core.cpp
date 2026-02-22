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
    mpfr_srcptr lhs_src = mpfr_cached_srcptr(left);
    mpfr_srcptr rhs_src = mpfr_cached_srcptr(right);
    if (!lhs_src) {
      mpfr_set_from_value(scratch.lhs.value, left);
      lhs_src = scratch.lhs.value;
    }
    if (!rhs_src) {
      mpfr_set_from_value(scratch.rhs.value, right);
      rhs_src = scratch.rhs.value;
    }
    mpfr_binary_optimized(op, scratch.out.value, lhs_src, rhs_src);
    return high_precision_value_from_mpfr(result_kind, scratch.out.value);
  }
#endif

  if (result_kind == Value::NumericKind::F64) {
    const double lhs = static_cast<double>(value_to_long_double(left));
    const double rhs = static_cast<double>(value_to_long_double(right));
    double out = 0.0;
    if (!try_eval_float_binary_fast<double>(op, lhs, rhs, out)) {
      if (op == BinaryOp::Pow) {
        if (const auto integral_exp = integral_exponent_if_safe(static_cast<long double>(rhs));
            integral_exp.has_value()) {
          out = powi_double_numeric_core(lhs, *integral_exp);
        } else {
          out = std::pow(lhs, rhs);
        }
      } else {
        throw EvalException("unsupported float numeric operator");
      }
    }
    return cast_float_kind(result_kind, static_cast<long double>(out));
  }
  if (result_kind == Value::NumericKind::F32) {
    const float lhs = static_cast<float>(value_to_long_double(left));
    const float rhs = static_cast<float>(value_to_long_double(right));
    float out = 0.0F;
    if (!try_eval_float_binary_fast<float>(op, lhs, rhs, out)) {
      if (op == BinaryOp::Pow) {
        if (const auto integral_exp = integral_exponent_if_safe(static_cast<long double>(rhs));
            integral_exp.has_value()) {
          out = powi_float_numeric_core(lhs, *integral_exp);
        } else {
          out = std::pow(lhs, rhs);
        }
      } else {
        throw EvalException("unsupported float numeric operator");
      }
    }
    return cast_float_kind(result_kind, static_cast<long double>(out));
  }

  const auto lhs = value_to_long_double(left);
  const auto rhs = value_to_long_double(right);
  long double out = 0.0L;
  if (op != BinaryOp::Pow && try_eval_float_binary_fast<long double>(op, lhs, rhs, out)) {
    return cast_float_kind(result_kind, out);
  }
  if (op == BinaryOp::Pow) {
    if (const auto integral_exp = integral_exponent_if_safe(rhs); integral_exp.has_value()) {
      out = powi_long_double(lhs, *integral_exp);
    } else {
      out = std::pow(lhs, rhs);
    }
  } else {
    throw EvalException("unsupported float numeric operator");
  }
  return cast_float_kind(result_kind, out);
}

#if defined(SPARK_HAS_MPFR)
struct HighPrecisionBinaryMemoEntry {
  bool valid = false;
  BinaryOp op = BinaryOp::Add;
  Value::NumericKind kind = Value::NumericKind::F128;
  const Value::NumericValue* lhs_numeric = nullptr;
  const Value::NumericValue* rhs_numeric = nullptr;
  std::uint64_t lhs_revision = 0;
  std::uint64_t rhs_revision = 0;
  Value result = Value::nil();
};

HighPrecisionBinaryMemoEntry& high_precision_binary_memo_slot(
    BinaryOp op, Value::NumericKind kind, const Value::NumericValue* lhs,
    const Value::NumericValue* rhs) {
  constexpr std::size_t kMemoSize = 2048;
  static thread_local std::array<HighPrecisionBinaryMemoEntry, kMemoSize> cache{};
  const auto h0 = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(lhs) >> 4U);
  const auto h1 = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(rhs) >> 4U);
  const auto h2 = static_cast<std::size_t>(static_cast<int>(op)) * 0x9e3779b97f4a7c15ULL;
  const auto h3 = static_cast<std::size_t>(static_cast<int>(kind)) * 0xbf58476d1ce4e5b9ULL;
  const auto index = (h0 ^ (h1 * 1315423911ULL) ^ h2 ^ h3) & (kMemoSize - 1);
  return cache[index];
}

std::optional<Value> try_eval_high_precision_same_kind(BinaryOp op, const Value& left,
                                                        const Value& right) {
  if (left.kind != Value::Kind::Numeric || right.kind != Value::Kind::Numeric ||
      !left.numeric_value || !right.numeric_value) {
    return std::nullopt;
  }
  const auto kind = left.numeric_value->kind;
  if (kind != right.numeric_value->kind || !is_high_precision_float_kind_local(kind)) {
    return std::nullopt;
  }
  const auto* lhs_numeric = &left.numeric_value.value();
  const auto* rhs_numeric = &right.numeric_value.value();
  auto& memo = high_precision_binary_memo_slot(op, kind, lhs_numeric, rhs_numeric);
  if (memo.valid && memo.op == op && memo.kind == kind &&
      memo.lhs_numeric == lhs_numeric && memo.rhs_numeric == rhs_numeric &&
      memo.lhs_revision == lhs_numeric->revision &&
      memo.rhs_revision == rhs_numeric->revision) {
    return memo.result;
  }

  const auto store_memo = [&](const Value& value) -> Value {
    memo.valid = true;
    memo.op = op;
    memo.kind = kind;
    memo.lhs_numeric = lhs_numeric;
    memo.rhs_numeric = rhs_numeric;
    memo.lhs_revision = lhs_numeric->revision;
    memo.rhs_revision = rhs_numeric->revision;
    memo.result = value;
    return value;
  };

  auto& scratch = mpfr_scratch_for_kind(kind);
  mpfr_srcptr lhs_src = mpfr_cached_srcptr(left);
  mpfr_srcptr rhs_src = mpfr_cached_srcptr(right);
  if (!lhs_src) {
    mpfr_set_from_value(scratch.lhs.value, left);
    lhs_src = scratch.lhs.value;
  }
  if (!rhs_src) {
    mpfr_set_from_value(scratch.rhs.value, right);
    rhs_src = scratch.rhs.value;
  }
  const bool direct_kernel_eligible =
      mpfr_direct_kernel_enabled() &&
      lhs_numeric->parsed_float_valid && rhs_numeric->parsed_float_valid &&
      !lhs_numeric->parsed_int_valid && !rhs_numeric->parsed_int_valid &&
      lhs_numeric->payload.empty() && rhs_numeric->payload.empty() &&
      !lhs_numeric->high_precision_cache && !rhs_numeric->high_precision_cache;

  if (is_comparison_op(op)) {
    const int cmp = mpfr_cmp(lhs_src, rhs_src);
    switch (op) {
      case BinaryOp::Eq:
        return store_memo(Value::bool_value_of(cmp == 0));
      case BinaryOp::Ne:
        return store_memo(Value::bool_value_of(cmp != 0));
      case BinaryOp::Lt:
        return store_memo(Value::bool_value_of(cmp < 0));
      case BinaryOp::Lte:
        return store_memo(Value::bool_value_of(cmp <= 0));
      case BinaryOp::Gt:
        return store_memo(Value::bool_value_of(cmp > 0));
      case BinaryOp::Gte:
        return store_memo(Value::bool_value_of(cmp >= 0));
      default:
        return std::nullopt;
    }
  }

  if (direct_kernel_eligible) {
    mpfr_binary_direct(op, scratch.out.value, lhs_src, rhs_src);
  } else {
    mpfr_binary_optimized(op, scratch.out.value, lhs_src, rhs_src);
  }
  return store_memo(high_precision_value_from_mpfr(kind, scratch.out.value));
}
#endif

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
  ++numeric.revision;
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
  ++numeric.revision;
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
  ++numeric.revision;
  auto cache = ensure_unique_high_precision_cache(numeric);
  mpfr_set(cache->value, value, MPFR_RNDN);
  cache->populated = true;
  ++cache->epoch;
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
  ++numeric.revision;
  auto cache = ensure_unique_high_precision_cache(numeric);
  cache->populated = true;
  ++cache->epoch;
}

void mark_high_precision_numeric_metadata_only(Value::NumericValue& numeric,
                                               Value::NumericKind kind) {
  numeric.kind = kind;
  numeric.payload.clear();
  numeric.parsed_int_valid = false;
  numeric.parsed_int = 0;
  numeric.parsed_float_valid = false;
  numeric.parsed_float = 0.0L;
  ++numeric.revision;
  if (auto cache = mpfr_cache_from_numeric(numeric); cache) {
    ++cache->epoch;
  }
}
#endif

bool copy_numeric_value_inplace_internal(Value& target, const Value& source) {
  if (!is_numeric_kind(target) || !is_numeric_kind(source)) {
    return false;
  }
  if (&target == &source) {
    return true;
  }
  if (target.kind == Value::Kind::Int && source.kind == Value::Kind::Int) {
    target.int_value = source.int_value;
    return true;
  }
  if (target.kind == Value::Kind::Double && source.kind == Value::Kind::Double) {
    target.double_value = source.double_value;
    return true;
  }
  if (target.kind != Value::Kind::Numeric || source.kind != Value::Kind::Numeric ||
      !target.numeric_value || !source.numeric_value ||
      target.numeric_value->kind != source.numeric_value->kind) {
    return false;
  }

  const auto kind = target.numeric_value->kind;
#if defined(SPARK_HAS_MPFR)
  if (is_high_precision_float_kind_local(kind)) {
    // Share immutable MPFR cache pointer and keep copy-on-write semantics.
    auto& out = *target.numeric_value;
    const auto& in = *source.numeric_value;
    out.kind = in.kind;
    out.payload = in.payload;
    out.parsed_int_valid = in.parsed_int_valid;
    out.parsed_int = in.parsed_int;
    out.parsed_float_valid = in.parsed_float_valid;
    out.parsed_float = in.parsed_float;
    ++out.revision;
    out.high_precision_cache = in.high_precision_cache;
    return true;
  }
#endif

  auto& out = *target.numeric_value;
  const auto& in = *source.numeric_value;
  out.kind = in.kind;
  out.payload = in.payload;
  out.parsed_int_valid = in.parsed_int_valid;
  out.parsed_int = in.parsed_int;
  out.parsed_float_valid = in.parsed_float_valid;
  out.parsed_float = in.parsed_float;
  ++out.revision;
  out.high_precision_cache.reset();
  return true;
}

