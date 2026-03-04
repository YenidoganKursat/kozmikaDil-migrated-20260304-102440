#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include "../platform_support_suite.h"
#include "spark/application/wiring/compiler_wiring.h"
#include "spark/core/dto/compiler_pipeline_dto.h"
#include "spark/core/mode/compiler_mode.h"

namespace platform_support_test {

namespace {

std::string make_temp_program_path() {
  char buffer[L_tmpnam];
  const char* name = std::tmpnam(buffer);
  assert(name != nullptr);
  return std::string(name) + ".k";
}

void test_core_ui_parse_typecheck_and_lower() {
  const std::string path = make_temp_program_path();
  {
    FILE* out = std::fopen(path.c_str(), "wb");
    assert(out != nullptr);
    const char* source = "x = 2 + 4\nprint(x)\n";
    const auto wrote = std::fwrite(source, 1, std::strlen(source), out);
    assert(wrote == std::strlen(source));
    std::fclose(out);
  }

  spark::application::wiring::CompilerWiring wiring;

  spark::core::dto::ProgramBundle bundle;
  spark::core::dto::FilePath file_path{path};
  const bool parsed = wiring.ui().parse_and_typecheck(file_path, bundle);
  assert(parsed);
  assert(static_cast<bool>(bundle.program));

  spark::core::dto::PipelineProducts products;
  const bool lowered = wiring.ui().lower_to_products(bundle, products);
  assert(lowered);
  assert(!products.ir.empty());
  assert(!products.c_source.empty());

  std::remove(path.c_str());
}

void test_mode_sanitization() {
  spark::core::mode::CompilerMode mode;
  mode.tuning.auto_pgo_runs = -5;
  const auto sanitized = spark::core::mode::sanitize_mode(mode);
  assert(sanitized.tuning.auto_pgo_runs == 0);
}

void test_platform_support_cpu_fallback_route() {
  spark::application::wiring::CompilerWiring wiring;

  spark::core::dto::PlatformSupportRequest request;
  request.preferred_gpu_backends = {{"webgpu"}};
  request.list_operations = {{"map"}, {"filter"}};
  request.matrix_operations = {{"matmul"}};
  request.require_gpu_full_coverage = false;
  request.allow_cpu_fallback = true;
  request.strict_mode = false;

  spark::core::dto::PlatformSupportResponse response;
  const bool ok = wiring.platform_support_ui().evaluate_support(request, response);
  assert(ok);
  assert(response.supported);
  assert(response.list_compatible);
  assert(response.matrix_compatible);
  assert(!response.list_routes.empty());
  assert(!response.matrix_routes.empty());
  assert(!response.snapshot.cpu_targets.empty());
  assert(!response.snapshot.micro_targets.empty());

  bool saw_cpu_route = false;
  for (const auto& route : response.list_routes) {
    if (!route.execute_on_gpu && route.selected_backend.value == "cpu") {
      saw_cpu_route = true;
      break;
    }
  }
  if (!saw_cpu_route) {
    for (const auto& route : response.matrix_routes) {
      if (!route.execute_on_gpu && route.selected_backend.value == "cpu") {
        saw_cpu_route = true;
        break;
      }
    }
  }
  assert(saw_cpu_route);
}

void test_platform_support_strict_gpu_rejects_fallback() {
  spark::application::wiring::CompilerWiring wiring;

  spark::core::dto::PlatformSupportRequest request;
  request.preferred_gpu_backends = {{"webgpu"}};
  request.list_operations = {{"map"}};
  request.matrix_operations = {{"matmul"}};
  request.require_gpu_full_coverage = true;
  request.allow_cpu_fallback = false;
  request.strict_mode = true;

  spark::core::dto::PlatformSupportResponse response;
  const bool ok = wiring.platform_support_ui().evaluate_support(request, response);
  assert(!ok);
  assert(!response.supported);
  assert(!response.list_compatible || !response.matrix_compatible);
}

}  // namespace

void run_platform_support_core_architecture_tests() {
  test_core_ui_parse_typecheck_and_lower();
  test_mode_sanitization();
}

void run_platform_support_platform_support_tests() {
  test_platform_support_cpu_fallback_route();
  test_platform_support_strict_gpu_rejects_fallback();
}

}  // namespace platform_support_test
