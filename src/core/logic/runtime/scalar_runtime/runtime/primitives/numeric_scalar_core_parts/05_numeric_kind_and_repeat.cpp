#if defined(SPARK_HAS_MPFR)
namespace {

using MpfrRepeatStepFn = void (*)(mpfr_t out, mpfr_srcptr lhs, mpfr_srcptr rhs);

void mpfr_repeat_add_step(mpfr_t out, mpfr_srcptr lhs, mpfr_srcptr rhs) {
  mpfr_add(out, lhs, rhs, MPFR_RNDN);
}

void mpfr_repeat_sub_step(mpfr_t out, mpfr_srcptr lhs, mpfr_srcptr rhs) {
  mpfr_sub(out, lhs, rhs, MPFR_RNDN);
}

void mpfr_repeat_mul_step(mpfr_t out, mpfr_srcptr lhs, mpfr_srcptr rhs) {
  mpfr_mul(out, lhs, rhs, MPFR_RNDN);
}

void mpfr_repeat_div_step(mpfr_t out, mpfr_srcptr lhs, mpfr_srcptr rhs) {
  mpfr_div(out, lhs, rhs, MPFR_RNDN);
}

void mpfr_repeat_mod_step(mpfr_t out, mpfr_srcptr lhs, mpfr_srcptr rhs) {
  mpfr_fmod(out, lhs, rhs, MPFR_RNDN);
}

MpfrRepeatStepFn mpfr_repeat_step_controller(BinaryOp op) {
  switch (op) {
    case BinaryOp::Add:
      return &mpfr_repeat_add_step;
    case BinaryOp::Sub:
      return &mpfr_repeat_sub_step;
    case BinaryOp::Mul:
      return &mpfr_repeat_mul_step;
    case BinaryOp::Div:
      return &mpfr_repeat_div_step;
    case BinaryOp::Mod:
      return &mpfr_repeat_mod_step;
    default:
      return nullptr;
  }
}

void mpfr_repeat_apply_unrolled(MpfrRepeatStepFn step, mpfr_t acc, mpfr_srcptr rhs,
                                long long remaining) {
  for (long long i = 0; i + 3 < remaining; i += 4) {
    step(acc, acc, rhs);
    step(acc, acc, rhs);
    step(acc, acc, rhs);
    step(acc, acc, rhs);
  }
  for (long long i = (remaining & ~3LL); i < remaining; ++i) {
    step(acc, acc, rhs);
  }
}

}  // namespace
#endif

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
    if (const auto src = mpfr_cached_srcptr(value); src) {
      return mpfr_get_d(src, MPFR_RNDN);
    }
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
    if (const auto src = mpfr_cached_srcptr(value); src) {
      mpfr_set(parsed.value, src, MPFR_RNDN);
    } else {
      mpfr_set_from_value(parsed.value, value);
    }
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
#if defined(SPARK_HAS_MPFR)
    if (is_extended_int_kind_local(kind)) {
      I128 compact = 0;
      if (try_extract_i128_exact_from_value(input, compact)) {
        return Value::numeric_int_value_of(kind, compact);
      }
      GmpInt wide(static_cast<mp_bitcnt_t>(extended_int_bits_for_kind(kind)) + 64U);
      if (!mpz_set_from_integer_value(wide.value, input)) {
        const long double truncated = std::trunc(value_to_long_double(input));
        mpz_set_d(wide.value, static_cast<double>(truncated));
      }
      mpz_clamp_signed_bits(wide.value, extended_int_bits_for_kind(kind));
      return big_int_value_from_mpz(kind, wide.value);
    }
#endif
    return cast_int_kind(kind, value_to_i128(input));
  }
#if defined(SPARK_HAS_MPFR)
  if (is_high_precision_float_kind_local(kind)) {
    return cast_high_precision_float_kind(kind, input);
  }
  if (input.kind == Value::Kind::Numeric && input.numeric_value &&
      is_high_precision_float_kind_local(input.numeric_value->kind)) {
    return cast_float_kind(kind, static_cast<long double>(numeric_value_to_double(input)));
  }
#endif
  return cast_float_kind(kind, value_to_long_double(input));
}

bool cast_numeric_to_kind_inplace(Value::NumericKind kind, const Value& input,
                                  Value& target) {
  if (!is_numeric_kind(input)) {
    return false;
  }
  if (target.kind != Value::Kind::Numeric || !target.numeric_value ||
      target.numeric_value->kind != kind) {
    return false;
  }

  if (numeric_kind_is_int(kind)) {
#if defined(SPARK_HAS_MPFR)
    if (is_extended_int_kind_local(kind)) {
      I128 compact = 0;
      if (try_extract_i128_exact_from_value(input, compact)) {
        assign_numeric_int_value_inplace(target, kind, compact);
        return true;
      }
      GmpInt wide(static_cast<mp_bitcnt_t>(extended_int_bits_for_kind(kind)) + 64U);
      if (!mpz_set_from_integer_value(wide.value, input)) {
        const long double truncated = std::trunc(value_to_long_double(input));
        mpz_set_d(wide.value, static_cast<double>(truncated));
      }
      mpz_clamp_signed_bits(wide.value, extended_int_bits_for_kind(kind));
      assign_numeric_big_int_value_inplace(target, kind, mpz_to_decimal_string(wide.value));
      return true;
    }
#endif
    assign_numeric_int_value_inplace(target, kind, value_to_i128(input));
    return true;
  }

#if defined(SPARK_HAS_MPFR)
  if (is_high_precision_float_kind_local(kind)) {
    auto& numeric = *target.numeric_value;
    I128 compact = 0;
    const bool has_exact_i128 = try_extract_i128_exact_from_value(input, compact);
    if (has_exact_i128) {
      numeric.kind = kind;
      numeric.payload.clear();
      numeric.parsed_int_valid = true;
      numeric.parsed_int = compact;
      numeric.parsed_float_valid = false;
      numeric.parsed_float = 0.0L;
      ++numeric.revision;
      if (numeric.high_precision_cache) {
        numeric.high_precision_cache.reset();
      }
      return true;
    }
    auto cache = ensure_unique_high_precision_cache(numeric);
    mpfr_set_from_value(cache->value, input);
    cache->populated = true;
    // Preserve exact cache value and only refresh metadata.
    numeric.kind = kind;
    numeric.payload.clear();
    numeric.parsed_int_valid = false;
    numeric.parsed_int = 0;
    numeric.parsed_float_valid = false;
    numeric.parsed_float = 0.0L;
    ++numeric.revision;
    ++cache->epoch;
    return true;
  }
  if (input.kind == Value::Kind::Numeric && input.numeric_value &&
      is_high_precision_float_kind_local(input.numeric_value->kind)) {
    assign_numeric_float_value_inplace(
        target, kind, static_cast<long double>(numeric_value_to_double(input)));
    return true;
  }
#endif

  assign_numeric_float_value_inplace(target, kind, value_to_long_double(input));
  return true;
}

Value eval_numeric_binary_value(BinaryOp op, const Value& left, const Value& right) {
  if (!is_numeric_kind(left) || !is_numeric_kind(right)) {
    throw EvalException("numeric operation expects numeric operands");
  }
#if defined(SPARK_HAS_MPFR)
  if (const auto fast = try_eval_high_precision_same_kind(op, left, right); fast.has_value()) {
    return *fast;
  }
#endif
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

bool eval_numeric_binary_arithmetic_inplace_fast(BinaryOp op, const Value& left, const Value& right,
                                                 Value& target) {
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

  // Idempotence rule: (x % y) % y == x % y for y != 0.
  // Running one step preserves exact runtime semantics and removes O(n) work.
  if (op == BinaryOp::Mod) {
    if (!eval_numeric_binary_value_inplace(op, target, rhs, target)) {
      target = eval_numeric_binary_value(op, target, rhs);
    }
    return true;
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
    auto target_cache = ensure_unique_high_precision_cache(*target.numeric_value);
    auto& scratch = mpfr_scratch_for_kind(result_kind);
    if (const auto rhs_src = mpfr_cached_srcptr(rhs); rhs_src) {
      mpfr_set(scratch.rhs.value, rhs_src, MPFR_RNDN);
    } else {
      mpfr_set_from_value(scratch.rhs.value, rhs);
    }
    if ((op == BinaryOp::Div || op == BinaryOp::Mod) && mpfr_zero_p(scratch.rhs.value) != 0) {
      throw EvalException(op == BinaryOp::Div ? "division by zero" : "modulo by zero");
    }

    bool pow_rhs_ui_valid = false;
    unsigned long pow_rhs_ui = 0UL;
    bool pow_rhs_si_valid = false;
    long pow_rhs_si = 0L;
    bool rhs_si_valid = false;
    long rhs_si = 0L;
    bool rhs_mod_ui_valid = false;
    unsigned long rhs_mod_ui = 0UL;
    if (op == BinaryOp::Pow && mpfr_integer_p(scratch.rhs.value) != 0) {
      if (mpfr_fits_ulong_p(scratch.rhs.value, MPFR_RNDN) != 0) {
        pow_rhs_ui = mpfr_get_ui(scratch.rhs.value, MPFR_RNDN);
        pow_rhs_ui_valid = true;
      } else if (mpfr_fits_slong_p(scratch.rhs.value, MPFR_RNDN) != 0) {
        pow_rhs_si = mpfr_get_si(scratch.rhs.value, MPFR_RNDN);
        pow_rhs_si_valid = true;
      }
    } else if ((op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul ||
                op == BinaryOp::Div || op == BinaryOp::Mod) &&
               mpfr_integer_p(scratch.rhs.value) != 0 &&
               mpfr_fits_slong_p(scratch.rhs.value, MPFR_RNDN) != 0) {
      rhs_si = mpfr_get_si(scratch.rhs.value, MPFR_RNDN);
      rhs_si_valid = true;
      if (op == BinaryOp::Mod && rhs_si != 0L &&
          rhs_si != std::numeric_limits<long>::min()) {
        rhs_mod_ui = static_cast<unsigned long>(rhs_si < 0L ? -rhs_si : rhs_si);
        rhs_mod_ui_valid = rhs_mod_ui != 0UL;
      }
    }

    const auto apply_pow_step = [&](mpfr_t out, const mpfr_t lhs) {
      if (pow_rhs_ui_valid) {
        if (pow_rhs_ui == 2UL) {
          mpfr_sqr(out, lhs, MPFR_RNDN);
          return;
        }
        mpfr_pow_ui(out, lhs, pow_rhs_ui, MPFR_RNDN);
        return;
      }
      if (pow_rhs_si_valid) {
        if (pow_rhs_si == -1L) {
          mpfr_ui_div(out, 1UL, lhs, MPFR_RNDN);
          return;
        }
        mpfr_pow_si(out, lhs, pow_rhs_si, MPFR_RNDN);
        return;
      }
      mpfr_pow(out, lhs, scratch.rhs.value, MPFR_RNDN);
    };
    const auto repeat_step = mpfr_repeat_step_controller(op);

    const bool target_is_zero = mpfr_zero_p(target_cache->value) != 0;
    const bool rhs_is_number = mpfr_number_p(scratch.rhs.value) != 0;
    if (target_is_zero && rhs_is_number) {
      // Exact stable shortcuts from arithmetic identities:
      // 0*x = 0, 0/x = 0 (x != 0), 0%x = 0, 0^x = 0 for x>0, 0^0 = 1.
      switch (op) {
        case BinaryOp::Mul:
        case BinaryOp::Div:
        case BinaryOp::Mod:
          return true;
        case BinaryOp::Pow: {
          const int rhs_sign = mpfr_sgn(scratch.rhs.value);
          if (rhs_sign > 0) {
            return true;
          }
          if (rhs_sign == 0) {
            mpfr_set_ui(target_cache->value, 1U, MPFR_RNDN);
            target_cache->populated = true;
            mark_high_precision_numeric_metadata_only(*target.numeric_value, result_kind);
            return true;
          }
          break;
        }
        default:
          break;
      }
    }

    // Exact zero/identity/idempotent shortcuts for stable semantics and lower overhead.
    if ((op == BinaryOp::Add || op == BinaryOp::Sub) && mpfr_zero_p(scratch.rhs.value) != 0) {
      return true;
    }
    if (op == BinaryOp::Mul) {
      if (mpfr_zero_p(scratch.rhs.value) != 0) {
        mpfr_set_ui(target_cache->value, 0U, MPFR_RNDN);
        target_cache->populated = true;
        mark_high_precision_numeric_metadata_only(*target.numeric_value, result_kind);
        return true;
      }
      if (mpfr_cmp_si(scratch.rhs.value, 1L) == 0) {
        return true;
      }
      if (mpfr_cmp_si(scratch.rhs.value, -1L) == 0) {
        if ((remaining & 1LL) != 0LL) {
          mpfr_neg(target_cache->value, target_cache->value, MPFR_RNDN);
          target_cache->populated = true;
          mark_high_precision_numeric_metadata_only(*target.numeric_value, result_kind);
        }
        return true;
      }
    }
    if (op == BinaryOp::Div) {
      if (mpfr_cmp_si(scratch.rhs.value, 1L) == 0) {
        return true;
      }
      if (mpfr_cmp_si(scratch.rhs.value, -1L) == 0) {
        if ((remaining & 1LL) != 0LL) {
          mpfr_neg(target_cache->value, target_cache->value, MPFR_RNDN);
          target_cache->populated = true;
          mark_high_precision_numeric_metadata_only(*target.numeric_value, result_kind);
        }
        return true;
      }
    }
    if (op == BinaryOp::Mod) {
      mpfr_fmod(target_cache->value, target_cache->value, scratch.rhs.value, MPFR_RNDN);
      target_cache->populated = true;
      mark_high_precision_numeric_metadata_only(*target.numeric_value, result_kind);
      return true;
    }

    // Exact collapse for repeated scaling by +/-2^k:
    // x <- x * (±2^k)  or  x <- x / (±2^k)
    // This is semantically exact in MPFR and avoids per-iteration mul/div calls.
    if (remaining > 1 && (op == BinaryOp::Mul || op == BinaryOp::Div)) {
      if (const auto factor = mpfr_try_integer_pow2_factor(scratch.rhs.value); factor.valid) {
        __int128 total_shift =
            static_cast<__int128>(factor.shift) * static_cast<__int128>(remaining);
        while (total_shift > 0) {
          const long chunk = static_cast<long>(
              std::min<__int128>(total_shift, std::numeric_limits<long>::max()));
          if (op == BinaryOp::Mul) {
            mpfr_mul_2si(target_cache->value, target_cache->value, chunk, MPFR_RNDN);
          } else {
            mpfr_div_2si(target_cache->value, target_cache->value, chunk, MPFR_RNDN);
          }
          total_shift -= static_cast<__int128>(chunk);
        }
        if (factor.negative && ((remaining & 1LL) != 0LL)) {
          mpfr_neg(target_cache->value, target_cache->value, MPFR_RNDN);
        }
        target_cache->populated = true;
        mark_high_precision_numeric_metadata_only(*target.numeric_value, result_kind);
        return true;
      }
    }

    // Strict exact closed-form for repeated add/sub when operands have a
    // bounded exact source bit-width and the precision budget can prove that
    // all intermediate sums are representable without per-step rounding.
    if (remaining > 1 && (op == BinaryOp::Add || op == BinaryOp::Sub)) {
      bool rhs_bounded_origin = false;
      int rhs_source_bits = 0;
      if (rhs.kind == Value::Kind::Numeric && rhs.numeric_value) {
        if (rhs.numeric_value->parsed_float_valid) {
          rhs_bounded_origin = true;
          rhs_source_bits = std::numeric_limits<long double>::digits;
        } else if (rhs.numeric_value->parsed_int_valid) {
          rhs_bounded_origin = true;
          rhs_source_bits = bit_width_i128_signed(static_cast<I128>(rhs.numeric_value->parsed_int)) + 1;
        } else if (is_high_precision_float_kind_local(rhs.numeric_value->kind)) {
          if (const auto rhs_src = mpfr_cached_srcptr(rhs); rhs_src) {
            if (mpfr_zero_p(rhs_src) != 0) {
              rhs_bounded_origin = true;
              rhs_source_bits = 1;
            } else if (mpfr_number_p(rhs_src) != 0) {
              const auto min_prec = mpfr_min_prec(rhs_src);
              if (min_prec > 0) {
                rhs_bounded_origin = true;
                rhs_source_bits = static_cast<int>(min_prec);
              }
            }
          }
        }
      } else if (rhs.kind == Value::Kind::Double) {
        rhs_bounded_origin = true;
        rhs_source_bits = std::numeric_limits<double>::digits;
      } else if (rhs.kind == Value::Kind::Int) {
        rhs_bounded_origin = true;
        rhs_source_bits = std::numeric_limits<long long>::digits;
      }

      if (rhs_bounded_origin) {
        int lhs_source_bits = static_cast<int>(mpfr_precision_for_kind(result_kind));
        if (target.numeric_value->parsed_float_valid) {
          lhs_source_bits = std::numeric_limits<long double>::digits;
        } else if (target.numeric_value->parsed_int_valid) {
          lhs_source_bits =
              bit_width_i128_signed(static_cast<I128>(target.numeric_value->parsed_int)) + 1;
        } else if (target_cache->populated) {
          if (mpfr_zero_p(target_cache->value) != 0) {
            lhs_source_bits = 1;
          } else if (mpfr_number_p(target_cache->value) != 0) {
            const auto min_prec = mpfr_min_prec(target_cache->value);
            if (min_prec > 0) {
              lhs_source_bits = static_cast<int>(min_prec);
            }
          }
        }

        const int source_bits = std::max(lhs_source_bits, rhs_source_bits);
        const int repeat_bits = bit_width_u64(static_cast<unsigned long long>(remaining));
        const int required_bits = source_bits + repeat_bits + 1;
        const int precision_bits = static_cast<int>(mpfr_precision_for_kind(result_kind));

        if (required_bits <= precision_bits) {
          if (remaining <= static_cast<long long>(std::numeric_limits<long>::max())) {
            mpfr_mul_si(scratch.tmp.value, scratch.rhs.value, static_cast<long>(remaining),
                        MPFR_RNDN);
          } else {
            mpfr_set_ui(scratch.tmp.value, 0U, MPFR_RNDN);
            long long chunk_remaining = remaining;
            constexpr long long kChunkMax =
                static_cast<long long>(std::numeric_limits<long>::max());
            while (chunk_remaining > 0) {
              const long step =
                  static_cast<long>(chunk_remaining > kChunkMax ? kChunkMax : chunk_remaining);
              mpfr_mul_si(scratch.out.value, scratch.rhs.value, step, MPFR_RNDN);
              mpfr_add(scratch.tmp.value, scratch.tmp.value, scratch.out.value, MPFR_RNDN);
              chunk_remaining -= static_cast<long long>(step);
            }
          }
          if (op == BinaryOp::Sub) {
            mpfr_neg(scratch.tmp.value, scratch.tmp.value, MPFR_RNDN);
          }
          mpfr_add(target_cache->value, target_cache->value, scratch.tmp.value, MPFR_RNDN);
          target_cache->populated = true;
          mark_high_precision_numeric_metadata_only(*target.numeric_value, result_kind);
          return true;
        }
      }
    }

    const auto apply_mpfr_step_once = [&](mpfr_t out, const mpfr_t lhs) {
      if (rhs_si_valid) {
        switch (op) {
          case BinaryOp::Add:
            mpfr_add_si(out, lhs, rhs_si, MPFR_RNDN);
            return true;
          case BinaryOp::Sub:
            mpfr_sub_si(out, lhs, rhs_si, MPFR_RNDN);
            return true;
          case BinaryOp::Mul:
            mpfr_mul_si(out, lhs, rhs_si, MPFR_RNDN);
            return true;
          case BinaryOp::Div:
            if (rhs_si == 0L) {
              throw EvalException("division by zero");
            }
            mpfr_div_si(out, lhs, rhs_si, MPFR_RNDN);
            return true;
          case BinaryOp::Mod:
            if (rhs_si == 0L) {
              throw EvalException("modulo by zero");
            }
            if (rhs_mod_ui_valid) {
              mpfr_fmod_ui(out, lhs, rhs_mod_ui, MPFR_RNDN);
            } else {
              mpfr_fmod(out, lhs, scratch.rhs.value, MPFR_RNDN);
            }
            return true;
          default:
            break;
        }
      }
      if (repeat_step) {
        repeat_step(out, lhs, scratch.rhs.value);
        return true;
      }
      if (op == BinaryOp::Pow) {
        apply_pow_step(out, lhs);
        return true;
      }
      return false;
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
        mark_high_precision_numeric_metadata_only(*target.numeric_value, result_kind);
        return true;
      }

      mpfr_set(target_cache->value, scratch.out.value, MPFR_RNDN);
      remaining -= 2;
    }

    if (repeat_step && !rhs_si_valid) {
      mpfr_repeat_apply_unrolled(repeat_step, target_cache->value, scratch.rhs.value, remaining);
    } else if (rhs_si_valid) {
      for (long long i = 0; i + 3 < remaining; i += 4) {
        apply_mpfr_step_once(target_cache->value, target_cache->value);
        apply_mpfr_step_once(target_cache->value, target_cache->value);
        apply_mpfr_step_once(target_cache->value, target_cache->value);
        apply_mpfr_step_once(target_cache->value, target_cache->value);
      }
      for (long long i = (remaining & ~3LL); i < remaining; ++i) {
        apply_mpfr_step_once(target_cache->value, target_cache->value);
      }
    } else if (op == BinaryOp::Pow) {
      for (long long i = 0; i + 3 < remaining; i += 4) {
        apply_pow_step(target_cache->value, target_cache->value);
        apply_pow_step(target_cache->value, target_cache->value);
        apply_pow_step(target_cache->value, target_cache->value);
        apply_pow_step(target_cache->value, target_cache->value);
      }
      for (long long i = (remaining & ~3LL); i < remaining; ++i) {
        apply_pow_step(target_cache->value, target_cache->value);
      }
    } else {
      return false;
    }
    target_cache->populated = true;
    mark_high_precision_numeric_metadata_only(*target.numeric_value, result_kind);
    return true;
  }
#endif

  const auto fast_target_kind = runtime_numeric_kind(target);
  const auto fast_rhs_kind = runtime_numeric_kind(rhs);
  const auto fast_result_kind = promote_result_kind(op, fast_target_kind, fast_rhs_kind);
  if (!numeric_kind_is_int(fast_result_kind) && !is_high_precision_float_kind_local(fast_result_kind) &&
      target.kind == Value::Kind::Numeric && target.numeric_value &&
      target.numeric_value->kind == fast_result_kind && rhs.kind == Value::Kind::Numeric &&
      rhs.numeric_value && rhs.numeric_value->kind == fast_result_kind) {
    const auto read_numeric_float = [](const Value::NumericValue& numeric) {
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

    if (fast_result_kind == Value::NumericKind::F64 &&
        (op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul ||
         op == BinaryOp::Div || op == BinaryOp::Mod || op == BinaryOp::Pow)) {
      double acc = static_cast<double>(read_numeric_float(*target.numeric_value));
      const double step = static_cast<double>(read_numeric_float(*rhs.numeric_value));
      if (op == BinaryOp::Pow) {
        if (step == 1.0) {
          return true;
        }
        if (step == 0.0) {
          assign_numeric_float_value_inplace(target, fast_result_kind, 1.0L);
          return true;
        }
      }
      if ((op == BinaryOp::Div || op == BinaryOp::Mod) && step == 0.0) {
        throw EvalException(op == BinaryOp::Div ? "division by zero" : "modulo by zero");
      }
      if (op == BinaryOp::Mod) {
        double out = 0.0;
        if (!try_eval_float_binary_fast<double>(op, acc, step, out)) {
          return false;
        }
        assign_numeric_float_value_inplace(target, fast_result_kind, static_cast<long double>(out));
        return true;
      }
      const auto step_integral_exp = integral_exponent_if_safe(static_cast<long double>(step));
      const long long unrolled = remaining / 8LL;
      const long long tail = remaining % 8LL;
      auto apply_once = [&]() {
        switch (op) {
          case BinaryOp::Add:
            acc += step;
            break;
          case BinaryOp::Sub:
            acc -= step;
            break;
          case BinaryOp::Mul:
            acc *= step;
            break;
          case BinaryOp::Div:
            acc /= step;
            break;
          case BinaryOp::Mod:
            acc = fast_fmod_scalar(acc, step);
            break;
          case BinaryOp::Pow:
            if (step_integral_exp.has_value()) {
              acc = powi_double_numeric_core(acc, *step_integral_exp);
            } else {
              acc = std::pow(acc, step);
            }
            break;
          default:
            break;
        }
      };
      for (long long i = 0; i < unrolled; ++i) {
        apply_once();
        apply_once();
        apply_once();
        apply_once();
        apply_once();
        apply_once();
        apply_once();
        apply_once();
      }
      for (long long i = 0; i < tail; ++i) {
        apply_once();
      }
      assign_numeric_float_value_inplace(target, fast_result_kind, static_cast<long double>(acc));
      return true;
    }

    if (fast_result_kind == Value::NumericKind::F32 &&
        (op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul ||
         op == BinaryOp::Div || op == BinaryOp::Mod || op == BinaryOp::Pow)) {
      float acc = static_cast<float>(read_numeric_float(*target.numeric_value));
      const float step = static_cast<float>(read_numeric_float(*rhs.numeric_value));
      if (op == BinaryOp::Pow) {
        if (step == 1.0F) {
          return true;
        }
        if (step == 0.0F) {
          assign_numeric_float_value_inplace(target, fast_result_kind, 1.0L);
          return true;
        }
      }
      if ((op == BinaryOp::Div || op == BinaryOp::Mod) && step == 0.0F) {
        throw EvalException(op == BinaryOp::Div ? "division by zero" : "modulo by zero");
      }
      if (op == BinaryOp::Mod) {
        float out = 0.0F;
        if (!try_eval_float_binary_fast<float>(op, acc, step, out)) {
          return false;
        }
        assign_numeric_float_value_inplace(target, fast_result_kind, static_cast<long double>(out));
        return true;
      }
      const auto step_integral_exp = integral_exponent_if_safe(static_cast<long double>(step));
      const long long unrolled = remaining / 8LL;
      const long long tail = remaining % 8LL;
      auto apply_once = [&]() {
        switch (op) {
          case BinaryOp::Add:
            acc += step;
            break;
          case BinaryOp::Sub:
            acc -= step;
            break;
          case BinaryOp::Mul:
            acc *= step;
            break;
          case BinaryOp::Div:
            acc /= step;
            break;
          case BinaryOp::Mod:
            acc = fast_fmod_scalar(acc, step);
            break;
          case BinaryOp::Pow:
            if (step_integral_exp.has_value()) {
              acc = powi_float_numeric_core(acc, *step_integral_exp);
            } else {
              acc = std::pow(acc, step);
            }
            break;
          default:
            break;
        }
      };
      for (long long i = 0; i < unrolled; ++i) {
        apply_once();
        apply_once();
        apply_once();
        apply_once();
        apply_once();
        apply_once();
        apply_once();
        apply_once();
      }
      for (long long i = 0; i < tail; ++i) {
        apply_once();
      }
      assign_numeric_float_value_inplace(target, fast_result_kind, static_cast<long double>(acc));
      return true;
    }
  }

  if (numeric_kind_is_int(fast_result_kind) && !is_extended_int_kind_local(fast_result_kind) &&
      target.kind == Value::Kind::Numeric &&
      target.numeric_value && target.numeric_value->kind == fast_result_kind &&
      rhs.kind == Value::Kind::Numeric && rhs.numeric_value &&
      rhs.numeric_value->kind == fast_result_kind &&
      (op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul ||
       op == BinaryOp::Mod)) {
    const int bits = effective_int_bits(fast_result_kind);
    const I128 one = 1;
    const I128 lo = (bits >= 128) ? i128_min() : -(one << (bits - 1));
    const I128 hi = (bits >= 128) ? i128_max() : ((one << (bits - 1)) - 1);
    auto acc = clamp_to_signed_bits(value_to_i128(target), bits);
    const auto step = clamp_to_signed_bits(value_to_i128(rhs), bits);

    if (op == BinaryOp::Mod) {
      if (step == 0) {
        throw EvalException("modulo by zero");
      }
      // (x % y) % y == x % y for y != 0, so one step is enough.
      acc = clamp_to_signed_bits(acc % step, bits);
      assign_numeric_int_value_inplace(target, fast_result_kind, acc);
      return true;
    }

    if (op == BinaryOp::Add || op == BinaryOp::Sub) {
      if (step == 0) {
        return true;
      }
      const bool sub_unsafe_negate = (op == BinaryOp::Sub && step == i128_min());
      if (!sub_unsafe_negate) {
        const I128 delta = (op == BinaryOp::Add) ? step : -step;
        const I128 n = static_cast<I128>(remaining);
        if (delta > 0) {
          if (acc < hi) {
            const I128 room = hi - acc;
            const I128 needed = room / delta + ((room % delta) != 0 ? 1 : 0);
            if (n >= needed) {
              acc = hi;
            } else {
              acc += delta * n;
            }
          }
          assign_numeric_int_value_inplace(target, fast_result_kind, acc);
          return true;
        }
        if (delta < 0) {
          const I128 step_abs = -delta;
          if (acc > lo) {
            const I128 room = acc - lo;
            const I128 needed = room / step_abs + ((room % step_abs) != 0 ? 1 : 0);
            if (n >= needed) {
              acc = lo;
            } else {
              acc -= step_abs * n;
            }
          }
          assign_numeric_int_value_inplace(target, fast_result_kind, acc);
          return true;
        }
        return true;
      }
    }

    if (op == BinaryOp::Mul) {
      if (step == 1) {
        return true;
      }
      if (step == 0) {
        assign_numeric_int_value_inplace(target, fast_result_kind, 0);
        return true;
      }
      const long long unrolled = remaining / 8LL;
      const long long tail = remaining % 8LL;
      auto apply_once = [&]() {
        I128 out = 0;
        if (__builtin_mul_overflow(acc, step, &out)) {
          const bool non_negative = (acc == 0 || step == 0) || ((acc > 0) == (step > 0));
          out = non_negative ? i128_max() : i128_min();
        }
        acc = clamp_to_signed_bits(out, bits);
      };
      for (long long i = 0; i < unrolled; ++i) {
        apply_once();
        apply_once();
        apply_once();
        apply_once();
        apply_once();
        apply_once();
        apply_once();
        apply_once();
      }
      for (long long i = 0; i < tail; ++i) {
        apply_once();
      }
      assign_numeric_int_value_inplace(target, fast_result_kind, acc);
      return true;
    }
  }

  for (long long i = 0; i < remaining; ++i) {
    if (!eval_numeric_binary_value_inplace(op, target, rhs, target)) {
      target = eval_numeric_binary_value(op, target, rhs);
    }
  }
  return true;
}
