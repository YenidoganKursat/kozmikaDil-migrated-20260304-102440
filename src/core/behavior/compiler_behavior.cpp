#include "spark/core/behavior/compiler_behavior_impl.h"

namespace spark::core::behavior {

bool CompilerBehaviorImpl::parse_and_typecheck(const dto::FilePath& source_file,
                                               dto::ProgramBundle& out_bundle) {
  diagnostics_.clear();
  const bool parsed = driver_.construct_program_bundle(source_file, out_bundle, diagnostics_);
  if (!parsed) {
    return false;
  }
  return manager_.run_typecheck(out_bundle, diagnostics_);
}

bool CompilerBehaviorImpl::lower_to_products(const dto::ProgramBundle& bundle,
                                             dto::PipelineProducts& out_products) {
  diagnostics_.clear();
  return servis_.lower_to_products(bundle, out_products, diagnostics_);
}

}  // namespace spark::core::behavior
