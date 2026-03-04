#include "spark/port/input/source_input_port.h"

#include <fstream>
#include <sstream>
#include <string>

#include "spark/core/common/compiler_common.h"

namespace spark::port::input {

bool FilesystemSourceInputPort::load(const core::dto::FilePath& file_path,
                                     core::dto::SourceUnit& out_unit,
                                     std::vector<core::dto::DiagnosticEntry>& diagnostics) {
  std::ifstream in(file_path.value);
  if (!in.is_open()) {
    diagnostics.push_back(core::common::make_diagnostic(
        file_path.value, "cannot open source file: " + file_path.value));
    return false;
  }

  std::ostringstream buffer;
  buffer << in.rdbuf();
  out_unit.source_id.value = file_path.value;
  out_unit.path = file_path;
  out_unit.source.value = buffer.str();
  return true;
}

}  // namespace spark::port::input
