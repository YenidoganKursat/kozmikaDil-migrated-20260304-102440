#pragma once

#include "spark/core/behavior/compiler_behavior.h"
#include "spark/core/mode/compiler_mode.h"

namespace spark::core::ui {

class CompilerUiFacade {
 public:
  explicit CompilerUiFacade(behavior::CompilerBehavior& behavior)
      : behavior_(behavior) {}

  bool parse_and_typecheck(const dto::FilePath& source_file, dto::ProgramBundle& out_bundle) {
    return behavior_.parse_and_typecheck(source_file, out_bundle);
  }

  bool lower_to_products(const dto::ProgramBundle& bundle,
                         dto::PipelineProducts& out_products) {
    return behavior_.lower_to_products(bundle, out_products);
  }

  const std::vector<dto::DiagnosticEntry>& diagnostics() const {
    return behavior_.diagnostics();
  }

 private:
  behavior::CompilerBehavior& behavior_;
};

CompilerUiFacade make_compiler_ui(behavior::CompilerBehavior& behavior);

}  // namespace spark::core::ui
