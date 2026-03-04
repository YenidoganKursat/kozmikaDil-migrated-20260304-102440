#include "spark/core/logic/platform_support_logic.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace spark::core::logic {

namespace {

std::vector<std::string> collect_unique_operations(const dto::PlatformSupportRequest& request) {
  std::vector<std::string> out;
  out.reserve(request.list_operations.size() + request.matrix_operations.size());
  std::unordered_set<std::string> seen;
  seen.reserve(out.capacity());

  for (const auto& item : request.list_operations) {
    if (seen.insert(item.value).second) {
      out.push_back(item.value);
    }
  }
  for (const auto& item : request.matrix_operations) {
    if (seen.insert(item.value).second) {
      out.push_back(item.value);
    }
  }
  return out;
}

std::vector<std::string> collect_backends(const dto::PlatformSupportRequest& request) {
  std::vector<std::string> out;
  out.reserve(request.preferred_gpu_backends.size());
  for (const auto& backend : request.preferred_gpu_backends) {
    out.push_back(backend.value);
  }
  return out;
}

}  // namespace

bool PlatformSupportLogic::execute(const dto::PlatformSupportRequest& request,
                                   dto::PlatformSupportResponse& out_response) const {
  const auto normalized = manager_.normalize_request(request);
  const auto backends = collect_backends(normalized);
  const auto operations = collect_unique_operations(normalized);
  const auto snapshot = driver_.collect_snapshot(backends, operations);
  out_response = servis_.evaluate(normalized, snapshot);
  return out_response.supported;
}

}  // namespace spark::core::logic
