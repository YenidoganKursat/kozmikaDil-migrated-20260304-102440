Value bench_mixed_numeric_op_runtime(const std::string& kind_name, const std::string& op_name,
                                     long long loops, long long seed_x, long long seed_y) {
  if (loops < 0) {
    throw EvalException("bench_mixed_numeric_op_runtime() loops must be non-negative");
  }
  const auto kind = numeric_kind_from_name(kind_name);

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

  if (numeric_kind_is_int(kind)) {
    const auto int_bits_for_kind = [](Value::NumericKind k) -> int {
      switch (k) {
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
          return 0;
        default:
          return 64;
      }
    };
    const auto clamp_signed_bits_i64 = [](long long value, int bits) -> long long {
      if (bits <= 0 || bits >= 63) {
        return value;
      }
      const long long hi = (static_cast<long long>(1) << (bits - 1)) - 1LL;
      const long long lo = -1LL * (static_cast<long long>(1) << (bits - 1));
      if (value > hi) {
        return hi;
      }
      if (value < lo) {
        return lo;
      }
      return value;
    };
    const auto domain_bits_for_kind = [&](int bits, BinaryOp current_op) -> int {
      if (current_op == BinaryOp::Pow) {
        if (bits <= 8) {
          return 3;
        }
        if (bits <= 16) {
          return 4;
        }
        if (bits <= 32) {
          return 6;
        }
        return 7;
      }
      if (current_op == BinaryOp::Mul) {
        if (bits <= 8) {
          return 4;
        }
        if (bits <= 16) {
          return 8;
        }
        if (bits <= 32) {
          return 12;
        }
        return 16;
      }
      if (bits <= 8) {
        return 6;
      }
      if (bits <= 16) {
        return 12;
      }
      if (bits <= 32) {
        return 18;
      }
      return 20;
    };
    const auto exponent_mask_for_kind = [&](int bits) -> int {
      if (bits <= 8) {
        return 3;
      }
      if (bits <= 16) {
        return 7;
      }
      return 7;
    };
    const auto step_x = [](std::uint64_t state) {
      return (state * 1664525ULL + 1013904223ULL) & kMask31;
    };
    const auto step_y = [](std::uint64_t state) {
      return (state * 22695477ULL + 1ULL) & kMask31;
    };

    const int bits = int_bits_for_kind(kind);
    const int dom_bits = domain_bits_for_kind(bits, op);
    const long long x_mask = (1LL << dom_bits) - 1LL;
    const long long x_bias = 1LL << (dom_bits - 1);
    const int exp_mask = exponent_mask_for_kind(bits);

    for (long long i = 0; i < loops; ++i) {
      sx = step_x(sx);
      sy = step_y(sy);

      const long long x = static_cast<long long>(sx & static_cast<std::uint64_t>(x_mask)) - x_bias;
      long long y = static_cast<long long>(sy & static_cast<std::uint64_t>(x_mask)) - x_bias;

      if ((op == BinaryOp::Div || op == BinaryOp::Mod) && y == 0) {
        y = 1LL;
      }

      long long out = 0LL;
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
          out = x % y;
          break;
        case BinaryOp::Pow: {
          const auto exponent =
              static_cast<unsigned long long>(sy & static_cast<std::uint64_t>(exp_mask));
          long long result = 1LL;
          long long factor = x;
          unsigned long long e = exponent;
          while (e > 0ULL) {
            if ((e & 1ULL) != 0ULL) {
              long long next = 0LL;
              (void)__builtin_mul_overflow(result, factor, &next);
              result = next;
            }
            e >>= 1ULL;
            if (e > 0ULL) {
              long long next_factor = 0LL;
              (void)__builtin_mul_overflow(factor, factor, &next_factor);
              factor = next_factor;
            }
          }
          out = result;
          break;
        }
        default:
          throw EvalException("bench_mixed_numeric_op_runtime() unsupported operator");
      }

      out = clamp_signed_bits_i64(out, bits);
      if ((i & kSinkMask) == 0LL) {
        sink = static_cast<double>(out);
      }
      if ((i & kChecksumMask) == 0LL) {
        acc += static_cast<double>(out);
      }
    }

    return Value::double_value_of(acc);
  }

  if (numeric_kind_is_high_precision_float(kind)) {
#if defined(SPARK_HAS_MPFR)
    const auto step_x = [](std::uint64_t state) {
      return (state * 1664525ULL + 1013904223ULL) & kMask31;
    };
    const auto step_y = [](std::uint64_t state) {
      return (state * 22695477ULL + 1ULL) & kMask31;
    };
    const auto precision = mpfr_precision_for_kind(kind);
    MpfrValue x(precision);
    MpfrValue y(precision);
    MpfrValue out_hp(precision);
    MpfrValue tmp(precision);

    constexpr long long kDomain = 65536LL;
    constexpr long long kBias = 32767LL;

    for (long long i = 0; i < loops; ++i) {
      sx = step_x(sx);
      sy = step_y(sy);
      const long long x_raw =
          static_cast<long long>(sx % static_cast<std::uint64_t>(kDomain)) - kBias;
      mpfr_set_si(x.value, x_raw, MPFR_RNDN);

      switch (op) {
        case BinaryOp::Add: {
          const long y_raw = static_cast<long>(
              static_cast<long long>(sy % static_cast<std::uint64_t>(kDomain)) - kBias);
          mpfr_add_si(out_hp.value, x.value, y_raw, MPFR_RNDN);
          break;
        }
        case BinaryOp::Sub: {
          const long y_raw = static_cast<long>(
              static_cast<long long>(sy % static_cast<std::uint64_t>(kDomain)) - kBias);
          mpfr_sub_si(out_hp.value, x.value, y_raw, MPFR_RNDN);
          break;
        }
        case BinaryOp::Mul: {
          const long y_raw = static_cast<long>(
              static_cast<long long>(sy % static_cast<std::uint64_t>(kDomain)) - kBias);
          mpfr_mul_si(out_hp.value, x.value, y_raw, MPFR_RNDN);
          break;
        }
        case BinaryOp::Div: {
          long y_raw = static_cast<long>(
              static_cast<long long>(sy % static_cast<std::uint64_t>(kDomain)) - kBias);
          if (y_raw == 0L) {
            y_raw = 1L;
          }
          mpfr_div_si(out_hp.value, x.value, y_raw, MPFR_RNDN);
          break;
        }
        case BinaryOp::Mod: {
          long y_raw = static_cast<long>(
              static_cast<long long>(sy % static_cast<std::uint64_t>(kDomain)) - kBias);
          if (y_raw == 0L) {
            y_raw = 1L;
          }
          const unsigned long mag =
              static_cast<unsigned long>(y_raw < 0L ? -y_raw : y_raw);
          mpfr_fmod_ui(out_hp.value, x.value, mag == 0UL ? 1UL : mag, MPFR_RNDN);
          break;
        }
        case BinaryOp::Pow: {
          long exp_raw = static_cast<long>(static_cast<long long>(sy % 9ULL) - 4LL);
          if (x_raw == 0LL && exp_raw < 0L) {
            exp_raw = 1L;
          }
          switch (exp_raw) {
            case 0L:
              mpfr_set_ui(out_hp.value, 1U, MPFR_RNDN);
              break;
            case 1L:
              mpfr_set(out_hp.value, x.value, MPFR_RNDN);
              break;
            case 2L:
              mpfr_sqr(out_hp.value, x.value, MPFR_RNDN);
              break;
            case 3L:
              mpfr_sqr(out_hp.value, x.value, MPFR_RNDN);
              mpfr_mul(out_hp.value, out_hp.value, x.value, MPFR_RNDN);
              break;
            case 4L:
              mpfr_sqr(out_hp.value, x.value, MPFR_RNDN);
              mpfr_sqr(out_hp.value, out_hp.value, MPFR_RNDN);
              break;
            case -1L:
              mpfr_ui_div(out_hp.value, 1UL, x.value, MPFR_RNDN);
              break;
            case -2L:
              mpfr_sqr(tmp.value, x.value, MPFR_RNDN);
              mpfr_ui_div(out_hp.value, 1UL, tmp.value, MPFR_RNDN);
              break;
            case -3L:
              mpfr_sqr(tmp.value, x.value, MPFR_RNDN);
              mpfr_mul(tmp.value, tmp.value, x.value, MPFR_RNDN);
              mpfr_ui_div(out_hp.value, 1UL, tmp.value, MPFR_RNDN);
              break;
            case -4L:
              mpfr_sqr(tmp.value, x.value, MPFR_RNDN);
              mpfr_sqr(tmp.value, tmp.value, MPFR_RNDN);
              mpfr_ui_div(out_hp.value, 1UL, tmp.value, MPFR_RNDN);
              break;
            default:
              if (exp_raw >= 0L) {
                mpfr_pow_ui(out_hp.value, x.value, static_cast<unsigned long>(exp_raw), MPFR_RNDN);
              } else {
                mpfr_pow_ui(tmp.value, x.value, static_cast<unsigned long>(-exp_raw), MPFR_RNDN);
                mpfr_ui_div(out_hp.value, 1UL, tmp.value, MPFR_RNDN);
              }
              break;
          }
          break;
        }
        default:
          throw EvalException("bench_mixed_numeric_op_runtime() unsupported operator");
      }

      const double out_d = mpfr_get_d(out_hp.value, MPFR_RNDN);
      if ((i & kSinkMask) == 0LL) {
        sink = out_d;
      }
      if ((i & kChecksumMask) == 0LL) {
        acc += out_d;
      }
    }
    return Value::double_value_of(acc);
#else
    throw EvalException("bench_mixed_numeric_op_runtime() high-precision requires MPFR");
#endif
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
