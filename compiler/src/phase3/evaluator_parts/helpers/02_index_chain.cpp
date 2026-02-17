#include <vector>

#include "../internal_helpers.h"

namespace spark {

IndexChain flatten_index_chain(const Expr& expr) {
  IndexChain out;
  std::vector<const IndexExpr*> chain;
  const Expr* current = &expr;
  while (current->kind == Expr::Kind::Index) {
    const auto& index_expr = static_cast<const IndexExpr&>(*current);
    chain.push_back(&index_expr);
    current = index_expr.target.get();
  }
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    for (const auto& item : (*it)->indices) {
      out.indices.push_back(&item);
    }
  }
  out.root = current;
  if (!out.root) {
    return {};
  }
  return out;
}

AssignmentRoot flatten_index_target(const Expr& expr) {
  const auto chain = flatten_index_chain(expr);
  if (!chain.root || chain.root->kind != Expr::Kind::Variable) {
    return {};
  }
  return {&static_cast<const VariableExpr&>(*chain.root), chain.indices};
}

}  // namespace spark
