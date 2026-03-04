#pragma once

#include "spark/core/dto/compiler_pipeline_dto.h"

namespace spark::core::behavior {

class CompilerBehavior {
 public:
  virtual ~CompilerBehavior() = default;

  virtual bool parse_and_typecheck(const dto::FilePath& source_file,
                                   dto::ProgramBundle& out_bundle) = 0;
  virtual bool lower_to_products(const dto::ProgramBundle& bundle,
                                 dto::PipelineProducts& out_products) = 0;
  virtual const std::vector<dto::DiagnosticEntry>& diagnostics() const = 0;
};

}  // namespace spark::core::behavior
