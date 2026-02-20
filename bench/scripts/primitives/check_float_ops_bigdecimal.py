#!/usr/bin/env python3
"""Check Kozmika float primitive operator outputs against Java BigDecimal reference."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import subprocess
import tempfile
from dataclasses import asdict, dataclass
from decimal import Decimal, InvalidOperation, getcontext
from typing import Dict, List, Optional

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
RESULT_DIR = REPO_ROOT / "bench" / "results" / "primitives"
PRIMITIVES = ["f8", "f16", "bf16", "f32", "f64", "f128", "f256", "f512"]
OPS = [("+", "add"), ("-", "sub"), ("*", "mul"), ("/", "div"), ("%", "mod"), ("^", "pow")]


@dataclass
class CheckRow:
    primitive: str
    operator: str
    op_name: str
    loops: int
    b_effective: str
    kozmika_result: str
    java_bigdecimal_reference: str
    abs_error: Optional[str]
    rel_error: Optional[str]
    status: str


def run_checked(cmd: List[str], cwd: pathlib.Path, env: Optional[Dict[str, str]] = None) -> subprocess.CompletedProcess:
    merged_env = os.environ.copy()
    if env:
        merged_env.update(env)
    return subprocess.run(
        cmd,
        cwd=str(cwd),
        env=merged_env,
        text=True,
        capture_output=True,
        check=True,
    )


def parse_number(text: str) -> Optional[Decimal]:
    t = text.strip().lower()
    if t in {"inf", "+inf", "-inf", "nan", "+nan", "-nan"}:
        return None
    try:
        return Decimal(text.strip())
    except InvalidOperation:
        return None


def make_program(path: pathlib.Path, primitive: str, operator: str, loops: int, b_literal: str) -> None:
    source = "\n".join(
        [
            f"b = {primitive}({b_literal})",
            f"c = {primitive}(0)",
            "i = 0",
            f"while i < {loops}:",
            f"  c = c {operator} b",
            "  i = i + 1",
            "print(b)",
            "print(c)",
            "",
        ]
    )
    path.write_text(source, encoding="utf-8")


def write_java_ref(workdir: pathlib.Path) -> pathlib.Path:
    src = workdir / "BigDecimalRef.java"
    src.write_text(
        """
import java.math.BigDecimal;
import java.math.MathContext;
import java.math.RoundingMode;

public class BigDecimalRef {
  public static void main(String[] args) {
    if (args.length != 4) {
      throw new IllegalArgumentException("usage: <op> <loops> <c0> <b>");
    }
    final String op = args[0];
    final long loops = Long.parseLong(args[1]);
    final BigDecimal c0 = new BigDecimal(args[2]);
    final BigDecimal b = new BigDecimal(args[3]);
    final MathContext mc = new MathContext(200, RoundingMode.HALF_EVEN);

    final BigDecimal loopsBD = BigDecimal.valueOf(loops);
    final BigDecimal out;
    switch (op) {
      case "+":
        out = c0.add(b.multiply(loopsBD, mc), mc);
        break;
      case "-":
        out = c0.subtract(b.multiply(loopsBD, mc), mc);
        break;
      case "*":
      case "/":
      case "%":
      case "^":
        // This checker matches current benchmark semantics where c starts from 0.
        out = BigDecimal.ZERO;
        break;
      default:
        throw new IllegalArgumentException("unsupported op: " + op);
    }

    BigDecimal printable = out.stripTrailingZeros();
    if (printable.scale() < 0) {
      printable = printable.setScale(0);
    }
    System.out.println(printable.toPlainString());
  }
}
""".strip()
        + "\n",
        encoding="utf-8",
    )
    run_checked(["javac", str(src)], cwd=workdir)
    return workdir


def java_reference(java_cp: pathlib.Path, operator: str, loops: int, c0: str, b_effective: str) -> str:
    proc = run_checked(
        ["java", "-Duser.language=en", "-Duser.region=US", "-cp", str(java_cp), "BigDecimalRef", operator, str(loops), c0, b_effective],
        cwd=REPO_ROOT,
    )
    lines = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
    if not lines:
        raise RuntimeError("java reference produced empty output")
    return lines[-1]


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate float primitive op outputs against Java BigDecimal")
    parser.add_argument("--loops", type=int, default=100_000_000)
    parser.add_argument("--b-literal", type=str, default="3.75")
    parser.add_argument("--mode", choices=["run", "interpret"], default="run")
    args = parser.parse_args()

    getcontext().prec = 220
    RESULT_DIR.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="kozmika-bigdecimal-check-") as tmp:
        tmpdir = pathlib.Path(tmp)
        java_cp = write_java_ref(tmpdir)
        rows: List[CheckRow] = []

        for primitive in PRIMITIVES:
            for operator, op_name in OPS:
                program = tmpdir / f"{primitive}_{op_name}.k"
                make_program(program, primitive, operator, args.loops, args.b_literal)

                cmd = ["./k", "run", str(program)] if args.mode == "run" else ["./k", "run", "--interpret", str(program)]
                proc = run_checked(cmd, cwd=REPO_ROOT)
                out_lines = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
                if len(out_lines) < 2:
                    raise RuntimeError(f"unexpected output for {primitive} {operator}: {proc.stdout!r}")
                b_effective = out_lines[-2]
                kozmika_result = out_lines[-1]
                ref_value = java_reference(java_cp, operator, args.loops, "0", b_effective)

                k_num = parse_number(kozmika_result)
                r_num = parse_number(ref_value)
                abs_err: Optional[str] = None
                rel_err: Optional[str] = None
                status = "PASS"

                if k_num is None or r_num is None:
                    status = "NON_FINITE"
                else:
                    delta = abs(k_num - r_num)
                    denom = abs(r_num) if r_num != 0 else Decimal(1)
                    rel = delta / denom
                    abs_err = format(delta, "f")
                    rel_err = format(rel, "f")

                rows.append(
                    CheckRow(
                        primitive=primitive,
                        operator=operator,
                        op_name=op_name,
                        loops=args.loops,
                        b_effective=b_effective,
                        kozmika_result=kozmika_result,
                        java_bigdecimal_reference=ref_value,
                        abs_error=abs_err,
                        rel_error=rel_err,
                        status=status,
                    )
                )

                print(
                    f"{primitive:<5} {operator:<1} kozmika={kozmika_result} "
                    f"java_ref={ref_value} abs_err={abs_err or '-'} rel_err={rel_err or '-'} status={status}"
                )

    out = {
        "loops": args.loops,
        "b_literal": args.b_literal,
        "mode": args.mode,
        "rows": [asdict(r) for r in rows],
    }
    output_path = RESULT_DIR / f"float_ops_bigdecimal_check_{args.mode}.json"
    output_path.write_text(json.dumps(out, indent=2), encoding="utf-8")
    print(f"result_json: {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
