#pragma once

#include "spark/core/behavior/platform_support_behavior.h"

namespace spark::core::ui {

class PlatformSupportUiFacade {
 public:
  explicit PlatformSupportUiFacade(behavior::PlatformSupportBehavior& behavior)
      : behavior_(behavior) {}

  bool evaluate_support(const dto::PlatformSupportRequest& request,
                        dto::PlatformSupportResponse& out_response) {
    return behavior_.evaluate_support(request, out_response);
  }

 private:
  behavior::PlatformSupportBehavior& behavior_;
};

PlatformSupportUiFacade make_platform_support_ui(behavior::PlatformSupportBehavior& behavior);

}  // namespace spark::core::ui
