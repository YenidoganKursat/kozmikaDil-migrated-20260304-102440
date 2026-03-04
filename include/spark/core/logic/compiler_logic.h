#pragma once

#include "spark/core/dto/compiler_pipeline_dto.h"
#include "spark/core/driver/compiler_driver.h"
#include "spark/core/manager/compiler_manager.h"
#include "spark/core/mapper/compiler_mapper.h"
#include "spark/core/servis/compiler_servis.h"

namespace spark::core::logic {

class CompilerLogic {
 public:
  CompilerLogic(driver::CompilerDriver& driver, manager::CompilerManager& manager,
                servis::CompilerServis& servis)
      : driver_(driver), manager_(manager), servis_(servis) {}

  bool execute_compile(const dto::CompileRequest& request, dto::CompileResponse& out_response) const;

 private:
  driver::CompilerDriver& driver_;
  manager::CompilerManager& manager_;
  servis::CompilerServis& servis_;
};

}  // namespace spark::core::logic
