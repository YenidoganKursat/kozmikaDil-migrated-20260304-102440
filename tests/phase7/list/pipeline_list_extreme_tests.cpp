#include <cassert>
#include <cmath>

#include "../phase7_support.h"

namespace phase7_test {

namespace {

void test_long_fused_pipeline_reuses_cached_plan() {
  constexpr auto source = R"(
values = list_fill_affine(2048, 17, 9, 97, 1.0)
first = values.map_add(1).map_mul(2).filter_gt(10).map_add(-3).reduce_sum()
second = values.map_add(1).map_mul(2).filter_gt(10).map_add(-3).reduce_sum()
stats = values.pipeline_stats()
)";
  const auto first = run_and_get(source, "first");
  const auto second = run_and_get(source, "second");
  const auto stats = as_number_list(run_and_get(source, "stats"));
  assert(std::fabs(as_number(first) - as_number(second)) < 1e-9);
  assert(stats.size() == 8);
  assert(stats[2] >= 1.0);  // cache_hit_count
  assert(stats[3] >= 2.0);  // fused_count
}

void test_scan_sum_last_equals_reduce_sum() {
  constexpr auto source = R"(
values = list_fill_affine(512, 13, 7, 41, 1.0)
scan = values.map_add(2).scan_sum()
last = scan[len(scan) - 1]
sum_ref = values.map_add(2).reduce_sum()
)";
  const auto last = run_and_get(source, "last");
  const auto sum_ref = run_and_get(source, "sum_ref");
  assert(std::fabs(as_number(last) - as_number(sum_ref)) < 1e-9);
}

}  // namespace

void run_phase7_list_extreme_tests() {
  test_long_fused_pipeline_reuses_cached_plan();
  test_scan_sum_last_equals_reduce_sum();
}

}  // namespace phase7_test
