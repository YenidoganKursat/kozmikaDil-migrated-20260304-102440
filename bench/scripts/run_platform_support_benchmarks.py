#!/usr/bin/env python3
import argparse
import csv
import json
import os
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent.parent
K_BIN = ROOT_DIR / "k"

import sys

sys.path.insert(0, str(SCRIPT_DIR / "platform_support"))
from definitions import dispatch_variants, platform_support_targets  # noqa: E402
from utils import first_float, is_close, load_json, run  # noqa: E402


def run_multiarch(args):
    script = ROOT_DIR / "scripts/core/pipeline/platform_support/multiarch_build.py"
    command = [
        "python3",
        str(script),
        "--program",
        "bench/programs/scalar_runtime/scalar_sum.k",
        "--targets",
        platform_support_targets(),
        "--json-out",
        "bench/results/platform_support_multiarch.json",
        "--lto",
        args.lto,
        "--run-host-smoke",
    ]
    status, output = run(command, cwd=ROOT_DIR)
    report = load_json(ROOT_DIR / "bench/results/platform_support_multiarch.json")
    targets = report.get("targets", [])
    build_ok = sum(int(t.get("build_ok", False)) for t in targets)
    pass_flag = status == 0 and build_ok >= args.min_multiarch_success
    return {
        "section": "multiarch",
        "pass": pass_flag,
        "status": status,
        "build_ok": build_ok,
        "total_targets": len(targets),
        "min_required": args.min_multiarch_success,
        "output": output,
        "report": report,
    }


def run_platform_matrix(args):
    script = ROOT_DIR / "scripts/core/pipeline/platform_support/platform_matrix.py"
    command = [
        "python3",
        str(script),
        "--preset",
        "market",
        "--include-experimental",
        "--include-embedded",
        "--include-gpu-experimental",
        "--include-gpu-planning",
        "--json-out",
        "bench/results/platform_support_platform_matrix.json",
    ]
    if args.platform_probe_build:
        command.extend(["--run-multiarch-probe", "--multiarch-lto", "off"])
    status, output = run(command, cwd=ROOT_DIR)
    report = load_json(ROOT_DIR / "bench/results/platform_support_platform_matrix.json")
    cpu_summary = report.get("cpu", {}).get("summary", {})
    gpu_summary = report.get("gpu", {}).get("summary", {})
    # Keep gate realistic for heterogeneous CI hosts:
    # - platform matrix command must run,
    # - at least one stable CPU target must be listed.
    pass_flag = status == 0 and int(cpu_summary.get("stable", 0)) >= 1
    return {
        "section": "platform_matrix",
        "pass": pass_flag,
        "status": status,
        "cpu_summary": cpu_summary,
        "gpu_summary": gpu_summary,
        "output": output,
        "report": report,
    }


def run_dispatch():
    program = ROOT_DIR / "bench/programs/platform_support/dispatch_consistency.k"
    records = []
    ref_value = None
    for variant in dispatch_variants():
        env = dict(os.environ)
        if variant.arch:
            env["SPARK_CPU_ARCH"] = variant.arch
        else:
            env.pop("SPARK_CPU_ARCH", None)
        if variant.features:
            env["SPARK_CPU_FEATURES"] = variant.features
        else:
            env.pop("SPARK_CPU_FEATURES", None)

        status, output = run([str(K_BIN), "run", "--interpret", str(program)], env=env, cwd=ROOT_DIR)
        parsed = 0.0
        parse_ok = False
        if status == 0:
            try:
                parsed = first_float(output)
                parse_ok = True
            except Exception:  # noqa: BLE001
                parse_ok = False
        if parse_ok and ref_value is None:
            ref_value = parsed
        equal = parse_ok and ref_value is not None and is_close(parsed, ref_value, rel_tol=1e-9, abs_tol=1e-9)
        records.append(
            {
                "name": variant.name,
                "status": status,
                "parse_ok": parse_ok,
                "value": parsed,
                "equal_to_ref": equal,
                "arch": variant.arch,
                "features": variant.features,
            }
        )

    pass_flag = all(row["status"] == 0 and row["parse_ok"] and row["equal_to_ref"] for row in records)
    return {
        "section": "dispatch",
        "pass": pass_flag,
        "records": records,
        "reference_value": ref_value,
    }


def run_pgo(args):
    script = ROOT_DIR / "scripts/pgo_cycle.sh"
    command = [
        "bash",
        str(script),
        "--program",
        args.pgo_program,
        "--out-dir",
        "bench/results/platform_support/pgo",
        "--lto",
        args.lto,
        "--runs",
        str(args.runs),
        "--warmup-runs",
        str(args.warmup_runs),
        "--profile-runs",
        str(args.profile_runs),
    ]
    status, output = run(command, cwd=ROOT_DIR)
    report = load_json(ROOT_DIR / "bench/results/platform_support/pgo/platform_support_pgo_cycle.json")
    speedup = float(report.get("speedup_vs_baseline", 0.0))
    pass_flag = status == 0 and speedup >= args.min_pgo_speedup
    return {
        "section": "pgo",
        "pass": pass_flag,
        "status": status,
        "speedup_vs_baseline": speedup,
        "min_required": args.min_pgo_speedup,
        "output": output,
        "report": report,
    }


def run_bolt(args):
    script = ROOT_DIR / "scripts/bolt_opt.sh"
    binary = ROOT_DIR / "bench/results/platform_support/pgo/native_pgo.bin"
    command = [
        "bash",
        str(script),
        "--binary",
        str(binary),
        "--profile-cmd",
        str(binary),
        "--out-dir",
        "bench/results/platform_support/bolt",
        "--runs",
        str(args.runs),
        "--warmup-runs",
        str(args.warmup_runs),
        "--profile-runs",
        str(args.profile_runs),
    ]
    status, output = run(command, cwd=ROOT_DIR)
    report = load_json(ROOT_DIR / "bench/results/platform_support/bolt/platform_support_bolt.json")
    skipped = bool(report.get("skipped", False))
    speedup = float(report.get("speedup_vs_baseline", 0.0))
    pass_flag = status == 0 and (skipped or speedup >= args.min_bolt_speedup)
    return {
        "section": "bolt",
        "pass": pass_flag,
        "status": status,
        "skipped": skipped,
        "speedup_vs_baseline": speedup,
        "min_required": args.min_bolt_speedup,
        "output": output,
        "report": report,
    }


def run_autofdo(args):
    script = ROOT_DIR / "scripts/autofdo_cycle.sh"
    command = [
        "bash",
        str(script),
        "--program",
        args.pgo_program,
        "--out-dir",
        "bench/results/platform_support/autofdo",
        "--lto",
        args.lto,
        "--runs",
        str(args.runs),
        "--warmup-runs",
        str(args.warmup_runs),
        "--profile-runs",
        str(args.profile_runs),
    ]
    status, output = run(command, cwd=ROOT_DIR)
    report = load_json(ROOT_DIR / "bench/results/platform_support/autofdo/platform_support_autofdo_cycle.json")
    skipped = bool(report.get("skipped", False))
    speedup = float(report.get("speedup_vs_baseline", 0.0))
    pass_flag = status == 0 and (skipped or speedup >= args.min_autofdo_speedup)
    return {
        "section": "autofdo",
        "pass": pass_flag,
        "status": status,
        "skipped": skipped,
        "speedup_vs_baseline": speedup,
        "min_required": args.min_autofdo_speedup,
        "output": output,
        "report": report,
    }


def write_report(args, sections):
    out_json = ROOT_DIR / "bench/results/platform_support_benchmarks.json"
    out_csv = ROOT_DIR / "bench/results/platform_support_benchmarks.csv"
    out_md = ROOT_DIR / "bench/report_platform_support.md"
    out_json.parent.mkdir(parents=True, exist_ok=True)

    payload = {
        "config": {
            "lto": args.lto,
            "runs": args.runs,
            "warmup_runs": args.warmup_runs,
            "profile_runs": args.profile_runs,
            "min_multiarch_success": args.min_multiarch_success,
            "min_pgo_speedup": args.min_pgo_speedup,
            "min_autofdo_speedup": args.min_autofdo_speedup,
            "min_bolt_speedup": args.min_bolt_speedup,
        },
        "sections": sections,
    }
    out_json.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    with out_csv.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["section", "pass", "key", "value"])
        for section in sections:
            writer.writerow([section["section"], int(section["pass"]), "status", section.get("status", 0)])
            if section["section"] == "multiarch":
                writer.writerow(["multiarch", int(section["pass"]), "build_ok", section.get("build_ok", 0)])
                writer.writerow(["multiarch", int(section["pass"]), "total_targets", section.get("total_targets", 0)])
            if section["section"] == "platform_matrix":
                cpu = section.get("cpu_summary", {})
                gpu = section.get("gpu_summary", {})
                writer.writerow(["platform_matrix", int(section["pass"]), "cpu_total", cpu.get("total", 0)])
                writer.writerow(["platform_matrix", int(section["pass"]), "cpu_stable", cpu.get("stable", 0)])
                writer.writerow(
                    ["platform_matrix", int(section["pass"]), "cpu_build_ok", cpu.get("build_ok", 0)]
                )
                writer.writerow(["platform_matrix", int(section["pass"]), "gpu_total", gpu.get("total", 0)])
                writer.writerow(
                    ["platform_matrix", int(section["pass"]), "gpu_available", gpu.get("available", 0)]
                )
            if section["section"] == "dispatch":
                writer.writerow(["dispatch", int(section["pass"]), "reference_value", section.get("reference_value", 0.0)])
                for row in section.get("records", []):
                    writer.writerow(["dispatch", int(section["pass"]), f"{row['name']}_equal", int(row["equal_to_ref"])])
            if section["section"] == "pgo":
                writer.writerow(["pgo", int(section["pass"]), "speedup_vs_baseline", section.get("speedup_vs_baseline", 0.0)])
            if section["section"] == "autofdo":
                writer.writerow(["autofdo", int(section["pass"]), "skipped", int(section.get("skipped", False))])
                writer.writerow(["autofdo", int(section["pass"]), "speedup_vs_baseline", section.get("speedup_vs_baseline", 0.0)])
            if section["section"] == "bolt":
                writer.writerow(["bolt", int(section["pass"]), "skipped", int(section.get("skipped", False))])
                writer.writerow(["bolt", int(section["pass"]), "speedup_vs_baseline", section.get("speedup_vs_baseline", 0.0)])

    by_name = {entry["section"]: entry for entry in sections}
    multiarch = by_name.get("multiarch", {})
    platform_matrix = by_name.get("platform_matrix", {})
    dispatch = by_name.get("dispatch", {})
    pgo = by_name.get("pgo", {})
    autofdo = by_name.get("autofdo", {})
    bolt = by_name.get("bolt", {})

    autofdo_summary = "skipped" if autofdo.get("skipped", False) else f"{autofdo.get('speedup_vs_baseline', 0.0):.4f}x"
    bolt_summary = "skipped" if bolt.get("skipped", False) else f"{bolt.get('speedup_vs_baseline', 0.0):.4f}x"
    md_lines = [
        "# Phase 10 Benchmark Report",
        "",
        f"- multiarch: {'PASS' if multiarch.get('pass', False) else 'FAIL'} ({multiarch.get('build_ok', 0)}/{multiarch.get('total_targets', 0)})",
        f"- platform matrix: {'PASS' if platform_matrix.get('pass', False) else 'FAIL'} "
        f"(cpu stable={platform_matrix.get('cpu_summary', {}).get('stable', 0)}, "
        f"gpu available={platform_matrix.get('gpu_summary', {}).get('available', 0)})",
        f"- dispatch equivalence: {'PASS' if dispatch.get('pass', False) else 'FAIL'}",
        f"- PGO/LTO gain: {'PASS' if pgo.get('pass', False) else 'FAIL'} ({pgo.get('speedup_vs_baseline', 0.0):.4f}x)",
        f"- AutoFDO gain: {'PASS' if autofdo.get('pass', False) else 'FAIL'} ({autofdo_summary})",
        f"- BOLT gain: {'PASS' if bolt.get('pass', False) else 'FAIL'} ({bolt_summary})",
        "",
        f"- JSON: `{out_json}`",
        f"- CSV: `{out_csv}`",
    ]
    out_md.write_text("\n".join(md_lines) + "\n", encoding="utf-8")
    return out_json, out_csv, out_md


def main():
    parser = argparse.ArgumentParser()
    # Build-time is secondary in this project; default to full LTO for peak release-perf.
    parser.add_argument("--lto", choices=["off", "thin", "full"], default="full")
    parser.add_argument("--runs", type=int, default=11)
    parser.add_argument("--warmup-runs", type=int, default=2)
    # Higher profiling run count gives more stable PGO profile quality on this host.
    parser.add_argument("--profile-runs", type=int, default=5)
    parser.add_argument("--pgo-program", default="bench/programs/platform_support/pgo_call_chain_large.k")
    parser.add_argument("--min-multiarch-success", type=int, default=2)
    parser.add_argument("--min-pgo-speedup", type=float, default=1.0)
    parser.add_argument("--min-autofdo-speedup", type=float, default=1.0)
    parser.add_argument("--min-bolt-speedup", type=float, default=1.0)
    parser.add_argument("--platform-probe-build", action="store_true")
    args = parser.parse_args()

    sections = [
        run_multiarch(args),
        run_platform_matrix(args),
        run_dispatch(),
        run_pgo(args),
        run_autofdo(args),
        run_bolt(args),
    ]
    out_json, out_csv, out_md = write_report(args, sections)

    passed = sum(int(section["pass"]) for section in sections)
    print(f"platform_support sections: {passed}/{len(sections)} passed")
    print(f"results json: {out_json}")
    print(f"results csv: {out_csv}")
    print(f"report md: {out_md}")


if __name__ == "__main__":
    main()
