# Platform Support Matrix

This document defines portability tiers for Kozmika runtime/build targets.

## Tier Model

- `stable`:
  - userland OS targets expected to work with standard toolchains and libc.
  - default CI/phase10 focus.
- `experimental`:
  - targets supported by architecture, but success depends on cross-toolchain/sysroot availability.
- `embedded`:
  - bare-metal/MCU targets (no standard userland runtime).
  - current mode is `aot`; native cross-build is required when toolchains are present.
  - portability workflows execute strict native readiness on Linux and keep interpret smoke coverage enabled.

## Current Catalog

- stable:
  - `x86_64-linux-gnu`
  - `aarch64-linux-gnu`
  - `x86_64-apple-darwin`
  - `arm64-apple-darwin`
- experimental:
  - `i686-linux-gnu`
  - `riscv64-linux-gnu`
  - `armv7-linux-gnueabihf`
  - `riscv32-linux-gnu`
  - `ppc64le-linux-gnu`
  - `s390x-linux-gnu`
  - `mips64el-linux-gnuabi64`
  - `mipsel-linux-gnu`
  - `x86_64-w64-mingw32`
  - `aarch64-w64-mingw32`
  - `x86_64-unknown-freebsd`
  - `aarch64-unknown-freebsd`
  - `aarch64-linux-android`
  - `x86_64-linux-android`
  - `armv7a-linux-androideabi`
  - `wasm32-wasi`
  - `wasm32-unknown-emscripten`
  - `loongarch64-linux-gnu`
- embedded:
  - `arm-none-eabi`
  - `riscv64-unknown-elf`
  - `avr-none-elf`
  - `xtensa-esp32-elf`

## GPU Backend Catalog

- stable:
  - `metal` (macOS)
- experimental:
  - `cuda`
  - `rocm_hip`
  - `oneapi_sycl`
  - `opencl`
  - `vulkan_compute`
- planning:
  - `webgpu`
- `webgpu` is required on policy for all host classes for capability continuity; runtime probe remains planning-only.

## Commands

- list presets:
  - `python3 scripts/core/pipeline/phase10/multiarch_build.py --list-presets`
- core preset (default):
  - `python3 scripts/core/pipeline/phase10/multiarch_build.py --preset core --run-host-smoke`
- market-wide report:
  - `python3 scripts/core/pipeline/phase10/multiarch_build.py --preset market --include-experimental --include-embedded --run-host-smoke`
- unified CPU+GPU matrix:
  - `python3 scripts/core/pipeline/phase10/platform_matrix.py --preset market --include-experimental --include-embedded --include-gpu-experimental --include-gpu-planning`
  - `python3 .github/scripts/microcontroller_readiness_gate.py --require-interpret-smoke --json-out bench/results/phase10_microcontroller_gate.json`
  - strict microcontroller native build gate:
    `python3 .github/scripts/microcontroller_readiness_gate.py --require-native-mcu --require-interpret-smoke --json-out bench/results/phase10_microcontroller_gate_strict.json`
- GPU backend infra bootstrap + verification (backend başına):
  - `python3 scripts/core/pipeline/phase10/gpu_backend_infra.py --backends cuda --provision --strict`
  - `python3 scripts/core/pipeline/phase10/gpu_backend_infra.py --backends rocm_hip --provision --strict`
  - `python3 scripts/core/pipeline/phase10/gpu_backend_infra.py --backends oneapi_sycl --provision --strict`
  - `python3 scripts/core/pipeline/phase10/gpu_backend_infra.py --backends opencl --provision --strict`
  - `python3 scripts/core/pipeline/phase10/gpu_backend_infra.py --backends vulkan_compute --provision --strict`
  - `python3 scripts/core/pipeline/phase10/gpu_backend_infra.py --backends metal`
- Unified portability policy gate:
  - `python3 .github/scripts/platform_support_gate.py --policy docs/platform_support_policy.json --platform-matrix bench/results/phase10_platform_matrix_Linux.json --cross-perf bench/results/phase10_cross_platform_perf_Linux.json --microcontroller bench/results/phase10_microcontroller_gate_Linux.json --gpu-smoke bench/results/phase10_gpu_smoke_Linux.json --report-out bench/results/phase10_platform_support_gate_Linux.json`
- GPU smoke matrix (all backends, report mode):
  - `python3 scripts/core/pipeline/phase10/gpu_smoke_matrix.py --backends all --include-planning`
- GPU probe/detect perf matrix:
  - `python3 scripts/core/pipeline/phase10/gpu_backend_perf.py --backends all --include-planning --runs 7 --warmup 2`
- GPU runtime perf matrix (backend request -> effective backend + latency):
  - `python3 scripts/core/pipeline/phase10/gpu_backend_runtime_perf.py --program bench/programs/phase8/matmul_core_f64.k --runs 5 --warmup 1 --max-perf`
  - strict portability gate (all GPU requests must route and pass):  
    `python3 scripts/core/pipeline/phase10/gpu_backend_runtime_perf.py --program bench/programs/phase8/matmul_core_f64.k --backends all --runs 3 --warmup 1 --max-perf --allow-missing-runtime --require-portable-routing --require-gpu-coverage`
- Strict GPU smoke on dedicated host (example: CUDA):
  - `python3 scripts/core/pipeline/phase10/gpu_smoke_matrix.py --backends cuda --fail-on-unavailable cuda`

## CI Gates

- `platform_readiness_gate.py` runs on CI and fails if platform catalog coverage regresses.
- CI also runs `platform_matrix.py` on `ubuntu`, `macos`, and `windows` hosts to keep portability tooling cross-host stable.
- CI also runs `gpu_smoke_matrix.py` on `ubuntu`, `macos`, and `windows` hosts and enforces strict host-required GPU availability in dedicated policy checks.
- CI also runs `gpu_backend_perf.py` on `ubuntu`, `macos`, and `windows` hosts and uploads per-host perf JSON reports.
- CI runs `gpu_backend_runtime_perf.py` with strict portability checks (`--require-portable-routing --require-gpu-coverage`) and uploads per-host runtime perf reports.
- CI also runs `platform_support_gate.py` against collected artifacts on `ubuntu`, `macos`, and `windows` hosts.
- Reports are uploaded as workflow artifacts (`ci-platform-matrix-*`).
- `.github/workflows/gpu-backend-cicd.yml` is added for strict backend infra+smoke validation across OS/runner families (Linux CUDA/ROCm/oneAPI/OpenCL/Vulkan, Windows oneAPI/OpenCL/Vulkan, macOS Metal). This workflow is intended for CI/CD-style validation and writes per-backend artifacts.
- Manual GPU strict workflow:
  - `.github/workflows/gpu-backend-smoke.yml`
  - `workflow_dispatch` ile `cuda`, `rocm_hip`, `oneapi_sycl`, `opencl`, `vulkan_compute`, `metal` backendleri strict smoke edilebilir.
  - Required runner labels per backend:
    - `cuda`: `[self-hosted, linux, x64, gpu, cuda]`
    - `cuda` (Windows): `[self-hosted, windows, x64, gpu, cuda]`
    - `rocm_hip`: `[self-hosted, linux, x64, gpu, rocm]`
    - `oneapi_sycl`: `[self-hosted, linux, x64, gpu, oneapi]`
    - `oneapi_sycl` (Windows): `[self-hosted, windows, x64, gpu, oneapi]`
    - `opencl`: `[self-hosted, linux, x64, gpu, opencl]`
    - `opencl` (Windows): `[self-hosted, windows, x64, gpu, opencl]`
    - `vulkan_compute`: `[self-hosted, linux, x64, gpu, vulkan]`
    - `vulkan_compute` (Windows): `[self-hosted, windows, x64, gpu, vulkan]`
    - `metal`: macOS hosted (`macos-latest`) or self-hosted macGPU label equivalent

## Practical Notes

- Raspberry Pi:
  - 64-bit OS (`aarch64`) is in stable tier.
  - 32-bit OS (`armv7`) is experimental tier.
- Additional CPU families are tracked in `scripts/core/pipeline/phase10/target_catalog.py` as explicit catalog entries:
  - `ppc64le`, `s390x`, `loongarch64`, `mips`, `mips64`, `riscv32`, `x86`
  - on unsupported/mixed CPUs runtime returns scalar-safe fallbacks and still prints a deterministic feature/dispatch report via `k --print-cpu-features`.
- Arduino/MCU class:
  - listed under embedded tier; requires dedicated runtime port and allocator/thread/time abstractions before full AOT support.
