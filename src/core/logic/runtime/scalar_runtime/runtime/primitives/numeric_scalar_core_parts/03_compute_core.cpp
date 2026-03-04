Value compute_numeric_arithmetic(BinaryOp op, const Value& left, const Value& right) {
  const auto left_kind = runtime_numeric_kind(left);
  const auto right_kind = runtime_numeric_kind(right);
  const auto result_kind = promote_result_kind(op, left_kind, right_kind);

  // Same-kind float short path: avoid repeated generic value->long double
  // conversions when both operands are already normalized numeric primitives.
  if (!numeric_kind_is_int(result_kind) &&
      !is_high_precision_float_kind_local(result_kind) &&
      left.kind == Value::Kind::Numeric && right.kind == Value::Kind::Numeric &&
      left.numeric_value && right.numeric_value &&
      left.numeric_value->kind == result_kind &&
      right.numeric_value->kind == result_kind) {
    const auto read_numeric_scalar = [](const Value::NumericValue& numeric) {
      if (numeric.parsed_float_valid) {
        return numeric.parsed_float;
      }
      if (numeric.parsed_int_valid) {
        return static_cast<long double>(numeric.parsed_int);
      }
      if (!numeric.payload.empty()) {
        return parse_long_double(numeric.payload);
      }
      return 0.0L;
    };
    const auto lhs_ld = read_numeric_scalar(*left.numeric_value);
    const auto rhs_ld = read_numeric_scalar(*right.numeric_value);

    if (result_kind == Value::NumericKind::F64) {
      const double lhs = static_cast<double>(lhs_ld);
      const double rhs = static_cast<double>(rhs_ld);
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
      const float lhs = static_cast<float>(lhs_ld);
      const float rhs = static_cast<float>(rhs_ld);
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

    long double out = 0.0L;
    if (op != BinaryOp::Pow && try_eval_float_binary_fast<long double>(op, lhs_ld, rhs_ld, out)) {
      return cast_float_kind(result_kind, out);
    }
    if (op == BinaryOp::Pow) {
      if (const auto integral_exp = integral_exponent_if_safe(rhs_ld); integral_exp.has_value()) {
        out = powi_long_double(lhs_ld, *integral_exp);
      } else {
        out = std::pow(lhs_ld, rhs_ld);
      }
      return cast_float_kind(result_kind, out);
    }
    throw EvalException("unsupported float numeric operator");
  }

  if (numeric_kind_is_int(result_kind) && op != BinaryOp::Div) {
#if defined(SPARK_HAS_MPFR)
    if (is_extended_int_kind_local(result_kind)) {
      Value out = Value::nil();
      if (eval_extended_int_binary(op, result_kind, left, right, out)) {
        return out;
      }
      throw EvalException("unsupported extended integer numeric operator");
    }
#endif
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
        if (try_fast_i128_mod_pow2_nonneg(lhs, rhs, out)) {
          break;
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
    // Exact integer fast-path for high-precision lanes:
    // If both operands are exact i128 integers representable at the target
    // precision, arithmetic can be resolved without MPFR conversion.
    {
      I128 lhs_i = 0;
      I128 rhs_i = 0;
      if (try_extract_i128_exact_from_value(left, lhs_i) &&
          try_extract_i128_exact_from_value(right, rhs_i)) {
        const int precision_bits = static_cast<int>(mpfr_precision_for_kind(result_kind));
        const auto exact_representable = [precision_bits](I128 v) {
          return bit_width_i128_signed(v) <= precision_bits;
        };

        if (exact_representable(lhs_i) && exact_representable(rhs_i)) {
          bool handled = true;
          bool fallback_to_mpfr = false;
          I128 out_i = 0;

          switch (op) {
            case BinaryOp::Add:
              if (__builtin_add_overflow(lhs_i, rhs_i, &out_i)) {
                fallback_to_mpfr = true;
              }
              break;
            case BinaryOp::Sub:
              if (__builtin_sub_overflow(lhs_i, rhs_i, &out_i)) {
                fallback_to_mpfr = true;
              }
              break;
            case BinaryOp::Mul:
              if (__builtin_mul_overflow(lhs_i, rhs_i, &out_i)) {
                fallback_to_mpfr = true;
              }
              break;
            case BinaryOp::Mod:
              if (rhs_i == 0) {
                throw EvalException("modulo by zero");
              }
              out_i = lhs_i % rhs_i;
              break;
            case BinaryOp::Pow: {
              if (rhs_i < 0) {
                fallback_to_mpfr = true;
                break;
              }
              I128 base = lhs_i;
              I128 result = 1;
              auto n = static_cast<unsigned __int128>(rhs_i);
              while (n > 0U) {
                if ((n & 1U) != 0U) {
                  I128 next = 0;
                  if (__builtin_mul_overflow(result, base, &next)) {
                    fallback_to_mpfr = true;
                    break;
                  }
                  result = next;
                }
                n >>= 1U;
                if (n != 0U) {
                  I128 next_base = 0;
                  if (__builtin_mul_overflow(base, base, &next_base)) {
                    fallback_to_mpfr = true;
                    break;
                  }
                  base = next_base;
                }
              }
              out_i = result;
              break;
            }
            default:
              handled = false;
              break;
          }

          if (handled && !fallback_to_mpfr && exact_representable(out_i)) {
            Value out;
            out.kind = Value::Kind::Numeric;
            Value::NumericValue numeric;
            numeric.kind = result_kind;
            numeric.payload.clear();
            numeric.parsed_int_valid = true;
            numeric.parsed_int = out_i;
            numeric.parsed_float_valid = false;
            numeric.parsed_float = 0.0L;
            out.numeric_value = std::move(numeric);
            return out;
          }
        }
      }
    }

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
    mpfr_binary_hybrid_adaptive(op, result_kind, scratch.out.value, lhs_src, rhs_src);
    return high_precision_value_from_mpfr(result_kind, scratch.out.value);
  }
#endif

  if (result_kind == Value::NumericKind::F64) {
    if (op == BinaryOp::Div && numeric_kind_is_int(left_kind) &&
        numeric_kind_is_int(right_kind)) {
      const auto lhs_i = value_to_i128(left);
      const auto rhs_i = value_to_i128(right);
      if (rhs_i == 0) {
        throw EvalException("division by zero");
      }
      double fast_div = 0.0;
      if (try_fast_i128_div_pow2_to_f64_nonneg_exact(lhs_i, rhs_i, fast_div)) {
        return cast_float_kind(result_kind, static_cast<long double>(fast_div));
      }
    }
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
  constexpr std::size_t kMemoSize = 16384;
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
  const bool comparison = is_comparison_op(op);

  // Fast exact-integer route for high-precision lanes.
  // Keeps semantics exact and avoids MPFR path entirely when possible.
  if (!comparison) {
    I128 lhs_i = 0;
    I128 rhs_i = 0;
    if (try_extract_i128_exact_from_value(left, lhs_i) &&
        try_extract_i128_exact_from_value(right, rhs_i)) {
      bool handled = true;
      bool fallback_to_mpfr = false;
      I128 out_i = 0;
      switch (op) {
        case BinaryOp::Add:
          if (__builtin_add_overflow(lhs_i, rhs_i, &out_i)) {
            fallback_to_mpfr = true;
          }
          break;
        case BinaryOp::Sub:
          if (__builtin_sub_overflow(lhs_i, rhs_i, &out_i)) {
            fallback_to_mpfr = true;
          }
          break;
        case BinaryOp::Mul:
          if (__builtin_mul_overflow(lhs_i, rhs_i, &out_i)) {
            fallback_to_mpfr = true;
          }
          break;
        case BinaryOp::Div:
          if (rhs_i == 0) {
            throw EvalException("division by zero");
          }
          if ((lhs_i % rhs_i) != 0) {
            fallback_to_mpfr = true;
          } else {
            out_i = lhs_i / rhs_i;
          }
          break;
        case BinaryOp::Mod:
          if (rhs_i == 0) {
            throw EvalException("modulo by zero");
          }
          out_i = lhs_i % rhs_i;
          break;
        case BinaryOp::Pow: {
          if (rhs_i < 0) {
            fallback_to_mpfr = true;
            break;
          }
          I128 base = lhs_i;
          I128 result = 1;
          auto n = static_cast<unsigned __int128>(rhs_i);
          while (n > 0U) {
            if ((n & 1U) != 0U) {
              I128 next = 0;
              if (__builtin_mul_overflow(result, base, &next)) {
                fallback_to_mpfr = true;
                break;
              }
              result = next;
            }
            n >>= 1U;
            if (n != 0U) {
              I128 next_base = 0;
              if (__builtin_mul_overflow(base, base, &next_base)) {
                fallback_to_mpfr = true;
                break;
              }
              base = next_base;
            }
          }
          out_i = result;
          break;
        }
        default:
          handled = false;
          break;
      }
      if (handled && !fallback_to_mpfr) {
        return high_precision_value_from_i128(kind, out_i);
      }
    }
  }

  const auto* lhs_numeric = &left.numeric_value.value();
  const auto* rhs_numeric = &right.numeric_value.value();
  const bool memo_enabled = comparison;
  HighPrecisionBinaryMemoEntry* memo_ptr = nullptr;
  if (memo_enabled) {
    const bool commutative_key = op == BinaryOp::Eq || op == BinaryOp::Ne;
    if (commutative_key &&
        reinterpret_cast<std::uintptr_t>(lhs_numeric) >
            reinterpret_cast<std::uintptr_t>(rhs_numeric)) {
      const auto* tmp = lhs_numeric;
      lhs_numeric = rhs_numeric;
      rhs_numeric = tmp;
    }
    auto& memo = high_precision_binary_memo_slot(op, kind, lhs_numeric, rhs_numeric);
    memo_ptr = &memo;
    if (memo.valid && memo.op == op && memo.kind == kind &&
        memo.lhs_numeric == lhs_numeric && memo.rhs_numeric == rhs_numeric &&
        memo.lhs_revision == lhs_numeric->revision &&
        memo.rhs_revision == rhs_numeric->revision) {
      return memo.result;
    }
  }

  const auto store_memo = [&](const Value& value) -> Value {
    if (!memo_ptr) {
      return value;
    }
    auto& memo = *memo_ptr;
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
  if (comparison) {
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

  mpfr_binary_hybrid_adaptive(op, kind, scratch.out.value, lhs_src, rhs_src);
  return store_memo(high_precision_value_from_mpfr(kind, scratch.out.value));
}
#endif

void assign_numeric_big_int_value_inplace(Value& target, Value::NumericKind kind, const std::string& payload) {
  if (target.kind != Value::Kind::Numeric || !target.numeric_value) {
    target = Value::numeric_value_of(kind, payload);
    return;
  }
  auto& numeric = *target.numeric_value;
  numeric.kind = kind;
  numeric.payload = payload;
  numeric.parsed_int_valid = false;
  numeric.parsed_int = 0;
  numeric.parsed_float_valid = false;
  numeric.parsed_float = 0.0L;
  ++numeric.revision;
  if (numeric.high_precision_cache) {
    numeric.high_precision_cache.reset();
  }
}

void assign_numeric_int_value_inplace(Value& target, Value::NumericKind kind, I128 value) {
  if (is_extended_int_kind_local(kind)) {
    if (target.kind != Value::Kind::Numeric || !target.numeric_value) {
      target = Value::numeric_int_value_of(kind, value);
      return;
    }
    auto& numeric = *target.numeric_value;
    numeric.kind = kind;
    if (!numeric.payload.empty()) {
      if (numeric.payload.capacity() > 64) {
        std::string{}.swap(numeric.payload);
      } else {
        numeric.payload.clear();
      }
    }
    numeric.parsed_int_valid = true;
    numeric.parsed_int = value;
    numeric.parsed_float_valid = true;
    numeric.parsed_float = static_cast<long double>(value);
    ++numeric.revision;
    if (numeric.high_precision_cache) {
      numeric.high_precision_cache.reset();
    }
    return;
  }

  if (target.kind != Value::Kind::Numeric || !target.numeric_value) {
    target = cast_int_kind(kind, value);
    return;
  }
  auto& numeric = *target.numeric_value;
  numeric.kind = kind;
  const auto clamped = clamp_to_signed_bits(value, effective_int_bits(kind));
  // Keep int metadata compact: int fast paths use parsed fields, not payload text.
  if (!numeric.payload.empty()) {
    if (numeric.payload.capacity() > 64) {
      std::string{}.swap(numeric.payload);
    } else {
      numeric.payload.clear();
    }
  }
  numeric.parsed_int_valid = true;
  numeric.parsed_int = clamped;
  numeric.parsed_float_valid = true;
  numeric.parsed_float = static_cast<long double>(clamped);
  ++numeric.revision;
  if (numeric.high_precision_cache) {
    numeric.high_precision_cache.reset();
  }
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
  if (mpfr_integer_p(value) != 0 && mpfr_fits_slong_p(value, MPFR_RNDN) != 0) {
    numeric.parsed_int_valid = true;
    numeric.parsed_int = static_cast<I128>(mpfr_get_si(value, MPFR_RNDN));
  } else if (mpfr_integer_p(value) != 0 && mpfr_fits_ulong_p(value, MPFR_RNDN) != 0) {
    numeric.parsed_int_valid = true;
    numeric.parsed_int = static_cast<I128>(mpfr_get_ui(value, MPFR_RNDN));
  } else {
    numeric.parsed_int_valid = false;
    numeric.parsed_int = 0;
  }
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
  // Defensive invariant: metadata-only representation must still retain an
  // authoritative MPFR cache value. This prevents accidental value collapse to
  // 0 when payload/parsed fields are intentionally cleared.
  if (numeric.kind != kind) {
    numeric.kind = kind;
  }
  auto cache = ensure_unique_high_precision_cache(numeric);
  if (cache) {
    cache->populated = true;
  }

  if (numeric.kind == kind && numeric.payload.empty() && !numeric.parsed_int_valid &&
      !numeric.parsed_float_valid) {
    ++numeric.revision;
    if (cache) {
      ++cache->epoch;
    }
    return;
  }
  numeric.kind = kind;
  numeric.payload.clear();
  numeric.parsed_int_valid = false;
  numeric.parsed_int = 0;
  numeric.parsed_float_valid = false;
  numeric.parsed_float = 0.0L;
  ++numeric.revision;
  if (cache) {
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
      !target.numeric_value || !source.numeric_value) {
    return false;
  }

  const auto source_kind = source.numeric_value->kind;
#if defined(SPARK_HAS_MPFR)
  if (is_high_precision_float_kind_local(source_kind)) {
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
  if (numeric_kind_is_int(source_kind) &&
      (!is_extended_int_kind_local(source_kind) || in.payload.empty())) {
    out.kind = in.kind;
    if (!out.payload.empty()) {
      if (out.payload.capacity() > 64) {
        std::string{}.swap(out.payload);
      } else {
        out.payload.clear();
      }
    }
    out.parsed_int_valid = in.parsed_int_valid;
    out.parsed_int = in.parsed_int;
    out.parsed_float_valid = in.parsed_float_valid;
    out.parsed_float = in.parsed_float;
    ++out.revision;
    if (is_extended_int_kind_local(source_kind) && in.high_precision_cache) {
      // Wide integer lanes share immutable mpz cache and switch to copy-on-write
      // when a mutating op requests uniqueness.
      out.high_precision_cache = in.high_precision_cache;
    } else if (out.high_precision_cache) {
      out.high_precision_cache.reset();
    }
    return true;
  }
  out.kind = in.kind;
  out.payload = in.payload;
  out.parsed_int_valid = in.parsed_int_valid;
  out.parsed_int = in.parsed_int;
  out.parsed_float_valid = in.parsed_float_valid;
  out.parsed_float = in.parsed_float;
  ++out.revision;
  if (out.high_precision_cache) {
    out.high_precision_cache.reset();
  }
  return true;
}
