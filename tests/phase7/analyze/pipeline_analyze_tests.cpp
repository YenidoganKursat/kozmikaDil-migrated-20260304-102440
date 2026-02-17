#include <cassert>

#include "../phase7_support.h"

namespace phase7_test {

namespace {

void test_pipeline_ir_dump_contains_nodes() {
  constexpr auto source = R"(
values = [1, 2, 3, 4]
total = values.map_add(1).filter_gt(2).reduce_sum()
)";
  const auto dump = analyze_dump(source, "pipeline");
  assert(dump.find("pipeline_") != std::string::npos);
  assert(dump.find("map_add") != std::string::npos);
  assert(dump.find("filter_gt") != std::string::npos);
  assert(dump.find("reduce_sum") != std::string::npos);
}

void test_fusion_plan_dump_contains_fused_state() {
  constexpr auto source = R"(
values = [1, 2, 3]
total = values.map_add(1).reduce_sum()
)";
  const auto dump = analyze_dump(source, "fusion");
  assert(dump.find("fused=yes") != std::string::npos);
  assert(dump.find("terminal=reduce_sum") != std::string::npos);
}

void test_why_not_fused_reports_mutating_stage() {
  constexpr auto source = R"(
values = [1, 2]
out = values.append(3).reduce_sum()
)";
  const auto dump = analyze_dump(source, "why");
  assert(dump.find("pipeline contains mutating stage") != std::string::npos);
}

}  // namespace

void run_phase7_analyze_tests() {
  test_pipeline_ir_dump_contains_nodes();
  test_fusion_plan_dump_contains_fused_state();
  test_why_not_fused_reports_mutating_stage();
}

}  // namespace phase7_test
