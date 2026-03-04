#include "spark/core/servis/platform_support_servis.h"

#include <string>
#include <unordered_map>
#include <utility>

namespace spark::core::servis {

namespace {

struct RouteBuildResult {
  std::vector<dto::OperationRoute> routes;
  std::vector<dto::CapabilityReason> warnings;
  bool covered = true;
  bool gpu_complete = true;
  bool used_cpu_fallback = false;
};

std::string make_cap_key(const std::string& backend, const std::string& operation) {
  return backend + "::" + operation;
}

RouteBuildResult build_routes(
    const std::vector<dto::OperationName>& operations,
    const std::vector<dto::BackendName>& preferred_backends,
    const dto::PlatformSupportSnapshot& snapshot,
    bool allow_cpu_fallback) {
  RouteBuildResult out;
  out.routes.reserve(operations.size());

  std::unordered_map<std::string, bool> cap_index;
  cap_index.reserve(snapshot.gpu_operation_caps.size());
  for (const auto& cap : snapshot.gpu_operation_caps) {
    cap_index[make_cap_key(cap.backend.value, cap.operation.value)] = cap.supported;
  }

  for (const auto& operation : operations) {
    dto::OperationRoute route;
    route.operation = operation;

    bool routed = false;
    for (const auto& backend : preferred_backends) {
      const auto key = make_cap_key(backend.value, operation.value);
      const auto it = cap_index.find(key);
      if (it != cap_index.end() && it->second) {
        route.selected_backend = backend;
        route.execute_on_gpu = true;
        route.reason.value = "gpu_route";
        routed = true;
        break;
      }
    }

    if (!routed) {
      out.gpu_complete = false;
      if (allow_cpu_fallback) {
        route.selected_backend.value = "cpu";
        route.execute_on_gpu = false;
        route.reason.value = "cpu_fallback";
        out.used_cpu_fallback = true;
        out.warnings.push_back(
            {"operation '" + operation.value + "' has no GPU route; CPU fallback selected"});
      } else {
        route.selected_backend.value = "none";
        route.execute_on_gpu = false;
        route.reason.value = "unsupported";
        out.covered = false;
        out.warnings.push_back({"operation '" + operation.value + "' is unsupported"});
      }
    }

    out.routes.push_back(std::move(route));
  }

  return out;
}

}  // namespace

dto::PlatformSupportResponse PlatformSupportServis::evaluate(
    const dto::PlatformSupportRequest& request,
    const dto::PlatformSupportSnapshot& snapshot) const {
  dto::PlatformSupportResponse response;
  response.snapshot = snapshot;

  const auto list_result = build_routes(
      request.list_operations, request.preferred_gpu_backends, snapshot, request.allow_cpu_fallback);
  const auto matrix_result = build_routes(
      request.matrix_operations, request.preferred_gpu_backends, snapshot, request.allow_cpu_fallback);

  response.list_routes = list_result.routes;
  response.matrix_routes = matrix_result.routes;
  response.warnings.reserve(list_result.warnings.size() + matrix_result.warnings.size());
  response.warnings.insert(response.warnings.end(), list_result.warnings.begin(), list_result.warnings.end());
  response.warnings.insert(response.warnings.end(), matrix_result.warnings.begin(), matrix_result.warnings.end());

  const bool list_ok = list_result.covered &&
                       (!request.require_gpu_full_coverage || list_result.gpu_complete);
  const bool matrix_ok = matrix_result.covered &&
                         (!request.require_gpu_full_coverage || matrix_result.gpu_complete);

  response.list_compatible = list_ok;
  response.matrix_compatible = matrix_ok;
  response.supported = list_ok && matrix_ok;

  if (request.strict_mode && (list_result.used_cpu_fallback || matrix_result.used_cpu_fallback)) {
    response.supported = false;
    response.warnings.push_back({"strict_mode enabled: CPU fallback is not allowed"});
  }

  return response;
}

}  // namespace spark::core::servis
