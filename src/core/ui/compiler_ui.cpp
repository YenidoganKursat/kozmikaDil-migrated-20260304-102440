#include "spark/core/ui/compiler_ui.h"

namespace spark::core::ui {

CompilerUiFacade make_compiler_ui(behavior::CompilerBehavior& behavior) {
  return CompilerUiFacade(behavior);
}

}  // namespace spark::core::ui
