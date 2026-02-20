#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>

#include <gmp.h>
#include <mpfr.h>

namespace {

enum class EvalKind {
  F8,
  F16,
  BF16,
  F32,
  F64,
  MPFR,
};

struct TypeSpec {
  const char* name;
  EvalKind kind;
  mpfr_prec_t precision;
  int eps_bits;
};

struct OpSpec {
  const char* symbol;
};

constexpr TypeSpec kTypes[] = {
    {"f8", EvalKind::F8, 64, 4},
    {"f16", EvalKind::F16, 64, 11},
    {"bf16", EvalKind::BF16, 64, 8},
    {"f32", EvalKind::F32, 64, 24},
    {"f64", EvalKind::F64, 64, 53},
    {"f128", EvalKind::MPFR, 113, 113},
    {"f256", EvalKind::MPFR, 237, 237},
    {"f512", EvalKind::MPFR, 493, 493},
};

constexpr OpSpec kOps[] = {
    {"+"},
    {"-"},
    {"*"},
    {"/"},
    {"%"},
    {"^"},
};

constexpr unsigned long kSeedBase = 123456789UL;

inline std::uint32_t float_to_u32(float value) {
  std::uint32_t out = 0;
  std::memcpy(&out, &value, sizeof(out));
  return out;
}

inline float u32_to_float(std::uint32_t bits) {
  float out = 0.0f;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

std::uint32_t round_shift_right_even_u32(std::uint32_t value, int shift) {
  if (shift == 0) {
    return value;
  }
  if (shift >= 32) {
    return 0;
  }
  const auto truncated = value >> shift;
  const auto mask = (std::uint32_t{1} << shift) - 1U;
  const auto remainder = value & mask;
  const auto halfway = std::uint32_t{1} << (shift - 1);
  if (remainder > halfway) {
    return truncated + 1U;
  }
  if (remainder < halfway) {
    return truncated;
  }
  return (truncated & 1U) ? (truncated + 1U) : truncated;
}

std::uint16_t f32_to_f16_bits_rne(float value) {
  const auto bits = float_to_u32(value);
  const auto sign = static_cast<std::uint16_t>((bits >> 16) & 0x8000U);
  const auto exp = static_cast<int>((bits >> 23) & 0xFFU);
  const auto frac = bits & 0x7FFFFFU;

  if (exp == 0xFF) {
    if (frac == 0) {
      return static_cast<std::uint16_t>(sign | 0x7C00U);
    }
    auto payload = static_cast<std::uint16_t>(frac >> 13);
    if (payload == 0) {
      payload = 1;
    }
    return static_cast<std::uint16_t>(sign | 0x7C00U | payload);
  }

  const auto exp_unbiased = exp - 127;
  auto half_exp = exp_unbiased + 15;
  if (half_exp >= 0x1F) {
    return static_cast<std::uint16_t>(sign | 0x7C00U);
  }

  if (half_exp <= 0) {
    if (half_exp < -10) {
      return sign;
    }
    const auto mantissa = frac | 0x800000U;
    const auto shift = 14 - half_exp;
    const auto half_frac = round_shift_right_even_u32(mantissa, shift);
    if (half_frac >= 0x400U) {
      return static_cast<std::uint16_t>(sign | 0x0400U);
    }
    return static_cast<std::uint16_t>(sign | half_frac);
  }

  auto half_frac = round_shift_right_even_u32(frac, 13);
  if (half_frac >= 0x400U) {
    half_frac = 0;
    ++half_exp;
    if (half_exp >= 0x1F) {
      return static_cast<std::uint16_t>(sign | 0x7C00U);
    }
  }
  return static_cast<std::uint16_t>(sign | (static_cast<std::uint16_t>(half_exp) << 10) |
                                    static_cast<std::uint16_t>(half_frac));
}

float f16_bits_to_f32(std::uint16_t bits) {
  const auto sign = static_cast<std::uint32_t>(bits & 0x8000U) << 16;
  const auto exp = static_cast<std::uint32_t>((bits >> 10) & 0x1FU);
  const auto frac = static_cast<std::uint32_t>(bits & 0x03FFU);
  if (exp == 0) {
    if (frac == 0) {
      return u32_to_float(sign);
    }
    const auto magnitude = std::ldexp(static_cast<float>(frac), -24);
    return (sign != 0U) ? -magnitude : magnitude;
  }
  if (exp == 0x1F) {
    return u32_to_float(sign | 0x7F800000U | (frac << 13));
  }
  const auto out_exp = exp + (127U - 15U);
  return u32_to_float(sign | (out_exp << 23) | (frac << 13));
}

float quantize_bf16_rne(float value) {
  auto bits = float_to_u32(value);
  const auto exp = bits & 0x7F800000U;
  const auto frac = bits & 0x007FFFFFU;
  if (exp == 0x7F800000U) {
    if (frac != 0) {
      bits |= 0x00010000U;
    }
    return u32_to_float(bits & 0xFFFF0000U);
  }
  const auto lsb = (bits >> 16) & 1U;
  bits += 0x7FFFU + lsb;
  bits &= 0xFFFF0000U;
  return u32_to_float(bits);
}

std::uint8_t f32_to_f8_e4m3fn_bits_rne(float value) {
  const auto bits = float_to_u32(value);
  const auto sign = static_cast<std::uint8_t>((bits & 0x80000000U) ? 0x80U : 0x00U);
  const auto exp = static_cast<int>((bits >> 23) & 0xFFU);
  const auto frac = bits & 0x7FFFFFU;

  if (exp == 0xFF) {
    if (frac == 0) {
      return static_cast<std::uint8_t>(sign | 0x7EU);
    }
    return static_cast<std::uint8_t>(sign | 0x7FU);
  }
  if ((bits & 0x7FFFFFFFU) == 0U) {
    return sign;
  }

  const auto exp_unbiased = exp - 127;
  auto f8_exp = exp_unbiased + 7;
  if (f8_exp >= 0x0F) {
    return static_cast<std::uint8_t>(sign | 0x7EU);
  }
  if (f8_exp <= 0) {
    const auto scaled = std::ldexp(std::fabs(static_cast<double>(value)), 9);
    const auto rounded = std::nearbyint(scaled);
    if (rounded <= 0.0) {
      return sign;
    }
    if (rounded >= 8.0) {
      return static_cast<std::uint8_t>(sign | 0x08U);
    }
    return static_cast<std::uint8_t>(sign | static_cast<std::uint8_t>(rounded));
  }

  auto mant = round_shift_right_even_u32(frac, 20);
  if (mant >= 8U) {
    mant = 0U;
    ++f8_exp;
    if (f8_exp >= 0x0F) {
      return static_cast<std::uint8_t>(sign | 0x7EU);
    }
  }
  return static_cast<std::uint8_t>(sign | (static_cast<std::uint8_t>(f8_exp) << 3) |
                                   static_cast<std::uint8_t>(mant));
}

float f8_e4m3fn_bits_to_f32(std::uint8_t bits) {
  const auto negative = (bits & 0x80U) != 0U;
  const auto exp = static_cast<int>((bits >> 3) & 0x0FU);
  const auto frac = static_cast<int>(bits & 0x07U);
  if (exp == 0) {
    const auto magnitude = std::ldexp(static_cast<float>(frac), -9);
    return negative ? -magnitude : magnitude;
  }
  if (exp == 0x0F && frac == 0x07) {
    return std::numeric_limits<float>::quiet_NaN();
  }
  const auto exponent = (exp == 0x0F) ? 8 : (exp - 7);
  const auto magnitude = std::ldexp(1.0f + (static_cast<float>(frac) / 8.0f), exponent);
  return negative ? -magnitude : magnitude;
}

long double quantize_scalar(EvalKind kind, long double value) {
  switch (kind) {
    case EvalKind::F8:
      return static_cast<long double>(
          f8_e4m3fn_bits_to_f32(f32_to_f8_e4m3fn_bits_rne(static_cast<float>(value))));
    case EvalKind::F16:
      return static_cast<long double>(
          f16_bits_to_f32(f32_to_f16_bits_rne(static_cast<float>(value))));
    case EvalKind::BF16:
      return static_cast<long double>(quantize_bf16_rne(static_cast<float>(value)));
    case EvalKind::F32:
      return static_cast<long double>(static_cast<float>(value));
    case EvalKind::F64:
      return static_cast<long double>(static_cast<double>(value));
    case EvalKind::MPFR:
      return value;
  }
  return value;
}

long double apply_scalar_op(const std::string& op, long double lhs, long double rhs) {
  if (op == "+") {
    return lhs + rhs;
  }
  if (op == "-") {
    return lhs - rhs;
  }
  if (op == "*") {
    return lhs * rhs;
  }
  if (op == "/") {
    return lhs / rhs;
  }
  if (op == "%") {
    return std::fmod(lhs, rhs);
  }
  if (op == "^") {
    return std::pow(lhs, rhs);
  }
  return std::numeric_limits<long double>::quiet_NaN();
}

bool is_non_finite(const mpfr_t value) {
  return mpfr_number_p(value) == 0;
}

void apply_mpfr_op(mpfr_t out, const mpfr_t lhs, const mpfr_t rhs, const std::string& op) {
  if (op == "+") {
    mpfr_add(out, lhs, rhs, MPFR_RNDN);
    return;
  }
  if (op == "-") {
    mpfr_sub(out, lhs, rhs, MPFR_RNDN);
    return;
  }
  if (op == "*") {
    mpfr_mul(out, lhs, rhs, MPFR_RNDN);
    return;
  }
  if (op == "/") {
    mpfr_div(out, lhs, rhs, MPFR_RNDN);
    return;
  }
  if (op == "%") {
    mpfr_fmod(out, lhs, rhs, MPFR_RNDN);
    return;
  }
  if (op == "^") {
    mpfr_pow(out, lhs, rhs, MPFR_RNDN);
    return;
  }
  mpfr_set_nan(out);
}

double as_double(const mpfr_t value) {
  return mpfr_get_d(value, MPFR_RNDN);
}

void random_near_one(mpfr_t out, gmp_randstate_t rng) {
  mpfr_urandomb(out, rng);
  mpfr_add_d(out, out, 0.5, MPFR_RNDN);
  if (gmp_urandomm_ui(rng, 2U) == 1U) {
    mpfr_neg(out, out, MPFR_RNDN);
  }
}

long random_small_int(gmp_randstate_t rng, long lo, long hi) {
  const auto span = static_cast<unsigned long>(hi - lo + 1);
  return lo + static_cast<long>(gmp_urandomm_ui(rng, span));
}

int modulo_stress_shift(EvalKind kind) {
  switch (kind) {
    case EvalKind::F8:
      return 4;
    case EvalKind::F16:
      return 8;
    case EvalKind::BF16:
      return 12;
    case EvalKind::F32:
    case EvalKind::F64:
    case EvalKind::MPFR:
      return 20;
  }
  return 12;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: mpfr_stepwise_reference <loops>\n";
    return 1;
  }
  const long long loops = std::atoll(argv[1]);
  if (loops <= 0) {
    std::cerr << "loops must be > 0\n";
    return 1;
  }

  for (const auto& op_spec : kOps) {
    const std::string op(op_spec.symbol);
    for (const auto& type : kTypes) {
      const auto p = type.precision;
      const auto pref = std::max<mpfr_prec_t>(p * 2 + 256, 512);
      const auto pstats = 8192;

      mpfr_t a, b, impl;
      mpfr_t ahi, bhi, href;
      mpfr_t diff, abs_ref, rel, eps, eps_ratio;
      mpfr_t max_abs, max_rel, max_eps;
      mpfr_inits2(p, a, b, impl, nullptr);
      mpfr_inits2(pref, ahi, bhi, href, nullptr);
      mpfr_inits2(pstats, diff, abs_ref, rel, eps, eps_ratio, max_abs, max_rel, max_eps, nullptr);
      mpfr_set_ui(max_abs, 0u, MPFR_RNDN);
      mpfr_set_ui(max_rel, 0u, MPFR_RNDN);
      mpfr_set_ui(max_eps, 0u, MPFR_RNDN);
      mpfr_set_ui_2exp(eps, 1u, -(static_cast<long>(type.eps_bits) - 1), MPFR_RNDN);

      long long first_nonzero = -1;
      long long non_finite_count = 0;
      double h1 = 0.0;
      double h2 = 0.0;
      double h3 = 0.0;

      gmp_randstate_t rng;
      gmp_randinit_mt(rng);
      gmp_randseed_ui(rng, kSeedBase + static_cast<unsigned long>(type.eps_bits) +
                               static_cast<unsigned long>(op[0]));

      for (long long step = 0; step < loops; ++step) {
        random_near_one(ahi, rng);
        random_near_one(bhi, rng);

        if (op == "%") {
          mpfr_mul_2si(ahi, ahi, modulo_stress_shift(type.kind), MPFR_RNDN);
        }
        if (op == "^") {
          const auto exp = random_small_int(rng, -4, 4);
          mpfr_set_si(bhi, exp, MPFR_RNDN);
          if (exp < 0 && mpfr_zero_p(ahi) != 0) {
            mpfr_set_d(ahi, 1.0, MPFR_RNDN);
          }
        } else if ((op == "/" || op == "%") && mpfr_zero_p(bhi) != 0) {
          mpfr_set_d(bhi, 1.0, MPFR_RNDN);
        }

        if (type.kind == EvalKind::MPFR) {
          mpfr_set(a, ahi, MPFR_RNDN);
          mpfr_set(b, bhi, MPFR_RNDN);
          if ((op == "/" || op == "%") && mpfr_zero_p(b) != 0) {
            mpfr_set_d(b, 1.0, MPFR_RNDN);
            mpfr_set_d(bhi, 1.0, MPFR_RNDN);
          }
          apply_mpfr_op(impl, a, b, op);
          if (is_non_finite(impl)) {
            ++non_finite_count;
            continue;
          }
        } else {
          auto lhs = quantize_scalar(type.kind, static_cast<long double>(mpfr_get_d(ahi, MPFR_RNDN)));
          auto rhs = quantize_scalar(type.kind, static_cast<long double>(mpfr_get_d(bhi, MPFR_RNDN)));
          if ((op == "/" || op == "%") && rhs == 0.0L) {
            rhs = quantize_scalar(type.kind, 1.0L);
            mpfr_set_d(bhi, static_cast<double>(rhs), MPFR_RNDN);
          }
          auto out = apply_scalar_op(op, lhs, rhs);
          if (!std::isfinite(static_cast<double>(out))) {
            ++non_finite_count;
            continue;
          }
          out = quantize_scalar(type.kind, out);
          mpfr_set_d(impl, static_cast<double>(out), MPFR_RNDN);
        }

        apply_mpfr_op(href, ahi, bhi, op);
        if (is_non_finite(href)) {
          ++non_finite_count;
          continue;
        }

        mpfr_sub(diff, impl, href, MPFR_RNDN);
        mpfr_abs(diff, diff, MPFR_RNDN);
        if (mpfr_cmp(diff, max_abs) > 0) {
          mpfr_set(max_abs, diff, MPFR_RNDN);
        }

        mpfr_abs(abs_ref, href, MPFR_RNDN);
        if (mpfr_zero_p(abs_ref) != 0) {
          mpfr_set_ui(abs_ref, 1u, MPFR_RNDN);
        }
        mpfr_div(rel, diff, abs_ref, MPFR_RNDN);
        if (mpfr_cmp(rel, max_rel) > 0) {
          mpfr_set(max_rel, rel, MPFR_RNDN);
        }

        mpfr_div(eps_ratio, diff, eps, MPFR_RNDN);
        if (mpfr_cmp(eps_ratio, max_eps) > 0) {
          mpfr_set(max_eps, eps_ratio, MPFR_RNDN);
        }

        if (first_nonzero < 0 && mpfr_zero_p(diff) == 0) {
          first_nonzero = step;
        }

        const auto c = as_double(impl);
        h1 += c;
        h2 += c * c;
        h3 += c * static_cast<double>(step + 1);
      }

      std::cout << type.name << ","
                << op << ","
                << loops << ","
                << std::setprecision(17) << as_double(max_abs) << ","
                << std::setprecision(17) << as_double(max_rel) << ","
                << std::setprecision(17) << as_double(max_eps) << ","
                << first_nonzero << ","
                << non_finite_count << ","
                << std::setprecision(17) << h1 << ","
                << std::setprecision(17) << h2 << ","
                << std::setprecision(17) << h3 << "\n";

      gmp_randclear(rng);
      mpfr_clears(a, b, impl, ahi, bhi, href, nullptr);
      mpfr_clears(diff, abs_ref, rel, eps, eps_ratio, max_abs, max_rel, max_eps, nullptr);
    }
  }

  return 0;
}
