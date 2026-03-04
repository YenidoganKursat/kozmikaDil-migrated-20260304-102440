#include "spark/core/driver/compiler_driver.h"

#include <vector>

#include "spark/core/common/compiler_common.h"
#include "spark/parser.h"
#include "spark/port/input/source_input_port.h"

namespace spark::core::driver {

CompilerDriver::CompilerDriver(port::input::SourceInputPort* source_port)
    : source_port_(source_port) {}

bool CompilerDriver::construct_program_bundle(const dto::FilePath& source_file,
                                              dto::ProgramBundle& out_bundle,
                                              std::vector<dto::DiagnosticEntry>& diagnostics) const {
  dto::SourceUnit unit;
  if (!source_port_) {
    port::input::FilesystemSourceInputPort fallback;
    if (!fallback.load(source_file, unit, diagnostics)) {
      return false;
    }
  } else if (!source_port_->load(source_file, unit, diagnostics)) {
    return false;
  }

  try {
    Parser parser(unit.source.value);
    out_bundle.source_unit = unit;
    out_bundle.program = parser.parse_program();
    out_bundle.checker = TypeChecker{};
    return static_cast<bool>(out_bundle.program);
  } catch (const ParseException& ex) {
    diagnostics.push_back(core::common::make_diagnostic(source_file.value, ex.what()));
    return false;
  }
}

}  // namespace spark::core::driver
