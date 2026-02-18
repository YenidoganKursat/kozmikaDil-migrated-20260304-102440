#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../internal_helpers.h"

namespace spark {

namespace {

bool env_bool_enabled_binary_expr(const char* name, bool fallback) {
  const auto* value = std::getenv(name);
  if (!value || *value == '\0') {
    return fallback;
  }
  const std::string text = value;
  if (text == "0" || text == "false" || text == "False" || text == "off" || text == "OFF" ||
      text == "no" || text == "NO") {
    return false;
  }
  return true;
}

bool is_container_arith_op(const BinaryOp op) {
  return op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul ||
         op == BinaryOp::Div || op == BinaryOp::Mod;
}

double apply_scalar_binary(const BinaryOp op, const double lhs, const double rhs) {
  switch (op) {
    case BinaryOp::Add:
      return lhs + rhs;
    case BinaryOp::Sub:
      return lhs - rhs;
    case BinaryOp::Mul:
      return lhs * rhs;
    case BinaryOp::Div:
      if (rhs == 0.0) {
        throw EvalException("division by zero");
      }
      return lhs / rhs;
    case BinaryOp::Mod:
      if (rhs == 0.0) {
        throw EvalException("modulo by zero");
      }
      return std::fmod(lhs, rhs);
    case BinaryOp::Eq:
    case BinaryOp::Ne:
    case BinaryOp::Lt:
    case BinaryOp::Lte:
    case BinaryOp::Gt:
    case BinaryOp::Gte:
    case BinaryOp::And:
    case BinaryOp::Or:
      break;
  }
  throw EvalException("unsupported fused arithmetic operator");
}

bool value_is_numeric_scalar(const Value& value) {
  return value.kind == Value::Kind::Int || value.kind == Value::Kind::Double;
}

double value_to_scalar_double(const Value& value) {
  if (value.kind == Value::Kind::Int) {
    return static_cast<double>(value.int_value);
  }
  if (value.kind == Value::Kind::Double) {
    return value.double_value;
  }
  throw EvalException("expected numeric scalar");
}

enum class DenseContainerKind { List, Matrix };

struct DenseContainer {
  const Value* value = nullptr;
  const std::vector<double>* dense = nullptr;
  std::vector<double> scratch;
};

struct FusedNode {
  enum class Kind { Constant, Container, Binary };

  Kind kind = Kind::Constant;
  double constant = 0.0;
  std::size_t container_index = 0;
  BinaryOp op = BinaryOp::Add;
  int left = -1;
  int right = -1;
};

struct BuildResult {
  bool ok = false;
  bool produces_container = false;
  int node_index = -1;
};

struct BuildContext {
  std::optional<DenseContainerKind> container_kind;
  std::vector<DenseContainer> containers;
  std::vector<FusedNode> nodes;
  std::unordered_map<std::string, std::size_t> container_index_by_name;
  bool list_add_elementwise = false;
};

int append_constant(BuildContext& context, const double value) {
  FusedNode node;
  node.kind = FusedNode::Kind::Constant;
  node.constant = value;
  context.nodes.push_back(node);
  return static_cast<int>(context.nodes.size() - 1);
}

int append_container(BuildContext& context, const std::size_t index) {
  FusedNode node;
  node.kind = FusedNode::Kind::Container;
  node.container_index = index;
  context.nodes.push_back(node);
  return static_cast<int>(context.nodes.size() - 1);
}

int append_binary(BuildContext& context, const BinaryOp op, const int left, const int right) {
  FusedNode node;
  node.kind = FusedNode::Kind::Binary;
  node.op = op;
  node.left = left;
  node.right = right;
  context.nodes.push_back(node);
  return static_cast<int>(context.nodes.size() - 1);
}

bool node_is_constant(const BuildContext& context, const int index, double& out) {
  if (index < 0 || static_cast<std::size_t>(index) >= context.nodes.size()) {
    return false;
  }
  const auto& node = context.nodes[static_cast<std::size_t>(index)];
  if (node.kind != FusedNode::Kind::Constant) {
    return false;
  }
  out = node.constant;
  return true;
}

BuildResult build_fused_tree(const Expr& expr, Interpreter& self,
                             const std::shared_ptr<Environment>& env,
                             BuildContext& context) {
  switch (expr.kind) {
    case Expr::Kind::Number: {
      const auto& number = static_cast<const NumberExpr&>(expr);
      return BuildResult{
          .ok = true,
          .produces_container = false,
          .node_index = append_constant(context, number.value),
      };
    }
    case Expr::Kind::Variable: {
      const auto& variable = static_cast<const VariableExpr&>(expr);
      const auto* value = env ? env->get_ptr(variable.name) : nullptr;
      if (!value) {
        throw EvalException("undefined variable: " + variable.name);
      }
      if (value_is_numeric_scalar(*value)) {
        return BuildResult{
            .ok = true,
            .produces_container = false,
            .node_index = append_constant(context, value_to_scalar_double(*value)),
        };
      }
      if (value->kind != Value::Kind::List && value->kind != Value::Kind::Matrix) {
        return BuildResult{};
      }

      const auto kind = (value->kind == Value::Kind::List) ? DenseContainerKind::List
                                                            : DenseContainerKind::Matrix;
      if (!context.container_kind.has_value()) {
        context.container_kind = kind;
      } else if (context.container_kind != kind) {
        return BuildResult{};
      }

      auto it = context.container_index_by_name.find(variable.name);
      std::size_t index = 0;
      if (it != context.container_index_by_name.end()) {
        index = it->second;
      } else {
        index = context.containers.size();
        context.containers.push_back(DenseContainer{.value = value});
        context.container_index_by_name.emplace(variable.name, index);
      }
      return BuildResult{
          .ok = true,
          .produces_container = true,
          .node_index = append_container(context, index),
      };
    }
    case Expr::Kind::Unary: {
      const auto& unary = static_cast<const UnaryExpr&>(expr);
      if (unary.op != UnaryOp::Neg) {
        return BuildResult{};
      }
      auto child = build_fused_tree(*unary.operand, self, env, context);
      if (!child.ok) {
        return BuildResult{};
      }
      if (!child.produces_container) {
        double value = 0.0;
        if (!node_is_constant(context, child.node_index, value)) {
          return BuildResult{};
        }
        return BuildResult{
            .ok = true,
            .produces_container = false,
            .node_index = append_constant(context, -value),
        };
      }

      const auto zero_index = append_constant(context, 0.0);
      return BuildResult{
          .ok = true,
          .produces_container = true,
          .node_index = append_binary(context, BinaryOp::Sub, zero_index, child.node_index),
      };
    }
    case Expr::Kind::Binary: {
      const auto& binary = static_cast<const BinaryExpr&>(expr);
      if (!is_container_arith_op(binary.op)) {
        return BuildResult{};
      }

      auto left = build_fused_tree(*binary.left, self, env, context);
      if (!left.ok) {
        return BuildResult{};
      }
      auto right = build_fused_tree(*binary.right, self, env, context);
      if (!right.ok) {
        return BuildResult{};
      }

      if (!left.produces_container && !right.produces_container) {
        double lhs = 0.0;
        double rhs = 0.0;
        if (!node_is_constant(context, left.node_index, lhs) ||
            !node_is_constant(context, right.node_index, rhs)) {
          return BuildResult{};
        }
        return BuildResult{
            .ok = true,
            .produces_container = false,
            .node_index = append_constant(context, apply_scalar_binary(binary.op, lhs, rhs)),
        };
      }

      if (!context.container_kind.has_value()) {
        return BuildResult{};
      }
      if (*context.container_kind == DenseContainerKind::List &&
          left.produces_container && right.produces_container &&
          binary.op == BinaryOp::Add && !context.list_add_elementwise) {
        return BuildResult{};
      }
      if (*context.container_kind == DenseContainerKind::Matrix &&
          left.produces_container && right.produces_container &&
          binary.op == BinaryOp::Mul) {
        // matrix * matrix means matmul in this language, not elementwise.
        return BuildResult{};
      }

      return BuildResult{
          .ok = true,
          .produces_container = true,
          .node_index = append_binary(context, binary.op, left.node_index, right.node_index),
      };
    }
    case Expr::Kind::Bool:
    case Expr::Kind::List:
    case Expr::Kind::Attribute:
    case Expr::Kind::Call:
    case Expr::Kind::Index:
      break;
  }
  return BuildResult{};
}

std::size_t list_size_for_dense(const Value& value) {
  if (value.kind != Value::Kind::List) {
    return 0;
  }
  if (!value.list_value.empty()) {
    return value.list_value.size();
  }
  if (value.list_cache.materialized_version == value.list_cache.version &&
      !value.list_cache.promoted_f64.empty()) {
    return value.list_cache.promoted_f64.size();
  }
  return 0;
}

const std::vector<double>* dense_list_if_materialized(const Value& value, const std::size_t expected_size) {
  if (value.kind != Value::Kind::List) {
    return nullptr;
  }
  const auto& cache = value.list_cache;
  const auto cache_ready =
      cache.materialized_version == cache.version &&
      (cache.plan == Value::LayoutTag::PackedDouble || cache.plan == Value::LayoutTag::PromotedPackedDouble);
  if (!cache_ready) {
    return nullptr;
  }
  if (cache.promoted_f64.size() != expected_size) {
    return nullptr;
  }
  return &cache.promoted_f64;
}

const std::vector<double>* dense_matrix_if_materialized(const Value& value, const std::size_t expected_size) {
  if (value.kind != Value::Kind::Matrix || !value.matrix_value) {
    return nullptr;
  }
  const auto& cache = value.matrix_cache;
  const auto cache_ready =
      cache.materialized_version == cache.version &&
      cache.plan == Value::LayoutTag::PackedDouble;
  if (!cache_ready) {
    return nullptr;
  }
  if (cache.promoted_f64.size() != expected_size) {
    return nullptr;
  }
  return &cache.promoted_f64;
}

bool is_integer_like(const double value) {
  constexpr double kTol = 1e-12;
  const auto rounded = std::llround(value);
  return std::fabs(value - static_cast<double>(rounded)) <= kTol;
}

bool matrix_source_is_integral(const Value& value) {
  if (value.kind != Value::Kind::Matrix || !value.matrix_value) {
    return false;
  }
  const auto total = value.matrix_value->rows * value.matrix_value->cols;
  if (const auto* dense = dense_matrix_if_materialized(value, total)) {
    for (const auto entry : *dense) {
      if (!is_integer_like(entry)) {
        return false;
      }
    }
    return true;
  }
  const auto& data = value.matrix_value->data;
  if (data.size() != total) {
    return false;
  }
  for (const auto& cell : data) {
    if (cell.kind == Value::Kind::Int) {
      continue;
    }
    if (cell.kind == Value::Kind::Double && is_integer_like(cell.double_value)) {
      continue;
    }
    return false;
  }
  return true;
}

bool fused_matrix_prefers_int_output(const BuildContext& context) {
  for (const auto& node : context.nodes) {
    if (node.kind == FusedNode::Kind::Binary &&
        (node.op == BinaryOp::Div || node.op == BinaryOp::Mod)) {
      return false;
    }
    if (node.kind == FusedNode::Kind::Constant && !is_integer_like(node.constant)) {
      return false;
    }
  }
  for (const auto& container : context.containers) {
    if (!container.value || !matrix_source_is_integral(*container.value)) {
      return false;
    }
  }
  return true;
}

Value make_list_from_dense(std::vector<double>&& dense, const std::optional<double> precomputed_sum) {
  const auto size = dense.size();
  const bool dense_only_enabled = env_bool_enabled_binary_expr("SPARK_LIST_OPS_DENSE_ONLY", false);
  std::size_t dense_only_min = 32u * 1024u;
  if (const auto* min_env = std::getenv("SPARK_LIST_OPS_DENSE_ONLY_MIN")) {
    const auto parsed = std::strtoull(min_env, nullptr, 10);
    if (parsed > 0) {
      dense_only_min = static_cast<std::size_t>(parsed);
    }
  }

  std::vector<Value> out_data;
  if (!(dense_only_enabled && size >= dense_only_min)) {
    out_data.resize(size);
    for (std::size_t i = 0; i < size; ++i) {
      out_data[i] = Value::double_value_of(dense[i]);
    }
  }

  auto out = Value::list_value_of(std::move(out_data));
  out.list_cache.live_plan = true;
  out.list_cache.plan = Value::LayoutTag::PackedDouble;
  out.list_cache.operation = "binary_fused";
  out.list_cache.analyzed_version = out.list_cache.version;
  out.list_cache.materialized_version = out.list_cache.version;
  out.list_cache.promoted_f64 = std::move(dense);
  if (precomputed_sum.has_value()) {
    out.list_cache.reduced_sum_version = out.list_cache.version;
    out.list_cache.reduced_sum_value = *precomputed_sum;
    out.list_cache.reduced_sum_is_int = false;
  } else {
    out.list_cache.reduced_sum_version = std::numeric_limits<std::uint64_t>::max();
    out.list_cache.reduced_sum_value = 0.0;
    out.list_cache.reduced_sum_is_int = false;
  }
  return out;
}

Value make_matrix_from_dense(std::size_t rows, std::size_t cols, std::vector<double>&& dense,
                             const std::optional<double> precomputed_sum) {
  const auto total = rows * cols;
  const bool dense_only_enabled = env_bool_enabled_binary_expr("SPARK_MATRIX_OPS_DENSE_ONLY", false);
  std::size_t dense_only_min = 16u * 1024u;
  if (const auto* min_env = std::getenv("SPARK_MATRIX_OPS_DENSE_ONLY_MIN")) {
    const auto parsed = std::strtoull(min_env, nullptr, 10);
    if (parsed > 0) {
      dense_only_min = static_cast<std::size_t>(parsed);
    }
  }

  std::vector<Value> out_data;
  if (!(dense_only_enabled && total >= dense_only_min)) {
    out_data.resize(total);
    for (std::size_t i = 0; i < total; ++i) {
      out_data[i] = Value::double_value_of(dense[i]);
    }
  }

  auto out = Value::matrix_value_of(rows, cols, std::move(out_data));
  out.matrix_cache.plan = Value::LayoutTag::PackedDouble;
  out.matrix_cache.live_plan = true;
  out.matrix_cache.operation = "binary_fused";
  out.matrix_cache.analyzed_version = out.matrix_cache.version;
  out.matrix_cache.materialized_version = out.matrix_cache.version;
  if (precomputed_sum.has_value()) {
    out.matrix_cache.reduced_sum_version = out.matrix_cache.version;
    out.matrix_cache.reduced_sum_value = *precomputed_sum;
    out.matrix_cache.reduced_sum_is_int = false;
  } else {
    out.matrix_cache.reduced_sum_version = std::numeric_limits<std::uint64_t>::max();
    out.matrix_cache.reduced_sum_value = 0.0;
    out.matrix_cache.reduced_sum_is_int = false;
  }
  out.matrix_cache.promoted_f64 = std::move(dense);
  return out;
}

std::optional<Value> try_fused_container_eval(const BinaryExpr& binary, Interpreter& self,
                                              const std::shared_ptr<Environment>& env) {
  BuildContext context;
  context.list_add_elementwise = env_bool_enabled_binary_expr("SPARK_LIST_ADD_ELEMENTWISE", false);
  auto root = build_fused_tree(binary, self, env, context);
  if (!root.ok || !root.produces_container || context.containers.empty() ||
      !context.container_kind.has_value()) {
    return std::nullopt;
  }

  std::size_t total = 0;
  std::size_t rows = 0;
  std::size_t cols = 0;

  if (*context.container_kind == DenseContainerKind::List) {
    const auto* base = context.containers.front().value;
    if (!base) {
      return std::nullopt;
    }
    total = list_size_for_dense(*base);
    for (auto& container : context.containers) {
      if (!container.value || container.value->kind != Value::Kind::List) {
        return std::nullopt;
      }
      const auto size = list_size_for_dense(*container.value);
      if (size != total) {
        throw EvalException("list elementwise arithmetic expects equal sizes");
      }
      if (const auto* dense = dense_list_if_materialized(*container.value, total)) {
        container.dense = dense;
        continue;
      }
      if (container.value->list_value.size() != total) {
        return std::nullopt;
      }
      container.scratch.resize(total);
      for (std::size_t i = 0; i < total; ++i) {
        const auto& item = container.value->list_value[i];
        if (!value_is_numeric_scalar(item)) {
          throw EvalException("list arithmetic expects numeric list elements");
        }
        container.scratch[i] = value_to_scalar_double(item);
      }
      container.dense = &container.scratch;
    }
  } else {
    const auto* base = context.containers.front().value;
    if (!base || !base->matrix_value) {
      return std::nullopt;
    }
    rows = base->matrix_value->rows;
    cols = base->matrix_value->cols;
    total = rows * cols;
    for (auto& container : context.containers) {
      if (!container.value || container.value->kind != Value::Kind::Matrix ||
          !container.value->matrix_value) {
        return std::nullopt;
      }
      if (container.value->matrix_value->rows != rows || container.value->matrix_value->cols != cols) {
        throw EvalException("matrix shapes must match for elementwise arithmetic");
      }
      if (const auto* dense = dense_matrix_if_materialized(*container.value, total)) {
        container.dense = dense;
        continue;
      }
      const auto& data = container.value->matrix_value->data;
      if (data.size() != total) {
        throw EvalException("matrix arithmetic requires materialized matrix payload");
      }
      container.scratch.resize(total);
      for (std::size_t i = 0; i < total; ++i) {
        if (!value_is_numeric_scalar(data[i])) {
          throw EvalException("matrix arithmetic expects numeric matrix cells");
        }
        container.scratch[i] = value_to_scalar_double(data[i]);
      }
      container.dense = &container.scratch;
    }
  }

  std::vector<double> stack(context.nodes.size(), 0.0);
  std::vector<int> dynamic_nodes;
  dynamic_nodes.reserve(context.nodes.size());
  for (std::size_t idx = 0; idx < context.nodes.size(); ++idx) {
    const auto& node = context.nodes[idx];
    if (node.kind == FusedNode::Kind::Constant) {
      stack[idx] = node.constant;
      continue;
    }
    dynamic_nodes.push_back(static_cast<int>(idx));
  }

  std::vector<double> out_dense(total, 0.0);
  double out_sum = 0.0;
  for (std::size_t i = 0; i < total; ++i) {
    for (const auto idx : dynamic_nodes) {
      const auto& node = context.nodes[static_cast<std::size_t>(idx)];
      if (node.kind == FusedNode::Kind::Container) {
        const auto* dense = context.containers[node.container_index].dense;
        if (!dense) {
          return std::nullopt;
        }
        stack[static_cast<std::size_t>(idx)] = (*dense)[i];
        continue;
      }
      const auto lhs = stack[static_cast<std::size_t>(node.left)];
      const auto rhs = stack[static_cast<std::size_t>(node.right)];
      stack[static_cast<std::size_t>(idx)] = apply_scalar_binary(node.op, lhs, rhs);
    }
    const auto value = stack[static_cast<std::size_t>(root.node_index)];
    out_dense[i] = value;
    out_sum += value;
  }

  if (*context.container_kind == DenseContainerKind::List) {
    return make_list_from_dense(std::move(out_dense), out_sum);
  }
  if (fused_matrix_prefers_int_output(context)) {
    std::vector<Value> out_data(total);
    for (std::size_t i = 0; i < total; ++i) {
      out_data[i] = Value::int_value_of(static_cast<long long>(std::llround(out_dense[i])));
    }
    return Value::matrix_value_of(rows, cols, std::move(out_data));
  }
  return make_matrix_from_dense(rows, cols, std::move(out_dense), out_sum);
}

}  // namespace

Value evaluate_case_binary(const BinaryExpr& binary, Interpreter& self,
                           const std::shared_ptr<Environment>& env) {
  if (env_bool_enabled_binary_expr("SPARK_BINARY_EXPR_FUSION", false) &&
      is_container_arith_op(binary.op)) {
    if (const auto fused = try_fused_container_eval(binary, self, env); fused.has_value()) {
      return *fused;
    }
  }

  auto lhs = self.evaluate(*binary.left, env);
  auto rhs = self.evaluate(*binary.right, env);
  return self.eval_binary(binary.op, lhs, rhs);
}

}  // namespace spark
