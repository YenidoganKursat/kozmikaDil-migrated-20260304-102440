#pragma once

#include "spark/core/dto/platform_support_dto.h"

namespace spark::core::servis {

class PlatformSupportServis {
 public:
  dto::PlatformSupportResponse evaluate(const dto::PlatformSupportRequest& request,
                                        const dto::PlatformSupportSnapshot& snapshot) const;
};

}  // namespace spark::core::servis
