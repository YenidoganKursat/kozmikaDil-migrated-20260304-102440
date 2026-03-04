#include "spark/core/servis/compiler_servis.h"

#include "spark/codegen.h"
#include "spark/core/common/compiler_common.h"

namespace spark::core::servis {

bool CompilerServis::lower_to_products(const dto::ProgramBundle& bundle,
                                       dto::PipelineProducts& out_products,
                                       std::vector<dto::DiagnosticEntry>& diagnostics) const {
  if (!bundle.program) {
    diagnostics.push_back(core::common::make_diagnostic(
        bundle.source_unit.source_id.value, "lower requested without program AST"));
    return false;
  }

  CodeGenerator generator;
  const auto ir = generator.generate(*bundle.program, {});
  out_products.ir = ir.output;
  out_products.diagnostics = ir.diagnostics;
  for (const auto& item : ir.diagnostics) {
    diagnostics.push_back(core::common::make_diagnostic(bundle.source_unit.source_id.value, item));
  }
  if (!ir.success) {
    return false;
  }

  IRToCGenerator ir_to_c;
  const auto c_result = ir_to_c.translate(ir.output, {});
  out_products.c_source = c_result.output;
  for (const auto& item : c_result.diagnostics) {
    out_products.diagnostics.push_back(item);
    diagnostics.push_back(core::common::make_diagnostic(bundle.source_unit.source_id.value, item));
  }
  return c_result.success;
}

}  // namespace spark::core::servis
