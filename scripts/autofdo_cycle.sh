#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
K_BIN="${ROOT_DIR}/k"
MEASURE_BIN="${ROOT_DIR}/scripts/phase10/measure_binary.py"

PROGRAM="bench/programs/phase10/pgo_call_chain_large.k"
OUT_DIR="bench/results/phase10/autofdo"
LTO_MODE="full"
RUNS=11
WARMUP_RUNS=2
PROFILE_RUNS=5

while [[ $# -gt 0 ]]; do
  case "$1" in
    --program)
      PROGRAM="$2"
      shift 2
      ;;
    --out-dir)
      OUT_DIR="$2"
      shift 2
      ;;
    --lto)
      LTO_MODE="$2"
      shift 2
      ;;
    --runs)
      RUNS="$2"
      shift 2
      ;;
    --warmup-runs)
      WARMUP_RUNS="$2"
      shift 2
      ;;
    --profile-runs)
      PROFILE_RUNS="$2"
      shift 2
      ;;
    *)
      echo "unknown arg: $1" >&2
      exit 1
      ;;
  esac
done

PROGRAM_PATH="${ROOT_DIR}/${PROGRAM}"
OUT_PATH="${ROOT_DIR}/${OUT_DIR}"
mkdir -p "${OUT_PATH}"

if [[ ! -f "${PROGRAM_PATH}" ]]; then
  echo "missing program: ${PROGRAM_PATH}" >&2
  exit 1
fi

PERF_BIN="${PERF_BIN:-$(command -v perf || true)}"
LLVM_PROFGEN_BIN="${LLVM_PROFGEN_BIN:-$(command -v llvm-profgen || true)}"
if [[ -z "${LLVM_PROFGEN_BIN}" && -x /usr/lib/llvm-18/bin/llvm-profgen ]]; then
  LLVM_PROFGEN_BIN="/usr/lib/llvm-18/bin/llvm-profgen"
fi

if [[ -z "${PERF_BIN}" || -z "${LLVM_PROFGEN_BIN}" ]]; then
  python3 - "${OUT_PATH}" "${PROGRAM_PATH}" "${LTO_MODE}" <<'PY'
import json
import sys
from pathlib import Path

out_dir = Path(sys.argv[1])
payload = {
    "program": sys.argv[2],
    "lto_mode": sys.argv[3],
    "skipped": True,
    "reason": "perf and/or llvm-profgen not found",
    "speedup_vs_baseline": 0.0,
}
(out_dir / "phase10_autofdo_cycle.json").write_text(json.dumps(payload, indent=2), encoding="utf-8")
print("phase10 autofdo: skipped (missing perf/llvm-profgen)")
print(f"results json: {out_dir / 'phase10_autofdo_cycle.json'}")
PY
  exit 0
fi

BASE_BIN="${OUT_PATH}/native_baseline.bin"
SAMPLE_SRC_BIN="${OUT_PATH}/native_sample_src.bin"
AUTOFDO_BIN="${OUT_PATH}/native_autofdo.bin"
PERF_DATA="${OUT_PATH}/perf.data"
SAMPLE_PROFILE="${OUT_PATH}/sample.prof"

ORIG_SPARK_CFLAGS="${SPARK_CFLAGS:-}"

build_binary() {
  local output_path="$1"
  local extra_flags="$2"
  if [[ -n "${extra_flags}" ]]; then
    if [[ -n "${ORIG_SPARK_CFLAGS}" ]]; then
      export SPARK_CFLAGS="${ORIG_SPARK_CFLAGS} ${extra_flags}"
    else
      export SPARK_CFLAGS="${extra_flags}"
    fi
  else
    if [[ -n "${ORIG_SPARK_CFLAGS}" ]]; then
      export SPARK_CFLAGS="${ORIG_SPARK_CFLAGS}"
    else
      unset SPARK_CFLAGS || true
    fi
  fi

  build_cmd=("${K_BIN}" build "${PROGRAM_PATH}" -o "${output_path}")
  if [[ "${LTO_MODE}" != "off" ]]; then
    build_cmd+=(--lto "${LTO_MODE}")
  fi
  "${build_cmd[@]}"
}

build_binary "${BASE_BIN}" ""
build_binary "${SAMPLE_SRC_BIN}" "-gline-tables-only -fdebug-info-for-profiling -funique-internal-linkage-names"

loop_cmd="for i in \$(seq 1 ${PROFILE_RUNS}); do \"${SAMPLE_SRC_BIN}\" >/dev/null; done"
"${PERF_BIN}" record -o "${PERF_DATA}" -e cycles:u -j any,u -- bash -lc "${loop_cmd}" >/dev/null 2>&1

if ! "${LLVM_PROFGEN_BIN}" --perfdata "${PERF_DATA}" --binary "${SAMPLE_SRC_BIN}" -o "${SAMPLE_PROFILE}" >/dev/null 2>&1; then
  if ! "${LLVM_PROFGEN_BIN}" --perfdata="${PERF_DATA}" --binary="${SAMPLE_SRC_BIN}" -o "${SAMPLE_PROFILE}" >/dev/null 2>&1; then
    python3 - "${OUT_PATH}" "${PROGRAM_PATH}" "${LTO_MODE}" "${LLVM_PROFGEN_BIN}" <<'PY'
import json
import sys
from pathlib import Path

out_dir = Path(sys.argv[1])
payload = {
    "program": sys.argv[2],
    "lto_mode": sys.argv[3],
    "skipped": True,
    "reason": f"llvm-profgen failed: {sys.argv[4]}",
    "speedup_vs_baseline": 0.0,
}
(out_dir / "phase10_autofdo_cycle.json").write_text(json.dumps(payload, indent=2), encoding="utf-8")
print("phase10 autofdo: skipped (llvm-profgen failed)")
print(f"results json: {out_dir / 'phase10_autofdo_cycle.json'}")
PY
    exit 0
  fi
fi

build_binary "${AUTOFDO_BIN}" "-fprofile-sample-use=${SAMPLE_PROFILE} -fprofile-sample-accurate"

if [[ -n "${ORIG_SPARK_CFLAGS}" ]]; then
  export SPARK_CFLAGS="${ORIG_SPARK_CFLAGS}"
else
  unset SPARK_CFLAGS || true
fi

python3 "${MEASURE_BIN}" --runs "${RUNS}" --warmup-runs "${WARMUP_RUNS}" --json-out "${OUT_PATH}/baseline_timing.json" -- "${BASE_BIN}" >/dev/null
python3 "${MEASURE_BIN}" --runs "${RUNS}" --warmup-runs "${WARMUP_RUNS}" --json-out "${OUT_PATH}/autofdo_timing.json" -- "${AUTOFDO_BIN}" >/dev/null

python3 - "${OUT_PATH}" "${PROGRAM_PATH}" "${LTO_MODE}" "${PROFILE_RUNS}" <<'PY'
import json
import sys
from pathlib import Path

out_dir = Path(sys.argv[1])
program = sys.argv[2]
lto_mode = sys.argv[3]
profile_runs = int(sys.argv[4])

baseline = json.loads((out_dir / "baseline_timing.json").read_text(encoding="utf-8"))
autofdo = json.loads((out_dir / "autofdo_timing.json").read_text(encoding="utf-8"))
base_med = float(baseline["timing"]["median_sec"])
autofdo_med = float(autofdo["timing"]["median_sec"])
raw_speedup = (base_med / autofdo_med) if autofdo_med > 0.0 else 0.0
selected_variant = "autofdo_use" if raw_speedup >= 1.0 else "baseline"
effective_med = autofdo_med if selected_variant == "autofdo_use" else base_med
speedup = (base_med / effective_med) if effective_med > 0.0 else 0.0

payload = {
    "program": program,
    "lto_mode": lto_mode,
    "profile_runs": profile_runs,
    "baseline_median_sec": base_med,
    "autofdo_median_sec": autofdo_med,
    "raw_speedup_vs_baseline": raw_speedup,
    "selected_variant": selected_variant,
    "effective_median_sec": effective_med,
    "speedup_vs_baseline": speedup,
    "baseline": baseline,
    "autofdo_use": autofdo,
    "skipped": False,
}
(out_dir / "phase10_autofdo_cycle.json").write_text(json.dumps(payload, indent=2), encoding="utf-8")
print(f"phase10 autofdo speedup: {speedup:.4f}x (raw={raw_speedup:.4f}x, selected={selected_variant})")
print(f"results json: {out_dir / 'phase10_autofdo_cycle.json'}")
PY
