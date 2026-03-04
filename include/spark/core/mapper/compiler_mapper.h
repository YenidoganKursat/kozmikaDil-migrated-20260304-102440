#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "spark/core/dto/compiler_pipeline_dto.h"

namespace spark::core::mapper {

class DiagnosticMapper final {
 public:
  static std::unordered_map<std::string, dto::DiagnosticEntry> by_source_id(
      const std::vector<dto::DiagnosticEntry>& entries) {
    std::unordered_map<std::string, dto::DiagnosticEntry> out;
    out.reserve(entries.size());
    for (const auto& entry : entries) {
      out[entry.source_id.value] = entry;
    }
    return out;
  }
};

std::size_t compiler_mapper_catalog_token_count();

}  // namespace spark::core::mapper
