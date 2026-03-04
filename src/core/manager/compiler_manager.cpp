#include "spark/core/manager/compiler_manager.h"

#include "spark/core/common/compiler_common.h"

namespace spark::core::manager {

bool CompilerManager::run_typecheck(dto::ProgramBundle& bundle,
                                    std::vector<dto::DiagnosticEntry>& diagnostics) const {
  if (!bundle.program) {
    diagnostics.push_back(core::common::make_diagnostic(
        bundle.source_unit.source_id.value, "typecheck requested without program AST"));
    return false;
  }

  bundle.checker = TypeChecker{};
  bundle.checker.check(*bundle.program);
  for (const auto& err : bundle.checker.diagnostics()) {
    diagnostics.push_back(core::common::make_diagnostic(bundle.source_unit.source_id.value, err));
  }
  return !bundle.checker.has_errors();
}

}  // namespace spark::core::manager
