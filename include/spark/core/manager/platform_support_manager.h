#pragma once

#include "spark/core/dto/platform_support_dto.h"

namespace spark::core::manager {

class PlatformSupportManager {
 public:
  dto::PlatformSupportRequest normalize_request(const dto::PlatformSupportRequest& request) const;
};

}  // namespace spark::core::manager
