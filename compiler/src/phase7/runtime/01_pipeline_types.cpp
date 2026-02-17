#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {
namespace phase7 {

enum class StageKind {
  MapAdd,
  MapMul,
  FilterGt,
  FilterLt,
  FilterNonZero,
  ZipAdd,
  ReduceSum,
  ScanSum,
  ToList,
};

struct Stage {
  StageKind kind = StageKind::ToList;
  std::string name;
  std::vector<Value> args;
};

struct PipelineChain {
  std::string base_name;
  Value* base_ptr = nullptr;
  Value base_value = Value::nil();
  std::vector<Stage> stages;
  bool uses_phase7_only_stage = false;
};

struct PipelineStats {
  std::uint64_t cached_version = std::numeric_limits<std::uint64_t>::max();
  std::string signature;
  Value::LayoutTag last_plan = Value::LayoutTag::Unknown;
  std::string last_why_not_fused;
  std::vector<double> numeric_cache;
  std::size_t analyze_count = 0;
  std::size_t materialize_count = 0;
  std::size_t cache_hit_count = 0;
  std::size_t fused_count = 0;
  std::size_t fallback_count = 0;
  std::size_t last_allocations = 0;
  std::size_t total_allocations = 0;
};

std::optional<StageKind> stage_kind_from_name(const std::string_view name) {
  if (name == "map_add") {
    return StageKind::MapAdd;
  }
  if (name == "map_mul") {
    return StageKind::MapMul;
  }
  if (name == "filter_gt") {
    return StageKind::FilterGt;
  }
  if (name == "filter_lt") {
    return StageKind::FilterLt;
  }
  if (name == "filter_nonzero") {
    return StageKind::FilterNonZero;
  }
  if (name == "zip_add") {
    return StageKind::ZipAdd;
  }
  if (name == "reduce_sum" || name == "sum") {
    return StageKind::ReduceSum;
  }
  if (name == "scan_sum") {
    return StageKind::ScanSum;
  }
  if (name == "to_list" || name == "collect") {
    return StageKind::ToList;
  }
  return std::nullopt;
}

bool is_phase7_only_stage(const StageKind kind) {
  return kind == StageKind::MapMul || kind == StageKind::FilterGt ||
         kind == StageKind::FilterLt || kind == StageKind::FilterNonZero ||
         kind == StageKind::ZipAdd || kind == StageKind::ScanSum ||
         kind == StageKind::ToList;
}

bool is_terminal_stage(const StageKind kind) {
  return kind == StageKind::ReduceSum || kind == StageKind::ScanSum || kind == StageKind::ToList;
}

std::string stage_name(const StageKind kind) {
  switch (kind) {
    case StageKind::MapAdd:
      return "map_add";
    case StageKind::MapMul:
      return "map_mul";
    case StageKind::FilterGt:
      return "filter_gt";
    case StageKind::FilterLt:
      return "filter_lt";
    case StageKind::FilterNonZero:
      return "filter_nonzero";
    case StageKind::ZipAdd:
      return "zip_add";
    case StageKind::ReduceSum:
      return "reduce_sum";
    case StageKind::ScanSum:
      return "scan_sum";
    case StageKind::ToList:
      return "to_list";
  }
  return "unknown";
}

std::string pipeline_signature(const PipelineChain& chain) {
  std::string out;
  for (std::size_t i = 0; i < chain.stages.size(); ++i) {
    if (i > 0) {
      out += "->";
    }
    out += stage_name(chain.stages[i].kind);
  }
  return out;
}

bool is_numeric_value(const Value& value) {
  return value.kind == Value::Kind::Int || value.kind == Value::Kind::Double;
}

double numeric_value_as_double(const Value& value) {
  if (value.kind == Value::Kind::Int) {
    return static_cast<double>(value.int_value);
  }
  if (value.kind == Value::Kind::Double) {
    return value.double_value;
  }
  throw EvalException("pipeline expects numeric values");
}

bool is_integer_like(const double value) {
  const auto rounded = std::llround(value);
  return std::fabs(value - static_cast<double>(rounded)) <= 1e-12;
}

bool env_flag_enabled(const char* name, const bool default_value) {
  const auto* value = std::getenv(name);
  if (!value) {
    return default_value;
  }
  const std::string text = value;
  if (text.empty()) {
    return default_value;
  }
  if (text == "0" || text == "false" || text == "False" || text == "FALSE" ||
      text == "off" || text == "OFF" || text == "no" || text == "NO") {
    return false;
  }
  return true;
}

std::unordered_map<std::string, PipelineStats>& pipeline_stats_store() {
  static std::unordered_map<std::string, PipelineStats> store;
  return store;
}

std::string resolve_pipeline_key(const std::shared_ptr<Environment>& env, const std::string& name) {
  if (!env || name.empty()) {
    return "";
  }
  auto* current = env.get();
  while (current != nullptr) {
    if (current->values.find(name) != current->values.end()) {
      const auto id = reinterpret_cast<std::uintptr_t>(current);
      return std::to_string(id) + "::" + name;
    }
    current = current->parent.get();
  }
  return "";
}

PipelineStats* get_pipeline_stats(const std::shared_ptr<Environment>& env, const std::string& name) {
  const auto key = resolve_pipeline_key(env, name);
  if (key.empty()) {
    return nullptr;
  }
  auto& store = pipeline_stats_store();
  return &store[key];
}

const PipelineStats* get_pipeline_stats_const(const std::shared_ptr<Environment>& env,
                                              const std::string& name) {
  const auto key = resolve_pipeline_key(env, name);
  if (key.empty()) {
    return nullptr;
  }
  const auto& store = pipeline_stats_store();
  const auto it = store.find(key);
  if (it == store.end()) {
    return nullptr;
  }
  return &it->second;
}

bool build_pipeline_chain(const CallExpr& call, Interpreter& self,
                          const std::shared_ptr<Environment>& env, PipelineChain& out);
Value execute_list_pipeline(PipelineChain& chain, const std::shared_ptr<Environment>& env);
Value execute_matrix_pipeline(PipelineChain& chain, const std::shared_ptr<Environment>& env);

}  // namespace phase7
}  // namespace spark
