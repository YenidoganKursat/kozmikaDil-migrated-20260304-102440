#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../semantic_runtime/evaluator_parts/internal_helpers.h"

namespace spark {

namespace {

std::string to_lower_ascii(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

std::string canonical_backend_name(std::string backend) {
  backend = to_lower_ascii(std::move(backend));
  if (backend == "nvidia") {
    return "cuda";
  }
  if (backend == "rocm" || backend == "hip") {
    return "rocm_hip";
  }
  if (backend == "sycl" || backend == "oneapi" || backend == "intel") {
    return "oneapi_sycl";
  }
  if (backend == "cl") {
    return "opencl";
  }
  if (backend == "vk" || backend == "vulkan") {
    return "vulkan_compute";
  }
  return backend;
}

std::string canonical_operation_name(std::string op) {
  op = to_lower_ascii(std::move(op));
  if (op == "maps" || op == "mapper") {
    return "map";
  }
  if (op == "filters") {
    return "filter";
  }
  if (op == "reduce_sum" || op == "sum") {
    return "reduce";
  }
  if (op == "scan_sum" || op == "prefix_sum") {
    return "scan";
  }
  if (op == "set_contains") {
    return "contains";
  }
  if (op == "set_union") {
    return "union";
  }
  if (op == "set_intersection") {
    return "intersection";
  }
  if (op == "set_difference") {
    return "difference";
  }
  if (op == "parallelgpustream") {
    return "pipeline";
  }
  return op;
}

const std::unordered_set<std::string>& gpu_common_ops() {
  static const std::unordered_set<std::string> ops = {
      "pipeline",
      "map",
      "filter",
      "reduce",
      "scan",
      "distinct",
      "contains",
      "union",
      "intersection",
      "difference",
      "group_by",
      "sort",
      "matmul",
      "matmul_f32",
      "matmul_f64",
  };
  return ops;
}

const std::unordered_map<std::string, std::unordered_set<std::string>>& backend_caps() {
  static const std::unordered_map<std::string, std::unordered_set<std::string>> caps = {
      {"cuda", gpu_common_ops()},
      {"rocm_hip", gpu_common_ops()},
      {"oneapi_sycl", gpu_common_ops()},
      {"opencl", gpu_common_ops()},
      {"vulkan_compute", gpu_common_ops()},
      {"metal", gpu_common_ops()},
      // planning backend: exposed but does not claim runtime capability.
      {"webgpu", {}},
  };
  return caps;
}

bool is_host_backend(const std::string& backend) {
  return backend == "auto" || backend == "own" || backend == "blas";
}

std::vector<std::string> parse_operation_list_value(const Value& value) {
  if (value.kind != Value::Kind::List) {
    throw EvalException("gpu_orchestrate() expects second argument as list of operation names");
  }
  std::vector<std::string> out;
  out.reserve(value.list_value.size());
  for (const auto& item : value.list_value) {
    if (item.kind != Value::Kind::String) {
      throw EvalException("gpu_orchestrate() operation list entries must be string values");
    }
    out.push_back(canonical_operation_name(item.string_value));
  }
  return out;
}

}  // namespace

bool gpu_backend_supports_operation(const std::string& backend, const std::string& operation) {
  const auto backend_name = canonical_backend_name(backend);
  const auto op_name = canonical_operation_name(operation);
  if (backend_name.empty() || op_name.empty()) {
    return false;
  }
  if (is_host_backend(backend_name)) {
    return false;
  }
  if (op_name == "object_read" || op_name == "object_write" || op_name == "object_method") {
    return false;
  }
  const auto& caps = backend_caps();
  const auto it = caps.find(backend_name);
  if (it == caps.end()) {
    return false;
  }
  return it->second.find(op_name) != it->second.end();
}

Value gpu_backend_builtin_value(const std::vector<Value>& args) {
  if (!args.empty()) {
    throw EvalException("gpu_backend() expects no arguments");
  }
  const char* raw = std::getenv("SPARK_MATMUL_BACKEND");
  if (!raw || *raw == '\0') {
    return Value::string_value_of("auto");
  }
  return Value::string_value_of(canonical_backend_name(std::string(raw)));
}

Value gpu_supports_builtin_value(const std::vector<Value>& args) {
  if (args.size() != 2) {
    throw EvalException("gpu_supports() expects exactly two arguments: backend, operation");
  }
  if (args[0].kind != Value::Kind::String || args[1].kind != Value::Kind::String) {
    throw EvalException("gpu_supports() backend and operation must be string values");
  }
  const bool supported = gpu_backend_supports_operation(args[0].string_value, args[1].string_value);
  return Value::bool_value_of(supported);
}

Value gpu_orchestrate_builtin_value(const std::vector<Value>& args) {
  if (args.size() != 2) {
    throw EvalException("gpu_orchestrate() expects exactly two arguments: backend, operation_list");
  }
  if (args[0].kind != Value::Kind::String) {
    throw EvalException("gpu_orchestrate() first argument must be backend string");
  }

  const auto backend = canonical_backend_name(args[0].string_value);
  const auto ops = parse_operation_list_value(args[1]);

  std::vector<Value> route_bits;
  route_bits.reserve(ops.size());
  std::vector<Value> fallback_ops;
  fallback_ops.reserve(ops.size());

  long long gpu_steps = 0;
  long long cpu_steps = 0;
  for (const auto& op : ops) {
    const bool on_gpu = gpu_backend_supports_operation(backend, op);
    route_bits.push_back(Value::int_value_of(on_gpu ? 1 : 0));
    if (on_gpu) {
      ++gpu_steps;
    } else {
      ++cpu_steps;
      fallback_ops.push_back(Value::string_value_of(op));
    }
  }

  std::vector<Value> out;
  out.reserve(6);
  out.push_back(Value::string_value_of(backend));
  out.push_back(Value::bool_value_of(cpu_steps == 0 && !ops.empty()));
  out.push_back(Value::int_value_of(gpu_steps));
  out.push_back(Value::int_value_of(cpu_steps));
  out.push_back(Value::list_value_of(std::move(route_bits)));
  out.push_back(Value::list_value_of(std::move(fallback_ops)));
  return Value::list_value_of(std::move(out));
}

}  // namespace spark
