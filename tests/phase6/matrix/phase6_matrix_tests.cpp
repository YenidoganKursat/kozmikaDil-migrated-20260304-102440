#include <cassert>
#include <cmath>
#include <vector>

#include "../phase6_support.h"

namespace phase6_test {

namespace {

void test_matrix_numeric_promote_and_cache() {
  constexpr auto source = R"(
m = [[1, 2.5]; [3, 4]]
s1 = m.reduce_sum()
s2 = m.reduce_sum()
stats = m.cache_stats()
plan = m.plan_id()
)";
  const auto s1 = run_and_get(source, "s1");
  const auto s2 = run_and_get(source, "s2");
  const auto stats = as_int_list(run_and_get(source, "stats"));
  const auto plan = run_and_get(source, "plan");
  const auto matrix = run_and_get(source, "m");

  assert(std::fabs(as_number(s1) - 10.5) < 1e-12);
  assert(std::fabs(as_number(s2) - 10.5) < 1e-12);
  assert(stats.size() == 6);
  assert(stats[0] >= 1);   // analyze_count
  assert(stats[1] == 0);   // materialize_count (already packed-double at literal creation)
  assert(stats[2] >= 1);   // cache_hit_count
  assert(plan.kind == spark::Value::Kind::Int);
  assert(plan.int_value == 2);  // PackedDouble (literal already promoted to f64 cells)

  assert(matrix.kind == spark::Value::Kind::Matrix);
  assert(matrix.matrix_value != nullptr);
  for (const auto& cell : matrix.matrix_value->data) {
    assert(cell.kind == spark::Value::Kind::Double);
  }
}

void test_matrix_cache_invalidation_on_write() {
  constexpr auto source = R"(
m = [[1, 2.5]; [3, 4]]
_ = m.reduce_sum()
before = m.cache_stats()
m[0,0] = 10
after_write = m.cache_stats()
s = m.reduce_sum()
after_reduce = m.cache_stats()
)";
  const auto before = as_int_list(run_and_get(source, "before"));
  const auto after_write = as_int_list(run_and_get(source, "after_write"));
  const auto after_reduce = as_int_list(run_and_get(source, "after_reduce"));
  const auto sum = run_and_get(source, "s");

  assert(std::fabs(as_number(sum) - 19.5) < 1e-12);
  assert(before.size() == 6);
  assert(after_write.size() == 6);
  assert(after_reduce.size() == 6);
  assert(after_write[3] >= before[3] + 1);  // invalidation count
  assert(after_reduce[0] >= before[0] + 1); // re-analyze after mutation
}

void test_matrix_cache_bytes_api_and_boxed_plan() {
  constexpr auto source = R"(
m_num = [[1, 2.5]; [3, 4]]
_ = m_num.reduce_sum()
num_bytes = m_num.cache_bytes()

m_boxed = [[1, True]; [2, 3]]
boxed_plan = m_boxed.plan_id()
boxed_cell = m_boxed[0,1]
)";
  const auto bytes = run_and_get(source, "num_bytes");
  const auto boxed_plan = run_and_get(source, "boxed_plan");
  const auto boxed_cell = run_and_get(source, "boxed_cell");

  assert(bytes.kind == spark::Value::Kind::Int);
  assert(bytes.int_value == 0);  // numeric matrix literal is already packed-f64
  assert(boxed_plan.kind == spark::Value::Kind::Int);
  assert(boxed_plan.int_value == 6);  // BoxedAny
  assert(boxed_cell.kind == spark::Value::Kind::Bool);
}

}  // namespace

void run_matrix_phase6_tests() {
  test_matrix_numeric_promote_and_cache();
  test_matrix_cache_invalidation_on_write();
  test_matrix_cache_bytes_api_and_boxed_plan();
}

}  // namespace phase6_test
