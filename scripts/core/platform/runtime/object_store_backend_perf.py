#!/usr/bin/env python3
"""Object-store portability/perf matrix across backend requests."""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import statistics
import subprocess
import sys
import time
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
  sys.path.insert(0, str(SCRIPT_DIR))

from gpu_backend_catalog import canonicalize_backend_name, known_backend_names


REPO_ROOT = Path(__file__).resolve().parents[4]
K_BIN = REPO_ROOT / "k"
SPARKC_CANDIDATES = (
    REPO_ROOT / "build" / "compiler" / "sparkc",
    REPO_ROOT / "build_local_full" / "compiler" / "sparkc",
    REPO_ROOT / "build_local" / "compiler" / "sparkc",
)
HOST_BACKENDS = {"auto", "own", "blas"}


def resolve_exec_bin() -> Path:
  def runnable(candidate: Path) -> bool:
    if not candidate.exists() or not os.access(candidate, os.X_OK):
      return False
    try:
      probe = subprocess.run(
          [str(candidate), "--help"],
          stdout=subprocess.PIPE,
          stderr=subprocess.PIPE,
          text=True,
          check=False,
          timeout=2,
      )
      return probe.returncode in (0, 1, 2)
    except (OSError, subprocess.SubprocessError):
      return False

  best: Path | None = None
  for candidate in SPARKC_CANDIDATES:
    if not runnable(candidate):
      continue
    if best is None or candidate.stat().st_mtime > best.stat().st_mtime:
      best = candidate
  return best if best is not None else K_BIN


def parse_csv(value: str) -> list[str]:
  return [item.strip() for item in value.split(",") if item.strip()]


def parse_bool(token: str) -> bool:
  normalized = token.strip().lower()
  return normalized in {"1", "true", "ok", "pass"}


def parse_program_output(text: str) -> dict[str, float | bool]:
  lines = [line.strip() for line in text.splitlines() if line.strip()]
  if len(lines) < 4:
    raise ValueError(f"object-store output expected >=4 lines, got {len(lines)}")
  return {
      "object_total": float(lines[0]),
      "matmul_diff": float(lines[1]),
      "matmul_ok": parse_bool(lines[2]),
      "backend_id": float(lines[3]),
  }


def run_once(command: list[str], env: dict[str, str], cwd: Path) -> tuple[int, str, float]:
  t0 = time.perf_counter()
  proc = subprocess.run(
      command,
      cwd=str(cwd),
      env=env,
      stdout=subprocess.PIPE,
      stderr=subprocess.STDOUT,
      text=True,
      check=False,
  )
  elapsed = time.perf_counter() - t0
  return proc.returncode, proc.stdout, elapsed


def resolve_requested_backends(raw: str) -> tuple[list[tuple[str, str]], list[str], list[str]]:
  requested = parse_csv(raw)
  if len(requested) == 1 and requested[0].strip().lower() == "all":
    requested = ["auto", "own", "blas", *known_backend_names()]
  if not requested:
    raise SystemExit("no backend selected")

  known_gpu = set(known_backend_names())
  selected_pairs: list[tuple[str, str]] = []
  unknown: list[str] = []
  seen: set[str] = set()
  for item in requested:
    canonical = canonicalize_backend_name(item)
    if canonical not in known_gpu and canonical not in HOST_BACKENDS:
      unknown.append(item)
      continue
    if canonical in seen:
      continue
    seen.add(canonical)
    selected_pairs.append((item, canonical))

  if unknown:
    raise SystemExit(f"unknown backend(s): {', '.join(unknown)}")

  requested_backends = [item[1] for item in selected_pairs]
  requested_gpu_backends = sorted([item for item in requested_backends if item in known_gpu])
  return selected_pairs, requested_backends, requested_gpu_backends


def backend_id_to_effective(backend_id: float) -> str:
  return "blas" if int(backend_id) == 1 else "own"


def main() -> int:
  parser = argparse.ArgumentParser(description="Object-store backend portability/perf matrix")
  parser.add_argument("--program", default="bench/programs/platform_support/object_store_backend_perf.k")
  parser.add_argument(
      "--backends",
      default="auto,own,blas,cuda,rocm_hip,oneapi_sycl,opencl,vulkan_compute,metal,webgpu",
      help="comma-separated backend list or 'all'",
  )
  parser.add_argument("--runs", type=int, default=3)
  parser.add_argument("--warmup", type=int, default=1)
  parser.add_argument("--json-out", default="bench/results/platform_support_object_store_backend_perf.json")
  parser.add_argument("--csv-out", default="bench/results/platform_support_object_store_backend_perf.csv")
  parser.add_argument(
      "--matmul-diff-tolerance",
      type=float,
      default=1e-6,
      help="absolute tolerance for matmul diff acceptance",
  )
  parser.add_argument(
      "--object-total-abs-tolerance",
      type=float,
      default=1e-6,
      help="absolute tolerance for object_total consistency across backend requests",
  )
  parser.add_argument(
      "--object-total-rel-tolerance",
      type=float,
      default=1e-12,
      help="relative tolerance for object_total consistency across backend requests",
  )
  parser.add_argument("--allow-missing-runtime", action="store_true")
  parser.add_argument("--require-portable-routing", action="store_true")
  parser.add_argument("--require-gpu-coverage", action="store_true")
  args = parser.parse_args()

  program = REPO_ROOT / args.program
  if not program.exists():
    raise SystemExit(f"missing program: {program}")

  exec_bin = resolve_exec_bin()
  if not exec_bin.exists():
    raise SystemExit(f"missing k launcher: {exec_bin}")

  selected_pairs, requested_backends, requested_gpu_backends = resolve_requested_backends(args.backends)
  known_gpu = set(known_backend_names())
  strict_errors: list[str] = []

  if args.require_gpu_coverage:
    missing_gpu = sorted(set(known_gpu) - set(requested_gpu_backends))
    if missing_gpu:
      strict_errors.append(f"missing gpu backend coverage: {', '.join(missing_gpu)}")

  runtime_probe = subprocess.run(
      [str(exec_bin), "env", "--print-cpu-features"],
      cwd=str(REPO_ROOT),
      stdout=subprocess.PIPE,
      stderr=subprocess.STDOUT,
      text=True,
      check=False,
  )
  runtime_ready = runtime_probe.returncode == 0
  if not runtime_ready and args.allow_missing_runtime:
    out_json = REPO_ROOT / args.json_out
    out_csv = REPO_ROOT / args.csv_out
    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_csv.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "exec_bin": str(exec_bin),
        "program": str(program),
        "runs": max(1, args.runs),
        "warmup": max(0, args.warmup),
        "requested_backends": requested_backends,
        "requested_gpu_backends": requested_gpu_backends,
        "skipped": True,
        "skip_reason": "runtime_not_built",
        "probe_output": runtime_probe.stdout,
        "strict": {
            "require_portable_routing": bool(args.require_portable_routing),
            "require_gpu_coverage": bool(args.require_gpu_coverage),
            "errors": strict_errors,
            "passed": len(strict_errors) == 0,
        },
        "results": [],
    }
    out_json.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    with out_csv.open("w", encoding="utf-8", newline="") as handle:
      writer = csv.writer(handle)
      writer.writerow(["skipped", "skip_reason"])
      writer.writerow([1, "runtime_not_built"])
    print("object-store backend perf: skipped (runtime_not_built)")
    print(f"object-store backend perf json: {out_json}")
    print(f"object-store backend perf csv: {out_csv}")
    return 0 if not strict_errors else 1
  if not runtime_ready:
    raise SystemExit(runtime_probe.stdout.strip() or "k runtime probe failed")

  rows: list[dict[str, object]] = []
  base_total: float | None = None
  command = [str(exec_bin), "run", "--interpret", str(program)]

  for backend_input, backend in selected_pairs:
    env = dict(os.environ)
    env["SPARK_MATMUL_BACKEND"] = backend
    env["SPARK_MATMUL_AUTO_LEARN"] = "0"

    for _ in range(max(0, args.warmup)):
      run_once(command, env=env, cwd=REPO_ROOT)

    statuses: list[int] = []
    times: list[float] = []
    first_output = ""
    for i in range(max(1, args.runs)):
      status, output, elapsed = run_once(command, env=env, cwd=REPO_ROOT)
      statuses.append(status)
      times.append(elapsed)
      if i == 0:
        first_output = output

    status_ok = all(code == 0 for code in statuses)
    parse_ok = False
    parse_error = ""
    parsed: dict[str, float | bool] = {}
    if status_ok:
      try:
        parsed = parse_program_output(first_output)
        parse_ok = True
      except Exception as exc:  # noqa: BLE001
        parse_error = str(exc)

    object_total = float(parsed.get("object_total", 0.0)) if parse_ok else 0.0
    matmul_diff = float(parsed.get("matmul_diff", 0.0)) if parse_ok else 0.0
    matmul_ok = bool(parsed.get("matmul_ok", False)) if parse_ok else False
    backend_id = float(parsed.get("backend_id", -1.0)) if parse_ok else -1.0
    effective_backend = backend_id_to_effective(backend_id) if parse_ok else ""
    program_ok = bool(matmul_ok or abs(matmul_diff) <= args.matmul_diff_tolerance)

    total_consistent = False
    if parse_ok:
      if base_total is None:
        base_total = object_total
        total_consistent = True
      else:
        total_consistent = math.isclose(
            object_total,
            base_total,
            rel_tol=args.object_total_rel_tolerance,
            abs_tol=args.object_total_abs_tolerance,
        )

    median_sec = statistics.median(times) if times else 0.0
    mean_sec = statistics.fmean(times) if times else 0.0
    row = {
        "requested_backend_input": backend_input,
        "requested_backend": backend,
        "effective_backend": effective_backend,
        "status_ok": status_ok,
        "parse_ok": parse_ok,
        "parse_error": parse_error,
        "program_ok": program_ok,
        "total_consistent": total_consistent,
        "object_total": object_total,
        "matmul_diff": matmul_diff,
        "matmul_ok": matmul_ok,
        "backend_id": backend_id,
        "median_time_sec": median_sec,
        "mean_time_sec": mean_sec,
    }
    row["portable_route_ok"] = (
        bool(row["status_ok"])
        and bool(row["parse_ok"])
        and bool(row["program_ok"])
        and bool(row["total_consistent"])
        and str(row["effective_backend"]) in {"own", "blas"}
    )
    rows.append(row)
    print(
        f"[object-store] requested={backend:<14} effective={effective_backend or 'n/a':<4} "
        f"median={median_sec:.6f}s program_ok={program_ok} total_consistent={total_consistent}"
    )

  if base_total is None:
    strict_errors.append("no successful parse row found")

  for row in rows:
    requested = str(row.get("requested_backend", ""))
    if not bool(row.get("status_ok", False)):
      strict_errors.append(f"{requested}: non-zero status")
    if not bool(row.get("parse_ok", False)):
      strict_errors.append(f"{requested}: parse failure")
    if not bool(row.get("program_ok", False)):
      strict_errors.append(f"{requested}: program check failed")
    if not bool(row.get("total_consistent", False)):
      strict_errors.append(f"{requested}: object_total mismatch across backend requests")
    if args.require_portable_routing and requested in known_gpu and not bool(row.get("portable_route_ok", False)):
      strict_errors.append(f"{requested}: portable routing failed")

  out_json = REPO_ROOT / args.json_out
  out_csv = REPO_ROOT / args.csv_out
  out_json.parent.mkdir(parents=True, exist_ok=True)
  out_csv.parent.mkdir(parents=True, exist_ok=True)

  payload = {
      "exec_bin": str(exec_bin),
      "program": str(program),
      "runs": max(1, args.runs),
      "warmup": max(0, args.warmup),
      "requested_backends": requested_backends,
      "requested_gpu_backends": requested_gpu_backends,
      "summary": {
          "total_rows": len(rows),
          "status_ok": sum(1 for row in rows if bool(row.get("status_ok", False))),
          "parse_ok": sum(1 for row in rows if bool(row.get("parse_ok", False))),
          "program_ok": sum(1 for row in rows if bool(row.get("program_ok", False))),
          "total_consistent": sum(1 for row in rows if bool(row.get("total_consistent", False))),
          "portable_route_ok": sum(1 for row in rows if bool(row.get("portable_route_ok", False))),
      },
      "strict": {
          "require_portable_routing": bool(args.require_portable_routing),
          "require_gpu_coverage": bool(args.require_gpu_coverage),
          "errors": strict_errors,
          "passed": len(strict_errors) == 0,
      },
      "results": rows,
  }
  out_json.write_text(json.dumps(payload, indent=2), encoding="utf-8")

  with out_csv.open("w", encoding="utf-8", newline="") as handle:
    writer = csv.writer(handle)
    writer.writerow(
        [
            "requested_backend",
            "effective_backend",
            "status_ok",
            "parse_ok",
            "program_ok",
            "total_consistent",
            "portable_route_ok",
            "object_total",
            "matmul_diff",
            "matmul_ok",
            "backend_id",
            "median_time_sec",
            "mean_time_sec",
        ]
    )
    for row in rows:
      writer.writerow(
          [
              row.get("requested_backend", ""),
              row.get("effective_backend", ""),
              int(bool(row.get("status_ok", False))),
              int(bool(row.get("parse_ok", False))),
              int(bool(row.get("program_ok", False))),
              int(bool(row.get("total_consistent", False))),
              int(bool(row.get("portable_route_ok", False))),
              float(row.get("object_total", 0.0)),
              float(row.get("matmul_diff", 0.0)),
              int(bool(row.get("matmul_ok", False))),
              float(row.get("backend_id", 0.0)),
              float(row.get("median_time_sec", 0.0)),
              float(row.get("mean_time_sec", 0.0)),
          ]
      )

  print(f"object-store backend perf json: {out_json}")
  print(f"object-store backend perf csv: {out_csv}")
  if strict_errors:
    print("object-store backend perf strict FAIL")
    for item in strict_errors:
      print(f"  - {item}")
    return 1
  print("object-store backend perf strict PASS")
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
