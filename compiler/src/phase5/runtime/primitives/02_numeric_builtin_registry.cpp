#include <array>
#include <string>
#include <vector>

#include "phase3/evaluator_parts/internal_helpers.h"

namespace spark {

void register_numeric_primitive_builtins(const std::shared_ptr<Environment>& globals) {
  const std::array<std::string, 15> primitive_names = {
      "i8",  "i16", "i32", "i64",  "i128", "i256", "i512", "f8",
      "f16", "bf16","f32", "f64",  "f128", "f256", "f512",
  };

  for (const auto& primitive_name : primitive_names) {
    const auto kind = numeric_kind_from_name(primitive_name);
    globals->define(
        primitive_name,
        Value::builtin(
            primitive_name, [kind, primitive_name](const std::vector<Value>& args) -> Value {
              if (args.size() != 1) {
                throw EvalException(primitive_name + "() expects exactly one numeric argument");
              }
              return cast_numeric_to_kind(kind, args[0]);
            }));
  }
}

}  // namespace spark
