#include "spark/core/logic/compiler_logic.h"

namespace spark::core::logic {

bool CompilerLogic::execute_compile(const dto::CompileRequest& request,
                                    dto::CompileResponse& out_response) const {
  std::vector<dto::DiagnosticEntry> diagnostics;
  dto::ProgramBundle bundle;

  const bool parsed = driver_.construct_program_bundle(request.source_file, bundle, diagnostics);
  const bool typed = parsed ? manager_.run_typecheck(bundle, diagnostics) : false;
  const bool lowered = typed ? servis_.lower_to_products(bundle, out_response.products, diagnostics) : false;

  const auto diagnostics_by_source = mapper::DiagnosticMapper::by_source_id(diagnostics);
  out_response.diagnostics.clear();
  out_response.diagnostics.reserve(diagnostics_by_source.size());
  for (const auto& pair : diagnostics_by_source) {
    out_response.diagnostics.push_back(pair.second.message.value);
  }

  out_response.success = parsed && typed && lowered;
  return out_response.success;
}

}  // namespace spark::core::logic
