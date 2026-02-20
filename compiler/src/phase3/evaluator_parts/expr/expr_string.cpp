#include "../internal_helpers.h"

namespace spark {

Value evaluate_case_string(const StringExpr& expr, Interpreter&,
                           const std::shared_ptr<Environment>&) {
  return Value::string_value_of(expr.value);
}

}  // namespace spark
