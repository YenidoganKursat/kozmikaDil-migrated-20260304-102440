#pragma once

#include "spark/core/behavior/platform_support_behavior.h"
#include "spark/core/logic/platform_support_logic.h"

namespace spark::core::behavior {

class PlatformSupportBehaviorImpl final : public PlatformSupportBehavior {
 public:
  explicit PlatformSupportBehaviorImpl(logic::PlatformSupportLogic& logic) : logic_(logic) {}

  bool evaluate_support(const dto::PlatformSupportRequest& request,
                        dto::PlatformSupportResponse& out_response) override;

 private:
  logic::PlatformSupportLogic& logic_;
};

}  // namespace spark::core::behavior
