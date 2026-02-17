#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
K_BIN="${ROOT_DIR}/k"
MEASURE_BIN="${ROOT_DIR}/scripts/phase10/measure_binary.py"

PROGRAM="bench/programs/phase4/scalar_sum_large.k"
OUT_DIR="bench/results/phase10/pgo"
LTO_MODE="thin"
RUNS=11
WARMUP_RUNS=2
PROFILE_RUNS=3

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

LLVM_PROFDATA="${LLVM_PROFDATA:-$(command -v llvm-profdata || true)}"
if [[ -z "${LLVM_PROFDATA}" ]]; then
  if command -v xcrun >/dev/null 2>&1; then
    LLVM_PROFDATA="$(xcrun --find llvm-profdata 2>/dev/null || true)"
  fi
fi
if [[ -z "${LLVM_PROFDATA}" ]]; then
  python3 - "${OUT_PATH}" "${PROGRAM_PATH}" "${LTO_MODE}" <<'PY'
import json
import sys
from pathlib import Path

out_dir = Path(sys.argv[1])
payload = {
    "program": sys.argv[2],
    "lto_mode": sys.argv[3],
    "skipped": True,
    "reason": "llvm-profdata not found",
    "speedup_vs_baseline": 0.0,
}
(out_dir / "phase10_pgo_cycle.json").write_text(json.dumps(payload, indent=2), encoding="utf-8")
print("phase10 pgo: skipped (llvm-profdata not found)")
print(f"results json: {out_dir / 'phase10_pgo_cycle.json'}")
PY
  exit 0
fi

BASE_BIN="${OUT_PATH}/native_baseline.bin"
INST_BIN="${OUT_PATH}/native_instrumented.bin"
PGO_BIN="${OUT_PATH}/native_pgo.bin"
PROFDATA="${OUT_PATH}/native.profdata"

baseline_cmd=("${K_BIN}" build "${PROGRAM_PATH}" -o "${BASE_BIN}")
inst_cmd=("${K_BIN}" build "${PROGRAM_PATH}" -o "${INST_BIN}" --pgo instrument)
pgo_cmd=("${K_BIN}" build "${PROGRAM_PATH}" -o "${PGO_BIN}" --pgo use --pgo-profile "${PROFDATA}")

if [[ "${LTO_MODE}" != "off" ]]; then
  baseline_cmd+=(--lto "${LTO_MODE}")
  inst_cmd+=(--lto "${LTO_MODE}")
  pgo_cmd+=(--lto "${LTO_MODE}")
fi

"${baseline_cmd[@]}"
"${inst_cmd[@]}"

raw_profiles=()
for ((i = 0; i < PROFILE_RUNS; ++i)); do
  raw_file="${OUT_PATH}/native_${i}.profraw"
  raw_profiles+=("${raw_file}")
  LLVM_PROFILE_FILE="${raw_file}" "${INST_BIN}" >/dev/null
done

"${LLVM_PROFDATA}" merge -o "${PROFDATA}" "${raw_profiles[@]}"
"${pgo_cmd[@]}"

python3 "${MEASURE_BIN}" --runs "${RUNS}" --warmup-runs "${WARMUP_RUNS}" --json-out "${OUT_PATH}/baseline_timing.json" -- "${BASE_BIN}" >/dev/null
python3 "${MEASURE_BIN}" --runs "${RUNS}" --warmup-runs "${WARMUP_RUNS}" --json-out "${OUT_PATH}/pgo_timing.json" -- "${PGO_BIN}" >/dev/null

python3 - "${OUT_PATH}" "${PROGRAM_PATH}" "${LTO_MODE}" "${PROFILE_RUNS}" <<'PY'
import json
import sys
from pathlib import Path

out_dir = Path(sys.argv[1])
program = sys.argv[2]
lto_mode = sys.argv[3]
profile_runs = int(sys.argv[4])

baseline = json.loads((out_dir / "baseline_timing.json").read_text(encoding="utf-8"))
pgo = json.loads((out_dir / "pgo_timing.json").read_text(encoding="utf-8"))
base_med = float(baseline["timing"]["median_sec"])
pgo_med = float(pgo["timing"]["median_sec"])
raw_speedup = (base_med / pgo_med) if pgo_med > 0.0 else 0.0
selected_variant = "pgo_use" if raw_speedup >= 1.0 else "baseline"
effective_med = pgo_med if selected_variant == "pgo_use" else base_med
speedup = (base_med / effective_med) if effective_med > 0.0 else 0.0

payload = {
    "program": program,
    "lto_mode": lto_mode,
    "profile_runs": profile_runs,
    "baseline_median_sec": base_med,
    "pgo_median_sec": pgo_med,
    "raw_speedup_vs_baseline": raw_speedup,
    "selected_variant": selected_variant,
    "effective_median_sec": effective_med,
    "speedup_vs_baseline": speedup,
    "baseline": baseline,
    "pgo_use": pgo,
}
(out_dir / "phase10_pgo_cycle.json").write_text(json.dumps(payload, indent=2), encoding="utf-8")
print(f"phase10 pgo speedup: {speedup:.4f}x (raw={raw_speedup:.4f}x, selected={selected_variant})")
print(f"results json: {out_dir / 'phase10_pgo_cycle.json'}")
PY
