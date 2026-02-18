#include <cassert>
#include <cmath>
#include <cstdlib>

#include "../phase8_support.h"

namespace phase8_test {

namespace {

void assert_close(double lhs, double rhs, double tol = 1e-9) {
  assert(std::fabs(lhs - rhs) <= tol);
}

void test_matmul_basic_correctness() {
  constexpr auto source = R"(
a = [[1, 2, 3]; [4, 5, 6]]
b = [[7, 8]; [9, 10]; [11, 12]]
c = a.matmul(b)
)";
  const auto c = run_and_get(source, "c");
  assert(c.kind == spark::Value::Kind::Matrix);
  assert(c.matrix_value && c.matrix_value->rows == 2 && c.matrix_value->cols == 2);
  const auto flat = as_matrix_flat(c);
  assert_close(flat[0], 58.0);
  assert_close(flat[1], 64.0);
  assert_close(flat[2], 139.0);
  assert_close(flat[3], 154.0);
}

void test_matmul_add_and_axpby() {
  constexpr auto source = R"(
a = [[1, 2]; [3, 4]]
b = [[5, 6]; [7, 8]]
bias = [10, 20]
acc = [[1, 1]; [1, 1]]
c1 = a.matmul_add(b, bias)
c2 = a.matmul_axpby(b, 2.0, 0.5, acc)
stats = a.matmul_stats()
)";
  const auto c1 = as_matrix_flat(run_and_get(source, "c1"));
  const auto c2 = as_matrix_flat(run_and_get(source, "c2"));
  const auto stats = as_number_list(run_and_get(source, "stats"));

  assert_close(c1[0], 29.0);
  assert_close(c1[1], 42.0);
  assert_close(c1[2], 53.0);
  assert_close(c1[3], 70.0);

  assert_close(c2[0], 38.5);
  assert_close(c2[1], 44.5);
  assert_close(c2[2], 86.5);
  assert_close(c2[3], 100.5);

  assert(stats.size() == 12);
  assert(stats[7] >= 2.0);  // epilogue_fused_calls
}

void test_matmul_f32_and_cache_hits() {
  constexpr auto source = R"(
a = [[1.0, 2.0, 3.0]; [4.0, 5.0, 6.0]]
b = [[1.0, 2.0]; [3.0, 4.0]; [5.0, 6.0]]
c1 = a.matmul_f32(b)
c2 = a.matmul_f32(b)
stats = a.matmul_stats()
schedule = a.matmul_schedule()
)";
  setenv("SPARK_MATMUL_BACKEND", "own", 1);
  const auto c1 = as_matrix_flat(run_and_get(source, "c1"));
  const auto c2 = as_matrix_flat(run_and_get(source, "c2"));
  const auto stats = as_number_list(run_and_get(source, "stats"));
  const auto schedule = as_number_list(run_and_get(source, "schedule"));
  unsetenv("SPARK_MATMUL_BACKEND");

  assert(c1.size() == 4);
  assert(c2.size() == 4);
  assert_close(c1[0], 22.0);
  assert_close(c1[1], 28.0);
  assert_close(c1[2], 49.0);
  assert_close(c1[3], 64.0);
  assert_close(c2[0], 22.0);
  assert_close(c2[1], 28.0);
  assert_close(c2[2], 49.0);
  assert_close(c2[3], 64.0);

  assert(stats.size() == 12);
  assert(stats[0] >= 2.0);  // calls
  assert(stats[5] >= 1.0);  // cache_hit_a
  assert(stats[6] >= 1.0);  // cache_hit_b

  assert(schedule.size() == 10);
  assert(schedule[1] >= 8.0);  // tile_m
  assert(schedule[2] >= 8.0);  // tile_n
  assert(schedule[3] >= 8.0);  // tile_k
}

void test_fill_affine_and_expected_sum_builtin() {
  constexpr auto source = R"(
a = matrix_fill_affine(2, 3, 2, 1, 5, 0.5)
b = matrix_fill_affine(3, 2, 1, 2, 7, 0.25)
expected = matmul_expected_sum(a, b)
c = a.matmul_f64(b)
total = c.reduce_sum()
)";
  const auto expected = run_and_get(source, "expected");
  const auto total = run_and_get(source, "total");
  assert_close(as_number(expected), as_number(total), 1e-9);
}

void test_matmul_sum_fused_builtin() {
  constexpr auto source = R"(
a = matrix_fill_affine(8, 8, 3, 5, 17, 0.125)
b = matrix_fill_affine(8, 8, 7, 2, 19, 0.2)
sum_fused = matmul_sum(a, b)
sum_fused_f32 = matmul_sum_f32(a, b)
c = a.matmul_f64(b)
sum_ref = c.reduce_sum()
)";
  const auto sum_fused = as_number(run_and_get(source, "sum_fused"));
  const auto sum_fused_f32 = as_number(run_and_get(source, "sum_fused_f32"));
  const auto sum_ref = as_number(run_and_get(source, "sum_ref"));
  assert_close(sum_fused, sum_ref, 1e-9);
  assert_close(sum_fused_f32, sum_ref, 1e-4);
}

void test_matmul4_sum_fused_builtin() {
  constexpr auto source = R"(
a = matrix_fill_affine(6, 6, 3, 5, 17, 0.125)
b = matrix_fill_affine(6, 6, 7, 2, 19, 0.2)
c = matrix_fill_affine(6, 6, 11, 13, 23, 0.1)
d = matrix_fill_affine(6, 6, 5, 17, 29, 0.05)
sum_fused = matmul4_sum(a, b, c, d)
sum_fused_f32 = matmul4_sum_f32(a, b, c, d)
t1 = a.matmul_f64(b)
t2 = t1.matmul_f64(c)
t3 = t2.matmul_f64(d)
sum_ref = t3.reduce_sum()
)";
  const auto sum_fused = as_number(run_and_get(source, "sum_fused"));
  const auto sum_fused_f32 = as_number(run_and_get(source, "sum_fused_f32"));
  const auto sum_ref = as_number(run_and_get(source, "sum_ref"));
  assert_close(sum_fused, sum_ref, 1e-8);
  assert_close(sum_fused_f32, sum_ref, 1e-3);
}

void test_matmul4_sum_dense_only_affine_fill() {
  setenv("SPARK_MATRIX_FILL_DENSE_ONLY", "1", 1);
  constexpr auto source = R"(
n = 128
a = matrix_fill_affine(n, n, 3, 5, 17, 0.125)
b = matrix_fill_affine(n, n, 7, 2, 19, 0.2)
c = matrix_fill_affine(n, n, 11, 13, 23, 0.1)
d = matrix_fill_affine(n, n, 5, 17, 29, 0.05)
init_sum = accumulate_sum(0.0, a)
sum_fused = matmul4_sum(a, b, c, d)
t1 = a.matmul_f64(b)
t2 = t1.matmul_f64(c)
t3 = t2.matmul_f64(d)
sum_ref = t3.reduce_sum()
)";
  const auto init_sum = as_number(run_and_get(source, "init_sum"));
  const auto sum_fused = as_number(run_and_get(source, "sum_fused"));
  const auto sum_ref = as_number(run_and_get(source, "sum_ref"));
  unsetenv("SPARK_MATRIX_FILL_DENSE_ONLY");

  assert(init_sum > 0.0);
  assert_close(sum_fused, sum_ref, 1e-3);
}

}  // namespace

void run_phase8_matmul_tests() {
  test_matmul_basic_correctness();
  test_matmul_add_and_axpby();
  test_matmul_f32_and_cache_hits();
  test_fill_affine_and_expected_sum_builtin();
  test_matmul_sum_fused_builtin();
  test_matmul4_sum_fused_builtin();
  test_matmul4_sum_dense_only_affine_fill();
}

}  // namespace phase8_test
