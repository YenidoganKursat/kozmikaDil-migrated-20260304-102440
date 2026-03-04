#include "spark/port/output/diagnostic_output_port.h"

#include <iostream>

namespace spark::port::output {

void StdoutDiagnosticOutputPort::publish(
    const std::vector<core::dto::DiagnosticEntry>& diagnostics) {
  for (const auto& item : diagnostics) {
    std::cout << item.source_id.value << ": " << item.message.value << "\n";
  }
}

}  // namespace spark::port::output
