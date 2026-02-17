#!/usr/bin/env python3
import argparse
import csv
import json
import subprocess
from pathlib import Path


def run_once(command):
    proc = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False)
    return proc.returncode, proc.stdout


def normalize_output(text):
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


def collect_programs(root, max_cases):
    phase4 = sorted((root / "bench/programs/phase4").glob("*.k"))
    phase8 = sorted((root / "bench/programs/phase8").glob("*.k"))
    phase9 = sorted((root / "bench/programs/phase9").glob("*.k"))
    all_programs = phase4 + phase8 + phase9
    if max_cases > 0:
        all_programs = all_programs[:max_cases]
    return all_programs


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--max-cases", type=int, default=24)
    parser.add_argument("--json-out", default="bench/results/phase10_differential.json")
    parser.add_argument("--csv-out", default="bench/results/phase10_differential.csv")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    k_bin = root / "k"

    programs = collect_programs(root, args.max_cases)
    if not programs:
        raise SystemExit("no differential programs found")

    records = []
    for program in programs:
        interpret_cmd = [str(k_bin), "run", "--interpret", str(program)]
        native_cmd = [str(k_bin), "run", str(program)]

        interp_status, interp_output = run_once(interpret_cmd)
        native_status, native_output = run_once(native_cmd)
        interp_norm = normalize_output(interp_output)
        native_norm = normalize_output(native_output)

        status_ok = (interp_status == 0 and native_status == 0)
        equal = (interp_norm == native_norm)
        records.append(
            {
                "program": str(program.relative_to(root)),
                "interpret_status": interp_status,
                "native_status": native_status,
                "status_ok": status_ok,
                "equal_output": equal,
                "pass": bool(status_ok and equal),
                "interpret_output": interp_norm,
                "native_output": native_norm,
            }
        )

    payload = {
        "cases": records,
        "total": len(records),
        "passed": sum(int(item["pass"]) for item in records),
    }

    json_path = root / args.json_out
    csv_path = root / args.csv_out
    json_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    with csv_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["program", "pass", "status_ok", "equal_output", "interpret_status", "native_status"])
        for row in records:
            writer.writerow(
                [
                    row["program"],
                    "PASS" if row["pass"] else "FAIL",
                    int(row["status_ok"]),
                    int(row["equal_output"]),
                    row["interpret_status"],
                    row["native_status"],
                ]
            )

    print(f"phase10 differential: {payload['passed']}/{payload['total']} passed")
    print(f"results json: {json_path}")
    print(f"results csv: {csv_path}")


if __name__ == "__main__":
    main()
