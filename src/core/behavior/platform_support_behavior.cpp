#include "spark/core/behavior/platform_support_behavior_impl.h"

namespace spark::core::behavior {

bool PlatformSupportBehaviorImpl::evaluate_support(const dto::PlatformSupportRequest& request,
                                                   dto::PlatformSupportResponse& out_response) {
  return logic_.execute(request, out_response);
}

}  // namespace spark::core::behavior
