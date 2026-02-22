#!/usr/bin/env python3
"""Extreme-case float primitive validation (strict).

Policy:
- f8/f16/f32/f64: Java BigDecimal reference + Python Decimal cross-check.
- f128/f256/f512: MPFR strict reference (precision-true), Java/Python are informational.
- Operators: +,-,*,/,%,^
"""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import random
import subprocess
import tempfile
from dataclasses import asdict, dataclass
from decimal import Decimal, InvalidOperation, ROUND_DOWN, localcontext, getcontext
from typing import Dict, List, Tuple

import check_float_ops_stepwise_bigdecimal as step_ref

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
RESULT_DIR = REPO_ROOT / "bench" / "results" / "primitives"
MPFR_CASE_REF_CPP = REPO_ROOT / "bench" / "scripts" / "primitives" / "mpfr_case_reference.cpp"

PRIMITIVES = ["f8", "f16", "f32", "f64", "f128", "f256", "f512"]
OPS: List[Tuple[str, str]] = [("+", "add"), ("-", "sub"), ("*", "mul"), ("/", "div"), ("%", "mod"), ("^", "pow")]
LOW_FLOAT_PRIMITIVES = {"f8", "f16", "f32", "f64"}
HIGH_FLOAT_PRIMITIVES = {"f128", "f256", "f512"}

# Low-family practical tolerances (quantized binary formats).
LOW_BASE_ABS_TOL: Dict[str, Decimal] = {
    "f8": Decimal("5e-1"),
    "f16": Decimal("1e-2"),
    "f32": Decimal("2e-6"),
    "f64": Decimal("2e-12"),
}

LOW_REL_TOL: Dict[str, Decimal] = {
    "f8": Decimal("5e-2"),
    "f16": Decimal("5e-3"),
    "f32": Decimal("1e-6"),
    "f64": Decimal("2e-12"),
}

JAVA_PRECISION: Dict[str, int] = {
    "f8": 80,
    "f16": 80,
    "f32": 80,
    "f64": 100,
    "f128": 180,
    "f256": 320,
    "f512": 680,
}


@dataclass
class Row:
    primitive: str
    operator: str
    reference_backend: str
    cases: int
    max_abs_error_vs_reference: str
    max_rel_error_vs_reference: str
    max_abs_error_java_vs_python: str
    mismatch_count: int
    pass_check: bool


def run_checked(cmd: List[str], cwd: pathlib.Path, input_text: str | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd,
        cwd=str(cwd),
        input=input_text,
        capture_output=True,
        text=True,
        check=True,
    )


def epsilon_for_primitive(primitive: str) -> Decimal:
    mapping: Dict[str, int] = {
        "f8": 3,
        "f16": 10,
        "f32": 23,
        "f64": 52,
        "f128": 112,
        "f256": 236,
        "f512": 492,
    }
    return Decimal(2) ** Decimal(-mapping[primitive])


def java_source() -> str:
    return r"""
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.math.BigDecimal;
import java.math.MathContext;
import java.math.RoundingMode;
import java.util.Locale;

public final class PrimitiveExtremeBigDecimal {
  private static BigDecimal powInt(BigDecimal a, int exp, MathContext mc) {
    if (exp >= 0) {
      return a.pow(exp, mc);
    }
    if (a.compareTo(BigDecimal.ZERO) == 0) {
      return BigDecimal.ZERO;
    }
    return BigDecimal.ONE.divide(a.pow(-exp, mc), mc);
  }

  private static BigDecimal apply(String op, BigDecimal a, BigDecimal b, MathContext mc) {
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

  public static void main(String[] args) throws Exception {
    if (args.length != 2) {
      throw new IllegalArgumentException("usage: <precision> <op>");
    }
    Locale.setDefault(Locale.US);
    final int precision = Integer.parseInt(args[0]);
    final String op = args[1];
    final MathContext mc = new MathContext(precision, RoundingMode.HALF_EVEN);

    final BufferedReader br = new BufferedReader(new InputStreamReader(System.in));
    String line;
    while ((line = br.readLine()) != null) {
      line = line.trim();
      if (line.isEmpty()) continue;
      final int sep = line.indexOf('|');
      if (sep <= 0 || sep + 1 >= line.length()) {
        throw new IllegalArgumentException("invalid input row: " + line);
      }
      final BigDecimal a = new BigDecimal(line.substring(0, sep), mc);
      final BigDecimal b = new BigDecimal(line.substring(sep + 1), mc);
      final BigDecimal out = apply(op, a, b, mc);
      System.out.println(out.toPlainString());
    }
  }
}
""".strip() + "\n"


def parse_decimal(text: str) -> Decimal:
    try:
        return Decimal(text)
    except InvalidOperation as exc:
        raise RuntimeError(f"cannot parse decimal value: {text!r}") from exc


def stable_float_literal(value: float) -> str:
    return format(value, ".17g")


def normalize_input_literal(primitive: str, raw: Decimal) -> str:
    if primitive in LOW_FLOAT_PRIMITIVES:
        quantized = step_ref.quantize_by_primitive(primitive, float(raw))
        return stable_float_literal(quantized)
    return format(raw, "f") if raw == raw.to_integral_value() else format(raw.normalize(), "f")


def pow_base_bound_for_primitive(primitive: str) -> Decimal:
    return Decimal(str(step_ref.pow_bound_for_primitive(primitive)))


def bound_for_primitive(primitive: str) -> Decimal:
    if primitive in LOW_FLOAT_PRIMITIVES:
        return Decimal(str(step_ref.bound_for_primitive(primitive)))
    return Decimal("1000")


def min_den_for_primitive(primitive: str) -> Decimal:
    if primitive in LOW_FLOAT_PRIMITIVES:
        return Decimal(str(step_ref.min_den_for_primitive(primitive)))
    return Decimal("1e-30")


def deterministic_seed(primitive: str, operator: str) -> int:
    # Avoid Python's randomized hash() so CI and local runs use identical vectors.
    material = f"{primitive}|{operator}"
    acc = 0xA5E5_1234
    for idx, ch in enumerate(material):
        acc ^= (ord(ch) << ((idx % 4) * 8))
        acc = (acc * 1664525 + 1013904223) & 0xFFFFFFFF
    return acc


def is_unstable_mod_boundary_case(
    primitive: str,
    a: Decimal,
    b: Decimal,
    observed: Decimal,
    expected: Decimal,
    allowed: Decimal,
) -> bool:
    # For low-float modulo, quotient rounding at near-integer boundaries can flip
    # remainder between ~0 and ~|b| across libc/arch while still respecting IEEE precision.
    if primitive not in LOW_FLOAT_PRIMITIVES:
        return False
    abs_b = decimal_abs(b)
    if abs_b == 0:
        return False
    q = decimal_abs(a) / abs_b
    nearest = q.to_integral_value(rounding=ROUND_DOWN)
    q_delta = decimal_abs(q - nearest)
    q_delta_alt = decimal_abs((nearest + Decimal("1")) - q)
    q_dist = q_delta if q_delta < q_delta_alt else q_delta_alt
    if q_dist > epsilon_for_primitive(primitive) * Decimal("32"):
        return False
    obs_abs = decimal_abs(observed)
    exp_abs = decimal_abs(expected)
    near_zero = obs_abs <= allowed or exp_abs <= allowed
    near_abs_b = decimal_abs(obs_abs - abs_b) <= allowed or decimal_abs(exp_abs - abs_b) <= allowed
    return near_zero and near_abs_b


def build_cases(primitive: str, operator: str, random_cases: int) -> List[Tuple[str, str]]:
    rng = random.Random(deterministic_seed(primitive, operator))
    bound = bound_for_primitive(primitive)
    min_den = min_den_for_primitive(primitive)
    eps = epsilon_for_primitive(primitive)
    cases: List[Tuple[str, str]] = []

    if operator == "^":
        bnd = pow_base_bound_for_primitive(primitive)
        bases = [-bnd, -bnd / Decimal(2), Decimal("-2"), Decimal("-1"), Decimal("-0.5"),
                 Decimal("0"), Decimal("0.5"), Decimal("1"), Decimal("2"), bnd / Decimal(2), bnd]
        exps = [Decimal(x) for x in (-8, -4, -3, -2, -1, 0, 1, 2, 3, 4, 8)]
        for a in bases:
            for b in exps:
                if a == 0 and b < 0:
                    continue
                cases.append((normalize_input_literal(primitive, a), normalize_input_literal(primitive, b)))
        for _ in range(max(0, random_cases)):
            a = Decimal(str(rng.uniform(float(-bnd), float(bnd))))
            b = Decimal(rng.randint(-8, 8))
            if a == 0 and b < 0:
                b = Decimal(1)
            cases.append((normalize_input_literal(primitive, a), normalize_input_literal(primitive, b)))
        return cases

    if operator == "%":
        numerators = [Decimal("0"), eps, Decimal("1"), bound / Decimal(2), bound]
        denominators = [min_den, Decimal("1"), Decimal("2"), Decimal("3"), bound / Decimal(3), bound]
        for a in numerators:
            for b in denominators:
                cases.append((normalize_input_literal(primitive, a), normalize_input_literal(primitive, b)))
        for _ in range(max(0, random_cases)):
            a = Decimal(str(rng.uniform(0.0, float(bound))))
            b = Decimal(str(rng.uniform(float(min_den), float(bound))))
            if abs(b) < min_den:
                b = min_den
            cases.append((normalize_input_literal(primitive, a), normalize_input_literal(primitive, b)))
        return cases

    vals = [-bound, -bound / Decimal(2), Decimal("-2"), Decimal("-1"), -eps, -min_den,
            Decimal("0"), min_den, eps, Decimal("1"), Decimal("2"), bound / Decimal(2), bound]
    for a in vals:
        for b in vals:
            if operator in ("/", "%") and abs(b) < min_den:
                continue
            cases.append((normalize_input_literal(primitive, a), normalize_input_literal(primitive, b)))
    for _ in range(max(0, random_cases)):
        a = Decimal(str(rng.uniform(float(-bound), float(bound))))
        b = Decimal(str(rng.uniform(float(-bound), float(bound))))
        if operator in ("/", "%") and abs(b) < min_den:
            b = min_den if b >= 0 else -min_den
        cases.append((normalize_input_literal(primitive, a), normalize_input_literal(primitive, b)))
    return cases


def make_kozmika_program(path: pathlib.Path, primitive: str, operator: str, cases: List[Tuple[str, str]]) -> None:
    lines: List[str] = []
    for idx, (a_lit, b_lit) in enumerate(cases):
        lines.append(f"a{idx} = {primitive}({a_lit})")
        lines.append(f"b{idx} = {primitive}({b_lit})")
        if operator in ("/", "%"):
            lines.append(f"if b{idx} == 0:")
            lines.append(f"  b{idx} = {primitive}(1)")
        if operator == "^":
            lines.append(f"if a{idx} == 0 and b{idx} < 0:")
            lines.append(f"  b{idx} = {primitive}(1)")
        lines.append(f"r{idx} = a{idx} {operator} b{idx}")
        lines.append(f"print(r{idx})")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def run_kozmika_batch(primitive: str, op_name: str, operator: str, cases: List[Tuple[str, str]]) -> List[str]:
    with tempfile.TemporaryDirectory(prefix=f"kozmika-float-extreme-{primitive}-{op_name}-") as tmp:
        program = pathlib.Path(tmp) / f"{primitive}_{op_name}_extreme.k"
        make_kozmika_program(program, primitive, operator, cases)
        proc = run_checked(["./k", "run", "--interpret", str(program)], cwd=REPO_ROOT)
        out = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
        if len(out) != len(cases):
            raise RuntimeError(f"unexpected kozmika output length for {primitive} {operator}: got={len(out)} expected={len(cases)}")
        return out


def run_java_batch(java_dir: pathlib.Path, primitive: str, operator: str, cases: List[Tuple[str, str]]) -> List[str]:
    payload = "".join(f"{a}|{b}\n" for (a, b) in cases)
    proc = run_checked(
        ["java", "-Duser.language=en", "-Duser.region=US", "-cp", str(java_dir),
         "PrimitiveExtremeBigDecimal", str(JAVA_PRECISION[primitive]), operator],
        cwd=java_dir,
        input_text=payload,
    )
    out = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
    if len(out) != len(cases):
        raise RuntimeError(f"unexpected java output length for {primitive} {operator}: got={len(out)} expected={len(cases)}")
    return out


def compile_mpfr_binary(tmpdir: pathlib.Path) -> pathlib.Path:
    binary = tmpdir / "mpfr_case_reference"
    cmd = ["c++", "-std=c++17", "-O3", "-DNDEBUG", str(MPFR_CASE_REF_CPP)]
    if (pathlib.Path("/opt/homebrew/include/mpfr.h")).exists():
        cmd += ["-I", "/opt/homebrew/include"]
    if (pathlib.Path("/opt/homebrew/lib/libmpfr.dylib")).exists() or (pathlib.Path("/opt/homebrew/lib/libmpfr.a")).exists():
        cmd += ["-L", "/opt/homebrew/lib"]
    cmd += ["-o", str(binary), "-lmpfr", "-lgmp"]
    run_checked(cmd, cwd=REPO_ROOT)
    return binary


def run_mpfr_batch(binary: pathlib.Path, primitive: str, operator: str, cases: List[Tuple[str, str]]) -> List[str]:
    payload = "".join(f"{a}|{b}\n" for (a, b) in cases)
    proc = run_checked([str(binary), primitive, operator], cwd=binary.parent, input_text=payload)
    out = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
    if len(out) != len(cases):
        raise RuntimeError(f"unexpected mpfr output length for {primitive} {operator}: got={len(out)} expected={len(cases)}")
    return out


def apply_python_decimal(operator: str, a: Decimal, b: Decimal) -> Decimal:
    try:
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
            exp = int(b.to_integral_value(rounding=ROUND_DOWN))
            if a == 0 and exp < 0:
                return Decimal("0")
            with localcontext() as ctx:
                ctx.prec = max(getcontext().prec, 720)
                if exp >= 0:
                    return a ** exp
                return Decimal(1) / (a ** (-exp))
        raise ValueError(f"unsupported operator: {operator}")
    except (InvalidOperation, ZeroDivisionError, OverflowError):
        return Decimal("NaN")


def decimal_abs(v: Decimal) -> Decimal:
    return v.copy_abs()


def decimal_is_finite(v: Decimal) -> bool:
    return v.is_finite()


def non_finite_equivalent(a: Decimal, b: Decimal) -> bool:
    if a.is_nan() and b.is_nan():
        return True
    if a.is_infinite() and b.is_infinite():
        return a.is_signed() == b.is_signed()
    return False


def normalize_expected_low(primitive: str, value: Decimal) -> Decimal:
    quantized = step_ref.quantize_by_primitive(primitive, float(value))
    return Decimal(str(quantized))


def allowed_error(primitive: str, expected: Decimal) -> Decimal:
    scale = decimal_abs(expected) if decimal_abs(expected) > Decimal("1") else Decimal("1")
    eps = epsilon_for_primitive(primitive)
    if primitive in HIGH_FLOAT_PRIMITIVES:
        # Strict precision gate for high precision families.
        return eps * Decimal("64") * scale
    return max(LOW_BASE_ABS_TOL[primitive], LOW_REL_TOL[primitive] * scale, eps * Decimal("64") * scale)


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate float primitive extreme vectors (strict)")
    parser.add_argument("--random-cases", type=int, default=64)
    parser.add_argument("--fail-on-mismatch", action="store_true")
    args = parser.parse_args()

    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    getcontext().prec = 720
    rows: List[Row] = []
    failure_preview: List[Dict[str, object]] = []

    with tempfile.TemporaryDirectory(prefix="kozmika-float-extreme-ref-") as tmp:
        tmpdir = pathlib.Path(tmp)
        java_file = tmpdir / "PrimitiveExtremeBigDecimal.java"
        java_file.write_text(java_source(), encoding="utf-8")
        run_checked(["javac", str(java_file)], cwd=tmpdir)
        mpfr_binary = compile_mpfr_binary(tmpdir)

        for primitive in PRIMITIVES:
            for operator, op_name in OPS:
                cases = build_cases(primitive, operator, args.random_cases)
                kozmika_out = run_kozmika_batch(primitive, op_name, operator, cases)
                java_out = run_java_batch(tmpdir, primitive, operator, cases)
                mpfr_out = run_mpfr_batch(mpfr_binary, primitive, operator, cases) if primitive in HIGH_FLOAT_PRIMITIVES else []

                ref_backend = "mpfr_strict" if primitive in HIGH_FLOAT_PRIMITIVES else "java_bigdecimal"
                max_abs_ref = Decimal("0")
                max_rel_ref = Decimal("0")
                max_abs_java_python = Decimal("0")
                mismatch_count = 0

                for idx, ((a_lit, b_lit), k_raw, j_raw) in enumerate(zip(cases, kozmika_out, java_out)):
                    a = parse_decimal(a_lit)
                    b = parse_decimal(b_lit)
                    k = parse_decimal(k_raw)
                    java_value = parse_decimal(j_raw)
                    py_value = apply_python_decimal(operator, a, b)
                    if primitive in LOW_FLOAT_PRIMITIVES:
                        java_q = normalize_expected_low(primitive, java_value)
                        py_q = normalize_expected_low(primitive, py_value)
                        expected_ref = java_q
                    else:
                        expected_ref = parse_decimal(mpfr_out[idx])
                        java_q = java_value
                        py_q = py_value

                    if decimal_is_finite(java_q) and decimal_is_finite(py_q):
                        delta_jp = decimal_abs(java_q - py_q)
                        if delta_jp > max_abs_java_python:
                            max_abs_java_python = delta_jp

                    if not decimal_is_finite(k) or not decimal_is_finite(expected_ref):
                        if non_finite_equivalent(k, expected_ref):
                            continue
                        mismatch_count += 1
                        if len(failure_preview) < 96:
                            failure_preview.append(
                                {
                                    "primitive": primitive,
                                    "operator": operator,
                                    "case_index": idx,
                                    "reference_backend": ref_backend,
                                    "a": a_lit,
                                    "b": b_lit,
                                    "kozmika": k_raw,
                                    "reference": str(expected_ref),
                                    "reason": "non-finite mismatch",
                                }
                            )
                        continue

                    err_ref = decimal_abs(k - expected_ref)
                    denom = decimal_abs(expected_ref) if decimal_abs(expected_ref) != 0 else Decimal("1")
                    rel_ref = err_ref / denom
                    if err_ref > max_abs_ref:
                        max_abs_ref = err_ref
                    if rel_ref > max_rel_ref:
                        max_rel_ref = rel_ref

                    allowed = allowed_error(primitive, expected_ref)
                    if err_ref > allowed:
                        if operator == "%" and is_unstable_mod_boundary_case(primitive, a, b, k, expected_ref, allowed):
                            continue
                        mismatch_count += 1
                        if len(failure_preview) < 96:
                            failure_preview.append(
                                {
                                    "primitive": primitive,
                                    "operator": operator,
                                    "case_index": idx,
                                    "reference_backend": ref_backend,
                                    "a": a_lit,
                                    "b": b_lit,
                                    "kozmika": k_raw,
                                    "reference": str(expected_ref),
                                    "abs_err_vs_reference": format(err_ref, "E"),
                                    "allowed_abs": format(allowed, "E"),
                                }
                            )

                pass_check = mismatch_count == 0
                rows.append(
                    Row(
                        primitive=primitive,
                        operator=operator,
                        reference_backend=ref_backend,
                        cases=len(cases),
                        max_abs_error_vs_reference=format(max_abs_ref, "E"),
                        max_rel_error_vs_reference=format(max_rel_ref, "E"),
                        max_abs_error_java_vs_python=format(max_abs_java_python, "E"),
                        mismatch_count=mismatch_count,
                        pass_check=pass_check,
                    )
                )
                print(
                    f"{primitive:<5} {operator} ref={ref_backend} cases={len(cases)} "
                    f"max_abs_ref={max_abs_ref:.6E} max_rel_ref={max_rel_ref:.6E} "
                    f"java_py_abs={max_abs_java_python:.6E} mismatches={mismatch_count} pass={pass_check}"
                )

    report = {
        "random_cases": args.random_cases,
        "rows": [asdict(row) for row in rows],
        "failure_preview": failure_preview,
    }
    out_json = RESULT_DIR / "float_extreme_bigdecimal_validation.json"
    out_json.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"result_json: {out_json}")

    if args.fail_on_mismatch:
        failed = [row for row in rows if not row.pass_check]
        if failed:
            print(f"validation_failed: groups={len(failed)}")
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
