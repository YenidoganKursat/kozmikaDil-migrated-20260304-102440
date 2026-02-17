#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${SCRIPT_DIR}/run_phase4_benchmarks.py" \
  --phase 5 \
  --auto-native-pgo \
  --auto-native-pgo-runs 2 \
  "$@"
