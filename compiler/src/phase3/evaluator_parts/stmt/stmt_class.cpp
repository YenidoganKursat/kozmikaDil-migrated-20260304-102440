#include "../internal_helpers.h"

namespace spark {

Value execute_case_class_def(const ClassDefStmt& cls, Interpreter& self,
                            const std::shared_ptr<Environment>& env) {
  Value class_value = Value::nil();
  if (!env->set(cls.name, class_value)) {
    env->define(cls.name, class_value);
  }
  return Value::nil();
}

}  // namespace spark
