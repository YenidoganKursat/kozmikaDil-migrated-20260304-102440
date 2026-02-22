#include <cassert>
#include <cmath>

#include "../phase7_support.h"

namespace phase7_test {

namespace {

void test_large_matrix_pipeline_stays_fused() {
  constexpr auto source = R"(
m = matrix_fill_affine(64, 64, 19, 11, 131, 1.0)
first = m.map_add(1).map_mul(3).reduce_sum()
second = m.map_add(1).map_mul(3).reduce_sum()
stats = m.pipeline_stats()
)";
  const auto first = run_and_get(source, "first");
  const auto second = run_and_get(source, "second");
  const auto stats = as_number_list(run_and_get(source, "stats"));
  assert(std::fabs(as_number(first) - as_number(second)) < 1e-9);
  assert(stats.size() == 8);
  assert(stats[2] >= 1.0);  // cache_hit_count
  assert(stats[3] >= 2.0);  // fused_count
}

void test_matrix_pipeline_materialize_cell_access() {
  constexpr auto source = R"(
m = [[1, 2, 3]; [4, 5, 6]]
out = m.map_add(2).map_mul(2).to_list()
probe = out[1, 2]
)";
  const auto probe = run_and_get(source, "probe");
  assert(std::fabs(as_number(probe) - 16.0) < 1e-12);
}

}  // namespace

void run_phase7_matrix_extreme_tests() {
  test_large_matrix_pipeline_stays_fused();
  test_matrix_pipeline_materialize_cell_access();
}

}  // namespace phase7_test
