#pragma once

#include "spark/core/dto/platform_support_dto.h"

namespace spark::core::behavior {

class PlatformSupportBehavior {
 public:
  virtual ~PlatformSupportBehavior() = default;

  virtual bool evaluate_support(const dto::PlatformSupportRequest& request,
                                dto::PlatformSupportResponse& out_response) = 0;
};

}  // namespace spark::core::behavior
