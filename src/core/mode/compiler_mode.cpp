#include "spark/core/mode/compiler_mode.h"

namespace spark::core::mode {

CompilerMode sanitize_mode(const CompilerMode& mode) {
  CompilerMode out = mode;
  if (out.tuning.auto_pgo_runs < 0) {
    out.tuning.auto_pgo_runs = 0;
  }
  return out;
}

}  // namespace spark::core::mode
