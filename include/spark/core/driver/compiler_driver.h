#pragma once

#include <cstddef>
#include <vector>

#include "spark/core/dto/compiler_pipeline_dto.h"

namespace spark::port::input {
class SourceInputPort;
}

namespace spark::core::driver {

class CompilerDriver {
 public:
  explicit CompilerDriver(port::input::SourceInputPort* source_port = nullptr);

  bool construct_program_bundle(const dto::FilePath& source_file, dto::ProgramBundle& out_bundle,
                                std::vector<dto::DiagnosticEntry>& diagnostics) const;

 private:
  port::input::SourceInputPort* source_port_ = nullptr;
};

std::size_t compiler_driver_dto_registry_count();

}  // namespace spark::core::driver
