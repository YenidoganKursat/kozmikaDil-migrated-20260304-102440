#pragma once

#include <vector>

#include "spark/core/dto/compiler_pipeline_dto.h"

namespace spark::port::input {

class SourceInputPort {
 public:
  virtual ~SourceInputPort() = default;

  virtual bool load(const core::dto::FilePath& file_path, core::dto::SourceUnit& out_unit,
                    std::vector<core::dto::DiagnosticEntry>& diagnostics) = 0;
};

class FilesystemSourceInputPort final : public SourceInputPort {
 public:
  bool load(const core::dto::FilePath& file_path, core::dto::SourceUnit& out_unit,
            std::vector<core::dto::DiagnosticEntry>& diagnostics) override;
};

}  // namespace spark::port::input
