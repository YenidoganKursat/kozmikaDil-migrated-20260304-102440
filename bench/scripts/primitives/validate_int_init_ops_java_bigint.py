#!/usr/bin/env python3
"""Validate Kozmika integer init/+/-/*/%/^ against Java BigDecimal reference."""

from __future__ import annotations

import argparse
import json
import pathlib
import random
import shutil
import subprocess
import tempfile
from dataclasses import asdict, dataclass

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
RESULT_DIR = REPO_ROOT / "bench" / "results" / "primitives"

PRIMITIVES = ["i8", "i16", "i32", "i64", "i128", "i256", "i512"]
BITS = {
    "i8": 8,
    "i16": 16,
    "i32": 32,
    "i64": 64,
    "i128": 128,
    "i256": 256,
    "i512": 512,
}
OPS = ["+", "-", "*", "%", "^"]


@dataclass
class GroupSummary:
    primitive: str
    mode: str
    operator: str
    cases: int
    mismatch_count: int
    pass_check: bool


def run_checked(cmd: list[str], cwd: pathlib.Path | None = None, stdin: str | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        cwd=str(cwd or REPO_ROOT),
        input=stdin,
        capture_output=True,
        text=True,
        check=True,
    )


def parse_lines(text: str) -> list[str]:
    return [line.strip() for line in text.splitlines() if line.strip()]


def bounds(bits: int) -> tuple[int, int]:
    hi = (1 << (bits - 1)) - 1
    lo = -(1 << (bits - 1))
    return lo, hi


def derive_seed(seed_base: int, material: str) -> int:
    acc = (seed_base & 0xFFFFFFFF) ^ 0x9E37_79B9
    for idx, ch in enumerate(material):
        acc ^= (ord(ch) << ((idx % 4) * 8))
        acc = (acc * 1664525 + 1013904223) & 0xFFFFFFFF
    return acc


def generate_init_cases(bits: int, random_cases: int, seed_base: int) -> list[int]:
    lo, hi = bounds(bits)
    near = [lo, lo + 1, -1, 0, 1, hi - 1, hi]
    outside = [lo - 1, lo - 7, hi + 1, hi + 7, lo * 2, hi * 2]
    rng = random.Random(derive_seed(seed_base, f"init:{bits}"))
    cases = near + outside
    for _ in range(max(0, random_cases)):
        width = bits + rng.randint(0, 12)
        mag = rng.getrandbits(width)
        if rng.randint(0, 1) == 1:
            mag = -mag
        cases.append(mag)
    return cases


def generate_op_cases(bits: int, op: str, random_cases: int, seed_base: int) -> list[tuple[int, int]]:
    lo, hi = bounds(bits)
    seed = [lo, lo + 1, -2, -1, 0, 1, 2, hi - 1, hi]
    cases: list[tuple[int, int]] = []
    if op == "^":
        base_values = [-16, -8, -4, -2, -1, 0, 1, 2, 4, 8, 16]
        exponents = [0, 1, 2, 3, 4, 5, 6, 7, 8]
        for x in base_values:
            for y in exponents:
                cases.append((x, y))
    elif op == "%":
        den = [1, 2, 3, 5, 7, 11, 17, 31]
        for x in seed:
            for y in den:
                cases.append((x, y))
                cases.append((x, -y))
    else:
        for x in seed:
            for y in seed:
                cases.append((x, y))

    rng = random.Random(derive_seed(seed_base, f"op:{bits}:{op}"))
    for _ in range(max(0, random_cases)):
        wx = bits + rng.randint(0, 12)
        wy = bits + rng.randint(0, 12)
        x = rng.getrandbits(wx)
        y = rng.getrandbits(wy)
        if rng.randint(0, 1) == 1:
            x = -x
        if rng.randint(0, 1) == 1:
            y = -y
        if op == "%":
            if y == 0:
                y = 1
        if op == "^":
            x = rng.randint(-16, 16)
            y = abs(y) % 9
        cases.append((x, y))
    return cases


def build_kozmika_program_for_init(path: pathlib.Path, primitive: str, values: list[int]) -> None:
    lines = [f"print({primitive}({value}))" for value in values]
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def build_kozmika_program_for_ops(
    path: pathlib.Path,
    primitive: str,
    op: str,
    values: list[tuple[int, int]],
) -> None:
    lines: list[str] = []
    for x, y in values:
        if op == "%" and y == 0:
            y = 1
        if op == "^" and y < 0:
            y = -y
        lines.append(f"print({primitive}({x}) {op} {primitive}({y}))")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def run_kozmika_program(path: pathlib.Path) -> list[str]:
    proc = run_checked(["./k", "run", str(path)])
    return parse_lines(proc.stdout)


def build_java_ref_binary(tmpdir: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path]:
    source = tmpdir / "RefBigInt.java"
    source.write_text(
        """import java.io.*;
import java.math.BigDecimal;
import java.math.BigInteger;

public final class RefBigInt {
  private static boolean isUnbounded(int bits) {
    return bits >= 512;
  }

  private static BigDecimal clamp(BigDecimal value, int bits) {
    if (isUnbounded(bits)) {
      return value;
    }
    BigDecimal hi = new BigDecimal(BigInteger.ONE.shiftLeft(bits - 1).subtract(BigInteger.ONE));
    BigDecimal lo = new BigDecimal(BigInteger.ONE.shiftLeft(bits - 1).negate());
    if (value.compareTo(hi) > 0) return hi;
    if (value.compareTo(lo) < 0) return lo;
    return value;
  }

  private static BigDecimal applyOp(int bits, String op, BigDecimal xRaw, BigDecimal yRaw) {
    BigDecimal x = clamp(xRaw, bits);
    BigDecimal y = clamp(yRaw, bits);
    BigDecimal out;
    switch (op) {
      case "+": out = x.add(y); break;
      case "-": out = x.subtract(y); break;
      case "*": out = x.multiply(y); break;
      case "%":
        if (y.signum() == 0) y = BigDecimal.ONE;
        out = x.remainder(y);
        break;
      case "^":
        int exp = y.toBigInteger().intValue();
        if (exp < 0) exp = -exp;
        return x.pow(exp);  // Kozmika int pow keeps full result (no post-clamp).
      default:
        throw new IllegalArgumentException("unsupported op: " + op);
    }
    return isUnbounded(bits) ? out : clamp(out, bits);
  }

  public static void main(String[] args) throws Exception {
    BufferedReader reader = new BufferedReader(new InputStreamReader(System.in));
    String line;
    StringBuilder out = new StringBuilder();
    while ((line = reader.readLine()) != null) {
      line = line.trim();
      if (line.isEmpty()) continue;
      String[] p = line.split("\\t");
      String mode = p[0];
      int bits = Integer.parseInt(p[1]);
      BigDecimal x = new BigDecimal(p[2]);
      if (mode.equals("INIT")) {
        out.append(clamp(x, bits).toPlainString()).append('\\n');
      } else {
        String op = p[3];
        BigDecimal y = new BigDecimal(p[4]);
        out.append(applyOp(bits, op, x, y).toPlainString()).append('\\n');
      }
    }
    System.out.print(out.toString());
  }
}
""",
        encoding="utf-8",
    )
    run_checked(["javac", str(source)], cwd=tmpdir)
    return source, tmpdir


def run_java_ref(class_dir: pathlib.Path, lines: list[str]) -> list[str]:
    stdin = "\n".join(lines) + "\n"
    proc = run_checked(["java", "-cp", str(class_dir), "RefBigInt"], cwd=class_dir, stdin=stdin)
    return parse_lines(proc.stdout)


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate integer init/ops against Java BigDecimal")
    parser.add_argument("--random-init-cases", type=int, default=48)
    parser.add_argument("--random-op-cases", type=int, default=64)
    parser.add_argument("--random-seed", type=int, default=None)
    parser.add_argument("--randomize-seed", action="store_true")
    parser.add_argument("--fail-on-mismatch", action="store_true")
    args = parser.parse_args()

    seed_base = args.random_seed
    if seed_base is None:
        seed_base = random.SystemRandom().randrange(1, 2**31) if args.randomize_seed else 0
    print(f"random_seed_base={seed_base}")

    if shutil.which("javac") is None or shutil.which("java") is None:
        print("java/javac not found")
        return 1

    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    summaries: list[GroupSummary] = []
    mismatches: list[dict[str, object]] = []

    with tempfile.TemporaryDirectory(prefix="int-bigint-ref-") as tmp:
        tmpdir = pathlib.Path(tmp)
        _, class_dir = build_java_ref_binary(tmpdir)

        for primitive in PRIMITIVES:
            bits = BITS[primitive]
            init_cases = generate_init_cases(bits, args.random_init_cases, seed_base)
            init_program = tmpdir / f"{primitive}_init.k"
            build_kozmika_program_for_init(init_program, primitive, init_cases)
            got_init = run_kozmika_program(init_program)
            ref_init = run_java_ref(class_dir, [f"INIT\t{bits}\t{v}" for v in init_cases])
            if len(got_init) != len(ref_init):
                raise RuntimeError(f"init output size mismatch for {primitive}")
            init_mismatch = 0
            for idx, (got, ref) in enumerate(zip(got_init, ref_init)):
                if got != ref:
                    init_mismatch += 1
                    if len(mismatches) < 128:
                        mismatches.append(
                            {
                                "primitive": primitive,
                                "mode": "init",
                                "operator": "init",
                                "index": idx,
                                "input": str(init_cases[idx]),
                                "kozmika": got,
                                "java_bigdecimal": ref,
                            }
                        )
            summaries.append(
                GroupSummary(
                    primitive=primitive,
                    mode="init",
                    operator="init",
                    cases=len(init_cases),
                    mismatch_count=init_mismatch,
                    pass_check=init_mismatch == 0,
                )
            )
            print(f"{primitive:<4} init cases={len(init_cases)} mismatches={init_mismatch}")

            for op in OPS:
                op_cases = generate_op_cases(bits, op, args.random_op_cases, seed_base)
                op_program = tmpdir / f"{primitive}_{op}.k"
                build_kozmika_program_for_ops(op_program, primitive, op, op_cases)
                got_op = run_kozmika_program(op_program)
                ref_lines = [f"OP\t{bits}\t{x}\t{op}\t{y}" for x, y in op_cases]
                ref_op = run_java_ref(class_dir, ref_lines)
                if len(got_op) != len(ref_op):
                    raise RuntimeError(f"op output size mismatch for {primitive} {op}")
                op_mismatch = 0
                for idx, (got, ref) in enumerate(zip(got_op, ref_op)):
                    if got != ref:
                        op_mismatch += 1
                        if len(mismatches) < 128:
                            x, y = op_cases[idx]
                            mismatches.append(
                                {
                                    "primitive": primitive,
                                    "mode": "op",
                                    "operator": op,
                                    "index": idx,
                                    "x": str(x),
                                    "y": str(y),
                                    "kozmika": got,
                                    "java_bigdecimal": ref,
                                }
                            )
                summaries.append(
                    GroupSummary(
                        primitive=primitive,
                        mode="op",
                        operator=op,
                        cases=len(op_cases),
                        mismatch_count=op_mismatch,
                        pass_check=op_mismatch == 0,
                    )
                )
                print(f"{primitive:<4} op={op} cases={len(op_cases)} mismatches={op_mismatch}")

    out = {
        "random_init_cases": args.random_init_cases,
        "random_op_cases": args.random_op_cases,
        "random_seed_base": seed_base,
        "randomize_seed": bool(args.randomize_seed),
        "summaries": [asdict(item) for item in summaries],
        "mismatches_preview": mismatches,
    }
    out_path = RESULT_DIR / "int_init_ops_java_bigint_validation.json"
    out_path.write_text(json.dumps(out, indent=2), encoding="utf-8")
    print(f"result_json: {out_path}")

    if args.fail_on_mismatch:
        failed = [item for item in summaries if not item.pass_check]
        if failed:
            print(f"validation_failed_groups={len(failed)}")
            return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
