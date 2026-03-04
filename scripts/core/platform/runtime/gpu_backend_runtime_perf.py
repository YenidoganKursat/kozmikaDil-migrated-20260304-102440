#!/usr/bin/env python3
"""Runtime performance matrix for backend requests (including GPU aliases).

This benchmark runs a matmul_schedule_gpu matmul program for each requested backend name and
records:
- runtime latency (median/mean),
- correctness flags from program output,
- effective backend selected by runtime (own/blas).
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import statistics
import subprocess
import sys
import time
import tempfile
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
DEFAULT_MATRIX_SIZE = 128
DEFAULT_REPEATS = 3
N_ASSIGN_RE = re.compile(r"^(\s*)n\s*=\s*\d+\s*$", re.MULTILINE)
REPEATS_ASSIGN_RE = re.compile(r"^(\s*)repeats\s*=\s*\d+\s*$", re.MULTILINE)


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


def parse_matmul_schedule_gpu_output(text: str) -> dict[str, float]:
  lines = [line.strip() for line in text.splitlines() if line.strip()]
  if len(lines) < 14:
    raise ValueError(f"matmul_schedule_gpu output expected >=14 lines, got {len(lines)}")

  def as_bool(token: str) -> float:
    return 1.0 if token.lower() in {"1", "true", "ok", "pass"} else 0.0

  return {
      "total": float(lines[0]),
      "expected": float(lines[1]),
      "diff": float(lines[2]),
      "pass": as_bool(lines[3]),
      "calls": float(lines[4]),
      "own_calls": float(lines[5]),
      "blas_calls": float(lines[6]),
      "cache_hit_a": float(lines[7]),
      "cache_hit_b": float(lines[8]),
      "epilogue_fused": float(lines[9]),
      "tile_m": float(lines[10]),
      "tile_n": float(lines[11]),
      "tile_k": float(lines[12]),
      "backend_id": float(lines[13]),
  }


def estimate_workload(
  matrix_size: int,
  repeats: int,
  workload_scale: float,
  requested_matrix_size: int | None,
  requested_repeats: int | None,
  max_matrix_size: int,
) -> tuple[int, int]:
  if requested_matrix_size is None and requested_repeats is None:
    factor = max(1.0, float(workload_scale))
    if factor <= 1.0:
      return matrix_size, repeats
    scaled_matrix = int(round(matrix_size * (factor ** (1.0 / 3.0))))
    scaled_matrix = min(max_matrix_size, max(scaled_matrix, matrix_size))
    target_ops = 2.0 * (matrix_size**3) * repeats * factor
    scaled_repeats = int(round(target_ops / (2.0 * (scaled_matrix**3))))
    return scaled_matrix, max(1, scaled_repeats)
  return requested_matrix_size or matrix_size, requested_repeats or repeats


def build_workload_program(
  source: Path,
  matrix_size: int,
  repeats: int,
  generated_by_scale: bool,
) -> tuple[Path, bool]:
  if not source.exists():
    raise SystemExit(f"missing program: {source}")

  if matrix_size == DEFAULT_MATRIX_SIZE and repeats == DEFAULT_REPEATS and not generated_by_scale:
    return source, False

  text = source.read_text(encoding="utf-8")
  replaced = N_ASSIGN_RE.sub(rf"\1n = {matrix_size}", text, count=1)
  if replaced == text:
    raise SystemExit(f"program does not expose matmul matrix-size assignment: {source}")

  repeated = REPEATS_ASSIGN_RE.sub(rf"\1repeats = {repeats}", replaced, count=1)
  if repeated == replaced:
    raise SystemExit(f"program does not expose repeats assignment: {source}")

  with tempfile.NamedTemporaryFile("w", suffix=".k", prefix="platform_support_matmul_", dir=str(source.parent), delete=False) as handle:
    handle.write(repeated)
    return Path(handle.name), True


def compute_gflops(matrix_size: int, repeats: int, elapsed_sec: float) -> float:
  if elapsed_sec <= 0.0:
    return 0.0
  operations = 2.0 * (matrix_size**3) * repeats
  return operations / (1_000_000_000.0 * elapsed_sec)


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


def make_thread_env(thread_count: int, force_serial_own: bool) -> dict[str, str]:
  t = str(max(1, thread_count))
  out = {
      "OPENBLAS_NUM_THREADS": t,
      "OMP_NUM_THREADS": t,
      "MKL_NUM_THREADS": t,
      "VECLIB_MAXIMUM_THREADS": t,
      "BLIS_NUM_THREADS": t,
      "SPARK_MATMUL_OWN_THREADS": t,
      "SPARK_MATMUL_OWN_DEFAULT_THREADS": t,
  }
  if force_serial_own:
    out["SPARK_MATMUL_OWN_PAR_MIN_VOLUME"] = "999999999999"
    out["SPARK_MATMUL_OWN_DEFAULT_MIN_VOLUME"] = "999999999999"
  return out


DEFAULT_BACKEND_PROFILE = {
    "cuda": "latency_serial_1t_tile96",
    "rocm_hip": "throughput_default",
    "oneapi_sycl": "throughput_default",
    "opencl": "latency_serial_1t",
    "vulkan_compute": "latency_serial_1t_tile64",
    "metal": "latency_serial_1t_tile96",
    "webgpu": "latency_serial_1t_tile64",
}


def build_backend_candidate_profiles(backend: str, throughput_threads: int) -> list[tuple[str, dict[str, str]]]:
  serial_env = make_thread_env(1, force_serial_own=True)
  throughput_env = make_thread_env(throughput_threads, force_serial_own=False)
  candidates: list[tuple[str, dict[str, str]]] = [
      ("throughput_default", throughput_env),
      ("latency_serial_1t", serial_env),
  ]

  tile64 = dict(serial_env)
  tile64["SPARK_MATMUL_TILE_M"] = "64"
  tile64["SPARK_MATMUL_TILE_N"] = "64"
  tile64["SPARK_MATMUL_TILE_K"] = "64"
  candidates.append(("latency_serial_1t_tile64", tile64))

  tile96 = dict(serial_env)
  tile96["SPARK_MATMUL_TILE_M"] = "96"
  tile96["SPARK_MATMUL_TILE_N"] = "96"
  tile96["SPARK_MATMUL_TILE_K"] = "96"
  candidates.append(("latency_serial_1t_tile96", tile96))

  if backend in {"cuda", "rocm_hip", "vulkan_compute"}:
    tile128 = dict(throughput_env)
    tile128["SPARK_MATMUL_TILE_M"] = "128"
    tile128["SPARK_MATMUL_TILE_N"] = "128"
    tile128["SPARK_MATMUL_TILE_K"] = "128"
    candidates.append(("throughput_tile128", tile128))

  return candidates


def prioritize_candidates_for_backend(
  backend: str, candidates: list[tuple[str, dict[str, str]]]
) -> list[tuple[str, dict[str, str]]]:
  preferred = DEFAULT_BACKEND_PROFILE.get(backend, "")
  if not preferred:
    return candidates
  ordered = [item for item in candidates if item[0] == preferred]
  ordered.extend(item for item in candidates if item[0] != preferred)
  return ordered if ordered else candidates


def pick_best_profile(
  command: list[str],
  cwd: Path,
  base_env: dict[str, str],
  candidates: list[tuple[str, dict[str, str]]],
  probe_runs: int,
  diff_tolerance: float,
) -> tuple[str, dict[str, str]]:
  best_name = candidates[0][0]
  best_env = candidates[0][1]
  best_time = float("inf")

  for name, overlay in candidates:
    env = dict(base_env)
    env.update(overlay)

    # Warm cache/process path for this candidate before measured probes.
    run_once(command, env=env, cwd=cwd)

    times: list[float] = []
    first_output = ""
    status_ok = True
    for i in range(max(1, probe_runs)):
      status, output, elapsed = run_once(command, env=env, cwd=cwd)
      if i == 0:
        first_output = output
      times.append(elapsed)
      if status != 0:
        status_ok = False
        break

    parse_ok = False
    program_pass = False
    if status_ok:
      try:
        parsed = parse_matmul_schedule_gpu_output(first_output)
        parse_ok = True
        if bool(int(parsed.get("pass", 0.0)) == 1):
          program_pass = True
        elif abs(float(parsed.get("diff", 0.0))) <= diff_tolerance:
          program_pass = True
      except Exception:  # noqa: BLE001
        parse_ok = False
        program_pass = False

    score = statistics.median(times) if times else float("inf")
    if not (status_ok and parse_ok and program_pass):
      score = float("inf")
    if score < best_time:
      best_time = score
      best_name = name
      best_env = overlay
  return best_name, best_env


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


def main() -> int:
  parser = argparse.ArgumentParser(description="Runtime perf for backend requests")
  parser.add_argument("--program", default="bench/programs/matmul_schedule_gpu/matmul_core_f64.k")
  parser.add_argument("--workload-scale", type=float, default=1.0, help="matmul workload scale factor")
  parser.add_argument(
      "--workload-matrix-size",
      type=int,
      default=0,
      help="override matmul matrix size (default: default program matrix size)",
  )
  parser.add_argument(
      "--workload-repeats",
      type=int,
      default=0,
      help="override matmul repeat count (default: default program repeat count)",
  )
  parser.add_argument(
      "--workload-max-matrix-size",
      type=int,
      default=4096,
      help="upper bound for auto-scaled matrix size",
  )
  parser.add_argument(
      "--min-gflops",
      type=float,
      default=0.0,
      help="fail backend row if median GFLOPS is below this value",
  )
  parser.add_argument(
      "--program-diff-tolerance",
      type=float,
      default=1e-6,
      help="override numeric pass when diff is within this absolute tolerance",
  )
  parser.add_argument(
      "--backends",
      default="auto,own,blas,cuda,rocm_hip,oneapi_sycl,opencl,vulkan_compute,metal,webgpu",
      help="comma-separated backend list or 'all'",
  )
  parser.add_argument("--runs", type=int, default=5)
  parser.add_argument("--warmup", type=int, default=1)
  parser.add_argument("--json-out", default="bench/results/platform_support_gpu_runtime_perf.json")
  parser.add_argument("--csv-out", default="bench/results/platform_support_gpu_runtime_perf.csv")
  parser.add_argument("--max-perf", action="store_true")
  parser.add_argument("--allow-missing-runtime", action="store_true")
  parser.add_argument("--require-portable-routing", action="store_true")
  parser.add_argument("--require-gpu-coverage", action="store_true")
  parser.add_argument(
      "--matmul-auto-learn",
      action="store_true",
      help="enable matmul_schedule_gpu matmul auto-learn during benchmark runs (disabled by default to reduce benchmark overhead)",
  )
  parser.add_argument(
      "--backend-profile-probe-runs",
      type=int,
      default=3,
      help="profile probe runs per backend candidate before final timing",
  )
  parser.add_argument(
      "--disable-backend-profile-autotune",
      action="store_true",
      help="disable per-backend candidate profiling and use default throughput profile only",
  )
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

    skip_strict_errors = list(strict_errors)
    if args.require_portable_routing:
      skip_strict_errors.append("runtime_not_built while --require-portable-routing is set")

    payload = {
        "exec_bin": str(exec_bin),
        "program": str(program),
        "runs": max(1, args.runs),
        "warmup": max(0, args.warmup),
        "max_perf": bool(args.max_perf),
        "requested_backends": requested_backends,
        "requested_gpu_backends": requested_gpu_backends,
        "skipped": True,
        "skip_reason": "runtime_not_built",
        "probe_output": runtime_probe.stdout,
        "strict": {
            "require_portable_routing": bool(args.require_portable_routing),
            "require_gpu_coverage": bool(args.require_gpu_coverage),
            "errors": skip_strict_errors,
            "passed": len(skip_strict_errors) == 0,
        },
        "results": [],
    }
    out_json.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    with out_csv.open("w", encoding="utf-8", newline="") as handle:
      writer = csv.writer(handle)
      writer.writerow(["skipped", "skip_reason"])
      writer.writerow([1, "runtime_not_built"])
    print("gpu runtime perf: skipped (runtime_not_built)")
    print(f"gpu runtime perf json: {out_json}")
    print(f"gpu runtime perf csv: {out_csv}")
    if skip_strict_errors:
      print("gpu runtime perf strict FAIL")
      for item in skip_strict_errors:
        print(f"  - {item}")
      return 1
    return 0
  if not runtime_ready:
    raise SystemExit(runtime_probe.stdout.strip() or "k runtime probe failed")

  workload_matrix, workload_repeats = estimate_workload(
      DEFAULT_MATRIX_SIZE,
      DEFAULT_REPEATS,
      args.workload_scale,
      args.workload_matrix_size or None,
      args.workload_repeats or None,
      args.workload_max_matrix_size,
  )
  workload_generated = (
      workload_matrix != DEFAULT_MATRIX_SIZE
      or workload_repeats != DEFAULT_REPEATS
      or args.workload_scale != 1.0
  )
  generated_program: Path | None = None
  program, generated = build_workload_program(program, workload_matrix, workload_repeats, workload_generated)
  if generated:
    generated_program = program
  workload_ops = 2.0 * (workload_matrix**3) * workload_repeats

  rows: list[dict[str, object]] = []
  try:
    for backend_input, backend in selected_pairs:
      hw_threads = max(1, os.cpu_count() or 1)
      throughput_threads = hw_threads if args.max_perf else 1
      base_env = dict(os.environ)
      base_env["SPARK_MATMUL_BACKEND"] = backend
      base_env["SPARK_MATMUL_AUTO_LEARN"] = "1" if args.matmul_auto_learn else "0"

      command = [str(exec_bin), "run", "--interpret", str(program)]

      profile_candidates = build_backend_candidate_profiles(backend, throughput_threads)
      profile_candidates = prioritize_candidates_for_backend(backend, profile_candidates)
      profile_name = profile_candidates[0][0]
      profile_overlay = profile_candidates[0][1]
      if not args.disable_backend_profile_autotune:
        profile_name, profile_overlay = pick_best_profile(
            command=command,
            cwd=REPO_ROOT,
            base_env=base_env,
            candidates=profile_candidates,
            probe_runs=max(1, args.backend_profile_probe_runs),
            diff_tolerance=args.program_diff_tolerance,
        )
      env = dict(base_env)
      env.update(profile_overlay)
      selected_threads = int(env.get("SPARK_MATMUL_OWN_THREADS", str(throughput_threads)))

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
      parsed: dict[str, float] = {}
      parse_error = ""
      if status_ok:
        try:
          parsed = parse_matmul_schedule_gpu_output(first_output)
          parse_ok = True
        except Exception as exc:  # noqa: BLE001
          parse_error = str(exc)

      median_sec = statistics.median(times) if times else 0.0
      mean_sec = statistics.fmean(times) if times else 0.0
      median_gflops = compute_gflops(workload_matrix, workload_repeats, median_sec)
      mean_gflops = compute_gflops(workload_matrix, workload_repeats, mean_sec)
      effective_backend = ""
      if parse_ok:
        effective_backend = "blas" if int(parsed.get("backend_id", 0.0)) == 1 else "own"
      program_pass = False
      if parse_ok:
        if bool(int(parsed.get("pass", 0.0)) == 1):
          program_pass = True
        elif abs(float(parsed.get("diff", 0.0))) <= args.program_diff_tolerance:
          program_pass = True

      operation_count = 2.0 * (workload_matrix**3) * workload_repeats
      row = {
          "requested_backend_input": backend_input,
          "requested_backend": backend,
          "effective_backend": effective_backend,
          "status_ok": status_ok,
          "parse_ok": parse_ok,
          "parse_error": parse_error,
          "program_pass": program_pass,
          "median_time_sec": median_sec,
          "mean_time_sec": mean_sec,
          "median_gflops": median_gflops,
          "mean_gflops": mean_gflops,
          "operation_count": operation_count,
          "thread_count": selected_threads,
          "selected_profile": profile_name,
          "matmul_auto_learn": bool(args.matmul_auto_learn),
          "matrix_size": workload_matrix,
          "repeats": workload_repeats,
          "total": parsed.get("total", 0.0),
          "expected": parsed.get("expected", 0.0),
          "diff": parsed.get("diff", 0.0),
          "backend_id": parsed.get("backend_id", -1.0),
          "own_calls": parsed.get("own_calls", 0.0),
          "blas_calls": parsed.get("blas_calls", 0.0),
          "tile_m": parsed.get("tile_m", 0.0),
          "tile_n": parsed.get("tile_n", 0.0),
          "tile_k": parsed.get("tile_k", 0.0),
      }
      row["portable_route_ok"] = (
          bool(row["status_ok"])
          and bool(row["parse_ok"])
          and bool(row["program_pass"])
          and str(row["effective_backend"]) in {"own", "blas"}
      )
      if (
          args.min_gflops > 0.0
          and str(backend) in known_gpu
          and bool(row["portable_route_ok"])
          and median_gflops < args.min_gflops
      ):
        strict_errors.append(
          f"backend {backend} median gflops below minimum: {median_gflops:.6f} < {args.min_gflops:.6f}"
        )
      rows.append(row)
      print(
          f"[gpu-runtime] requested={backend:<14} effective={effective_backend or 'n/a':<4} "
          f"median={median_sec:.6f}s pass={row['program_pass']} "
          f"median_gflops={median_gflops:.6f} threads={selected_threads} profile={profile_name}"
      )
  finally:
    if generated_program is not None:
      generated_program.unlink(missing_ok=True)

  route_failures = [
      str(row["requested_backend"])
      for row in rows
      if str(row["requested_backend"]) in known_gpu and not bool(row.get("portable_route_ok", False))
  ]
  if args.require_portable_routing and route_failures:
    strict_errors.append(f"portable routing failed: {', '.join(route_failures)}")

  out_json = REPO_ROOT / args.json_out
  out_csv = REPO_ROOT / args.csv_out
  out_json.parent.mkdir(parents=True, exist_ok=True)
  out_csv.parent.mkdir(parents=True, exist_ok=True)

  payload = {
      "exec_bin": str(exec_bin),
      "program": str(program),
      "runs": max(1, args.runs),
      "warmup": max(0, args.warmup),
      "max_perf": bool(args.max_perf),
      "workload": {
          "matrix_size": workload_matrix,
          "repeats": workload_repeats,
          "scale": max(1.0, float(args.workload_scale)),
          "operation_count": workload_ops,
      },
      "requested_backends": requested_backends,
      "requested_gpu_backends": requested_gpu_backends,
      "summary": {
          "total_rows": len(rows),
          "status_ok": sum(1 for row in rows if bool(row.get("status_ok", False))),
          "parse_ok": sum(1 for row in rows if bool(row.get("parse_ok", False))),
          "program_pass": sum(1 for row in rows if bool(row.get("program_pass", False))),
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
            "requested_backend_input",
            "requested_backend",
            "effective_backend",
            "matrix_size",
            "repeats",
            "operation_count",
            "thread_count",
            "selected_profile",
            "status_ok",
            "parse_ok",
            "program_pass",
            "portable_route_ok",
            "median_time_sec",
            "mean_time_sec",
            "median_gflops",
            "mean_gflops",
            "diff",
            "backend_id",
            "own_calls",
            "blas_calls",
            "tile_m",
            "tile_n",
            "tile_k",
        ]
    )
    for row in rows:
      writer.writerow(
          [
              row["requested_backend_input"],
              row["requested_backend"],
              row["effective_backend"],
              row["matrix_size"],
              row["repeats"],
              row["operation_count"],
              row["thread_count"],
              row["selected_profile"],
              int(bool(row["status_ok"])),
              int(bool(row["parse_ok"])),
              int(bool(row["program_pass"])),
              int(bool(row["portable_route_ok"])),
              row["median_time_sec"],
              row["mean_time_sec"],
              row["median_gflops"],
              row["mean_gflops"],
              row["diff"],
              row["backend_id"],
              row["own_calls"],
              row["blas_calls"],
              row["tile_m"],
              row["tile_n"],
              row["tile_k"],
          ]
      )

  print(f"gpu runtime perf json: {out_json}")
  print(f"gpu runtime perf csv: {out_csv}")
  if strict_errors:
    print("gpu runtime perf strict FAIL")
    for item in strict_errors:
      print(f"  - {item}")
    return 1
  print("gpu runtime perf strict PASS")
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
