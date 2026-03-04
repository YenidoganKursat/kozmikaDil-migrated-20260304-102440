#include <cstdint>
#include <unordered_map>
#include <vector>

#include "../internal_helpers.h"

namespace spark {

namespace {

struct SwitchCaseRuntime {
  bool is_dynamic = true;
  Value literal_value = Value::nil();
  const Expr* dynamic_expr = nullptr;
  std::vector<FastStmtExecThunk> body_thunks;
};

struct SwitchRuntimePlan {
  std::vector<SwitchCaseRuntime> cases;
  std::vector<FastStmtExecThunk> default_thunks;
};

std::uint64_t switch_stmt_fingerprint(const SwitchStmt& stmt) {
  std::uint64_t hash = 1469598103934665603ULL;
  const auto mix = [&](std::uintptr_t value) {
    hash ^= static_cast<std::uint64_t>(value);
    hash *= 1099511628211ULL;
  };

  mix(reinterpret_cast<std::uintptr_t>(&stmt));
  mix(reinterpret_cast<std::uintptr_t>(stmt.selector.get()));
  mix(static_cast<std::uintptr_t>(stmt.cases.size()));
  for (const auto& switch_case : stmt.cases) {
    mix(reinterpret_cast<std::uintptr_t>(switch_case.match.get()));
    mix(static_cast<std::uintptr_t>(switch_case.body.size()));
    for (const auto& body_stmt : switch_case.body) {
      mix(reinterpret_cast<std::uintptr_t>(body_stmt.get()));
    }
  }
  mix(static_cast<std::uintptr_t>(stmt.default_body.size()));
  for (const auto& body_stmt : stmt.default_body) {
    mix(reinterpret_cast<std::uintptr_t>(body_stmt.get()));
  }
  return hash;
}

SwitchRuntimePlan build_switch_runtime_plan(const SwitchStmt& stmt) {
  SwitchRuntimePlan plan;
  plan.cases.reserve(stmt.cases.size());
  for (const auto& switch_case : stmt.cases) {
    SwitchCaseRuntime runtime_case;
    runtime_case.is_dynamic = true;
    if (switch_case.match) {
      switch (switch_case.match->kind) {
        case Expr::Kind::Number: {
          const auto& number = static_cast<const NumberExpr&>(*switch_case.match);
          runtime_case.is_dynamic = false;
          if (number.is_int) {
            runtime_case.literal_value =
                Value::int_value_of(static_cast<long long>(number.value));
          } else {
            runtime_case.literal_value = Value::double_value_of(number.value);
          }
          break;
        }
        case Expr::Kind::String: {
          const auto& text = static_cast<const StringExpr&>(*switch_case.match);
          runtime_case.is_dynamic = false;
          runtime_case.literal_value = Value::string_value_of(text.value);
          break;
        }
        case Expr::Kind::Bool: {
          const auto& boolean = static_cast<const BoolExpr&>(*switch_case.match);
          runtime_case.is_dynamic = false;
          runtime_case.literal_value = Value::bool_value_of(boolean.value);
          break;
        }
        default:
          runtime_case.dynamic_expr = switch_case.match.get();
          break;
      }
    }

    runtime_case.body_thunks.reserve(switch_case.body.size());
    for (const auto& stmt_ptr : switch_case.body) {
      runtime_case.body_thunks.push_back(make_fast_stmt_thunk(*stmt_ptr));
    }
    plan.cases.push_back(std::move(runtime_case));
  }

  plan.default_thunks.reserve(stmt.default_body.size());
  for (const auto& stmt_ptr : stmt.default_body) {
    plan.default_thunks.push_back(make_fast_stmt_thunk(*stmt_ptr));
  }
  return plan;
}

const SwitchRuntimePlan& runtime_plan_for(const SwitchStmt& stmt) {
  struct CacheEntry {
    std::uint64_t fingerprint = 0;
    SwitchRuntimePlan plan;
  };
  static thread_local std::unordered_map<const SwitchStmt*, CacheEntry> cache;
  const auto fingerprint = switch_stmt_fingerprint(stmt);
  auto it = cache.find(&stmt);
  if (it != cache.end() && it->second.fingerprint == fingerprint) {
    return it->second.plan;
  }
  auto rebuilt = build_switch_runtime_plan(stmt);
  cache[&stmt] = CacheEntry{fingerprint, std::move(rebuilt)};
  return cache[&stmt].plan;
}

Value execute_switch_block(const std::vector<FastStmtExecThunk>& thunks, Interpreter& self,
                           const std::shared_ptr<Environment>& env) {
  Value result = Value::nil();
  for (const auto& thunk : thunks) {
    result = execute_stmt_thunk(thunk, self, env);
  }
  return result;
}

}  // namespace

Value execute_case_switch(const SwitchStmt& stmt, Interpreter& self,
                          const std::shared_ptr<Environment>& env) {
  const auto selector = self.evaluate(*stmt.selector, env);
  const auto& plan = runtime_plan_for(stmt);

  try {
    for (const auto& runtime_case : plan.cases) {
      bool matched = false;
      if (runtime_case.is_dynamic) {
        if (!runtime_case.dynamic_expr) {
          continue;
        }
        const auto case_value = self.evaluate(*runtime_case.dynamic_expr, env);
        matched = selector.equals(case_value);
      } else {
        matched = selector.equals(runtime_case.literal_value);
      }
      if (matched) {
        return execute_switch_block(runtime_case.body_thunks, self, env);
      }
    }
    return execute_switch_block(plan.default_thunks, self, env);
  } catch (const Interpreter::BreakSignal&) {
    // break exits switch scope.
    return Value::nil();
  }
}

}  // namespace spark
