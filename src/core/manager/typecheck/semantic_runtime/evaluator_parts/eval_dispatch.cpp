#include "internal_helpers.h"

namespace spark {

Value Interpreter::evaluate(const Expr& expr, const std::shared_ptr<Environment>& env) {
  return execute_expr_fast(expr, *this, env);
}

Value Interpreter::execute(const Stmt& stmt, const std::shared_ptr<Environment>& env) {
  return execute_stmt_fast(stmt, *this, env);
}

}  // namespace spark
