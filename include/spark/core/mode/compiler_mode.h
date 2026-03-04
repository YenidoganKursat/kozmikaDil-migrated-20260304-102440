#pragma once

#include "spark/core/dto/compiler_pipeline_dto.h"

namespace spark::core::mode {

struct CompilerMode {
  bool allow_t5 = false;
  bool explain_layout = false;
  dto::BuildTuning tuning;
};

CompilerMode sanitize_mode(const CompilerMode& mode);

}  // namespace spark::core::mode
