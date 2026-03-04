#pragma once

#include <vector>

#include "spark/core/dto/compiler_pipeline_dto.h"

namespace spark::core::servis {

class CompilerServis {
 public:
  bool lower_to_products(const dto::ProgramBundle& bundle, dto::PipelineProducts& out_products,
                         std::vector<dto::DiagnosticEntry>& diagnostics) const;
};

}  // namespace spark::core::servis
