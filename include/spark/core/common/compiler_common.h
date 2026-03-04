#pragma once

#include <string>

#include "spark/core/dto/compiler_pipeline_dto.h"

namespace spark::core::common {

dto::DiagnosticEntry make_diagnostic(std::string source_id, std::string message);

}  // namespace spark::core::common
