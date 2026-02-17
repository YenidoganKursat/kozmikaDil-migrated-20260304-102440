# PHASE 1 — Toolchain (Ubuntu)

## Chosen Toolchain

- `C++20` for compiler implementation.
- `MLIR + LLVM` as the long-term lowering path.
- `CMake + Ninja` for build orchestration.

## Single-command bootstrap for Ubuntu (recommended)

```bash
sudo apt-get update
sudo apt-get install -y curl ca-certificates gnupg
curl -fsSL https://apt.llvm.org/llvm.sh | sudo bash -s -- 18
sudo apt-get install -y build-essential cmake ninja-build clang-18 lld-18 llvm-18-dev libmlir-18-dev

cd .
cmake -S . -B .build_phase1 -G Ninja -DSPARK_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build .build_phase1

bash bench/scripts/run_phase1_baselines.sh
```

## Alternative one-step if LLVM apt packages are unavailable

```bash
cd .
cmake -S . -B .build_phase1 -G Ninja -DSPARK_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build .build_phase1
bash bench/scripts/run_phase1_baselines.sh
```

## Notes

- If `ninja` is unavailable, script falls back to `make` and keeps running.
- `MLIR/LLVM headers and libs` may be optional in Phase 1 because no production compiler pipeline is yet active.
