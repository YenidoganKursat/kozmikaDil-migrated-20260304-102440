#include <cassert>
#include <cmath>

#include "../phase6_support.h"

namespace phase6_test {

namespace {

void test_matrix_plan_transition_after_hetero_write() {
  constexpr auto source = R"(
m = [[1, 2.5]; [3, 4]]
_ = m.reduce_sum()
plan_before = m.plan_id()
m[0,1] = True
plan_after = m.plan_id()
cell = m[0,1]
stats = m.cache_stats()
)";
  const auto plan_before = run_and_get(source, "plan_before");
  const auto plan_after = run_and_get(source, "plan_after");
  const auto cell = run_and_get(source, "cell");
  const auto stats = as_int_list(run_and_get(source, "stats"));

  assert(plan_before.kind == spark::Value::Kind::Int);
  assert(plan_before.int_value == 2);  // PackedDouble
  assert(plan_after.kind == spark::Value::Kind::Int);
  assert(plan_after.int_value == 6);   // BoxedAny
  assert(cell.kind == spark::Value::Kind::Bool);
  assert(cell.bool_value);
  assert(stats.size() == 6);
  assert(stats[3] >= 1);  // invalidation_count
}

void test_matrix_repeated_reduce_hits_cache() {
  constexpr auto source = R"(
m = [[1, 2.5]; [3, 4]]
a = m.reduce_sum()
b = m.reduce_sum()
c = m.reduce_sum()
stats = m.cache_stats()
)";
  const auto a = run_and_get(source, "a");
  const auto b = run_and_get(source, "b");
  const auto c = run_and_get(source, "c");
  const auto stats = as_int_list(run_and_get(source, "stats"));

  assert(std::fabs(as_number(a) - 10.5) < 1e-12);
  assert(std::fabs(as_number(b) - 10.5) < 1e-12);
  assert(std::fabs(as_number(c) - 10.5) < 1e-12);
  assert(stats.size() == 6);
  assert(stats[2] >= 2);  // cache_hit_count
}

void test_matrix_mutation_forces_rebuild() {
  constexpr auto source = R"(
m = [[1, 2.5]; [3, 4]]
_ = m.reduce_sum()
before = m.cache_stats()
m[1,0] = 30
after_write = m.cache_stats()
sum_after = m.reduce_sum()
after_reduce = m.cache_stats()
)";
  const auto before = as_int_list(run_and_get(source, "before"));
  const auto after_write = as_int_list(run_and_get(source, "after_write"));
  const auto after_reduce = as_int_list(run_and_get(source, "after_reduce"));
  const auto sum_after = run_and_get(source, "sum_after");

  assert(before.size() == 6);
  assert(after_write.size() == 6);
  assert(after_reduce.size() == 6);
  assert(after_write[3] >= before[3] + 1);   // invalidation_count
  assert(after_reduce[0] >= before[0] + 1);  // analyze_count
  assert(std::fabs(as_number(sum_after) - 37.5) < 1e-12);
}

}  // namespace

void run_matrix_phase6_extreme_tests() {
  test_matrix_plan_transition_after_hetero_write();
  test_matrix_repeated_reduce_hits_cache();
  test_matrix_mutation_forces_rebuild();
}

}  // namespace phase6_test
