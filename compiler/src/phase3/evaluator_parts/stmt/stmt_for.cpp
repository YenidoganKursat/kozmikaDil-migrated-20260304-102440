#include "../internal_helpers.h"

namespace spark {

Value execute_case_for(const ForStmt& for_stmt, Interpreter& self,
                      const std::shared_ptr<Environment>& env) {
  auto sequence = self.evaluate(*for_stmt.iterable, env);
  const auto& body = for_stmt.body;

  Value* loop_slot = nullptr;
  auto bind_loop_var = [&](const Value& value) mutable {
    if (!loop_slot) {
      if (!env->set(for_stmt.name, value)) {
        env->define(for_stmt.name, value);
      }
      loop_slot = env->get_ptr(for_stmt.name);
      if (!loop_slot) {
        throw EvalException("failed to bind loop variable");
      }
      return;
    }
    *loop_slot = value;
  };

  const auto execute_body = [&](Value& result) {
    if (body.empty()) {
      return;
    }
    if (body.size() == 1) {
      result = self.execute(*body.front(), env);
      return;
    }
    for (const auto& child : body) {
      result = self.execute(*child, env);
    }
  };

  if (for_stmt.is_async && sequence.kind == Value::Kind::Channel) {
    Value result = Value::nil();
    while (true) {
      const auto item = stream_next_value(sequence);
      if (item.kind == Value::Kind::Nil) {
        break;
      }
      bind_loop_var(item);
      execute_body(result);
    }
    return result;
  }

  if (sequence.kind != Value::Kind::List) {
    if (sequence.kind != Value::Kind::Matrix) {
      throw EvalException(for_stmt.is_async
                              ? "async for loop requires channel/list/matrix iterable"
                              : "for loop requires list or matrix iterable");
    }
  }
  Value result = Value::nil();
  if (sequence.kind == Value::Kind::List) {
    for (const auto& item : sequence.list_value) {
      bind_loop_var(item);
      execute_body(result);
    }
    return result;
  }

  const auto* matrix = sequence.matrix_value.get();
  const auto rows = matrix ? matrix->rows : 0;
  const auto cols = matrix ? matrix->cols : 0;
  if (matrix && cols == 1) {
    // Single-column matrices can stream scalar cells directly.
    for (std::size_t row = 0; row < rows; ++row) {
      bind_loop_var(matrix->data[row]);
      execute_body(result);
    }
    return result;
  }

  // Multi-column matrix iteration keeps Python-like row-list semantics, but
  // reuses a row buffer to avoid per-iteration list allocations.
  Value row_value = Value::list_value_of(std::vector<Value>(cols));
  for (std::size_t row = 0; row < rows; ++row) {
    const auto base = row * cols;
    for (std::size_t col = 0; col < cols; ++col) {
      row_value.list_value[col] = matrix->data[base + col];
    }
    bind_loop_var(row_value);
    execute_body(result);
  }
  return result;
}

}  // namespace spark
