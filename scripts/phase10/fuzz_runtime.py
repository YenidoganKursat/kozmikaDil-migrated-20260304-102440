#!/usr/bin/env python3
import argparse
import csv
import json
import random
import subprocess
import tempfile
from pathlib import Path


def build_program(rng):
    n = rng.randint(16, 256)
    use_hetero = rng.choice([True, False])
    add_const = rng.randint(-5, 7)
    mul_const = rng.choice([1, 2, 3, 4])
    body = [
        f"N = {n}",
        "values = []",
        "i = 0",
        "while i < N:",
    ]
    if use_hetero:
        body.extend(
            [
                "  if (i % 3) == 0:",
                "    values.append(i + 0.5)",
                "  else:",
                "    values.append(i)",
            ]
        )
    else:
        body.append("  values.append(i)")
    body.append("  i = i + 1")
    body.append(f"mapped = values.map_add({add_const})")
    body.append(f"mapped = mapped.map_mul({mul_const})")
    body.append("total = mapped.reduce_sum()")
    body.append("print(total)")
    body.append("print(mapped.plan_id())")
    return "\n".join(body) + "\n"


def run_command(command):
    proc = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False)
    return proc.returncode, proc.stdout


def normalize(text):
    lines = []
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        if stripped.startswith(("run:", "build:", "compile:", "warning:", "ir:", "cgen:", "cgen-warn:", "note:")):
            continue
        if stripped in {"True", "False"}:
            lines.append(stripped)
            continue
        try:
            float(stripped)
            lines.append(stripped)
        except ValueError:
            continue
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cases", type=int, default=120)
    parser.add_argument("--seed", type=int, default=4242)
    parser.add_argument("--json-out", default="bench/results/phase10_runtime_fuzz.json")
    parser.add_argument("--csv-out", default="bench/results/phase10_runtime_fuzz.csv")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    k_bin = root / "k"
    rng = random.Random(args.seed)

    records = []
    with tempfile.TemporaryDirectory(prefix="spark_phase10_runtime_fuzz_") as td:
        temp_dir = Path(td)
        for idx in range(max(1, args.cases)):
            source = build_program(rng)
            path = temp_dir / f"runtime_{idx}.k"
            path.write_text(source, encoding="utf-8")

            interp_status, interp_out = run_command([str(k_bin), "run", "--interpret", str(path)])
            native_status, native_out = run_command([str(k_bin), "run", "--allow-t5", str(path)])
            interp_norm = normalize(interp_out)
            native_norm = normalize(native_out)
            status_ok = (interp_status == 0 and native_status == 0)
            equal = (interp_norm == native_norm)
            records.append(
                {
                    "case": idx,
                    "status_ok": status_ok,
                    "equal_output": equal,
                    "pass": bool(status_ok and equal),
                    "interpret_status": interp_status,
                    "native_status": native_status,
                }
            )

    payload = {
        "seed": args.seed,
        "cases": len(records),
        "passed": sum(int(item["pass"]) for item in records),
        "records": records,
    }

    json_path = root / args.json_out
    csv_path = root / args.csv_out
    json_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    with csv_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["case", "pass", "status_ok", "equal_output", "interpret_status", "native_status"])
        for row in records:
            writer.writerow(
                [
                    row["case"],
                    int(row["pass"]),
                    int(row["status_ok"]),
                    int(row["equal_output"]),
                    row["interpret_status"],
                    row["native_status"],
                ]
            )

    print(f"phase10 runtime fuzz: {payload['passed']}/{payload['cases']} passed")
    print(f"results json: {json_path}")
    print(f"results csv: {csv_path}")


if __name__ == "__main__":
    main()
