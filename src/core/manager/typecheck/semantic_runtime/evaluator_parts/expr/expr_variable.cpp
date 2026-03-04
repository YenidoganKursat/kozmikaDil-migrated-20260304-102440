#include "../internal_helpers.h"

#include <array>
#include <cstdint>

namespace spark {

Value evaluate_case_variable(const VariableExpr& expr, Interpreter&,
                            const std::shared_ptr<Environment>& env) {
  struct VarLookupEntry {
    const VariableExpr* expr = nullptr;
    std::uint64_t env_id = 0;
    std::uint64_t values_epoch = 0;
    const Value* value = nullptr;
  };
  constexpr std::size_t kVarLookupCacheSize = 1024;
  static thread_local std::array<VarLookupEntry, kVarLookupCacheSize> kVarLookupCache{};

  const auto* expr_ptr = &expr;
  const auto env_id = env->stable_id;
  const auto raw_hash =
      static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(expr_ptr)) ^
      (static_cast<std::size_t>(env_id) * 11400714819323198485ull);
  auto& slot = kVarLookupCache[raw_hash & (kVarLookupCacheSize - 1)];

  if (slot.expr == expr_ptr && slot.env_id == env_id &&
      slot.values_epoch == env->values_epoch && slot.value != nullptr) {
    return *slot.value;
  }

  if (const auto* value = env->get_ptr(expr.name); value != nullptr) {
    slot.expr = expr_ptr;
    slot.env_id = env_id;
    slot.values_epoch = env->values_epoch;
    slot.value = value;
    return *value;
  }

  throw EvalException("undefined variable: " + expr.name);
}

}  // namespace spark
