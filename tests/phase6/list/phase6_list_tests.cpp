#include <cassert>
#include <cmath>
#include <vector>

#include "../phase6_support.h"

namespace phase6_test {

namespace {

void test_type_evolution_promote_and_widen() {
  expect_type("x = []\nx.append(1)\nx.append(2.5)\n", "x", "List[Float(f64)]");
  expect_type("x = []\nx.append(1)\nx.append(True)\n", "x", "List[Any]");
}

void test_reduce_sum_cache_hit_and_invalidation() {
  constexpr auto source = R"(
values = [1, 2, 3.5, 4]
first = values.reduce_sum()
second = values.reduce_sum()
before = values.cache_stats()
values.append(5)
third = values.reduce_sum()
after = values.cache_stats()
plan = values.plan_id()
)";

  const auto first = run_and_get(source, "first");
  const auto second = run_and_get(source, "second");
  const auto third = run_and_get(source, "third");
  const auto before = as_int_list(run_and_get(source, "before"));
  const auto after = as_int_list(run_and_get(source, "after"));
  const auto plan = run_and_get(source, "plan");

  assert(std::fabs(as_number(first) - 10.5) < 1e-12);
  assert(std::fabs(as_number(second) - 10.5) < 1e-12);
  assert(std::fabs(as_number(third) - 15.5) < 1e-12);
  assert(before.size() == 6);
  assert(after.size() == 6);
  assert(before[0] >= 1);          // analyze_count
  assert(before[1] >= 1);          // materialize_count
  assert(before[2] >= 1);          // cache_hit_count
  assert(before[3] == 0);          // invalidation_count before append
  assert(after[3] >= 1);           // invalidation_count after append
  assert(after[0] >= before[0] + 1);  // re-analyze happened
  assert(plan.kind == spark::Value::Kind::Int);
  assert(plan.int_value == 3);     // PromotedPackedDouble
}

void test_chunked_plan_selection() {
  constexpr auto source = R"(
values = [1, 2, 3, 4, True, False, 7, 8, 9, 10]
plan = values.plan_id()
stats = values.cache_stats()
)";
  const auto plan = run_and_get(source, "plan");
  const auto stats = as_int_list(run_and_get(source, "stats"));
  assert(plan.kind == spark::Value::Kind::Int);
  assert(plan.int_value == 4);  // ChunkedUnion
  assert(stats.size() == 6);
  assert(stats[0] == 0);        // plan_id() does not force cache materialization.
}

void test_map_add_keeps_order_and_uses_numeric_path() {
  constexpr auto source = R"(
values = [1, 2, 3.0]
mapped = values.map_add(2)
first = mapped[0]
last = mapped[2]
sum = mapped.reduce_sum()
)";
  const auto first = run_and_get(source, "first");
  const auto last = run_and_get(source, "last");
  const auto sum = run_and_get(source, "sum");
  assert(std::fabs(as_number(first) - 3.0) < 1e-12);
  assert(std::fabs(as_number(last) - 5.0) < 1e-12);
  assert(std::fabs(as_number(sum) - 12.0) < 1e-12);
}

void test_map_add_gather_plan_preserves_non_numeric_cells() {
  constexpr auto source = R"(
values = [1, 2.5, 3, 4.5, False]
mapped = values.map_add(1)
stats = values.cache_stats()
plan_used = stats[5]
tail = mapped[4]
sum = mapped.reduce_sum()
)";
  const auto plan_used = run_and_get(source, "plan_used");
  const auto tail = run_and_get(source, "tail");
  const auto sum = run_and_get(source, "sum");
  assert(plan_used.kind == spark::Value::Kind::Int);
  assert(plan_used.int_value == 5);  // GatherScatter for map_add()
  assert(tail.kind == spark::Value::Kind::Bool);
  assert(!as_bool(tail));
  assert(std::fabs(as_number(sum) - 15.0) < 1e-12);
}

void test_reduce_sum_chunk_plan_skips_non_numeric_cells() {
  constexpr auto source = R"(
values = [1, 2.5, False, False, 4, False, 6.5, False, False, False]
sum = values.reduce_sum()
stats = values.cache_stats()
plan_used = stats[5]
)";
  const auto sum = run_and_get(source, "sum");
  const auto plan_used = run_and_get(source, "plan_used");
  assert(std::fabs(as_number(sum) - 14.0) < 1e-12);
  assert(plan_used.kind == spark::Value::Kind::Int);
  assert(plan_used.int_value == 4);  // ChunkedUnion for reduce_sum()
}

void test_cache_bytes_and_index_write_invalidation() {
  constexpr auto source = R"(
values = [1, 2, 3.5]
_ = values.reduce_sum()
bytes_before = values.cache_bytes()
stats_before = values.cache_stats()
values[1] = 10
bytes_after_write = values.cache_bytes()
stats_after_write = values.cache_stats()
_ = values.reduce_sum()
bytes_after_rebuild = values.cache_bytes()
stats_after_reduce = values.cache_stats()
)";
  const auto bytes_before = run_and_get(source, "bytes_before");
  const auto bytes_after_write = run_and_get(source, "bytes_after_write");
  const auto bytes_after_rebuild = run_and_get(source, "bytes_after_rebuild");
  const auto stats_before = as_int_list(run_and_get(source, "stats_before"));
  const auto stats_after_write = as_int_list(run_and_get(source, "stats_after_write"));
  const auto stats_after_reduce = as_int_list(run_and_get(source, "stats_after_reduce"));

  assert(bytes_before.kind == spark::Value::Kind::Int);
  assert(bytes_after_write.kind == spark::Value::Kind::Int);
  assert(bytes_after_rebuild.kind == spark::Value::Kind::Int);
  assert(bytes_before.int_value > 0);
  assert(bytes_after_write.int_value == 0);
  assert(bytes_after_rebuild.int_value > 0);
  assert(stats_after_write[3] >= stats_before[3] + 1);  // invalidation count
  assert(stats_after_reduce[0] >= stats_before[0] + 1); // re-analysis count
}

}  // namespace

void run_list_phase6_tests() {
  test_type_evolution_promote_and_widen();
  test_reduce_sum_cache_hit_and_invalidation();
  test_chunked_plan_selection();
  test_map_add_keeps_order_and_uses_numeric_path();
  test_map_add_gather_plan_preserves_non_numeric_cells();
  test_reduce_sum_chunk_plan_skips_non_numeric_cells();
  test_cache_bytes_and_index_write_invalidation();
}

}  // namespace phase6_test
