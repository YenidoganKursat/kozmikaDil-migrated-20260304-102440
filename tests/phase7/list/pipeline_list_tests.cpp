#include <cassert>
#include <cmath>
#include <cstdlib>

#include "../phase7_support.h"

namespace phase7_test {

namespace {

void test_fused_map_filter_reduce_pipeline() {
  constexpr auto source = R"(
values = [1, 2, 3, 4, 5]
total = values.map_add(1).filter_gt(3).map_mul(2).reduce_sum()
stats = values.pipeline_stats()
plan = values.pipeline_plan_id()
)";
  const auto total = run_and_get(source, "total");
  const auto stats = as_number_list(run_and_get(source, "stats"));
  const auto plan = run_and_get(source, "plan");

  assert(std::fabs(as_number(total) - 30.0) < 1e-12);
  assert(stats.size() == 8);
  assert(stats[3] >= 1.0);  // fused_count
  assert(plan.kind == spark::Value::Kind::Int);
  assert(plan.int_value != 0);
}

void test_fallback_when_fusion_is_disabled() {
  constexpr auto source = R"(
values = [1, 2, 3, 4, 5]
total = values.map_add(1).filter_gt(2).reduce_sum()
stats = values.pipeline_stats()
)";
  setenv("SPARK_PIPELINE_FUSION", "0", 1);
  const auto total = run_and_get(source, "total");
  const auto stats = as_number_list(run_and_get(source, "stats"));
  unsetenv("SPARK_PIPELINE_FUSION");

  assert(std::fabs(as_number(total) - 18.0) < 1e-12);
  assert(stats.size() == 8);
  assert(stats[4] >= 1.0);  // fallback_count
}

void test_hetero_reduce_pipeline_steady_cache() {
  constexpr auto source = R"(
values = [1, 2.5, False, 4]
first = values.map_add(1).reduce_sum()
second = values.map_add(1).reduce_sum()
stats = values.pipeline_stats()
)";
  const auto first = run_and_get(source, "first");
  const auto second = run_and_get(source, "second");
  const auto stats = as_number_list(run_and_get(source, "stats"));

  assert(std::fabs(as_number(first) - 10.5) < 1e-12);
  assert(std::fabs(as_number(second) - 10.5) < 1e-12);
  assert(stats.size() == 8);
  assert(stats[2] >= 1.0);  // cache_hit_count
  assert(stats[3] >= 1.0);  // fused_count
}

void test_scan_sum_pipeline() {
  constexpr auto source = R"(
values = [1, 2, 3]
scan = values.map_add(1).scan_sum()
)";
  const auto scan = as_number_list(run_and_get(source, "scan"));
  assert(scan.size() == 3);
  assert(std::fabs(scan[0] - 2.0) < 1e-12);
  assert(std::fabs(scan[1] - 5.0) < 1e-12);
  assert(std::fabs(scan[2] - 9.0) < 1e-12);
}

void test_zip_add_pipeline() {
  constexpr auto source = R"(
a = [1, 2, 3]
b = [10, 20, 30]
total = a.zip_add(b).reduce_sum()
stats = a.pipeline_stats()
)";
  const auto total = run_and_get(source, "total");
  const auto stats = as_number_list(run_and_get(source, "stats"));
  assert(std::fabs(as_number(total) - 66.0) < 1e-12);
  assert(stats[3] >= 1.0);  // fused_count
}

}  // namespace

void run_phase7_list_tests() {
  test_fused_map_filter_reduce_pipeline();
  test_fallback_when_fusion_is_disabled();
  test_hetero_reduce_pipeline_steady_cache();
  test_scan_sum_pipeline();
  test_zip_add_pipeline();
}

}  // namespace phase7_test
