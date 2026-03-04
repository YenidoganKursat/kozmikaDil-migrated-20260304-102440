#include "spark/core/common/compiler_common.h"

namespace spark::core::common {

dto::DiagnosticEntry make_diagnostic(std::string source_id, std::string message) {
  dto::DiagnosticEntry out;
  out.source_id.value = std::move(source_id);
  out.message.value = std::move(message);
  return out;
}

}  // namespace spark::core::common
