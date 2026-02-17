#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {
namespace phase7 {

namespace {

bool should_intercept_chain(const PipelineChain& chain) {
  if (chain.stages.empty()) {
    return false;
  }
  if (chain.stages.size() == 1 &&
      (chain.stages.front().name == "sum" || chain.stages.front().name == "collect")) {
    return true;
  }
  if (chain.uses_phase7_only_stage) {
    return true;
  }
  // Keep legacy behavior for single-stage phase6 methods.
  return chain.stages.size() > 1;
}

}  // namespace

bool build_pipeline_chain(const CallExpr& call, Interpreter& self,
                          const std::shared_ptr<Environment>& env, PipelineChain& out) {
  std::vector<const CallExpr*> reversed_calls;
  const Expr* cursor = &call;
  while (cursor && cursor->kind == Expr::Kind::Call) {
    const auto& current_call = static_cast<const CallExpr&>(*cursor);
    if (!current_call.callee || current_call.callee->kind != Expr::Kind::Attribute) {
      break;
    }
    reversed_calls.push_back(&current_call);
    const auto& attr = static_cast<const AttributeExpr&>(*current_call.callee);
    cursor = attr.target.get();
  }

  if (reversed_calls.empty() || !cursor) {
    return false;
  }

  std::reverse(reversed_calls.begin(), reversed_calls.end());

  PipelineChain chain;
  chain.stages.reserve(reversed_calls.size());
  for (const auto* stage_call : reversed_calls) {
    const auto& attr = static_cast<const AttributeExpr&>(*stage_call->callee);
    const auto kind = stage_kind_from_name(attr.attribute);
    if (!kind.has_value()) {
      return false;
    }

    Stage stage;
    stage.kind = *kind;
    stage.name = attr.attribute;
    stage.args.reserve(stage_call->args.size());
    for (const auto& arg : stage_call->args) {
      stage.args.push_back(self.evaluate(*arg, env));
    }

    if (is_phase7_only_stage(stage.kind)) {
      chain.uses_phase7_only_stage = true;
    }
    chain.stages.push_back(std::move(stage));
  }

  if (!should_intercept_chain(chain)) {
    return false;
  }

  if (cursor->kind == Expr::Kind::Variable) {
    chain.base_name = static_cast<const VariableExpr&>(*cursor).name;
    chain.base_ptr = env->get_ptr(chain.base_name);
    if (!chain.base_ptr) {
      throw EvalException("undefined variable: " + chain.base_name);
    }
    chain.base_value = *chain.base_ptr;
  } else {
    chain.base_value = self.evaluate(*cursor, env);
  }

  out = std::move(chain);
  return true;
}

}  // namespace phase7
}  // namespace spark
