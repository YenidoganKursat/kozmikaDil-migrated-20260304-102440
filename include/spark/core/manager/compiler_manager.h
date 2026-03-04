#pragma once

#include <vector>

#include "spark/core/dto/compiler_pipeline_dto.h"

namespace spark::core::manager {

class CompilerManager {
 public:
  bool run_typecheck(dto::ProgramBundle& bundle,
                     std::vector<dto::DiagnosticEntry>& diagnostics) const;
};

}  // namespace spark::core::manager
