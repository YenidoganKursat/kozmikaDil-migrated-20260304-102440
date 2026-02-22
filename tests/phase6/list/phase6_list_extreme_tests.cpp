#include <cassert>
#include <cmath>
#include <vector>

#include "../phase6_support.h"

namespace phase6_test {

namespace {

void test_long_mutation_sequence_replans_cache() {
  constexpr auto source = R"(
values = [1, 2, 3.0]
i = 0
while i < 32:
  values.append(i)
  _ = values.reduce_sum()
  values[0] = i
  i = i + 1
stats = values.cache_stats()
plan = values.plan_id()
sum = values.reduce_sum()
)";
  const auto stats = as_int_list(run_and_get(source, "stats"));
  const auto plan = run_and_get(source, "plan");
  const auto sum = run_and_get(source, "sum");
  assert(stats.size() == 6);
  assert(stats[0] >= 2);   // analyze_count
  assert(stats[1] >= 1);   // materialize_count
  assert(stats[3] >= 32);  // invalidation_count
  assert(plan.kind == spark::Value::Kind::Int);
  assert(plan.int_value == 3);  // PromotedPackedDouble
  assert(std::isfinite(as_number(sum)));
}

void test_plan_transition_promote_to_chunked_after_bool_append() {
  constexpr auto source = R"(
values = [1, 2, 3.5, 4]
plan_before = values.plan_id()
values.append(True)
plan_after = values.plan_id()
stats = values.cache_stats()
)";
  const auto plan_before = run_and_get(source, "plan_before");
  const auto plan_after = run_and_get(source, "plan_after");
  const auto stats = as_int_list(run_and_get(source, "stats"));
  assert(plan_before.kind == spark::Value::Kind::Int);
  assert(plan_after.kind == spark::Value::Kind::Int);
  assert(plan_before.int_value == 3);  // PromotedPackedDouble
  assert(plan_after.int_value != plan_before.int_value);
  assert(plan_after.int_value == 4 || plan_after.int_value == 6);  // ChunkedUnion or BoxedAny
  assert(stats.size() == 6);
}

void test_chunked_random_access_after_in_place_rewrite() {
  constexpr auto source = R"(
values = [1, 2, 3, 4, True, False, 7, 8, 9, 10]
plan_before = values.plan_id()
v_before = values[5]
values[5] = 11
plan_after = values.plan_id()
v_after = values[5]
stats = values.cache_stats()
)";
  const auto plan_before = run_and_get(source, "plan_before");
  const auto v_before = run_and_get(source, "v_before");
  const auto plan_after = run_and_get(source, "plan_after");
  const auto v_after = run_and_get(source, "v_after");
  const auto stats = as_int_list(run_and_get(source, "stats"));

  assert(plan_before.kind == spark::Value::Kind::Int);
  assert(plan_before.int_value == 4);
  assert(v_before.kind == spark::Value::Kind::Bool);
  assert(!v_before.bool_value);
  assert(plan_after.kind == spark::Value::Kind::Int);
  assert(plan_after.int_value == 4);
  assert(v_after.kind == spark::Value::Kind::Int);
  assert(v_after.int_value == 11);
  assert(stats.size() == 6);
}

}  // namespace

void run_list_phase6_extreme_tests() {
  test_long_mutation_sequence_replans_cache();
  test_plan_transition_promote_to_chunked_after_bool_append();
  test_chunked_random_access_after_in_place_rewrite();
}

}  // namespace phase6_test
