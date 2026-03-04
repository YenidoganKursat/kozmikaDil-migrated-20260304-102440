#include "spark/core/driver/platform_support_driver.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>

namespace spark {
bool gpu_backend_supports_operation(const std::string& backend, const std::string& operation);
}

namespace spark::core::driver {

namespace {

std::string to_lower_ascii(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

std::string canonical_backend_name(std::string backend) {
  backend = to_lower_ascii(std::move(backend));
  if (backend == "nvidia") {
    return "cuda";
  }
  if (backend == "rocm" || backend == "hip") {
    return "rocm_hip";
  }
  if (backend == "sycl" || backend == "oneapi" || backend == "intel") {
    return "oneapi_sycl";
  }
  if (backend == "cl") {
    return "opencl";
  }
  if (backend == "vk" || backend == "vulkan") {
    return "vulkan_compute";
  }
  return backend;
}

std::vector<std::string> normalize_unique_tokens(const std::vector<std::string>& values,
                                                 bool canonical_backend) {
  std::vector<std::string> out;
  out.reserve(values.size());
  std::unordered_set<std::string> seen;
  seen.reserve(values.size());
  for (const auto& raw : values) {
    std::string token = to_lower_ascii(raw);
    if (canonical_backend) {
      token = canonical_backend_name(std::move(token));
    }
    if (token.empty()) {
      continue;
    }
    if (seen.insert(token).second) {
      out.push_back(std::move(token));
    }
  }
  return out;
}

std::vector<std::string> default_gpu_backends() {
  return {"cuda", "rocm_hip", "oneapi_sycl", "opencl", "vulkan_compute", "metal", "webgpu"};
}

std::vector<std::string> default_gpu_ops() {
  return {"pipeline", "map",         "filter",      "reduce",      "scan",
          "distinct", "contains",    "union",       "intersection","difference",
          "group_by", "sort",        "matmul",      "matmul_f32",  "matmul_f64"};
}

}  // namespace

dto::PlatformSupportSnapshot PlatformSupportDriver::collect_snapshot(
    const std::vector<std::string>& requested_backends,
    const std::vector<std::string>& requested_operations) const {
  dto::PlatformSupportSnapshot out;
  out.cpu_targets = known_cpu_targets();
  out.micro_targets = known_micro_targets();

  const auto backends = normalize_unique_tokens(
      requested_backends.empty() ? default_gpu_backends() : requested_backends, true);
  const auto operations = normalize_unique_tokens(
      requested_operations.empty() ? default_gpu_ops() : requested_operations, false);

  out.gpu_operation_caps.reserve(backends.size() * operations.size());
  for (const auto& backend : backends) {
    for (const auto& operation : operations) {
      dto::BackendOperationCapability cap;
      cap.backend.value = backend;
      cap.operation.value = operation;
      cap.supported = gpu_supports_operation(backend, operation);
      cap.reason.value = cap.supported ? "gpu_supported" : "cpu_fallback_required";
      out.gpu_operation_caps.push_back(std::move(cap));
    }
  }
  return out;
}

std::vector<dto::TargetCapability> PlatformSupportDriver::known_cpu_targets() const {
  static const std::vector<std::string> targets = {
      "x86_64-linux-gnu",          "aarch64-linux-gnu",        "i686-linux-gnu",
      "riscv64-linux-gnu",         "armv7-linux-gnueabihf",    "riscv32-linux-gnu",
      "ppc64le-linux-gnu",         "s390x-linux-gnu",          "loongarch64-linux-gnu",
      "mips64el-linux-gnuabi64",   "mipsel-linux-gnu",         "x86_64-apple-darwin",
      "arm64-apple-darwin",        "x86_64-w64-mingw32",       "aarch64-w64-mingw32",
      "x86_64-unknown-freebsd",    "aarch64-unknown-freebsd",  "aarch64-linux-android",
      "x86_64-linux-android",      "armv7a-linux-androideabi", "wasm32-wasi",
      "wasm32-unknown-emscripten",
  };

  std::vector<dto::TargetCapability> out;
  out.reserve(targets.size());
  for (const auto& triple : targets) {
    dto::TargetCapability capability;
    capability.target.value = triple;
    capability.build_supported = true;
    capability.interpret_supported = true;
    out.push_back(std::move(capability));
  }
  return out;
}

std::vector<dto::TargetCapability> PlatformSupportDriver::known_micro_targets() const {
  static const std::vector<std::string> targets = {
      "arm-none-eabi", "riscv64-unknown-elf", "avr-none-elf", "xtensa-esp32-elf"};

  std::vector<dto::TargetCapability> out;
  out.reserve(targets.size());
  for (const auto& triple : targets) {
    dto::TargetCapability capability;
    capability.target.value = triple;
    capability.build_supported = true;
    capability.interpret_supported = true;
    out.push_back(std::move(capability));
  }
  return out;
}

bool PlatformSupportDriver::gpu_supports_operation(const std::string& backend,
                                                   const std::string& operation) const {
  return spark::gpu_backend_supports_operation(canonical_backend_name(backend), to_lower_ascii(operation));
}

}  // namespace spark::core::driver
