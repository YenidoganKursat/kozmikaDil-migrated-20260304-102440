#!/usr/bin/env python3
import argparse
import csv
import json
import random
import subprocess
import tempfile
from pathlib import Path


TOKENS = [
    "def", "fn", "class", "if", "else", "while", "for", "in", "return", "async", "await",
    "task_group", "spawn", "join", "parallel_for", "parallel_reduce",
    "x", "y", "z", "i", "j", "k", "m", "n", "0", "1", "2", "3", "4", "5",
    "+", "-", "*", "/", "=", "==", "!=", "<", ">", ":", ",", "(", ")", "[", "]", "{", "}",
]


def random_program(rng, lines):
    out = []
    for _ in range(lines):
        length = rng.randint(3, 14)
        row = [rng.choice(TOKENS) for _ in range(length)]
        out.append(" ".join(row))
    return "\n".join(out) + "\n"


def run_parse(k_bin, path):
    proc = subprocess.run([str(k_bin), "parse", str(path)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False)
    return proc.returncode, proc.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cases", type=int, default=200)
    parser.add_argument("--seed", type=int, default=1337)
    parser.add_argument("--json-out", default="bench/results/phase10_parser_fuzz.json")
    parser.add_argument("--csv-out", default="bench/results/phase10_parser_fuzz.csv")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    k_bin = root / "k"
    rng = random.Random(args.seed)

    records = []
    with tempfile.TemporaryDirectory(prefix="spark_phase10_parser_fuzz_") as td:
        temp_dir = Path(td)
        for idx in range(max(1, args.cases)):
            source = random_program(rng, rng.randint(1, 24))
            path = temp_dir / f"fuzz_{idx}.k"
            path.write_text(source, encoding="utf-8")
            code, output = run_parse(k_bin, path)
            # parse fail (1) is allowed, crash/signal/non-standard failure is not.
            non_crash = (code == 0 or code == 1)
            records.append(
                {
                    "case": idx,
                    "returncode": code,
                    "non_crash": non_crash,
                    "pass": non_crash,
                    "sample_output": output[:240],
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
        writer.writerow(["case", "returncode", "non_crash", "pass"])
        for row in records:
            writer.writerow([row["case"], row["returncode"], int(row["non_crash"]), int(row["pass"])])

    print(f"phase10 parser fuzz: {payload['passed']}/{payload['cases']} passed")
    print(f"results json: {json_path}")
    print(f"results csv: {csv_path}")


if __name__ == "__main__":
    main()
