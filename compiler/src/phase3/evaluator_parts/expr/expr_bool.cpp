#include "../internal_helpers.h"

namespace spark {

Value evaluate_case_bool(const BoolExpr& expr, Interpreter&,
                        const std::shared_ptr<Environment>&) {
  return Value::bool_value_of(expr.value);
}

}  // namespace spark
