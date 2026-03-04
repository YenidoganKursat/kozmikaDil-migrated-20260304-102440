#pragma once

#include "spark/core/behavior/compiler_behavior_impl.h"
#include "spark/core/behavior/platform_support_behavior_impl.h"
#include "spark/core/driver/compiler_driver.h"
#include "spark/core/driver/platform_support_driver.h"
#include "spark/core/logic/platform_support_logic.h"
#include "spark/core/manager/compiler_manager.h"
#include "spark/core/manager/platform_support_manager.h"
#include "spark/core/servis/compiler_servis.h"
#include "spark/core/servis/platform_support_servis.h"
#include "spark/core/ui/compiler_ui.h"
#include "spark/core/ui/platform_support_ui.h"
#include "spark/port/input/source_input_port.h"
#include "spark/port/output/diagnostic_output_port.h"

namespace spark::application::wiring {

class CompilerWiring {
 public:
  CompilerWiring();

  core::ui::CompilerUiFacade& ui() {
    return ui_;
  }

  core::ui::PlatformSupportUiFacade& platform_support_ui() {
    return platform_support_ui_;
  }

  port::output::DiagnosticOutputPort& diagnostics() {
    return diagnostic_output_;
  }

 private:
  port::input::FilesystemSourceInputPort source_input_;
  port::output::StdoutDiagnosticOutputPort diagnostic_output_;

  core::driver::CompilerDriver driver_;
  core::manager::CompilerManager manager_;
  core::servis::CompilerServis servis_;
  core::behavior::CompilerBehaviorImpl behavior_;
  core::ui::CompilerUiFacade ui_;

  core::driver::PlatformSupportDriver platform_support_driver_;
  core::manager::PlatformSupportManager platform_support_manager_;
  core::servis::PlatformSupportServis platform_support_servis_;
  core::logic::PlatformSupportLogic platform_support_logic_;
  core::behavior::PlatformSupportBehaviorImpl platform_support_behavior_;
  core::ui::PlatformSupportUiFacade platform_support_ui_;
};

}  // namespace spark::application::wiring
