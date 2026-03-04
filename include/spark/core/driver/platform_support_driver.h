#pragma once

#include <string>
#include <vector>

#include "spark/core/dto/platform_support_dto.h"

namespace spark::core::driver {

class PlatformSupportDriver {
 public:
  dto::PlatformSupportSnapshot collect_snapshot(
      const std::vector<std::string>& requested_backends,
      const std::vector<std::string>& requested_operations) const;

  std::vector<dto::TargetCapability> known_cpu_targets() const;
  std::vector<dto::TargetCapability> known_micro_targets() const;
  bool gpu_supports_operation(const std::string& backend, const std::string& operation) const;
};

}  // namespace spark::core::driver
