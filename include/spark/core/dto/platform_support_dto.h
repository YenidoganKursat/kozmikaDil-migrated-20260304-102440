#pragma once

#include <string>
#include <vector>

namespace spark::core::dto {

// Atom DTO
struct BackendName {
  std::string value;
};

struct OperationName {
  std::string value;
};

struct TargetTriple {
  std::string value;
};

struct CapabilityReason {
  std::string value;
};

// Molecule DTO
struct BackendOperationCapability {
  BackendName backend;
  OperationName operation;
  bool supported = false;
  CapabilityReason reason;
};

struct TargetCapability {
  TargetTriple target;
  bool build_supported = false;
  bool interpret_supported = false;
};

// Compound DTO
struct PlatformSupportSnapshot {
  std::vector<TargetCapability> cpu_targets;
  std::vector<TargetCapability> micro_targets;
  std::vector<BackendOperationCapability> gpu_operation_caps;
};

struct OperationRoute {
  OperationName operation;
  BackendName selected_backend;
  bool execute_on_gpu = false;
  CapabilityReason reason;
};

struct PerformanceGatePolicy {
  double max_regression_percent = 5.0;
  double max_alloc_regression_percent = 10.0;
  double max_p95_regression_percent = 10.0;
};

struct CorrectnessGatePolicy {
  int random_samples_per_cell = 100000;
  bool enforce_bigdecimal_oracle = true;
  bool enforce_biginteger_oracle = true;
  bool enforce_cpu_gpu_differential = true;
};

// Tissue DTO
struct PlatformSupportRequest {
  std::vector<BackendName> preferred_gpu_backends;
  std::vector<OperationName> list_operations;
  std::vector<OperationName> matrix_operations;
  bool require_gpu_full_coverage = false;
  bool allow_cpu_fallback = true;
  bool strict_mode = false;
  PerformanceGatePolicy perf_policy;
  CorrectnessGatePolicy correctness_policy;
};

// Organ DTO
struct PlatformSupportResponse {
  bool supported = false;
  bool list_compatible = false;
  bool matrix_compatible = false;
  std::vector<OperationRoute> list_routes;
  std::vector<OperationRoute> matrix_routes;
  std::vector<CapabilityReason> warnings;
  PlatformSupportSnapshot snapshot;
};

// System DTO
struct PlatformSupportSystemState {
  PlatformSupportResponse last_report;
  std::vector<CapabilityReason> audit_log;
};

// Organism DTO
struct PlatformSupportOrganismSnapshot {
  PlatformSupportSystemState state;
  std::vector<std::string> notes;
};

}  // namespace spark::core::dto
