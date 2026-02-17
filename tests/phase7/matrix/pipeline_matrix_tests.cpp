#include <cassert>
#include <cmath>

#include "../phase7_support.h"

namespace phase7_test {

namespace {

void test_matrix_map_pipeline_reduce() {
  constexpr auto source = R"(
m = [[1, 2]; [3, 4]]
total = m.map_add(1).map_mul(2).reduce_sum()
stats = m.pipeline_stats()
)";
  const auto total = run_and_get(source, "total");
  const auto stats = as_number_list(run_and_get(source, "stats"));
  assert(std::fabs(as_number(total) - 28.0) < 1e-12);
  assert(stats.size() == 8);
  assert(stats[3] >= 1.0);  // fused_count
}

void test_matrix_map_pipeline_materialized_output() {
  constexpr auto source = R"(
m = [[1, 2]; [3, 4]]
mapped = m.map_add(2).map_mul(3).to_list()
cell = mapped[1, 1]
)";
  const auto cell = run_and_get(source, "cell");
  assert(std::fabs(as_number(cell) - 18.0) < 1e-12);
}

}  // namespace

void run_phase7_matrix_tests() {
  test_matrix_map_pipeline_reduce();
  test_matrix_map_pipeline_materialized_output();
}

}  // namespace phase7_test
