#include <algorithm>

#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {
namespace phase7 {

namespace {

struct RuntimeStage {
  StageKind kind = StageKind::ToList;
  double scalar = 0.0;
  std::vector<double> zip_values;
};

bool is_list_stage(const StageKind kind) {
  switch (kind) {
    case StageKind::MapAdd:
    case StageKind::MapMul:
    case StageKind::FilterGt:
    case StageKind::FilterLt:
    case StageKind::FilterNonZero:
    case StageKind::ZipAdd:
    case StageKind::ReduceSum:
    case StageKind::ScanSum:
    case StageKind::ToList:
      return true;
  }
  return false;
}

bool plan_requires_cache(const Value::LayoutTag plan) {
  return plan == Value::LayoutTag::PromotedPackedDouble ||
         plan == Value::LayoutTag::GatherScatter ||
         plan == Value::LayoutTag::ChunkedUnion;
}

bool is_terminal_compatible_with_hetero_plan(const StageKind terminal) {
  return terminal == StageKind::ReduceSum;
}

double expect_numeric_arg(const Stage& stage, const std::size_t index = 0) {
  if (stage.args.size() <= index) {
    throw EvalException(stage.name + "() missing required numeric argument");
  }
  if (!is_numeric_value(stage.args[index])) {
    throw EvalException(stage.name + "() expects numeric argument");
  }
  return numeric_value_as_double(stage.args[index]);
}

std::vector<double> expect_zip_arg(const Stage& stage) {
  if (stage.args.size() != 1) {
    throw EvalException("zip_add() expects exactly one list argument");
  }
  if (stage.args[0].kind != Value::Kind::List) {
    throw EvalException("zip_add() expects a list argument");
  }
  std::vector<double> values;
  values.reserve(stage.args[0].list_value.size());
  for (const auto& item : stage.args[0].list_value) {
    if (!is_numeric_value(item)) {
      throw EvalException("zip_add() list argument must contain only numeric values");
    }
    values.push_back(numeric_value_as_double(item));
  }
  return values;
}

std::vector<RuntimeStage> build_runtime_stages(const PipelineChain& chain,
                                               bool& has_value_transform,
                                               bool& has_zip) {
  std::vector<RuntimeStage> out;
  out.reserve(chain.stages.size());
  has_value_transform = false;
  has_zip = false;

  bool seen_terminal = false;
  for (std::size_t i = 0; i < chain.stages.size(); ++i) {
    const auto& stage = chain.stages[i];
    if (!is_list_stage(stage.kind)) {
      throw EvalException(stage.name + "() is not supported on list pipeline");
    }
    if (seen_terminal) {
      throw EvalException("pipeline terminal stage must be the last stage");
    }

    RuntimeStage runtime;
    runtime.kind = stage.kind;
    switch (stage.kind) {
      case StageKind::MapAdd:
      case StageKind::MapMul:
      case StageKind::FilterGt:
      case StageKind::FilterLt:
        runtime.scalar = expect_numeric_arg(stage);
        break;
      case StageKind::FilterNonZero:
        if (!stage.args.empty()) {
          throw EvalException(stage.name + "() expects no arguments");
        }
        break;
      case StageKind::ZipAdd:
        runtime.zip_values = expect_zip_arg(stage);
        has_zip = true;
        has_value_transform = true;
        break;
      case StageKind::ReduceSum:
      case StageKind::ScanSum:
      case StageKind::ToList:
        if (!stage.args.empty()) {
          throw EvalException(stage.name + "() expects no arguments");
        }
        seen_terminal = true;
        break;
    }

    if (stage.kind == StageKind::MapAdd || stage.kind == StageKind::MapMul) {
      has_value_transform = true;
    }
    out.push_back(std::move(runtime));
  }
  return out;
}

bool apply_runtime_stage(const RuntimeStage& stage, const std::size_t index,
                         double& value, bool& keep) {
  switch (stage.kind) {
    case StageKind::MapAdd:
      value += stage.scalar;
      return true;
    case StageKind::MapMul:
      value *= stage.scalar;
      return true;
    case StageKind::FilterGt:
      keep = keep && (value > stage.scalar);
      return true;
    case StageKind::FilterLt:
      keep = keep && (value < stage.scalar);
      return true;
    case StageKind::FilterNonZero:
      keep = keep && (value != 0.0);
      return true;
    case StageKind::ZipAdd:
      if (index >= stage.zip_values.size()) {
        keep = false;
        return true;
      }
      value += stage.zip_values[index];
      return true;
    case StageKind::ReduceSum:
    case StageKind::ScanSum:
    case StageKind::ToList:
      return false;
  }
  return false;
}

std::size_t infer_effective_length(const std::vector<RuntimeStage>& stages,
                                   const std::size_t source_size) {
  std::size_t out = source_size;
  for (const auto& stage : stages) {
    if (stage.kind == StageKind::ZipAdd) {
      out = std::min(out, stage.zip_values.size());
    }
  }
  return out;
}

bool materialize_numeric_cache_for_plan(const Value& source,
                                        const Value::LayoutTag plan,
                                        std::vector<double>& out) {
  out.clear();
  if (plan == Value::LayoutTag::PromotedPackedDouble) {
    out.reserve(source.list_value.size());
    for (const auto& item : source.list_value) {
      if (!is_numeric_value(item)) {
        return false;
      }
      out.push_back(numeric_value_as_double(item));
    }
    return true;
  }

  if (plan == Value::LayoutTag::GatherScatter || plan == Value::LayoutTag::ChunkedUnion) {
    out.reserve(source.list_value.size());
    for (const auto& item : source.list_value) {
      if (!is_numeric_value(item)) {
        continue;
      }
      out.push_back(numeric_value_as_double(item));
    }
    return true;
  }

  return true;
}

Value materialize_non_fused_list(const Value& source,
                                 const std::vector<RuntimeStage>& stages,
                                 const StageKind terminal,
                                 std::size_t& allocations) {
  std::vector<Value> current = source.list_value;

  auto run_transforms = [&](const std::size_t stop_at) {
    for (std::size_t i = 0; i < stop_at; ++i) {
      const auto& stage = stages[i];
      if (stage.kind == StageKind::ToList || stage.kind == StageKind::ReduceSum ||
          stage.kind == StageKind::ScanSum) {
        continue;
      }

      std::vector<Value> next;
      if (stage.kind == StageKind::ZipAdd) {
        const auto limit = std::min(current.size(), stage.zip_values.size());
        next.reserve(limit);
        for (std::size_t idx = 0; idx < limit; ++idx) {
          if (!is_numeric_value(current[idx])) {
            throw EvalException("zip_add() receiver must contain only numeric values");
          }
          next.push_back(Value::double_value_of(numeric_value_as_double(current[idx]) + stage.zip_values[idx]));
        }
        current = std::move(next);
        ++allocations;
        continue;
      }

      next.reserve(current.size());
      for (const auto& item : current) {
        if (!is_numeric_value(item)) {
          if (stage.kind == StageKind::MapAdd || stage.kind == StageKind::MapMul) {
            next.push_back(item);
          }
          continue;
        }
        auto value = numeric_value_as_double(item);
        bool keep = true;
        apply_runtime_stage(stage, 0, value, keep);
        if (!keep) {
          continue;
        }
        if (stage.kind == StageKind::FilterGt || stage.kind == StageKind::FilterLt ||
            stage.kind == StageKind::FilterNonZero) {
          next.push_back(item);
          continue;
        }
        next.push_back(Value::double_value_of(value));
      }
      current = std::move(next);
      ++allocations;
    }
  };

  const std::size_t transform_count = (terminal == StageKind::ToList && !stages.empty() &&
                                       !is_terminal_stage(stages.back().kind))
                                          ? stages.size()
                                          : (stages.empty() ? 0 : stages.size() - 1);
  run_transforms(transform_count);

  if (terminal == StageKind::ReduceSum) {
    auto tmp = Value::list_value_of(std::move(current));
    return list_reduce_sum_with_plan(tmp);
  }

  if (terminal == StageKind::ScanSum) {
    std::vector<Value> prefix;
    prefix.reserve(current.size());
    double running = 0.0;
    for (const auto& item : current) {
      if (!is_numeric_value(item)) {
        continue;
      }
      running += numeric_value_as_double(item);
      prefix.push_back(Value::double_value_of(running));
    }
    ++allocations;
    return Value::list_value_of(std::move(prefix));
  }

  return Value::list_value_of(std::move(current));
}

}  // namespace

Value execute_list_pipeline(PipelineChain& chain, const std::shared_ptr<Environment>& env) {
  // Keep a read-only view on the receiver to avoid copying large containers.
  const Value& source = chain.base_ptr ? *chain.base_ptr : chain.base_value;
  if (source.kind != Value::Kind::List) {
    throw EvalException("pipeline receiver must be a list");
  }

  bool has_value_transform = false;
  bool has_zip = false;
  const auto runtime_stages = build_runtime_stages(chain, has_value_transform, has_zip);

  StageKind terminal = StageKind::ToList;
  std::size_t transform_count = runtime_stages.size();
  if (!runtime_stages.empty() && is_terminal_stage(runtime_stages.back().kind)) {
    terminal = runtime_stages.back().kind;
    transform_count = runtime_stages.size() - 1;
  }

  const auto signature = pipeline_signature(chain);
  const std::string plan_operation =
      (terminal == StageKind::ReduceSum) ? "reduce_sum" : "map_add";
  const auto plan = choose_list_plan(source, plan_operation);
  if (chain.base_ptr) {
    chain.base_ptr->list_cache.live_plan = true;
    chain.base_ptr->list_cache.plan = plan;
    chain.base_ptr->list_cache.operation = "pipeline:" + signature;
    chain.base_ptr->list_cache.analyzed_version = chain.base_ptr->list_cache.version;
  }

  auto* stats = get_pipeline_stats(env, chain.base_name);
  if (stats) {
    if (stats->signature == signature &&
        stats->cached_version == source.list_cache.version) {
      stats->cache_hit_count += 1;
    } else {
      stats->analyze_count += 1;
      stats->signature = signature;
      stats->cached_version = source.list_cache.version;
      stats->numeric_cache.clear();
    }
    stats->last_plan = plan;
    stats->last_why_not_fused.clear();
  }

  bool can_fuse = env_flag_enabled("SPARK_PIPELINE_FUSION", true);
  std::string why_not_fused;

  if (!can_fuse) {
    why_not_fused = "fusion disabled by SPARK_PIPELINE_FUSION";
  }
  if (can_fuse && plan == Value::LayoutTag::BoxedAny) {
    can_fuse = false;
    why_not_fused = "boxed Any layout requires materialization";
  }
  if (can_fuse &&
      (plan == Value::LayoutTag::GatherScatter || plan == Value::LayoutTag::ChunkedUnion) &&
      !is_terminal_compatible_with_hetero_plan(terminal)) {
    can_fuse = false;
    why_not_fused = "heterogeneous layout cannot fuse order-sensitive terminal";
  }
  if (can_fuse && has_zip && plan_requires_cache(plan)) {
    can_fuse = false;
    why_not_fused = "zip_add cannot fuse on cached heterogeneous layout";
  }

  std::size_t allocations = 0;
  if (!can_fuse) {
    if (stats) {
      stats->fallback_count += 1;
      stats->last_why_not_fused = why_not_fused;
      stats->last_allocations = 0;
    }
    auto out = materialize_non_fused_list(source, runtime_stages, terminal, allocations);
    if (stats) {
      stats->last_allocations = allocations;
      stats->total_allocations += allocations;
    }
    return out;
  }

  std::vector<double> numeric_cache;
  if (plan_requires_cache(plan)) {
    if (stats && !stats->numeric_cache.empty() &&
        stats->signature == signature &&
        stats->cached_version == source.list_cache.version) {
      numeric_cache = stats->numeric_cache;
    } else {
      if (!materialize_numeric_cache_for_plan(source, plan, numeric_cache)) {
        throw EvalException("pipeline normalization failed: non-numeric element encountered");
      }
      if (stats) {
        stats->materialize_count += 1;
        stats->numeric_cache = numeric_cache;
      }
    }
  }

  if (stats) {
    stats->fused_count += 1;
    stats->last_allocations = 0;
  }

  const auto effective_length = infer_effective_length(
      runtime_stages,
      plan_requires_cache(plan) ? numeric_cache.size() : source.list_value.size());

  const bool packed_int_source = plan == Value::LayoutTag::PackedInt && !plan_requires_cache(plan);
  const bool packed_double_source = plan == Value::LayoutTag::PackedDouble && !plan_requires_cache(plan);

  if (terminal == StageKind::ReduceSum && transform_count == 0 && packed_int_source) {
    long long total = 0;
    for (const auto& item : source.list_value) {
      total += item.int_value;
    }
    if (stats) {
      stats->last_allocations = allocations;
      stats->total_allocations += allocations;
    }
    return Value::int_value_of(total);
  }

  const bool emit_int_reduce =
      terminal == StageKind::ReduceSum && plan == Value::LayoutTag::PackedInt &&
      !has_value_transform && !plan_requires_cache(plan);

  const auto source_sum = [&]() {
    double sum = 0.0;
    if (plan_requires_cache(plan)) {
      for (std::size_t i = 0; i < effective_length; ++i) {
        sum += numeric_cache[i];
      }
      return sum;
    }
    if (packed_int_source) {
      for (std::size_t i = 0; i < effective_length; ++i) {
        sum += static_cast<double>(source.list_value[i].int_value);
      }
      return sum;
    }
    if (packed_double_source) {
      for (std::size_t i = 0; i < effective_length; ++i) {
        sum += source.list_value[i].double_value;
      }
      return sum;
    }
    for (std::size_t i = 0; i < effective_length; ++i) {
      if (!is_numeric_value(source.list_value[i])) {
        continue;
      }
      sum += numeric_value_as_double(source.list_value[i]);
    }
    return sum;
  };

  if (terminal == StageKind::ReduceSum && transform_count == 1 &&
      runtime_stages[0].kind == StageKind::MapAdd) {
    const double add = runtime_stages[0].scalar;
    const double reduce_sum = source_sum() + add * static_cast<double>(effective_length);
    if (stats) {
      stats->last_allocations = allocations;
      stats->total_allocations += allocations;
    }
    return Value::double_value_of(reduce_sum);
  }

  if (terminal == StageKind::ReduceSum && transform_count == 1 &&
      runtime_stages[0].kind == StageKind::MapMul) {
    const double mul = runtime_stages[0].scalar;
    const double reduce_sum = source_sum() * mul;
    if (stats) {
      stats->last_allocations = allocations;
      stats->total_allocations += allocations;
    }
    return Value::double_value_of(reduce_sum);
  }

  if (terminal == StageKind::ReduceSum && transform_count == 2 &&
      runtime_stages[0].kind == StageKind::MapAdd &&
      runtime_stages[1].kind == StageKind::MapMul) {
    const double add = runtime_stages[0].scalar;
    const double mul = runtime_stages[1].scalar;
    const double reduce_sum =
        (source_sum() + add * static_cast<double>(effective_length)) * mul;
    if (stats) {
      stats->last_allocations = allocations;
      stats->total_allocations += allocations;
    }
    return Value::double_value_of(reduce_sum);
  }

  if (terminal == StageKind::ReduceSum && transform_count == 3 &&
      runtime_stages[0].kind == StageKind::MapAdd &&
      (runtime_stages[1].kind == StageKind::FilterGt ||
       runtime_stages[1].kind == StageKind::FilterLt) &&
      runtime_stages[2].kind == StageKind::MapMul) {
    const double add = runtime_stages[0].scalar;
    const double threshold = runtime_stages[1].scalar;
    const bool is_gt = runtime_stages[1].kind == StageKind::FilterGt;
    const double mul = runtime_stages[2].scalar;
    double reduce_sum = 0.0;

    const auto maybe_accumulate = [&](double value) {
      const auto mapped = value + add;
      const bool pass = is_gt ? (mapped > threshold) : (mapped < threshold);
      if (pass) {
        reduce_sum += mapped * mul;
      }
    };

    if (plan_requires_cache(plan)) {
      for (std::size_t i = 0; i < effective_length; ++i) {
        maybe_accumulate(numeric_cache[i]);
      }
    } else if (packed_int_source) {
      for (std::size_t i = 0; i < effective_length; ++i) {
        maybe_accumulate(static_cast<double>(source.list_value[i].int_value));
      }
    } else if (packed_double_source) {
      for (std::size_t i = 0; i < effective_length; ++i) {
        maybe_accumulate(source.list_value[i].double_value);
      }
    } else {
      for (std::size_t i = 0; i < effective_length; ++i) {
        if (!is_numeric_value(source.list_value[i])) {
          continue;
        }
        maybe_accumulate(numeric_value_as_double(source.list_value[i]));
      }
    }
    if (stats) {
      stats->last_allocations = allocations;
      stats->total_allocations += allocations;
    }
    return Value::double_value_of(reduce_sum);
  }

  double reduce_sum = 0.0;
  std::vector<Value> list_out;
  if (terminal == StageKind::ToList || terminal == StageKind::ScanSum ||
      (!runtime_stages.empty() && !is_terminal_stage(runtime_stages.back().kind))) {
    list_out.reserve(effective_length);
    allocations += 1;
  }

  const auto apply_transforms = [&](const std::size_t index, double& value) {
    bool keep = true;
    if (transform_count == 0) {
      return keep;
    }
    if (transform_count == 1) {
      apply_runtime_stage(runtime_stages[0], index, value, keep);
      return keep;
    }
    if (transform_count == 2) {
      apply_runtime_stage(runtime_stages[0], index, value, keep);
      if (!keep) {
        return false;
      }
      apply_runtime_stage(runtime_stages[1], index, value, keep);
      return keep;
    }
    if (transform_count == 3) {
      apply_runtime_stage(runtime_stages[0], index, value, keep);
      if (!keep) {
        return false;
      }
      apply_runtime_stage(runtime_stages[1], index, value, keep);
      if (!keep) {
        return false;
      }
      apply_runtime_stage(runtime_stages[2], index, value, keep);
      return keep;
    }
    for (std::size_t stage_index = 0; stage_index < transform_count; ++stage_index) {
      apply_runtime_stage(runtime_stages[stage_index], index, value, keep);
      if (!keep) {
        return false;
      }
    }
    return true;
  };

  double running_prefix = 0.0;
  for (std::size_t i = 0; i < effective_length; ++i) {
    double value = 0.0;
    if (plan_requires_cache(plan)) {
      value = numeric_cache[i];
    } else if (packed_int_source) {
      value = static_cast<double>(source.list_value[i].int_value);
    } else if (packed_double_source) {
      value = source.list_value[i].double_value;
    } else {
      if (!is_numeric_value(source.list_value[i])) {
        continue;
      }
      value = numeric_value_as_double(source.list_value[i]);
    }

    if (!apply_transforms(i, value)) {
      continue;
    }

    if (terminal == StageKind::ReduceSum) {
      reduce_sum += value;
      continue;
    }
    if (terminal == StageKind::ScanSum) {
      running_prefix += value;
      list_out.push_back(Value::double_value_of(running_prefix));
      continue;
    }

    if (has_value_transform || plan != Value::LayoutTag::PackedInt) {
      list_out.push_back(Value::double_value_of(value));
    } else {
      list_out.push_back(Value::int_value_of(static_cast<long long>(value)));
    }
  }

  if (stats) {
    stats->last_allocations = allocations;
    stats->total_allocations += allocations;
  }

  if (terminal == StageKind::ReduceSum) {
    if (emit_int_reduce && is_integer_like(reduce_sum)) {
      return Value::int_value_of(static_cast<long long>(std::llround(reduce_sum)));
    }
    return Value::double_value_of(reduce_sum);
  }
  return Value::list_value_of(std::move(list_out));
}

}  // namespace phase7
}  // namespace spark
