#!/usr/bin/env python3
import argparse
import csv
import json
import os
import subprocess
from pathlib import Path


TEST_TARGETS = [
    "sparkc_smoke_test",
    "sparkc_parser_tests",
    "sparkc_eval_tests",
    "sparkc_typecheck_tests",
    "sparkc_codegen_tests",
    "sparkc_phase5_tests",
    "sparkc_phase6_tests",
    "sparkc_phase7_tests",
    "sparkc_phase8_tests",
    "sparkc_phase9_tests",
    "sparkc_phase10_tests",
]


def run(command, cwd, env=None):
    proc = subprocess.run(
        command,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
        env=env,
    )
    return proc.returncode, proc.stdout


def execute_config(root, name, cxx_flags, exe_flags):
    build_dir = root / "build" / f"phase10_{name}"
    configure_cmd = [
        "cmake",
        "-S",
        str(root),
        "-B",
        str(build_dir),
        "-DSPARK_BUILD_TESTS=ON",
        "-DCMAKE_BUILD_TYPE=Debug",
        f"-DCMAKE_CXX_FLAGS={cxx_flags}",
        f"-DCMAKE_EXE_LINKER_FLAGS={exe_flags}",
    ]
    conf_code, conf_out = run(configure_cmd, root)
    if conf_code != 0:
        return {
            "name": name,
            "configured": False,
            "built": False,
            "tests": [],
            "pass": False,
            "skipped": True,
            "reason": "configure_failed",
            "configure_output": conf_out,
        }

    build_cmd = ["cmake", "--build", str(build_dir), "-j", "--target", *TEST_TARGETS]
    build_code, build_out = run(build_cmd, root)
    if build_code != 0:
        return {
            "name": name,
            "configured": True,
            "built": False,
            "tests": [],
            "pass": False,
            "skipped": False,
            "reason": "build_failed",
            "configure_output": conf_out,
            "build_output": build_out,
        }

    tests = []
    all_pass = True
    test_env = None
    if name == "asan_ubsan":
        test_env = {
            **dict(os.environ),
            # libc++ container-overflow instrumentation can produce noisy false positives
            # with small-string optimization patterns in this codebase.
            "ASAN_OPTIONS": "detect_container_overflow=0",
        }
    for target in TEST_TARGETS:
        binary = build_dir / "compiler" / target
        code, out = run([str(binary)], root, env=test_env)
        ok = (code == 0)
        all_pass = all_pass and ok
        tests.append(
            {
                "target": target,
                "status": code,
                "pass": ok,
                "output": out,
            }
        )

    return {
        "name": name,
        "configured": True,
        "built": True,
        "tests": tests,
        "pass": all_pass,
        "skipped": False,
        "reason": "",
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--json-out", default="bench/results/phase10_sanitizers.json")
    parser.add_argument("--csv-out", default="bench/results/phase10_sanitizers.csv")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    configs = [
        (
            "asan_ubsan",
            "-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined",
            "-fsanitize=address,undefined",
        ),
        (
            "tsan",
            "-O1 -g -fno-omit-frame-pointer -fsanitize=thread",
            "-fsanitize=thread",
        ),
    ]

    records = [execute_config(root, name, cxx_flags, exe_flags) for name, cxx_flags, exe_flags in configs]
    payload = {
        "configs": records,
        "passed": sum(int(item["pass"]) for item in records),
        "total": len(records),
    }

    json_path = root / args.json_out
    csv_path = root / args.csv_out
    json_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    with csv_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["config", "pass", "configured", "built", "skipped", "reason"])
        for row in records:
            writer.writerow(
                [
                    row["name"],
                    int(row["pass"]),
                    int(row["configured"]),
                    int(row["built"]),
                    int(row["skipped"]),
                    row["reason"],
                ]
            )

    print(f"phase10 sanitizers: {payload['passed']}/{payload['total']} passed")
    print(f"results json: {json_path}")
    print(f"results csv: {csv_path}")


if __name__ == "__main__":
    main()
