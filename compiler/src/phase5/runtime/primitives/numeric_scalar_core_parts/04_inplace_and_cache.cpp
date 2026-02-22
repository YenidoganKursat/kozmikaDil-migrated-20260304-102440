bool compute_numeric_arithmetic_inplace(BinaryOp op, const Value& left, const Value& right, Value& target) {
  if (target.kind != Value::Kind::Numeric || !target.numeric_value) {
    return false;
  }

  // Same-kind short path: when left/right/target already share the same numeric
  // primitive kind, skip promotion analysis and execute directly.
  const auto target_kind = target.numeric_value->kind;
  if (!is_high_precision_float_kind_local(target_kind) &&
      same_kind_short_path_enabled() &&
      left.kind == Value::Kind::Numeric && right.kind == Value::Kind::Numeric &&
      left.numeric_value && right.numeric_value) {
    const auto kind = target_kind;
    if (left.numeric_value->kind == kind && right.numeric_value->kind == kind) {
      if (numeric_kind_is_int(kind) && op != BinaryOp::Div && op != BinaryOp::Pow) {
        const auto lhs =
            left.numeric_value->parsed_int_valid ? left.numeric_value->parsed_int : value_to_i128(left);
        const auto rhs =
            right.numeric_value->parsed_int_valid ? right.numeric_value->parsed_int : value_to_i128(right);
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
          default:
            return false;
        }
        assign_numeric_int_value_inplace(target, kind, out);
        return true;
      }

      if (!numeric_kind_is_int(kind) && !is_high_precision_float_kind_local(kind)) {
        if (op == BinaryOp::Pow) {
          // Keep pow on the canonical path; short path is tuned for the
          // dominant arithmetic operators where we observe stable wins.
          return false;
        }
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
        const auto lhs = read_numeric_scalar(*left.numeric_value);
        const auto rhs = read_numeric_scalar(*right.numeric_value);
        if (kind == Value::NumericKind::F64) {
          double out = 0.0;
          if (!try_eval_float_binary_fast<double>(op, static_cast<double>(lhs),
                                                  static_cast<double>(rhs), out)) {
            return false;
          }
          assign_numeric_float_value_inplace(target, kind, static_cast<long double>(out));
          return true;
        }
        if (kind == Value::NumericKind::F32) {
          float out = 0.0F;
          if (!try_eval_float_binary_fast<float>(op, static_cast<float>(lhs),
                                                 static_cast<float>(rhs), out)) {
            return false;
          }
          assign_numeric_float_value_inplace(target, kind, static_cast<long double>(out));
          return true;
        }
        long double out = 0.0L;
        if (!try_eval_float_binary_fast<long double>(op, lhs, rhs, out)) {
          return false;
        }
        assign_numeric_float_value_inplace(target, kind, out);
        return true;
      }
    }
  }

  const auto left_kind = runtime_numeric_kind(left);
  const auto right_kind = runtime_numeric_kind(right);
  const auto result_kind = promote_result_kind(op, left_kind, right_kind);
  if (result_kind != target.numeric_value->kind) {
    return false;
  }

  // Generic fast scalar lane for non-integer, non-high-precision numeric pairs.
  if (left.kind == Value::Kind::Numeric && right.kind == Value::Kind::Numeric &&
      left.numeric_value && right.numeric_value &&
      left.numeric_value->kind == result_kind &&
      right.numeric_value->kind == result_kind &&
      !numeric_kind_is_int(result_kind) &&
      !is_high_precision_float_kind_local(result_kind)) {
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
    const auto lhs = read_numeric_scalar(*left.numeric_value);
    const auto rhs = read_numeric_scalar(*right.numeric_value);
    if (result_kind == Value::NumericKind::F64) {
      double out = 0.0;
      if (!try_eval_float_binary_fast<double>(op, static_cast<double>(lhs),
                                              static_cast<double>(rhs), out)) {
        if (op == BinaryOp::Pow) {
          if (const auto integral_exp = integral_exponent_if_safe(static_cast<long double>(rhs));
              integral_exp.has_value()) {
            out = powi_double_numeric_core(static_cast<double>(lhs), *integral_exp);
          } else {
            out = std::pow(static_cast<double>(lhs), static_cast<double>(rhs));
          }
        } else {
          return false;
        }
      }
      assign_numeric_float_value_inplace(target, result_kind, static_cast<long double>(out));
      return true;
    }
    if (result_kind == Value::NumericKind::F32) {
      float out = 0.0F;
      if (!try_eval_float_binary_fast<float>(op, static_cast<float>(lhs),
                                             static_cast<float>(rhs), out)) {
        if (op == BinaryOp::Pow) {
          if (const auto integral_exp = integral_exponent_if_safe(static_cast<long double>(rhs));
              integral_exp.has_value()) {
            out = powi_float_numeric_core(static_cast<float>(lhs), *integral_exp);
          } else {
            out = std::pow(static_cast<float>(lhs), static_cast<float>(rhs));
          }
        } else {
          return false;
        }
      }
      assign_numeric_float_value_inplace(target, result_kind, static_cast<long double>(out));
      return true;
    }
    long double out = 0.0L;
    if (!try_eval_float_binary_fast<long double>(op, lhs, rhs, out)) {
      if (op == BinaryOp::Pow) {
        if (const auto integral_exp = integral_exponent_if_safe(rhs); integral_exp.has_value()) {
          out = powi_long_double(lhs, *integral_exp);
        } else {
          out = std::pow(lhs, rhs);
        }
      } else {
        return false;
      }
    }
    assign_numeric_float_value_inplace(target, result_kind, out);
    return true;
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
    const auto transient_same_kind_float = [&](const Value& value) {
      if (value.kind != Value::Kind::Numeric || !value.numeric_value) {
        return false;
      }
      const auto& numeric = *value.numeric_value;
      return numeric.kind == result_kind && numeric.parsed_float_valid &&
             !numeric.parsed_int_valid && numeric.payload.empty() &&
             !numeric.high_precision_cache;
    };
    const bool direct_kernel_eligible =
        mpfr_direct_kernel_enabled() &&
        transient_same_kind_float(left) && transient_same_kind_float(right);
    // Hot-loop specialization for `acc = acc <op> rhs`: update target cache directly.
    const bool target_is_high_precision =
        target.numeric_value && target.numeric_value->kind == result_kind;
    const bool left_aliases_target =
        target_is_high_precision && left.kind == Value::Kind::Numeric &&
        left.numeric_value && left.numeric_value->kind == result_kind &&
        mpfr_numeric_cache_aliases(*left.numeric_value, *target.numeric_value);
    const bool right_aliases_target =
        target_is_high_precision && right.kind == Value::Kind::Numeric &&
        right.numeric_value && right.numeric_value->kind == result_kind &&
        mpfr_numeric_cache_aliases(*right.numeric_value, *target.numeric_value);
    if (left_aliases_target && op != BinaryOp::Pow) {
      auto target_cache = ensure_unique_high_precision_cache(*target.numeric_value);
      mpfr_srcptr rhs_src = mpfr_cached_srcptr(right);
      if (!rhs_src) {
        mpfr_set_from_value(scratch.rhs.value, right);
        rhs_src = scratch.rhs.value;
      }
      if (direct_kernel_eligible) {
        mpfr_binary_direct(op, target_cache->value, target_cache->value, rhs_src);
      } else {
        mpfr_binary_optimized(op, target_cache->value, target_cache->value, rhs_src);
      }
      target_cache->populated = true;
      mark_high_precision_numeric_metadata_only(*target.numeric_value, result_kind);
      return true;
    }

    // Generic memoized reuse:
    // - Pow: alias-safe (memo key includes operand revisions, so no stale hit).
    // - Other ops: only when target does not alias operands.
    if (target_is_high_precision) {
      const bool memo_allowed =
          (op == BinaryOp::Pow) || (!left_aliases_target && !right_aliases_target);
      if (memo_allowed) {
        if (const auto memo = try_eval_high_precision_same_kind(op, left, right); memo.has_value()) {
          return copy_numeric_value_inplace_internal(target, *memo);
        }
      }
    }

    // For non-alias cases, compute directly into target cache and skip scratch->assign copy.
    // This trims one mpfr_set per operation on hot paths like `c = a <op> b`.
    if (target_is_high_precision && !left_aliases_target && !right_aliases_target) {
      auto target_cache = ensure_unique_high_precision_cache(*target.numeric_value);
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
      if (direct_kernel_eligible) {
        mpfr_binary_direct(op, target_cache->value, lhs_src, rhs_src);
      } else {
        mpfr_binary_optimized(op, target_cache->value, lhs_src, rhs_src);
      }
      target_cache->populated = true;
      mark_high_precision_numeric_metadata_only(*target.numeric_value, result_kind);
      return true;
    }

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
    if (direct_kernel_eligible) {
      mpfr_binary_direct(op, scratch.out.value, lhs_src, rhs_src);
    } else {
      mpfr_binary_optimized(op, scratch.out.value, lhs_src, rhs_src);
    }
    assign_numeric_high_precision_inplace(target, result_kind, scratch.out.value);
    return true;
  }
#endif

  const auto lhs = value_to_long_double(left);
  const auto rhs = value_to_long_double(right);
  long double out = 0.0L;
  if (!try_eval_float_binary_fast<long double>(op, lhs, rhs, out)) {
    if (op == BinaryOp::Pow) {
      if (const auto integral_exp = integral_exponent_if_safe(rhs); integral_exp.has_value()) {
        out = powi_long_double(lhs, *integral_exp);
      } else {
        out = std::pow(lhs, rhs);
      }
    } else {
      return false;
    }
  }
  assign_numeric_float_value_inplace(target, result_kind, out);
  return true;
}

}  // namespace

bool copy_numeric_value_inplace(Value& target, const Value& source) {
  return copy_numeric_value_inplace_internal(target, source);
}

bool numeric_kind_is_high_precision_float(Value::NumericKind kind) {
  return is_high_precision_float_kind_local(kind);
}

long double normalize_numeric_float_value(Value::NumericKind kind, long double value) {
  if (numeric_kind_is_int(kind) || is_high_precision_float_kind_local(kind)) {
    return value;
  }
  return normalize_float_by_kind(kind, value);
}

void prewarm_numeric_runtime() {
#if defined(SPARK_HAS_MPFR)
  thread_local bool warmed = false;
  if (warmed) {
    return;
  }
  warmed = true;

  constexpr std::array<Value::NumericKind, 3> kKinds = {
      Value::NumericKind::F128, Value::NumericKind::F256, Value::NumericKind::F512};
  for (const auto kind : kKinds) {
    auto& scratch = mpfr_scratch_for_kind(kind);
    mpfr_set_ui(scratch.lhs.value, 0U, MPFR_RNDN);
    mpfr_set_ui(scratch.rhs.value, 0U, MPFR_RNDN);
    mpfr_set_ui(scratch.out.value, 0U, MPFR_RNDN);
    mpfr_set_ui(scratch.tmp.value, 0U, MPFR_RNDN);

    auto cache = acquire_mpfr_cache(mpfr_precision_for_kind(kind));
    mpfr_set_ui(cache->value, 0U, MPFR_RNDN);
    cache->populated = true;
  }
#endif
}

void initialize_high_precision_numeric_cache(Value& value) {
#if defined(SPARK_HAS_MPFR)
  if (value.kind != Value::Kind::Numeric || !value.numeric_value) {
    return;
  }
  auto& numeric = *value.numeric_value;
  if (!is_high_precision_float_kind_local(numeric.kind)) {
    return;
  }
  auto cache = get_or_init_high_precision_cache(numeric);
  cache->populated = true;
#else
  (void)value;
#endif
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

