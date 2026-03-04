#include "../internal_helpers.h"

namespace spark {

namespace {

std::shared_ptr<Value::Function> make_class_method_function(
    const FunctionDefStmt& stmt, const std::shared_ptr<Environment>& env) {
  auto fn_value = std::make_shared<Value::Function>();
  fn_value->program = nullptr;
  fn_value->body = &stmt.body;
  fn_value->params = stmt.params;
  fn_value->is_async = stmt.is_async;
  fn_value->closure = env;
  fn_value->closure_frozen = std::make_shared<Environment>(nullptr, true);
  for (const auto& name : env->keys()) {
    const auto value = env->get(name);
    fn_value->closure_snapshot[name] = value;
    fn_value->closure_frozen->define(name, value);
  }
  Value self_fn = Value::function(fn_value);
  fn_value->closure_snapshot[stmt.name] = self_fn;
  fn_value->closure_frozen->define(stmt.name, self_fn);
  return fn_value;
}

}  // namespace

Value execute_case_class_def(const ClassDefStmt& cls, Interpreter& self,
                            const std::shared_ptr<Environment>& env) {
  auto class_def = std::make_shared<Value::ClassValue>();
  class_def->name = cls.name;
  class_def->open_shape = cls.open_shape;

  auto class_scope = std::make_shared<Environment>(env);
  for (const auto& stmt : cls.body) {
    if (!stmt) {
      continue;
    }
    if (stmt->kind == Stmt::Kind::Assign) {
      const auto& assign = static_cast<const AssignStmt&>(*stmt);
      if (!assign.target || assign.target->kind != Expr::Kind::Variable || !assign.value) {
        throw EvalException("class field declaration requires `name = expr`");
      }
      const auto& field = static_cast<const VariableExpr&>(*assign.target);
      const auto value = self.evaluate(*assign.value, class_scope);
      class_def->field_defaults[field.name] = value;
      class_scope->define(field.name, value);
      continue;
    }
    if (stmt->kind == Stmt::Kind::FunctionDef) {
      const auto& method = static_cast<const FunctionDefStmt&>(*stmt);
      auto method_fn = make_class_method_function(method, class_scope);
      Value method_value = Value::function(std::move(method_fn));
      class_def->methods[method.name] = method_value;
      class_scope->define(method.name, method_value);
      continue;
    }
    throw EvalException("class body supports only field assignments and method definitions");
  }

  Value class_value = Value::class_value_of(class_def);
  if (!env->set(cls.name, class_value)) {
    env->define(cls.name, class_value);
  }
  return Value::nil();
}

}  // namespace spark
