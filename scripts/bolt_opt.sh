#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MEASURE_BIN="${ROOT_DIR}/scripts/phase10/measure_binary.py"

BINARY=""
PROFILE_CMD=""
OUT_DIR="bench/results/phase10/bolt"
RUNS=7
WARMUP_RUNS=1
PROFILE_RUNS=5

while [[ $# -gt 0 ]]; do
  case "$1" in
    --binary)
      BINARY="$2"
      shift 2
      ;;
    --profile-cmd)
      PROFILE_CMD="$2"
      shift 2
      ;;
    --out-dir)
      OUT_DIR="$2"
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

if [[ -z "${BINARY}" ]]; then
  echo "--binary is required" >&2
  exit 1
fi

BINARY_PATH="${BINARY}"
if [[ "${BINARY_PATH}" != /* ]]; then
  BINARY_PATH="${ROOT_DIR}/${BINARY_PATH}"
fi
if [[ ! -x "${BINARY_PATH}" ]]; then
  echo "binary not executable: ${BINARY_PATH}" >&2
  exit 1
fi

if [[ -z "${PROFILE_CMD}" ]]; then
  PROFILE_CMD="${BINARY_PATH}"
fi

OUT_PATH="${ROOT_DIR}/${OUT_DIR}"
mkdir -p "${OUT_PATH}"

PERF_BIN="${PERF_BIN:-$(command -v perf || true)}"
PERF2BOLT_BIN="${PERF2BOLT_BIN:-$(command -v perf2bolt || true)}"
LLVM_BOLT_BIN="${LLVM_BOLT_BIN:-$(command -v llvm-bolt || true)}"

if [[ -z "${PERF_BIN}" || -z "${PERF2BOLT_BIN}" || -z "${LLVM_BOLT_BIN}" ]]; then
  python3 - "${OUT_PATH}" "${BINARY_PATH}" "${PROFILE_CMD}" <<'PY'
import json
import sys
from pathlib import Path

out_dir = Path(sys.argv[1])
payload = {
    "binary": sys.argv[2],
    "profile_cmd": sys.argv[3],
    "skipped": True,
    "reason": "perf/perf2bolt/llvm-bolt toolchain not available",
}
(out_dir / "phase10_bolt.json").write_text(json.dumps(payload, indent=2), encoding="utf-8")
print("phase10 bolt: skipped (missing perf/perf2bolt/llvm-bolt)")
print(f"results json: {out_dir / 'phase10_bolt.json'}")
PY
  exit 0
fi

BASE_TIMING_JSON="${OUT_PATH}/baseline_timing.json"
BOLT_TIMING_JSON="${OUT_PATH}/bolt_timing.json"
PERF_DATA="${OUT_PATH}/perf.data"
FDATA="${OUT_PATH}/perf.fdata"
BOLT_BIN="${OUT_PATH}/$(basename "${BINARY_PATH}").bolt"

python3 "${MEASURE_BIN}" --runs "${RUNS}" --warmup-runs "${WARMUP_RUNS}" --json-out "${BASE_TIMING_JSON}" -- "${BINARY_PATH}" >/dev/null

loop_cmd="for i in \$(seq 1 ${PROFILE_RUNS}); do ${PROFILE_CMD}; done"
"${PERF_BIN}" record -o "${PERF_DATA}" -e cycles:u -j any,u -- bash -lc "${loop_cmd}" >/dev/null 2>&1
"${PERF2BOLT_BIN}" "${BINARY_PATH}" -p "${PERF_DATA}" -o "${FDATA}" >/dev/null
"${LLVM_BOLT_BIN}" "${BINARY_PATH}" -o "${BOLT_BIN}" \
  --data "${FDATA}" \
  --reorder-blocks=ext-tsp \
  --reorder-functions=hfsort+ \
  --split-functions \
  --split-all-cold \
  --icf=1 \
  --dyno-stats >/dev/null

python3 "${MEASURE_BIN}" --runs "${RUNS}" --warmup-runs "${WARMUP_RUNS}" --json-out "${BOLT_TIMING_JSON}" -- "${BOLT_BIN}" >/dev/null

python3 - "${OUT_PATH}" "${BINARY_PATH}" "${BOLT_BIN}" "${PROFILE_CMD}" <<'PY'
import json
import sys
from pathlib import Path

out_dir = Path(sys.argv[1])
baseline = json.loads((out_dir / "baseline_timing.json").read_text(encoding="utf-8"))
bolt = json.loads((out_dir / "bolt_timing.json").read_text(encoding="utf-8"))
base_med = float(baseline["timing"]["median_sec"])
bolt_med = float(bolt["timing"]["median_sec"])
raw_speedup = (base_med / bolt_med) if bolt_med > 0.0 else 0.0
selected_variant = "bolt_optimized" if raw_speedup >= 1.0 else "baseline"
effective_med = bolt_med if selected_variant == "bolt_optimized" else base_med
speedup = (base_med / effective_med) if effective_med > 0.0 else 0.0

payload = {
    "binary": sys.argv[2],
    "bolt_binary": sys.argv[3],
    "profile_cmd": sys.argv[4],
    "skipped": False,
    "baseline_median_sec": base_med,
    "bolt_median_sec": bolt_med,
    "raw_speedup_vs_baseline": raw_speedup,
    "selected_variant": selected_variant,
    "effective_median_sec": effective_med,
    "speedup_vs_baseline": speedup,
    "baseline": baseline,
    "bolt": bolt,
}
(out_dir / "phase10_bolt.json").write_text(json.dumps(payload, indent=2), encoding="utf-8")
print(f"phase10 bolt speedup: {speedup:.4f}x (raw={raw_speedup:.4f}x, selected={selected_variant})")
print(f"results json: {out_dir / 'phase10_bolt.json'}")
PY
