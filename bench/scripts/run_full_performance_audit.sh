#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
RESULT_DIR="${ROOT_DIR}/bench/results/full_performance_audit"
mkdir -p "$RESULT_DIR"

log() {
  echo "[full-perf] $*"
}

run_step() {
  local label="$1"
  shift
  log "$label :: start"
  if "$@"; then
    log "$label :: ok"
    return 0
  else
    log "$label :: fail (continuing)"
    return 0
  fi
}

log "Results dir: $RESULT_DIR"

run_step "Phase 10 benchmark matrix" \
  bash "${SCRIPT_DIR}/run_platform_support_benchmarks.sh" \
  > "${RESULT_DIR}/platform_support_benchmarks.log" 2>&1

run_step "Microcontroller readiness (interpret smoke)" \
  python3 "${ROOT_DIR}/.github/scripts/microcontroller_readiness_gate.py" \
    --require-interpret-smoke \
    --json-out "${RESULT_DIR}/platform_support_microcontroller_gate.json" \
  > "${RESULT_DIR}/platform_support_microcontroller_gate.log" 2>&1

run_step "Phase 9 benchmark matrix" \
  bash "${SCRIPT_DIR}/run_async_runtime_benchmarks.sh" \
  > "${RESULT_DIR}/async_runtime_benchmarks.log" 2>&1

run_step "Phase 8 benchmark matrix" \
  bash "${SCRIPT_DIR}/run_matmul_schedule_gpu_benchmarks.sh" \
  > "${RESULT_DIR}/matmul_schedule_gpu_benchmarks.log" 2>&1

run_step "Phase 7 benchmark matrix" \
  bash "${SCRIPT_DIR}/run_stream_pipeline_benchmarks.sh" \
  > "${RESULT_DIR}/stream_pipeline_benchmarks.log" 2>&1

run_step "Phase 4 benchmark matrix (stable + fast stability)" \
  bash "${SCRIPT_DIR}/run_scalar_runtime_benchmarks.sh" \
    --phase 4 --runs 3 --warmup-runs 1 --repeat 4 --stability-profile fast \
    > "${RESULT_DIR}/scalar_runtime_benchmarks.log" 2>&1

run_step "GPU smoke matrix (all backends)" \
  python3 "${ROOT_DIR}/scripts/core/pipeline/platform_support/gpu_smoke_matrix.py" \
    --backends all \
    --include-planning \
    --json-out "${RESULT_DIR}/platform_support_gpu_smoke_all.json" \
    > "${RESULT_DIR}/platform_support_gpu_smoke_all.log" 2>&1

run_step "GPU probe perf matrix (all backends)" \
  python3 "${ROOT_DIR}/scripts/core/pipeline/platform_support/gpu_backend_perf.py" \
    --backends all \
    --include-planning \
    --runs 7 \
    --warmup 2 \
    --json-out "${RESULT_DIR}/platform_support_gpu_backend_perf_all.json" \
    > "${RESULT_DIR}/platform_support_gpu_backend_perf_all.log" 2>&1

run_step "GPU runtime perf matrix (matmul core f64)" \
  python3 "${ROOT_DIR}/scripts/core/pipeline/platform_support/gpu_backend_runtime_perf.py" \
    --program "${ROOT_DIR}/bench/programs/matmul_schedule_gpu/matmul_core_f64.k" \
    --runs 5 \
    --warmup 1 \
    --max-perf \
    --allow-missing-runtime \
    --json-out "${RESULT_DIR}/platform_support_gpu_runtime_perf_all.json" \
    --csv-out "${RESULT_DIR}/platform_support_gpu_runtime_perf_all.csv" \
    > "${RESULT_DIR}/platform_support_gpu_runtime_perf_all.log" 2>&1

run_step "Multiarch host smoke (market preset)" \
  python3 "${ROOT_DIR}/scripts/core/pipeline/platform_support/multiarch_build.py" \
    --preset market \
    --include-experimental \
    --include-embedded \
    --run-host-smoke \
    --json-out "${RESULT_DIR}/platform_support_market_host_smoke.json" \
    > "${RESULT_DIR}/platform_support_market_host_smoke.log" 2>&1

run_step "Platform matrix report (market + gpu experimental/planning)" \
  python3 "${ROOT_DIR}/scripts/core/pipeline/platform_support/platform_matrix.py" \
    --preset market \
    --include-experimental \
    --include-embedded \
    --include-gpu-experimental \
    --include-gpu-planning \
    --json-out "${RESULT_DIR}/platform_support_platform_matrix_market.json" \
    > "${RESULT_DIR}/platform_support_platform_matrix_market.log" 2>&1

log "full_performance_audit completed"
