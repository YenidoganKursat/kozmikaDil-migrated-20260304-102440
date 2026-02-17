#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {
namespace phase7 {

namespace {

long long saturating_size_to_i64(const std::size_t value) {
  constexpr auto kMax = static_cast<std::size_t>(std::numeric_limits<long long>::max());
  return static_cast<long long>(value > kMax ? kMax : value);
}

}  // namespace

}  // namespace phase7

bool try_execute_pipeline_call(const CallExpr& call, Interpreter& self,
                               const std::shared_ptr<Environment>& env, Value& out) {
  phase7::PipelineChain chain;
  if (!phase7::build_pipeline_chain(call, self, env, chain)) {
    return false;
  }

  if (chain.base_value.kind == Value::Kind::List) {
    out = phase7::execute_list_pipeline(chain, env);
    return true;
  }
  if (chain.base_value.kind == Value::Kind::Matrix) {
    out = phase7::execute_matrix_pipeline(chain, env);
    return true;
  }

  throw EvalException("pipeline receiver must be list or matrix");
}

Value pipeline_stats_value(const std::shared_ptr<Environment>& env, const std::string& name) {
  const auto* stats = phase7::get_pipeline_stats_const(env, name);
  std::vector<Value> values;
  values.reserve(8);
  if (!stats) {
    for (std::size_t i = 0; i < 8; ++i) {
      values.push_back(Value::int_value_of(0));
    }
    return Value::list_value_of(std::move(values));
  }

  values.push_back(Value::int_value_of(saturating_size_to_i64(stats->analyze_count)));
  values.push_back(Value::int_value_of(saturating_size_to_i64(stats->materialize_count)));
  values.push_back(Value::int_value_of(saturating_size_to_i64(stats->cache_hit_count)));
  values.push_back(Value::int_value_of(saturating_size_to_i64(stats->fused_count)));
  values.push_back(Value::int_value_of(saturating_size_to_i64(stats->fallback_count)));
  values.push_back(Value::int_value_of(saturating_size_to_i64(stats->last_allocations)));
  values.push_back(Value::int_value_of(saturating_size_to_i64(stats->total_allocations)));
  values.push_back(Value::int_value_of(static_cast<long long>(stats->last_plan)));
  return Value::list_value_of(std::move(values));
}

Value pipeline_plan_id_value(const std::shared_ptr<Environment>& env, const std::string& name) {
  const auto* stats = phase7::get_pipeline_stats_const(env, name);
  if (!stats) {
    return Value::int_value_of(static_cast<long long>(Value::LayoutTag::Unknown));
  }
  return Value::int_value_of(static_cast<long long>(stats->last_plan));
}

}  // namespace spark
