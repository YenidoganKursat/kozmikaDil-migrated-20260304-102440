#include "../internal_helpers.h"

namespace spark {

Value evaluate_case_number(const NumberExpr& expr, Interpreter&,
                         const std::shared_ptr<Environment>&) {
  return expr.is_int ? Value::int_value_of(static_cast<long long>(expr.value))
                     : Value::double_value_of(expr.value);
}

}  // namespace spark
