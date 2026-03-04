#include "spark/application/wiring/compiler_wiring.h"

namespace spark::application::wiring {

CompilerWiring::CompilerWiring()
    : driver_(&source_input_),
      manager_(),
      servis_(),
      behavior_(driver_, manager_, servis_),
      ui_(behavior_),
      platform_support_driver_(),
      platform_support_manager_(),
      platform_support_servis_(),
      platform_support_logic_(platform_support_driver_, platform_support_manager_, platform_support_servis_),
      platform_support_behavior_(platform_support_logic_),
      platform_support_ui_(platform_support_behavior_) {}

}  // namespace spark::application::wiring
