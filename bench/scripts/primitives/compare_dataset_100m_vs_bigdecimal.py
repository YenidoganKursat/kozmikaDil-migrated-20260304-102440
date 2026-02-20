#!/usr/bin/env python3
"""Compare Kozmika primitive operator reductions against Java BigDecimal on the same dataset.

Dataset:
- Deterministic PRNG
- Binary-exact values in [-8.0, 8.0] using ((seed % 4097) - 2048) / 256.0
- 100M rows by default

Metric:
- For each primitive and operator, aggregate sum over all rows.
- Compare Kozmika aggregate sum vs Java BigDecimal aggregate sum (same dataset).
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import subprocess
import tempfile
from dataclasses import asdict, dataclass
from decimal import Decimal, InvalidOperation, getcontext
from typing import Dict, List

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
RESULT_DIR = REPO_ROOT / "bench" / "results" / "primitives"
PRIMITIVES = ["f8", "f16", "f32", "f64", "f128", "f256", "f512"]
OPS = [("+", "add"), ("-", "sub"), ("*", "mul"), ("/", "div"), ("%", "mod")]


@dataclass
class Row:
    primitive: str
    loops: int
    kozmika_add_sum: str
    kozmika_sub_sum: str
    kozmika_mul_sum: str
    kozmika_div_sum: str
    kozmika_mod_sum: str
    java_add_sum: str
    java_sub_sum: str
    java_mul_sum: str
    java_div_sum: str
    java_mod_sum: str
    add_abs_error: str
    sub_abs_error: str
    mul_abs_error: str
    div_abs_error: str
    mod_abs_error: str
    add_rel_error: str
    sub_rel_error: str
    mul_rel_error: str
    div_rel_error: str
    mod_rel_error: str


def run_checked(cmd: List[str], cwd: pathlib.Path) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=str(cwd), text=True, capture_output=True, check=True, env=os.environ.copy())


def parse_decimal(value: str) -> Decimal | None:
    text = value.strip().lower()
    if text in {"inf", "+inf", "-inf", "nan", "+nan", "-nan"}:
        return None
    try:
        return Decimal(value.strip())
    except (InvalidOperation, ValueError):
        return None


def fmt_decimal(value: Decimal | None) -> str:
    if value is None:
        return "NON_FINITE"
    out = format(value, "f")
    if "." in out:
        out = out.rstrip("0").rstrip(".")
    return out if out else "0"


def abs_and_rel_err(lhs: Decimal | None, rhs: Decimal | None) -> tuple[str, str]:
    if lhs is None or rhs is None:
        return "NON_FINITE", "NON_FINITE"
    delta = abs(lhs - rhs)
    denom = abs(rhs) if rhs != 0 else Decimal(1)
    rel = delta / denom
    return fmt_decimal(delta), fmt_decimal(rel)


def java_ref_source() -> str:
    return r"""
import java.math.BigDecimal;
import java.math.MathContext;
import java.math.RoundingMode;
import java.util.Locale;

public final class DatasetBigDecimalRef {
  private static long nextSeed(long seed) {
    return (1103515245L * seed + 12345L) & 0x7fffffffL;
  }

  public static void main(String[] args) {
    if (args.length != 1) {
      throw new IllegalArgumentException("usage: <loops>");
    }
    Locale.setDefault(Locale.US);
    final long loops = Long.parseLong(args[0]);
    final MathContext mc = new MathContext(120, RoundingMode.HALF_EVEN);
    BigDecimal sAdd = BigDecimal.ZERO;
    BigDecimal sSub = BigDecimal.ZERO;
    BigDecimal sMul = BigDecimal.ZERO;
    BigDecimal sDiv = BigDecimal.ZERO;
    BigDecimal sMod = BigDecimal.ZERO;
    long seed = 123456789L;

    for (long i = 0; i < loops; i++) {
      seed = nextSeed(seed);
      final double a = ((seed % 4097L) - 2048L) / 256.0;
      seed = nextSeed(seed);
      double b = ((seed % 4097L) - 2048L) / 256.0;
      if (b == 0.0) {
        b = 1.0 / 256.0;
      }

      final BigDecimal A = BigDecimal.valueOf(a);
      final BigDecimal B = BigDecimal.valueOf(b);
      sAdd = sAdd.add(A.add(B, mc), mc);
      sSub = sSub.add(A.subtract(B, mc), mc);
      sMul = sMul.add(A.multiply(B, mc), mc);
      sDiv = sDiv.add(A.divide(B, mc), mc);
      sMod = sMod.add(A.remainder(B, mc), mc);
    }

    System.out.println(sAdd.toPlainString());
    System.out.println(sSub.toPlainString());
    System.out.println(sMul.toPlainString());
    System.out.println(sDiv.toPlainString());
    System.out.println(sMod.toPlainString());
  }
}
""".strip() + "\n"


def get_java_reference(loops: int, tmpdir: pathlib.Path) -> Dict[str, Decimal]:
    java_file = tmpdir / "DatasetBigDecimalRef.java"
    java_file.write_text(java_ref_source(), encoding="utf-8")
    run_checked(["javac", str(java_file)], cwd=tmpdir)
    proc = run_checked(
        ["java", "-Duser.language=en", "-Duser.region=US", "-cp", str(tmpdir), "DatasetBigDecimalRef", str(loops)],
        cwd=tmpdir,
    )
    lines = [x.strip() for x in proc.stdout.splitlines() if x.strip()]
    if len(lines) != 5:
        raise RuntimeError(f"unexpected java reference output: {proc.stdout!r}")
    return {
        "add": Decimal(lines[0]),
        "sub": Decimal(lines[1]),
        "mul": Decimal(lines[2]),
        "div": Decimal(lines[3]),
        "mod": Decimal(lines[4]),
    }


def make_kozmika_program(path: pathlib.Path, primitive: str, loops: int) -> None:
    code = [
        "seed = i64(123456789)",
        "s_add = f64(0)",
        "s_sub = f64(0)",
        "s_mul = f64(0)",
        "s_div = f64(0)",
        "s_mod = f64(0)",
        "i = 0",
        f"while i < {loops}:",
        "  seed = (seed * 1103515245 + 12345) % 2147483648",
        "  a_raw = ((seed % 4097) - 2048) / 256.0",
        "  seed = (seed * 1103515245 + 12345) % 2147483648",
        "  b_raw = ((seed % 4097) - 2048) / 256.0",
        "  if b_raw == 0:",
        "    b_raw = 1.0 / 256.0",
        f"  a = {primitive}(a_raw)",
        f"  b = {primitive}(b_raw)",
        "  if b == 0:",
        f"    b = {primitive}(1.0 / 256.0)",
        "  s_add = s_add + (a + b)",
        "  s_sub = s_sub + (a - b)",
        "  s_mul = s_mul + (a * b)",
        "  s_div = s_div + (a / b)",
        "  s_mod = s_mod + (a % b)",
        "  i = i + 1",
        "print(s_add)",
        "print(s_sub)",
        "print(s_mul)",
        "print(s_div)",
        "print(s_mod)",
        "",
    ]
    path.write_text("\n".join(code), encoding="utf-8")


def run_kozmika_sums(primitive: str, loops: int, tmpdir: pathlib.Path) -> Dict[str, Decimal | None]:
    program = tmpdir / f"dataset_{primitive}.k"
    make_kozmika_program(program, primitive, loops)
    proc = run_checked(["./k", "run", str(program)], cwd=REPO_ROOT)
    out = [x.strip() for x in proc.stdout.splitlines() if x.strip()]
    if len(out) < 5:
        raise RuntimeError(f"unexpected kozmika output for {primitive}: {proc.stdout!r}")
    return {
        "add": parse_decimal(out[-5]),
        "sub": parse_decimal(out[-4]),
        "mul": parse_decimal(out[-3]),
        "div": parse_decimal(out[-2]),
        "mod": parse_decimal(out[-1]),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare Kozmika sums vs Java BigDecimal on the same 100M-style dataset")
    parser.add_argument("--loops", type=int, default=100_000_000)
    args = parser.parse_args()

    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    getcontext().prec = 200

    rows: List[Row] = []
    with tempfile.TemporaryDirectory(prefix="kozmika-dataset-compare-") as tmp:
        tmpdir = pathlib.Path(tmp)
        java_ref = get_java_reference(args.loops, tmpdir)

        for primitive in PRIMITIVES:
            ks = run_kozmika_sums(primitive, args.loops, tmpdir)
            add_abs, add_rel = abs_and_rel_err(ks["add"], java_ref["add"])
            sub_abs, sub_rel = abs_and_rel_err(ks["sub"], java_ref["sub"])
            mul_abs, mul_rel = abs_and_rel_err(ks["mul"], java_ref["mul"])
            div_abs, div_rel = abs_and_rel_err(ks["div"], java_ref["div"])
            mod_abs, mod_rel = abs_and_rel_err(ks["mod"], java_ref["mod"])

            row = Row(
                primitive=primitive,
                loops=args.loops,
                kozmika_add_sum=fmt_decimal(ks["add"]),
                kozmika_sub_sum=fmt_decimal(ks["sub"]),
                kozmika_mul_sum=fmt_decimal(ks["mul"]),
                kozmika_div_sum=fmt_decimal(ks["div"]),
                kozmika_mod_sum=fmt_decimal(ks["mod"]),
                java_add_sum=fmt_decimal(java_ref["add"]),
                java_sub_sum=fmt_decimal(java_ref["sub"]),
                java_mul_sum=fmt_decimal(java_ref["mul"]),
                java_div_sum=fmt_decimal(java_ref["div"]),
                java_mod_sum=fmt_decimal(java_ref["mod"]),
                add_abs_error=add_abs,
                sub_abs_error=sub_abs,
                mul_abs_error=mul_abs,
                div_abs_error=div_abs,
                mod_abs_error=mod_abs,
                add_rel_error=add_rel,
                sub_rel_error=sub_rel,
                mul_rel_error=mul_rel,
                div_rel_error=div_rel,
                mod_rel_error=mod_rel,
            )
            rows.append(row)
            print(
                f"{primitive:<5} "
                f"add_abs={add_abs} sub_abs={sub_abs} mul_abs={mul_abs} div_abs={div_abs} mod_abs={mod_abs}"
            )

    out = {
        "loops": args.loops,
        "dataset": "a,b in [-8,8], binary-exact; zero-denominator forced to 1/256",
        "rows": [asdict(r) for r in rows],
    }
    out_path = RESULT_DIR / "dataset_100m_vs_bigdecimal.json"
    out_path.write_text(json.dumps(out, indent=2), encoding="utf-8")
    print(f"result_json: {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
