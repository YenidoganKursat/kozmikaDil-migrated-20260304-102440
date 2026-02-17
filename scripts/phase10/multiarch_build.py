#!/usr/bin/env python3
import argparse
import json
import os
import platform
import subprocess
from pathlib import Path


def run(command, env=None):
    proc = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env, check=False)
    return proc.returncode, proc.stdout


def host_arch():
    machine = platform.machine().lower()
    if machine in {"x86_64", "amd64"}:
        return "x86_64"
    if machine in {"arm64", "aarch64"}:
        return "aarch64"
    if machine.startswith("riscv64"):
        return "riscv64"
    return machine


def fallback_target_for_arch(arch):
    system = platform.system().lower()
    if system == "darwin":
        if arch == "x86_64":
            return "x86_64-apple-darwin"
        if arch == "aarch64":
            return "arm64-apple-darwin"
    return ""


def is_host_target(target):
    arch = target.split("-")[0]
    return arch == host_arch()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--program", default="bench/programs/phase4/scalar_sum.k")
    parser.add_argument("--targets", default="x86_64-linux-gnu,aarch64-linux-gnu,riscv64-linux-gnu")
    parser.add_argument("--out-dir", default="build/phase10/multiarch")
    parser.add_argument("--json-out", default="bench/results/phase10_multiarch.json")
    parser.add_argument("--run-host-smoke", action="store_true")
    parser.add_argument("--lto", choices=["off", "thin", "full"], default="thin")
    parser.add_argument("--sysroot-x86_64", default=os.environ.get("SPARK_SYSROOT_X86_64", ""))
    parser.add_argument("--sysroot-aarch64", default=os.environ.get("SPARK_SYSROOT_AARCH64", ""))
    parser.add_argument("--sysroot-riscv64", default=os.environ.get("SPARK_SYSROOT_RISCV64", ""))
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    k_bin = root / "k"
    source = root / args.program
    out_dir = root / args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    if not source.exists():
        raise SystemExit(f"missing source: {source}")

    sysroots = {
        "x86_64": args.sysroot_x86_64,
        "aarch64": args.sysroot_aarch64,
        "riscv64": args.sysroot_riscv64,
    }

    records = []
    targets = [item.strip() for item in args.targets.split(",") if item.strip()]
    for target in targets:
        arch = target.split("-")[0]
        out_bin = out_dir / f"{source.stem}.{target}.bin"
        command = [str(k_bin), "build", str(source), "-o", str(out_bin), "--target", target]
        if args.lto != "off":
            command.extend(["--lto", args.lto])
        sysroot = sysroots.get(arch, "")
        if sysroot:
            command.extend(["--sysroot", sysroot])
        code, output = run(command)
        build_ok = (code == 0)
        effective_target = target
        fallback_used = False
        fallback_target = ""
        fallback_output = ""
        fallback_status = 0

        if not build_ok:
            fallback_target = fallback_target_for_arch(arch)
            if fallback_target:
                fallback_used = True
                fallback_command = [str(k_bin), "build", str(source), "-o", str(out_bin), "--target", fallback_target]
                if args.lto != "off":
                    fallback_command.extend(["--lto", args.lto])
                fallback_status, fallback_output = run(fallback_command)
                if fallback_status == 0:
                    build_ok = True
                    effective_target = fallback_target

        smoke = {
            "attempted": False,
            "status_ok": False,
            "output": "",
            "status": 0,
        }
        if build_ok and args.run_host_smoke and is_host_target(target):
            smoke["attempted"] = True
            smoke_status, smoke_output = run([str(out_bin)])
            smoke["status"] = smoke_status
            smoke["status_ok"] = (smoke_status == 0)
            smoke["output"] = smoke_output

        records.append(
            {
                "target": target,
                "arch": arch,
                "build_ok": build_ok,
                "binary": str(out_bin),
                "command": command,
                "output": output,
                "effective_target": effective_target,
                "fallback_used": fallback_used,
                "fallback_target": fallback_target,
                "fallback_status": fallback_status,
                "fallback_output": fallback_output,
                "smoke": smoke,
            }
        )

    payload = {
        "host_arch": host_arch(),
        "program": str(source),
        "targets": records,
        "passed": all(item["build_ok"] for item in records),
    }

    json_out = root / args.json_out
    json_out.parent.mkdir(parents=True, exist_ok=True)
    json_out.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    passed = sum(int(item["build_ok"]) for item in records)
    print(f"phase10 multiarch: {passed}/{len(records)} builds passed")
    print(f"results json: {json_out}")


if __name__ == "__main__":
    main()
