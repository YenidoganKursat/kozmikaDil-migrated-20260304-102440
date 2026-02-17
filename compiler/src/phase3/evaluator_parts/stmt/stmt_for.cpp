#include "../internal_helpers.h"

namespace spark {

Value execute_case_for(const ForStmt& for_stmt, Interpreter& self,
                      const std::shared_ptr<Environment>& env) {
  auto sequence = self.evaluate(*for_stmt.iterable, env);
  if (sequence.kind != Value::Kind::List) {
    if (sequence.kind != Value::Kind::Matrix) {
      throw EvalException("for loop requires list or matrix iterable");
    }
  }
  Value result = Value::nil();
  if (sequence.kind == Value::Kind::List) {
    for (const auto& item : sequence.list_value) {
      if (!env->set(for_stmt.name, item)) {
        env->define(for_stmt.name, item);
      }
      for (const auto& child : for_stmt.body) {
        result = self.execute(*child, env);
      }
    }
    return result;
  }

  auto rows = matrix_row_count(sequence);
  auto cols = matrix_col_count(sequence);
  for (std::size_t row = 0; row < rows; ++row) {
    auto line = matrix_row_as_list(sequence, static_cast<long long>(row));
    if (cols == 1) {
      if (!env->set(for_stmt.name, line.list_value[0])) {
        env->define(for_stmt.name, line.list_value[0]);
      }
    } else {
      if (!env->set(for_stmt.name, line)) {
        env->define(for_stmt.name, line);
      }
    }
    for (const auto& child : for_stmt.body) {
      result = self.execute(*child, env);
    }
  }
  return result;
}

}  // namespace spark
