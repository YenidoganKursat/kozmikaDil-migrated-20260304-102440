#include "../internal_helpers.h"

namespace spark {

Value execute_case_break(const BreakStmt& stmt, Interpreter& self,
                         const std::shared_ptr<Environment>& env) {
  (void)stmt;
  (void)self;
  (void)env;
  throw Interpreter::BreakSignal{};
}

}  // namespace spark
