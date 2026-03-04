#include "spark/core/ui/platform_support_ui.h"

namespace spark::core::ui {

PlatformSupportUiFacade make_platform_support_ui(behavior::PlatformSupportBehavior& behavior) {
  return PlatformSupportUiFacade(behavior);
}

}  // namespace spark::core::ui
