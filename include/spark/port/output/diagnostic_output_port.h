#pragma once

#include <vector>

#include "spark/core/dto/compiler_pipeline_dto.h"

namespace spark::port::output {

class DiagnosticOutputPort {
 public:
  virtual ~DiagnosticOutputPort() = default;
  virtual void publish(const std::vector<core::dto::DiagnosticEntry>& diagnostics) = 0;
};

class StdoutDiagnosticOutputPort final : public DiagnosticOutputPort {
 public:
  void publish(const std::vector<core::dto::DiagnosticEntry>& diagnostics) override;
};

}  // namespace spark::port::output
