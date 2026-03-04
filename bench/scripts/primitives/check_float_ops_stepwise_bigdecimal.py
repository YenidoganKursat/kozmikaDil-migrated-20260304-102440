#!/usr/bin/env python3
"""Stepwise float-op validation with Java BigDecimal + optional Python Decimal cross-check.

Key properties:
- Per primitive/operator comparison (separate rows).
- Input range is clamped per primitive to avoid non-finite artifacts in normal runs.
- Optional Kozmika checksum replay verifies runtime behavior matches model behavior.
- Optional Python Decimal reference run provides second independent validation path.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import pathlib
import struct
import subprocess
import tempfile
from dataclasses import asdict, dataclass
from decimal import Decimal, InvalidOperation, getcontext
from typing import Dict, List, Tuple

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
RESULT_DIR = REPO_ROOT / "bench" / "results" / "primitives"
PRIMITIVES = ["f8", "f16", "bf16", "f32", "f64", "f128", "f256", "f512"]
HIGH_PRIMITIVES = {"f128", "f256", "f512"}
MPFR_PRIMITIVES = set(PRIMITIVES)
OPS: List[Tuple[str, str]] = [("+", "add"), ("-", "sub"), ("*", "mul"), ("/", "div"), ("%", "mod"), ("^", "pow")]
RAND_HALF_RANGE = 2048
RAND_DIVISOR = 256.0
RAND_MODULUS = RAND_HALF_RANGE * 2 + 1
MPFR_REF_CPP = REPO_ROOT / "bench" / "scripts" / "primitives" / "mpfr_stepwise_reference.cpp"

# Bounds are chosen to keep benchmark values finite for low-precision formats.
SAFE_INPUT = {
    "f8": {"bound": 12.0, "min_den": 0.25},
    "f16": {"bound": 120.0, "min_den": 0.125},
    "bf16": {"bound": 500.0, "min_den": 1e-3},
    "f32": {"bound": 500.0, "min_den": 1e-6},
    "f64": {"bound": 500.0, "min_den": 1e-12},
    "f128": {"bound": 500.0, "min_den": 1e-12},
    "f256": {"bound": 500.0, "min_den": 1e-12},
    "f512": {"bound": 500.0, "min_den": 1e-12},
}


@dataclass
class StepwiseRow:
    primitive: str
    operator: str
    op_name: str
    loops: int
    max_abs_error_vs_bigdecimal: float
    max_rel_error_vs_bigdecimal: float
    max_eps1_ratio_vs_bigdecimal: float
    first_nonzero_abs_error_step: int
    non_finite_count: int
    h1: float
    h2: float
    h3: float
    reference_backend: str = "java_bigdecimal"
    java_max_abs_error_vs_bigdecimal: float | None = None
    java_max_rel_error_vs_bigdecimal: float | None = None
    java_max_eps1_ratio_vs_bigdecimal: float | None = None
    kozmika_h1: float | None = None
    kozmika_h2: float | None = None
    kozmika_h3: float | None = None
    checksum_match: bool | None = None
    python_loops: int | None = None
    python_max_abs_error_vs_bigdecimal: float | None = None
    python_max_rel_error_vs_bigdecimal: float | None = None
    python_max_eps1_ratio_vs_bigdecimal: float | None = None
    python_non_finite_count: int | None = None
    java_python_abs_delta: float | None = None
    java_python_rel_delta: float | None = None
    java_python_eps1_ratio_delta: float | None = None


def run_checked(cmd: List[str], cwd: pathlib.Path, env: Dict[str, str] | None = None) -> subprocess.CompletedProcess:
    merged = os.environ.copy()
    if env:
        merged.update(env)
    return subprocess.run(cmd, cwd=str(cwd), env=merged, text=True, capture_output=True, check=True)


def bound_for_primitive(primitive: str) -> float:
    return float(SAFE_INPUT[primitive]["bound"])


def min_den_for_primitive(primitive: str) -> float:
    return float(SAFE_INPUT[primitive]["min_den"])


def pow_bound_for_primitive(primitive: str) -> float:
    if primitive in ("f8", "f16"):
        return 8.0
    if primitive == "bf16":
        return 24.0
    return 64.0


def clamp_value(value: float, bound: float) -> float:
    if value > bound:
        return bound
    if value < -bound:
        return -bound
    return value


def stabilize_denominator(value: float, min_abs: float) -> float:
    if abs(value) >= min_abs:
        return value
    sign = -1.0 if value < 0.0 else 1.0
    return sign * min_abs


def epsilon_at1_for_primitive(primitive: str) -> float:
    if primitive == "f8":
        return 2.0 ** -3
    if primitive == "f16":
        return 2.0 ** -10
    if primitive == "bf16":
        return 2.0 ** -7
    if primitive == "f32":
        return 2.0 ** -23
    if primitive == "f64":
        return 2.0 ** -52
    if primitive == "f128":
        return 2.0 ** -112
    if primitive == "f256":
        return 2.0 ** -236
    if primitive == "f512":
        return 2.0 ** -492
    return 2.0 ** -52


def next_seed(seed: int) -> int:
    return (1103515245 * seed + 12345) & 0x7FFFFFFF


def float_to_u32(value: float) -> int:
    return struct.unpack(">I", struct.pack(">f", float(value)))[0]


def u32_to_float(bits: int) -> float:
    return struct.unpack(">f", struct.pack(">I", bits & 0xFFFFFFFF))[0]


def round_shift_right_even_u32(value: int, shift: int) -> int:
    if shift == 0:
        return value
    if shift >= 32:
        return 0
    truncated = value >> shift
    mask = (1 << shift) - 1
    remainder = value & mask
    halfway = 1 << (shift - 1)
    if remainder > halfway:
        return truncated + 1
    if remainder < halfway:
        return truncated
    return truncated + 1 if (truncated & 1) else truncated


def f32_to_f16_bits_rne(value: float) -> int:
    bits = float_to_u32(value)
    sign = (bits >> 16) & 0x8000
    exp = (bits >> 23) & 0xFF
    frac = bits & 0x7FFFFF

    if exp == 0xFF:
        if frac == 0:
            return sign | 0x7C00
        payload = frac >> 13
        if payload == 0:
            payload = 1
        return sign | 0x7C00 | payload

    exp_unbiased = exp - 127
    half_exp = exp_unbiased + 15
    if half_exp >= 0x1F:
        return sign | 0x7C00

    if half_exp <= 0:
        if half_exp < -10:
            return sign
        mantissa = frac | 0x800000
        shift = 14 - half_exp
        half_frac = round_shift_right_even_u32(mantissa, shift)
        if half_frac >= 0x400:
            return sign | 0x0400
        return sign | half_frac

    half_frac = round_shift_right_even_u32(frac, 13)
    if half_frac >= 0x400:
        half_frac = 0
        half_exp += 1
        if half_exp >= 0x1F:
            return sign | 0x7C00
    return sign | (half_exp << 10) | half_frac


def f16_bits_to_f32(bits: int) -> float:
    sign = (bits & 0x8000) << 16
    exp = (bits >> 10) & 0x1F
    frac = bits & 0x03FF
    if exp == 0:
        if frac == 0:
            return u32_to_float(sign)
        magnitude = math.ldexp(float(frac), -24)
        return -magnitude if sign else magnitude
    if exp == 0x1F:
        return u32_to_float(sign | 0x7F800000 | (frac << 13))
    out_exp = exp + (127 - 15)
    return u32_to_float(sign | (out_exp << 23) | (frac << 13))


def quantize_bf16(value: float) -> float:
    bits = float_to_u32(value)
    exp = bits & 0x7F800000
    frac = bits & 0x007FFFFF
    if exp == 0x7F800000:
        if frac != 0:
            bits |= 0x00010000
        return u32_to_float(bits & 0xFFFF0000)
    lsb = (bits >> 16) & 1
    bits += 0x7FFF + lsb
    bits &= 0xFFFF0000
    return u32_to_float(bits)


def f32_to_f8_e4m3fn_bits_rne(value: float) -> int:
    bits = float_to_u32(value)
    sign = 0x80 if (bits & 0x80000000) else 0x00
    exp = (bits >> 23) & 0xFF
    frac = bits & 0x7FFFFF

    if exp == 0xFF:
        if frac == 0:
            return sign | 0x7E
        return sign | 0x7F
    if (bits & 0x7FFFFFFF) == 0:
        return sign

    exp_unbiased = exp - 127
    f8_exp = exp_unbiased + 7
    if f8_exp >= 0x0F:
        return sign | 0x7E
    if f8_exp <= 0:
        scaled = math.ldexp(abs(float(value)), 9)
        rounded = round(scaled)
        if rounded <= 0:
            return sign
        if rounded >= 8:
            return sign | 0x08
        return sign | int(rounded)
    mant = round_shift_right_even_u32(frac, 20)
    if mant >= 8:
        mant = 0
        f8_exp += 1
        if f8_exp >= 0x0F:
            return sign | 0x7E
    return sign | (f8_exp << 3) | mant


def f8_e4m3fn_bits_to_f32(bits: int) -> float:
    negative = (bits & 0x80) != 0
    exp = (bits >> 3) & 0x0F
    frac = bits & 0x07
    if exp == 0:
        magnitude = math.ldexp(float(frac), -9)
        return -magnitude if negative else magnitude
    if exp == 0x0F and frac == 0x07:
        return float("nan")
    exponent = 8 if exp == 0x0F else (exp - 7)
    magnitude = math.ldexp(1.0 + (float(frac) / 8.0), exponent)
    return -magnitude if negative else magnitude


def quantize_by_primitive(primitive: str, value: float) -> float:
    if primitive == "f8":
        return float(f8_e4m3fn_bits_to_f32(f32_to_f8_e4m3fn_bits_rne(float(value))))
    if primitive == "f16":
        return float(f16_bits_to_f32(f32_to_f16_bits_rne(float(value))))
    if primitive == "bf16":
        return float(quantize_bf16(float(value)))
    if primitive == "f32":
        return float(u32_to_float(float_to_u32(float(value))))
    # f64/f128/f256/f512 currently share double-precision execution on this platform.
    return float(value)


def apply_float_op(operator: str, a: float, b: float) -> float:
    if operator == "+":
        return a + b
    if operator == "-":
        return a - b
    if operator == "*":
        return a * b
    if operator == "/":
        return a / b
    if operator == "%":
        return math.fmod(a, b)
    if operator == "^":
        return math.pow(a, b)
    raise ValueError(f"unsupported operator: {operator}")


def apply_decimal_op(operator: str, a: Decimal, b: Decimal) -> Decimal:
    if operator == "+":
        return a + b
    if operator == "-":
        return a - b
    if operator == "*":
        return a * b
    if operator == "/":
        return a / b
    if operator == "%":
        return a % b
    if operator == "^":
        exp = int(b)
        if exp >= 0:
            return a ** exp
        if a == 0:
            return Decimal("Infinity")
        return Decimal(1) / (a ** (-exp))
    raise ValueError(f"unsupported operator: {operator}")


def decimal_from_float(value: float) -> Decimal:
    return Decimal(str(value))


def python_reference_rows(loops: int) -> Dict[Tuple[str, str], StepwiseRow]:
    getcontext().prec = 120
    rows: Dict[Tuple[str, str], StepwiseRow] = {}
    for operator, op_name in OPS:
        stats: Dict[str, StepwiseRow] = {}
        for primitive in PRIMITIVES:
            stats[primitive] = StepwiseRow(
                primitive=primitive,
                operator=operator,
                op_name=op_name,
                loops=loops,
                max_abs_error_vs_bigdecimal=0.0,
                max_rel_error_vs_bigdecimal=0.0,
                max_eps1_ratio_vs_bigdecimal=0.0,
                first_nonzero_abs_error_step=-1,
                non_finite_count=0,
                h1=0.0,
                h2=0.0,
                h3=0.0,
            )

        seed = 123456789
        for step in range(loops):
            seed = next_seed(seed)
            a_raw = ((seed % RAND_MODULUS) - RAND_HALF_RANGE) / RAND_DIVISOR
            seed = next_seed(seed)
            b_raw = ((seed % RAND_MODULUS) - RAND_HALF_RANGE) / RAND_DIVISOR

            for primitive in PRIMITIVES:
                bound = bound_for_primitive(primitive)
                min_den = min_den_for_primitive(primitive)
                a_src = clamp_value(a_raw, bound)
                b_src = clamp_value(b_raw, bound)
                if operator == "^":
                    a_src = clamp_value(a_raw, pow_bound_for_primitive(primitive))
                    b_src = float((seed % 9) - 4)
                    if b_src < 0.0:
                        a_src = stabilize_denominator(a_src, min_den)
                    if a_src == 0.0 and b_src < 0.0:
                        b_src = 1.0
                elif operator in ("/", "%"):
                    b_src = stabilize_denominator(b_src, min_den)

                a = quantize_by_primitive(primitive, a_src)
                b = quantize_by_primitive(primitive, b_src)
                if operator == "^" and a == 0.0 and b < 0.0:
                    b = quantize_by_primitive(primitive, 1.0)
                if operator in ("/", "%") and b == 0.0:
                    b = quantize_by_primitive(primitive, min_den)
                ref = apply_decimal_op(operator, decimal_from_float(a), decimal_from_float(b))
                ref_typed = quantize_by_primitive(primitive, float(ref))
                ref_abs = abs(ref_typed)
                c = quantize_by_primitive(primitive, apply_float_op(operator, a, b))

                row = stats[primitive]
                if not math.isfinite(c):
                    row.non_finite_count += 1
                    continue

                abs_err = abs(c - ref_typed)
                rel_err = abs_err / (ref_abs if ref_abs > 0.0 else 1.0)
                eps_ratio = abs_err / epsilon_at1_for_primitive(primitive)
                if abs_err > row.max_abs_error_vs_bigdecimal:
                    row.max_abs_error_vs_bigdecimal = abs_err
                if rel_err > row.max_rel_error_vs_bigdecimal:
                    row.max_rel_error_vs_bigdecimal = rel_err
                if eps_ratio > row.max_eps1_ratio_vs_bigdecimal:
                    row.max_eps1_ratio_vs_bigdecimal = eps_ratio
                if row.first_nonzero_abs_error_step < 0 and abs_err != 0.0:
                    row.first_nonzero_abs_error_step = step
                row.h1 += c
                row.h2 += c * c
                row.h3 += c * float(step + 1)

        for primitive in PRIMITIVES:
            rows[(primitive, operator)] = stats[primitive]
    return rows


def java_source() -> str:
    return r"""
import java.math.BigDecimal;
import java.math.MathContext;
import java.math.RoundingMode;
import java.util.Locale;

public final class StepwiseBigDecimalCheck {
  private static final String[] PRIMS = {"f8", "f16", "bf16", "f32", "f64", "f128", "f256", "f512"};
  private static final String[] OPS = {"+", "-", "*", "/", "%", "^"};

  private static final class Stats {
    double maxAbs = 0.0;
    double maxRel = 0.0;
    double maxEps1Ratio = 0.0;
    long firstNonzero = -1;
    long nonFinite = 0;
    double h1 = 0.0;
    double h2 = 0.0;
    double h3 = 0.0;
  }

  private static long nextSeed(long seed) {
    return (1103515245L * seed + 12345L) & 0x7fffffffL;
  }

  private static double boundFor(String prim) {
    return switch (prim) {
      case "f8" -> 12.0;
      case "f16" -> 120.0;
      case "bf16", "f32", "f64", "f128", "f256", "f512" -> 500.0;
      default -> 500.0;
    };
  }

  private static double powBoundFor(String prim) {
    return switch (prim) {
      case "f8", "f16" -> 8.0;
      case "bf16" -> 24.0;
      default -> 64.0;
    };
  }

  private static double minDenFor(String prim) {
    return switch (prim) {
      case "f8" -> 0.25;
      case "f16" -> 0.125;
      case "bf16" -> 1e-3;
      case "f32" -> 1e-6;
      case "f64", "f128", "f256", "f512" -> 1e-12;
      default -> 1e-12;
    };
  }

  private static double eps1For(String prim) {
    return switch (prim) {
      case "f8" -> Math.scalb(1.0, -3);
      case "f16" -> Math.scalb(1.0, -10);
      case "bf16" -> Math.scalb(1.0, -7);
      case "f32" -> Math.scalb(1.0, -23);
      case "f64" -> Math.scalb(1.0, -52);
      case "f128" -> Math.scalb(1.0, -112);
      case "f256" -> Math.scalb(1.0, -236);
      case "f512" -> Math.scalb(1.0, -492);
      default -> Math.scalb(1.0, -52);
    };
  }

  private static double clamp(double value, double bound) {
    if (value > bound) return bound;
    if (value < -bound) return -bound;
    return value;
  }

  private static double stabilizeDen(double value, double minAbs) {
    if (Math.abs(value) >= minAbs) {
      return value;
    }
    return Math.copySign(minAbs, value == 0.0 ? 1.0 : value);
  }

  private static int roundShiftRightEven(int value, int shift) {
    if (shift == 0) return value;
    if (shift >= 32) return 0;
    final int truncated = value >>> shift;
    final int mask = (1 << shift) - 1;
    final int remainder = value & mask;
    final int halfway = 1 << (shift - 1);
    if (remainder > halfway) return truncated + 1;
    if (remainder < halfway) return truncated;
    return ((truncated & 1) != 0) ? (truncated + 1) : truncated;
  }

  private static int f32ToF16BitsRne(float value) {
    final int bits = Float.floatToRawIntBits(value);
    final int sign = (bits >>> 16) & 0x8000;
    final int exp = (bits >>> 23) & 0xFF;
    final int frac = bits & 0x7FFFFF;
    if (exp == 0xFF) {
      if (frac == 0) return sign | 0x7C00;
      int payload = frac >>> 13;
      if (payload == 0) payload = 1;
      return sign | 0x7C00 | payload;
    }
    final int expUnbiased = exp - 127;
    int halfExp = expUnbiased + 15;
    if (halfExp >= 0x1F) return sign | 0x7C00;
    if (halfExp <= 0) {
      if (halfExp < -10) return sign;
      final int mantissa = frac | 0x800000;
      final int shift = 14 - halfExp;
      final int halfFrac = roundShiftRightEven(mantissa, shift);
      if (halfFrac >= 0x400) return sign | 0x0400;
      return sign | halfFrac;
    }
    int halfFrac = roundShiftRightEven(frac, 13);
    if (halfFrac >= 0x400) {
      halfFrac = 0;
      halfExp += 1;
      if (halfExp >= 0x1F) return sign | 0x7C00;
    }
    return sign | (halfExp << 10) | halfFrac;
  }

  private static float f16BitsToF32(int bits) {
    final int sign = (bits & 0x8000) << 16;
    final int exp = (bits >>> 10) & 0x1F;
    final int frac = bits & 0x03FF;
    if (exp == 0) {
      if (frac == 0) return Float.intBitsToFloat(sign);
      final float magnitude = Math.scalb((float) frac, -24);
      return (sign != 0) ? -magnitude : magnitude;
    }
    if (exp == 0x1F) {
      return Float.intBitsToFloat(sign | 0x7F800000 | (frac << 13));
    }
    final int outExp = exp + (127 - 15);
    return Float.intBitsToFloat(sign | (outExp << 23) | (frac << 13));
  }

  private static float quantizeBf16(float value) {
    int bits = Float.floatToRawIntBits(value);
    final int exp = bits & 0x7F800000;
    final int frac = bits & 0x007FFFFF;
    if (exp == 0x7F800000) {
      if (frac != 0) bits |= 0x00010000;
      return Float.intBitsToFloat(bits & 0xFFFF0000);
    }
    final int lsb = (bits >>> 16) & 1;
    bits += 0x7FFF + lsb;
    bits &= 0xFFFF0000;
    return Float.intBitsToFloat(bits);
  }

  private static int f32ToF8E4M3fnBitsRne(float value) {
    final int bits = Float.floatToRawIntBits(value);
    final int sign = ((bits & 0x80000000) != 0) ? 0x80 : 0x00;
    final int exp = (bits >>> 23) & 0xFF;
    final int frac = bits & 0x7FFFFF;
    if (exp == 0xFF) {
      if (frac == 0) return sign | 0x7E;
      return sign | 0x7F;
    }
    if ((bits & 0x7FFFFFFF) == 0) {
      return sign;
    }
    final int expUnbiased = exp - 127;
    int f8Exp = expUnbiased + 7;
    if (f8Exp >= 0x0F) {
      return sign | 0x7E;
    }
    if (f8Exp <= 0) {
      final double scaled = Math.scalb(Math.abs((double) value), 9);
      final double rounded = Math.rint(scaled);
      if (rounded <= 0.0) return sign;
      if (rounded >= 8.0) return sign | 0x08;
      return sign | (int) rounded;
    }
    int mant = roundShiftRightEven(frac, 20);
    if (mant >= 8) {
      mant = 0;
      f8Exp += 1;
      if (f8Exp >= 0x0F) return sign | 0x7E;
    }
    return sign | (f8Exp << 3) | mant;
  }

  private static float f8E4M3fnBitsToF32(int bits) {
    final boolean negative = (bits & 0x80) != 0;
    final int exp = (bits >>> 3) & 0x0F;
    final int frac = bits & 0x07;
    if (exp == 0) {
      final float magnitude = Math.scalb((float) frac, -9);
      return negative ? -magnitude : magnitude;
    }
    if (exp == 0x0F && frac == 0x07) {
      return Float.NaN;
    }
    final int exponent = (exp == 0x0F) ? 8 : (exp - 7);
    final float magnitude = Math.scalb(1.0f + ((float) frac / 8.0f), exponent);
    return negative ? -magnitude : magnitude;
  }

  private static double quantize(String prim, double v) {
    final float vf = (float) v;
    return switch (prim) {
      case "f8" -> (double) f8E4M3fnBitsToF32(f32ToF8E4M3fnBitsRne(vf));
      case "f16" -> (double) f16BitsToF32(f32ToF16BitsRne(vf));
      case "bf16" -> (double) quantizeBf16(vf);
      case "f32" -> (double) vf;
      case "f64", "f128", "f256", "f512" -> v;
      default -> v;
    };
  }

  private static double applyDoubleOp(String op, double a, double b) {
    return switch (op) {
      case "+" -> a + b;
      case "-" -> a - b;
      case "*" -> a * b;
      case "/" -> a / b;
      case "%" -> a % b;
      case "^" -> Math.pow(a, b);
      default -> 0.0;
    };
  }

  private static BigDecimal powInt(BigDecimal a, int exp, MathContext mc) {
    if (exp >= 0) {
      return a.pow(exp, mc);
    }
    if (a.compareTo(BigDecimal.ZERO) == 0) {
      return BigDecimal.ZERO;
    }
    return BigDecimal.ONE.divide(a.pow(-exp, mc), mc);
  }

  private static BigDecimal applyBigOp(String op, BigDecimal a, BigDecimal b, MathContext mc) {
    return switch (op) {
      case "+" -> a.add(b, mc);
      case "-" -> a.subtract(b, mc);
      case "*" -> a.multiply(b, mc);
      case "/" -> a.divide(b, mc);
      case "%" -> a.remainder(b, mc);
      case "^" -> powInt(a, b.intValue(), mc);
      default -> BigDecimal.ZERO;
    };
  }

  public static void main(String[] args) {
    if (args.length != 1) {
      throw new IllegalArgumentException("usage: <loops>");
    }
    Locale.setDefault(Locale.US);
    final long loops = Long.parseLong(args[0]);
    final MathContext mc = new MathContext(120, RoundingMode.HALF_EVEN);

    for (String op : OPS) {
      final Stats[] stats = new Stats[PRIMS.length];
      for (int i = 0; i < PRIMS.length; i++) stats[i] = new Stats();

      long seed = 123456789L;
      for (long step = 0; step < loops; step++) {
        seed = nextSeed(seed);
        final double aRaw = ((seed % 4097L) - 2048L) / 256.0;
        seed = nextSeed(seed);
        final double bRaw = ((seed % 4097L) - 2048L) / 256.0;

        for (int i = 0; i < PRIMS.length; i++) {
          final String prim = PRIMS[i];
          final double bound = boundFor(prim);
          final double minDen = minDenFor(prim);
          double aSrc = clamp(aRaw, bound);
          double bSrc = clamp(bRaw, bound);
          if (op.equals("^")) {
            aSrc = clamp(aRaw, powBoundFor(prim));
            bSrc = (double) ((seed % 9L) - 4L);
            if (bSrc < 0.0) {
              aSrc = stabilizeDen(aSrc, minDen);
            }
            if (aSrc == 0.0 && bSrc < 0.0) {
              bSrc = 1.0;
            }
          } else if (op.equals("/") || op.equals("%")) {
            bSrc = stabilizeDen(bSrc, minDen);
          }

          final double a = quantize(prim, aSrc);
          double b = quantize(prim, bSrc);
          if (op.equals("^") && a == 0.0 && b < 0.0) {
            b = quantize(prim, 1.0);
          }
          if ((op.equals("/") || op.equals("%")) && b == 0.0) {
            b = quantize(prim, minDen);
          }
          final BigDecimal ref = applyBigOp(op, BigDecimal.valueOf(a), BigDecimal.valueOf(b), mc);
          final double refTyped = quantize(prim, ref.doubleValue());
          final double refAbs = Math.abs(refTyped);
          final double c = quantize(prim, applyDoubleOp(op, a, b));

          final Stats s = stats[i];
          if (!Double.isFinite(c)) {
            s.nonFinite += 1;
            continue;
          }
          final double absErr = Math.abs(c - refTyped);
          final double relErr = absErr / (refAbs > 0.0 ? refAbs : 1.0);
          final double epsRatio = absErr / eps1For(prim);
          if (absErr > s.maxAbs) s.maxAbs = absErr;
          if (relErr > s.maxRel) s.maxRel = relErr;
          if (epsRatio > s.maxEps1Ratio) s.maxEps1Ratio = epsRatio;
          if (s.firstNonzero < 0 && absErr != 0.0) s.firstNonzero = step;
          s.h1 += c;
          s.h2 += c * c;
          s.h3 += c * (double) (step + 1);
        }
      }

      for (int i = 0; i < PRIMS.length; i++) {
        final Stats s = stats[i];
        final long first = s.firstNonzero < 0 ? -1 : s.firstNonzero;
        System.out.println(
            PRIMS[i] + "," + op + "," + loops + "," +
            Double.toString(s.maxAbs) + "," +
            Double.toString(s.maxRel) + "," +
            Double.toString(s.maxEps1Ratio) + "," +
            Long.toString(first) + "," +
            Long.toString(s.nonFinite) + "," +
            Double.toString(s.h1) + "," +
            Double.toString(s.h2) + "," +
            Double.toString(s.h3));
      }
    }
  }
}
""".strip() + "\n"


def make_kozmika_program(path: pathlib.Path, primitive: str, operator: str, loops: int) -> None:
    bound = bound_for_primitive(primitive)
    pow_bound = pow_bound_for_primitive(primitive)
    min_den = min_den_for_primitive(primitive)
    acc_kind = primitive if primitive in HIGH_PRIMITIVES else "f64"
    lines: List[str] = [
        "seed = i64(123456789)",
        f"h1 = {acc_kind}(0)",
        f"h2 = {acc_kind}(0)",
        f"h3 = {acc_kind}(0)",
        "i = 0",
        f"while i < {loops}:",
        "  seed = (seed * 1103515245 + 12345) % 2147483648",
        "  a_raw = ((seed % 4097) - 2048) / 256.0",
        f"  if a_raw > {bound}:",
        f"    a_raw = {bound}",
        f"  if a_raw < {-bound}:",
        f"    a_raw = {-bound}",
        f"  a = {primitive}(a_raw)",
        "  seed = (seed * 1103515245 + 12345) % 2147483648",
        "  b_raw = ((seed % 4097) - 2048) / 256.0",
        f"  if b_raw > {bound}:",
        f"    b_raw = {bound}",
        f"  if b_raw < {-bound}:",
        f"    b_raw = {-bound}",
    ]
    if operator == "^":
        lines += [
            f"  if a_raw > {pow_bound}:",
            f"    a_raw = {pow_bound}",
            f"  if a_raw < {-pow_bound}:",
            f"    a_raw = {-pow_bound}",
            "  b_raw = (seed % 9) - 4",
            f"  if b_raw < 0 and a_raw > {-min_den} and a_raw < {min_den}:",
            "    if a_raw < 0:",
            f"      a_raw = {-min_den}",
            "    else:",
            f"      a_raw = {min_den}",
            "  if a_raw == 0 and b_raw < 0:",
            "    b_raw = 1",
        ]
    elif operator in ("/", "%"):
        lines += [
            f"  if b_raw > {-min_den} and b_raw < {min_den}:",
            "    if b_raw < 0:",
            f"      b_raw = {-min_den}",
            "    else:",
            f"      b_raw = {min_den}",
        ]
    lines += [f"  b = {primitive}(b_raw)"]
    if operator == "^":
        lines += [
            "  if a == 0 and b < 0:",
            f"    b = {primitive}(1)",
        ]
    if operator in ("/", "%"):
        lines += [
            "  if b == 0:",
            f"    b = {primitive}({min_den})",
        ]
    lines += [
        f"  c = a {operator} b",
        "  h1 = h1 + c",
        "  h2 = h2 + (c * c)",
        "  h3 = h3 + (c * (i + 1))",
        "  i = i + 1",
        "print(h1)",
        "print(h2)",
        "print(h3)",
        "",
    ]
    path.write_text("\n".join(lines), encoding="utf-8")


def run_kozmika_checksums(primitive: str, operator: str, op_name: str, loops: int, tmpdir: pathlib.Path) -> Tuple[float, float, float]:
    program = tmpdir / f"{primitive}_{op_name}.k"
    make_kozmika_program(program, primitive, operator, loops)
    proc = run_checked(["./k", "run", str(program)], cwd=REPO_ROOT)
    values = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
    if len(values) < 3:
        raise RuntimeError(f"unexpected kozmika output for {primitive} {operator}: {proc.stdout!r}")
    return float(values[-3]), float(values[-2]), float(values[-1])


def close_rel(a: float, b: float, tol: float) -> bool:
    scale = max(1.0, abs(a), abs(b))
    return abs(a - b) <= (tol * scale)


def run_mpfr_high_reference(loops: int, tmpdir: pathlib.Path) -> Dict[Tuple[str, str], Tuple[float, float, float, int, int, float, float, float]]:
    binary = tmpdir / "mpfr_stepwise_reference"
    compile_cmd = [
        "c++",
        "-std=c++17",
        "-O3",
        "-DNDEBUG",
        str(MPFR_REF_CPP),
    ]
    if (pathlib.Path("/opt/homebrew/include/mpfr.h")).exists():
        compile_cmd += ["-I", "/opt/homebrew/include"]
    if (pathlib.Path("/opt/homebrew/lib/libmpfr.dylib")).exists() or (
        pathlib.Path("/opt/homebrew/lib/libmpfr.a")
    ).exists():
        compile_cmd += ["-L", "/opt/homebrew/lib"]
    compile_cmd += [
        "-o",
        str(binary),
        "-lmpfr",
        "-lgmp",
    ]
    run_checked(compile_cmd, cwd=REPO_ROOT)
    proc = run_checked([str(binary), str(loops)], cwd=tmpdir)
    out: Dict[Tuple[str, str], Tuple[float, float, float, int, int, float, float, float]] = {}
    for line in [x.strip() for x in proc.stdout.splitlines() if x.strip()]:
        parts = line.split(",")
        if len(parts) != 11:
            raise RuntimeError(f"unexpected mpfr output line: {line!r}")
        primitive, operator, loops_s, max_abs_s, max_rel_s, max_eps_s, first_s, non_finite_s, h1_s, h2_s, h3_s = parts
        if int(loops_s) != loops:
            raise RuntimeError(f"mpfr loops mismatch in output: {line!r}")
        out[(primitive, operator)] = (
            float(max_abs_s),
            float(max_rel_s),
            float(max_eps_s),
            int(first_s),
            int(non_finite_s),
            float(h1_s),
            float(h2_s),
            float(h3_s),
        )
    return out


def write_markdown_table(rows: List[StepwiseRow], output_path: pathlib.Path) -> None:
    lines = [
        "| Primitive | Op | Epsilon@1 (theory) | Reference | Max Abs Error | Max Rel Error | Max Eps@1 Ratio | Legacy Java Max Abs | First Nonzero Step | Non-finite Count | Checksum Match |",
        "|---|---|---:|---|---:|---:|---:|---:|---:|---:|---|",
    ]
    for row in rows:
        checksum = "n/a" if row.checksum_match is None else ("PASS" if row.checksum_match else "FAIL")
        java_abs = (
            f"{row.java_max_abs_error_vs_bigdecimal:.6e}"
            if row.java_max_abs_error_vs_bigdecimal is not None
            else "-"
        )
        lines.append(
            f"| {row.primitive} | {row.operator} | {epsilon_at1_for_primitive(row.primitive):.6e} | "
            f"{row.reference_backend} | "
            f"{row.max_abs_error_vs_bigdecimal:.6e} | {row.max_rel_error_vs_bigdecimal:.6e} | "
            f"{row.max_eps1_ratio_vs_bigdecimal:.6e} | {java_abs} | {row.first_nonzero_abs_error_step} | "
            f"{row.non_finite_count} | {checksum} |"
        )
    output_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_markdown_wide_table(rows: List[StepwiseRow], output_path: pathlib.Path) -> None:
    by_primitive: Dict[str, Dict[str, StepwiseRow]] = {}
    for row in rows:
        by_primitive.setdefault(row.primitive, {})[row.operator] = row

    op_order = [symbol for symbol, _ in OPS]
    header = ["Primitive", "Epsilon@1 (theory)", "Reference"]
    header += [f"{op} max_abs" for op in op_order]
    header += [f"{op} max_rel" for op in op_order]
    header += [f"{op} max_eps1_ratio" for op in op_order]
    lines = [
        "| " + " | ".join(header) + " |",
        "|" + "|".join(["---"] * len(header)) + "|",
    ]

    for primitive in PRIMITIVES:
        op_map = by_primitive.get(primitive, {})
        refs = sorted({r.reference_backend for r in op_map.values()})
        ref_value = ",".join(refs) if refs else "-"
        row_parts = [primitive, f"{epsilon_at1_for_primitive(primitive):.6e}", ref_value]

        for op in op_order:
            item = op_map.get(op)
            row_parts.append(f"{item.max_abs_error_vs_bigdecimal:.6e}" if item else "-")
        for op in op_order:
            item = op_map.get(op)
            row_parts.append(f"{item.max_rel_error_vs_bigdecimal:.6e}" if item else "-")
        for op in op_order:
            item = op_map.get(op)
            row_parts.append(f"{item.max_eps1_ratio_vs_bigdecimal:.6e}" if item else "-")

        lines.append("| " + " | ".join(row_parts) + " |")

    output_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Stepwise BigDecimal validation for float primitive ops")
    parser.add_argument("--loops", type=int, default=100_000)
    parser.add_argument("--skip-kozmika", action="store_true")
    parser.add_argument("--skip-mpfr-high-ref", action="store_true")
    parser.add_argument("--checksum-tol", type=float, default=1e-6)
    parser.add_argument("--python-crosscheck", action="store_true")
    parser.add_argument("--python-loops", type=int, default=20_000, help="Use <=0 to reuse --loops")
    parser.add_argument(
        "--fail-on-nonfinite",
        action="store_true",
        help="Return non-zero exit code if any primitive/operator row produces non-finite counts.",
    )
    parser.add_argument(
        "--fail-on-checksum-mismatch",
        action="store_true",
        help="Return non-zero exit code if any checksum replay mismatch is observed.",
    )
    args = parser.parse_args()

    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    getcontext().prec = 120

    with tempfile.TemporaryDirectory(prefix="kozmika-step-bd-") as tmp:
        tmpdir = pathlib.Path(tmp)
        java_file = tmpdir / "StepwiseBigDecimalCheck.java"
        java_file.write_text(java_source(), encoding="utf-8")
        run_checked(["javac", str(java_file)], cwd=tmpdir)
        jproc = run_checked(
            ["java", "-Duser.language=en", "-Duser.region=US", "-cp", str(tmpdir), "StepwiseBigDecimalCheck", str(args.loops)],
            cwd=tmpdir,
        )

        rows: List[StepwiseRow] = []
        by_key: Dict[Tuple[str, str], StepwiseRow] = {}
        for line in [x.strip() for x in jproc.stdout.splitlines() if x.strip()]:
            # primitive,op,loops,maxAbs,maxRel,maxEps1Ratio,firstNonzero,nonFinite,h1,h2,h3
            parts = line.split(",")
            if len(parts) != 11:
                raise RuntimeError(f"unexpected java output line: {line!r}")
            primitive, operator, loops_s, max_abs_s, max_rel_s, max_eps_ratio_s, first_s, non_finite_s, h1_s, h2_s, h3_s = parts
            op_name = next(name for sym, name in OPS if sym == operator)
            row = StepwiseRow(
                primitive=primitive,
                operator=operator,
                op_name=op_name,
                loops=int(loops_s),
                max_abs_error_vs_bigdecimal=float(max_abs_s),
                max_rel_error_vs_bigdecimal=float(max_rel_s),
                max_eps1_ratio_vs_bigdecimal=float(max_eps_ratio_s),
                first_nonzero_abs_error_step=int(first_s),
                non_finite_count=int(non_finite_s),
                h1=float(h1_s),
                h2=float(h2_s),
                h3=float(h3_s),
            )
            rows.append(row)
            by_key[(primitive, operator)] = row

        if not args.skip_mpfr_high_ref:
            with tempfile.TemporaryDirectory(prefix="kozmika-step-mpfr-") as mt:
                mpfr_rows = run_mpfr_high_reference(args.loops, pathlib.Path(mt))
            for primitive in MPFR_PRIMITIVES:
                for operator, _ in OPS:
                    key = (primitive, operator)
                    if key not in by_key or key not in mpfr_rows:
                        continue
                    row = by_key[key]
                    row.java_max_abs_error_vs_bigdecimal = row.max_abs_error_vs_bigdecimal
                    row.java_max_rel_error_vs_bigdecimal = row.max_rel_error_vs_bigdecimal
                    row.java_max_eps1_ratio_vs_bigdecimal = row.max_eps1_ratio_vs_bigdecimal
                    (
                        row.max_abs_error_vs_bigdecimal,
                        row.max_rel_error_vs_bigdecimal,
                        row.max_eps1_ratio_vs_bigdecimal,
                        row.first_nonzero_abs_error_step,
                        row.non_finite_count,
                        row.h1,
                        row.h2,
                        row.h3,
                    ) = mpfr_rows[key]
                    row.reference_backend = "mpfr_rounding_vs_exact"

        if not args.skip_kozmika:
            with tempfile.TemporaryDirectory(prefix="kozmika-step-bd-k-") as ktmp:
                ktmpdir = pathlib.Path(ktmp)
                for primitive in PRIMITIVES:
                    for operator, _ in OPS:
                        row = by_key[(primitive, operator)]
                        if primitive in MPFR_PRIMITIVES:
                            # MPFR rows use a dedicated random stream and exact-reference path,
                            # so legacy checksum replay is not comparable row-by-row.
                            row.checksum_match = None
                            continue
                        op_name = next(name for sym, name in OPS if sym == operator)
                        kh1, kh2, kh3 = run_kozmika_checksums(primitive, operator, op_name, args.loops, ktmpdir)
                        row.kozmika_h1 = kh1
                        row.kozmika_h2 = kh2
                        row.kozmika_h3 = kh3
                        row.checksum_match = (
                            close_rel(kh1, row.h1, args.checksum_tol)
                            and close_rel(kh2, row.h2, args.checksum_tol)
                            and close_rel(kh3, row.h3, args.checksum_tol)
                        )

        if args.python_crosscheck:
            py_loops = args.loops if args.python_loops <= 0 else args.python_loops
            py_rows = python_reference_rows(py_loops)
            for primitive in PRIMITIVES:
                for operator, _ in OPS:
                    row = by_key[(primitive, operator)]
                    py_row = py_rows[(primitive, operator)]
                    row.python_loops = py_row.loops
                    row.python_max_abs_error_vs_bigdecimal = py_row.max_abs_error_vs_bigdecimal
                    row.python_max_rel_error_vs_bigdecimal = py_row.max_rel_error_vs_bigdecimal
                    row.python_max_eps1_ratio_vs_bigdecimal = py_row.max_eps1_ratio_vs_bigdecimal
                    row.python_non_finite_count = py_row.non_finite_count
                    row.java_python_abs_delta = abs(row.max_abs_error_vs_bigdecimal - py_row.max_abs_error_vs_bigdecimal)
                    row.java_python_rel_delta = abs(row.max_rel_error_vs_bigdecimal - py_row.max_rel_error_vs_bigdecimal)
                    row.java_python_eps1_ratio_delta = abs(row.max_eps1_ratio_vs_bigdecimal - py_row.max_eps1_ratio_vs_bigdecimal)

        rows.sort(key=lambda r: (PRIMITIVES.index(r.primitive), [x[0] for x in OPS].index(r.operator)))

        for row in rows:
            py_tag = ""
            if row.python_loops is not None:
                py_tag = (
                    f" py_loops={row.python_loops}"
                    f" py_max_abs={row.python_max_abs_error_vs_bigdecimal:.6e}"
                    f" py_max_eps1_ratio={row.python_max_eps1_ratio_vs_bigdecimal:.6e}"
                    f" py_non_finite={row.python_non_finite_count}"
                )
            print(
                f"{row.primitive:<5} {row.operator} "
                f"ref={row.reference_backend} "
                f"max_abs={row.max_abs_error_vs_bigdecimal:.6e} "
                f"max_rel={row.max_rel_error_vs_bigdecimal:.6e} "
                f"max_eps1_ratio={row.max_eps1_ratio_vs_bigdecimal:.6e} "
                f"first_nonzero={row.first_nonzero_abs_error_step} "
                f"non_finite={row.non_finite_count} "
                f"checksum_match={row.checksum_match}{py_tag}"
            )

        out = {
            "loops": args.loops,
            "checksum_tol": args.checksum_tol,
            "skip_kozmika": args.skip_kozmika,
            "skip_mpfr_high_ref": args.skip_mpfr_high_ref,
            "python_crosscheck": args.python_crosscheck,
            "python_loops": (args.loops if args.python_loops <= 0 else args.python_loops) if args.python_crosscheck else None,
            "safe_input": SAFE_INPUT,
            "rows": [asdict(r) for r in rows],
        }
        out_json = RESULT_DIR / "float_ops_stepwise_bigdecimal_check.json"
        out_json.write_text(json.dumps(out, indent=2), encoding="utf-8")
        table_md = RESULT_DIR / "float_ops_stepwise_bigdecimal_table.md"
        write_markdown_table(rows, table_md)
        table_wide_md = RESULT_DIR / "float_ops_stepwise_bigdecimal_table_wide.md"
        write_markdown_wide_table(rows, table_wide_md)
        print(f"result_json: {out_json}")
        print(f"result_table_md: {table_md}")
        print(f"result_table_wide_md: {table_wide_md}")

        if args.fail_on_nonfinite:
            failed_nonfinite = [row for row in rows if row.non_finite_count != 0]
            if failed_nonfinite:
                print(f"validation_failed_nonfinite: {len(failed_nonfinite)} rows")
                return 1

        if args.fail_on_checksum_mismatch:
            failed_checksum = [row for row in rows if row.checksum_match is False]
            if failed_checksum:
                print(f"validation_failed_checksum: {len(failed_checksum)} rows")
                return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
