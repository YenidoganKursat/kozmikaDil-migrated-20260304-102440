#!/usr/bin/env bash
set -euo pipefail

if ! command -v cmake >/dev/null 2>&1; then
  echo "cmake not found. Run scripts/ubuntu_toolchain.sh first (or install manually)." >&2
  exit 1
fi

cmake -S . -B .build_phase1 -G Ninja -DSPARK_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build .build_phase1
bash bench/scripts/run_phase1_baselines.sh
