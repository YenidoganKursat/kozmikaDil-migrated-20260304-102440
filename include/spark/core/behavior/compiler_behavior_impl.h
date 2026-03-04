#pragma once

#include "spark/core/behavior/compiler_behavior.h"
#include "spark/core/driver/compiler_driver.h"
#include "spark/core/manager/compiler_manager.h"
#include "spark/core/servis/compiler_servis.h"

namespace spark::core::behavior {

class CompilerBehaviorImpl final : public CompilerBehavior {
 public:
  CompilerBehaviorImpl(driver::CompilerDriver& driver, manager::CompilerManager& manager,
                       servis::CompilerServis& servis)
      : driver_(driver), manager_(manager), servis_(servis) {}

  bool parse_and_typecheck(const dto::FilePath& source_file,
                           dto::ProgramBundle& out_bundle) override;
  bool lower_to_products(const dto::ProgramBundle& bundle,
                         dto::PipelineProducts& out_products) override;

  const std::vector<dto::DiagnosticEntry>& diagnostics() const override {
    return diagnostics_;
  }

 private:
  driver::CompilerDriver& driver_;
  manager::CompilerManager& manager_;
  servis::CompilerServis& servis_;
  std::vector<dto::DiagnosticEntry> diagnostics_;
};

}  // namespace spark::core::behavior
