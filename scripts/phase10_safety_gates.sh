#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 "${ROOT_DIR}/scripts/phase10/differential_check.py" "$@"
python3 "${ROOT_DIR}/scripts/phase10/fuzz_parser.py"
python3 "${ROOT_DIR}/scripts/phase10/fuzz_runtime.py"
python3 "${ROOT_DIR}/scripts/phase10/run_sanitizers.py"
