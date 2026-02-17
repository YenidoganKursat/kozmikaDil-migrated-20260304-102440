#!/usr/bin/env bash
set -euo pipefail

echo "Installing toolchain dependencies for Phase 1"

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "This script targets Ubuntu/Debian-like Linux only." >&2
  exit 1
fi

if ! command -v apt-get >/dev/null 2>&1; then
  echo "apt-get not found. Install dependencies manually." >&2
  exit 1
fi

sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build clang git curl python3

curl -fsSL https://apt.llvm.org/llvm.sh | sudo bash -s -- 18
sudo apt-get install -y llvm-18-dev clang-18 lld-18 libmlir-18-dev

cat <<'EOF2'
Toolchain bootstrap complete.
Recommended Phase 1 build commands:
  cmake -S . -B .build_phase1 -G Ninja -DSPARK_BUILD_BENCHMARKS=ON
  cmake --build .build_phase1
  bash bench/scripts/run_phase1_baselines.sh
EOF2
