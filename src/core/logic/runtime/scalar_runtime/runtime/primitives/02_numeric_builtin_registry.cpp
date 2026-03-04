#include <array>
#include <string>
#include <vector>

#include "semantic_runtime/evaluator_parts/internal_helpers.h"

namespace spark {

void register_numeric_primitive_builtins(const std::shared_ptr<Environment>& globals) {
  const std::array<std::string, 25> primitive_names = {
      "i8",  "i16", "i32", "i64",  "i128", "i256", "i512", "f8",
      "f16", "bf16","f32", "f64",  "f128", "f256", "f512",
      "int", "integer", "ibig", "bigint",
      "i1024", "i2048", "i4096", "i8192", "i16384", "i32768",
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

  globals->define("gpu_backend",
                  Value::builtin("gpu_backend",
                                 [](const std::vector<Value>& args) -> Value {
                                   return gpu_backend_builtin_value(args);
                                 }));

  globals->define("gpu_supports",
                  Value::builtin("gpu_supports",
                                 [](const std::vector<Value>& args) -> Value {
                                   return gpu_supports_builtin_value(args);
                                 }));

  globals->define("gpu_orchestrate",
                  Value::builtin("gpu_orchestrate",
                                 [](const std::vector<Value>& args) -> Value {
                                   return gpu_orchestrate_builtin_value(args);
                                 }));

  // Alias for stream-style naming intent.
  globals->define("parallelGPUStreamPlan",
                  Value::builtin("parallelGPUStreamPlan",
                                 [](const std::vector<Value>& args) -> Value {
                                   return gpu_orchestrate_builtin_value(args);
                                 }));
}

}  // namespace spark
