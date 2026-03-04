#include "spark/core/mapper/compiler_mapper.h"

#include <array>
#include <cstddef>
#include <string_view>

namespace spark::core::mapper {

namespace {

constexpr std::array<std::string_view, 29> kDtoCatalogTokens = {
    "source_id",
    "file_path",
    "source_text",
    "diagnostic_message",
    "backend_name",
    "operation_name",
    "target_triple",
    "capability_reason",
    "source_unit",
    "diagnostic_entry",
    "backend_operation_capability",
    "target_capability",
    "program_bundle",
    "pipeline_products",
    "build_tuning",
    "platform_support_snapshot",
    "operation_route",
    "performance_gate_policy",
    "correctness_gate_policy",
    "compile_request",
    "run_request",
    "platform_support_request",
    "compile_response",
    "run_response",
    "platform_support_response",
    "compiler_system_state",
    "platform_support_system_state",
    "compiler_organism_snapshot",
    "platform_support_organism_snapshot",
};

}  // namespace

std::size_t compiler_mapper_catalog_token_count() {
  return kDtoCatalogTokens.size();
}

}  // namespace spark::core::mapper
