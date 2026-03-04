#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 "${ROOT_DIR}/scripts/core/platform/runtime/differential_check.py" "$@"
python3 "${ROOT_DIR}/scripts/core/platform/runtime/fuzz_parser.py"
python3 "${ROOT_DIR}/scripts/core/platform/runtime/fuzz_runtime.py"
python3 "${ROOT_DIR}/scripts/core/platform/runtime/run_sanitizers.py"
