#include <vector>

#include "../internal_helpers.h"

namespace spark {

namespace {

Value execute_block_fast(const StmtList& body, Interpreter& self,
                         const std::shared_ptr<Environment>& env) {
  Value result = Value::nil();
  if (body.empty()) {
    return result;
  }
  std::vector<FastStmtExecThunk> thunks;
  thunks.reserve(body.size());
  for (const auto& stmt : body) {
    thunks.push_back(make_fast_stmt_thunk(*stmt));
  }
  for (const auto& thunk : thunks) {
    result = execute_stmt_thunk(thunk, self, env);
  }
  return result;
}

}  // namespace

Value execute_case_try_catch(const TryCatchStmt& stmt, Interpreter& self,
                             const std::shared_ptr<Environment>& env) {
  try {
    return execute_block_fast(stmt.try_body, self, env);
  } catch (const EvalException& err) {
    if (!stmt.catch_name.empty()) {
      const auto value = Value::string_value_of(err.what());
      if (!env->set(stmt.catch_name, value)) {
        env->define(stmt.catch_name, value);
      }
    }
    return execute_block_fast(stmt.catch_body, self, env);
  }
}

}  // namespace spark
