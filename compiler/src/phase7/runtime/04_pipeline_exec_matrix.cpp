#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {
namespace phase7 {

namespace {

struct MatrixRuntimeStage {
  StageKind kind = StageKind::ToList;
  double scalar = 0.0;
};

std::vector<MatrixRuntimeStage> build_matrix_stages(const PipelineChain& chain) {
  std::vector<MatrixRuntimeStage> out;
  out.reserve(chain.stages.size());

  bool seen_terminal = false;
  for (const auto& stage : chain.stages) {
    if (seen_terminal) {
      throw EvalException("pipeline terminal stage must be the last stage");
    }
    MatrixRuntimeStage runtime;
    runtime.kind = stage.kind;
    switch (stage.kind) {
      case StageKind::MapAdd:
      case StageKind::MapMul:
        if (stage.args.size() != 1 || !is_numeric_value(stage.args[0])) {
          throw EvalException(stage.name + "() expects one numeric argument");
        }
        runtime.scalar = numeric_value_as_double(stage.args[0]);
        break;
      case StageKind::ReduceSum:
      case StageKind::ToList:
        if (!stage.args.empty()) {
          throw EvalException(stage.name + "() expects no arguments");
        }
        seen_terminal = true;
        break;
      case StageKind::ScanSum:
      case StageKind::FilterGt:
      case StageKind::FilterLt:
      case StageKind::FilterNonZero:
      case StageKind::ZipAdd:
        throw EvalException(stage.name + "() is not supported on matrix pipeline in phase7");
    }
    out.push_back(runtime);
  }
  return out;
}

double apply_matrix_map(const std::vector<MatrixRuntimeStage>& stages,
                        const std::size_t stop_at,
                        double value) {
  for (std::size_t i = 0; i < stop_at; ++i) {
    const auto& stage = stages[i];
    if (stage.kind == StageKind::MapAdd) {
      value += stage.scalar;
      continue;
    }
    if (stage.kind == StageKind::MapMul) {
      value *= stage.scalar;
      continue;
    }
  }
  return value;
}

}  // namespace

Value execute_matrix_pipeline(PipelineChain& chain, const std::shared_ptr<Environment>& env) {
  // Keep a read-only view on the receiver to avoid copying large matrix payloads.
  const Value& source = chain.base_ptr ? *chain.base_ptr : chain.base_value;
  if (source.kind != Value::Kind::Matrix || !source.matrix_value) {
    throw EvalException("pipeline receiver must be a matrix");
  }

  const auto stages = build_matrix_stages(chain);
  StageKind terminal = StageKind::ToList;
  std::size_t transform_count = stages.size();
  if (!stages.empty() && is_terminal_stage(stages.back().kind)) {
    terminal = stages.back().kind;
    transform_count = stages.size() - 1;
  }

  const auto signature = pipeline_signature(chain);
  const auto plan = choose_matrix_plan(source, "pipeline:" + signature);
  if (chain.base_ptr) {
    chain.base_ptr->matrix_cache.live_plan = true;
    chain.base_ptr->matrix_cache.plan = plan;
    chain.base_ptr->matrix_cache.operation = "pipeline:" + signature;
    chain.base_ptr->matrix_cache.analyzed_version = chain.base_ptr->matrix_cache.version;
  }

  auto* stats = get_pipeline_stats(env, chain.base_name);
  if (stats) {
    if (stats->signature == signature &&
        stats->cached_version == source.matrix_cache.version) {
      stats->cache_hit_count += 1;
    } else {
      stats->analyze_count += 1;
      stats->signature = signature;
      stats->cached_version = source.matrix_cache.version;
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
    why_not_fused = "boxed matrix layout requires fallback";
  }

  std::size_t allocations = 0;
  if (!can_fuse) {
    if (stats) {
      stats->fallback_count += 1;
      stats->last_why_not_fused = why_not_fused;
    }

    auto current = source;
    for (std::size_t i = 0; i < transform_count; ++i) {
      std::vector<Value> next;
      next.reserve(current.matrix_value->data.size());
      for (const auto& cell : current.matrix_value->data) {
        if (!is_numeric_value(cell)) {
          throw EvalException("matrix pipeline expects numeric elements");
        }
        auto value = numeric_value_as_double(cell);
        if (stages[i].kind == StageKind::MapAdd) {
          value += stages[i].scalar;
        } else if (stages[i].kind == StageKind::MapMul) {
          value *= stages[i].scalar;
        }
        next.push_back(Value::double_value_of(value));
      }
      current = Value::matrix_value_of(
          current.matrix_value->rows, current.matrix_value->cols, std::move(next));
      ++allocations;
    }

    if (terminal == StageKind::ReduceSum) {
      auto tmp = current;
      auto result = matrix_reduce_sum_with_plan(tmp);
      if (stats) {
        stats->last_allocations = allocations;
        stats->total_allocations += allocations;
      }
      return result;
    }

    if (stats) {
      stats->last_allocations = allocations;
      stats->total_allocations += allocations;
    }
    return current;
  }

  if (stats) {
    stats->fused_count += 1;
    stats->last_allocations = 0;
  }

  const bool packed_int_source = plan == Value::LayoutTag::PackedInt;
  const bool packed_double_source = plan == Value::LayoutTag::PackedDouble;
  const auto total_values = source.matrix_value->data.size();

  const auto source_sum = [&]() {
    double sum = 0.0;
    for (const auto& cell : source.matrix_value->data) {
      if (packed_int_source) {
        sum += static_cast<double>(cell.int_value);
      } else if (packed_double_source) {
        sum += cell.double_value;
      } else {
        if (!is_numeric_value(cell)) {
          throw EvalException("matrix pipeline expects numeric elements");
        }
        sum += numeric_value_as_double(cell);
      }
    }
    return sum;
  };

  if (terminal == StageKind::ReduceSum && transform_count == 0 && packed_int_source) {
    long long total = 0;
    for (const auto& cell : source.matrix_value->data) {
      total += cell.int_value;
    }
    if (stats) {
      stats->last_allocations = allocations;
      stats->total_allocations += allocations;
    }
    return Value::int_value_of(total);
  }

  if (terminal == StageKind::ReduceSum && transform_count == 1 &&
      stages[0].kind == StageKind::MapAdd) {
    const double add = stages[0].scalar;
    const double reduce_sum = source_sum() + add * static_cast<double>(total_values);
    if (stats) {
      stats->last_allocations = allocations;
      stats->total_allocations += allocations;
    }
    return Value::double_value_of(reduce_sum);
  }

  if (terminal == StageKind::ReduceSum && transform_count == 1 &&
      stages[0].kind == StageKind::MapMul) {
    const double mul = stages[0].scalar;
    const double reduce_sum = source_sum() * mul;
    if (stats) {
      stats->last_allocations = allocations;
      stats->total_allocations += allocations;
    }
    return Value::double_value_of(reduce_sum);
  }

  const bool emit_int_reduce =
      terminal == StageKind::ReduceSum &&
      plan == Value::LayoutTag::PackedInt &&
      transform_count == 0;

  if (terminal == StageKind::ReduceSum && transform_count == 2 &&
      stages[0].kind == StageKind::MapAdd &&
      stages[1].kind == StageKind::MapMul) {
    const double add = stages[0].scalar;
    const double mul = stages[1].scalar;
    const double reduce_sum =
        (source_sum() + add * static_cast<double>(total_values)) * mul;

    if (stats) {
      stats->last_allocations = allocations;
      stats->total_allocations += allocations;
    }
    return Value::double_value_of(reduce_sum);
  }

  const auto apply_transforms = [&](double value) {
    if (transform_count == 0) {
      return value;
    }
    if (transform_count == 1) {
      return apply_matrix_map(stages, 1, value);
    }
    if (transform_count == 2) {
      return apply_matrix_map(stages, 2, value);
    }
    if (transform_count == 3) {
      return apply_matrix_map(stages, 3, value);
    }
    return apply_matrix_map(stages, transform_count, value);
  };

  double reduce_sum = 0.0;
  std::vector<Value> out_data;
  if (terminal != StageKind::ReduceSum) {
    out_data.reserve(source.matrix_value->data.size());
    allocations += 1;
  }

  for (const auto& cell : source.matrix_value->data) {
    double value = 0.0;
    if (packed_int_source) {
      value = static_cast<double>(cell.int_value);
    } else if (packed_double_source) {
      value = cell.double_value;
    } else {
      if (!is_numeric_value(cell)) {
        throw EvalException("matrix pipeline expects numeric elements");
      }
      value = numeric_value_as_double(cell);
    }

    value = apply_transforms(value);
    if (terminal == StageKind::ReduceSum) {
      reduce_sum += value;
      continue;
    }
    out_data.push_back(Value::double_value_of(value));
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
  return Value::matrix_value_of(source.matrix_value->rows, source.matrix_value->cols,
                                std::move(out_data));
}

}  // namespace phase7
}  // namespace spark
