#include "spark/core/manager/platform_support_manager.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>

namespace spark::core::manager {

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

std::string canonical_operation_name(std::string op) {
  op = to_lower_ascii(std::move(op));
  if (op == "maps" || op == "mapper") {
    return "map";
  }
  if (op == "filters") {
    return "filter";
  }
  if (op == "reduce_sum" || op == "sum") {
    return "reduce";
  }
  if (op == "scan_sum" || op == "prefix_sum") {
    return "scan";
  }
  if (op == "set_contains") {
    return "contains";
  }
  if (op == "set_union") {
    return "union";
  }
  if (op == "set_intersection") {
    return "intersection";
  }
  if (op == "set_difference") {
    return "difference";
  }
  if (op == "parallelgpustream" || op == "parallelgpu") {
    return "pipeline";
  }
  return op;
}

template <typename Token>
void normalize_token_values(std::vector<Token>& values, bool backend_tokens) {
  std::unordered_set<std::string> seen;
  std::vector<Token> normalized;
  normalized.reserve(values.size());
  seen.reserve(values.size());
  for (auto& item : values) {
    std::string token = backend_tokens ? canonical_backend_name(item.value)
                                       : canonical_operation_name(item.value);
    if (token.empty()) {
      continue;
    }
    if (seen.insert(token).second) {
      item.value = std::move(token);
      normalized.push_back(std::move(item));
    }
  }
  values = std::move(normalized);
}

}  // namespace

dto::PlatformSupportRequest PlatformSupportManager::normalize_request(
    const dto::PlatformSupportRequest& request) const {
  dto::PlatformSupportRequest out = request;

  if (out.preferred_gpu_backends.empty()) {
    out.preferred_gpu_backends = {
        {"cuda"}, {"rocm_hip"}, {"oneapi_sycl"}, {"opencl"}, {"vulkan_compute"}, {"metal"}, {"webgpu"}};
  }

  if (out.list_operations.empty()) {
    out.list_operations = {
        {"pipeline"}, {"map"}, {"filter"}, {"reduce"}, {"scan"}, {"distinct"},
        {"contains"}, {"union"}, {"intersection"}, {"difference"}, {"sort"}};
  }

  if (out.matrix_operations.empty()) {
    out.matrix_operations = {
        {"map"}, {"reduce"}, {"matmul"}, {"matmul_f32"}, {"matmul_f64"}};
  }

  normalize_token_values(out.preferred_gpu_backends, true);
  normalize_token_values(out.list_operations, false);
  normalize_token_values(out.matrix_operations, false);

  return out;
}

}  // namespace spark::core::manager
