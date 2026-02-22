#include <cassert>
#include <cmath>

#include "../phase8_support.h"

namespace phase8_test {

namespace {

void assert_close(double lhs, double rhs, double tol = 1e-8) {
  assert(std::fabs(lhs - rhs) <= tol);
}

void test_medium_matmul_sum_cache_stability() {
  constexpr auto source = R"(
n = 32
a = matrix_fill_affine(n, n, 3, 5, 17, 0.125)
b = matrix_fill_affine(n, n, 7, 2, 19, 0.2)
s1 = matmul_sum(a, b)
s2 = matmul_sum(a, b)
tmp = a.matmul_f64(b)
ref = tmp.reduce_sum()
stats = a.matmul_stats()
)";
  const auto s1 = as_number(run_and_get(source, "s1"));
  const auto s2 = as_number(run_and_get(source, "s2"));
  const auto ref = as_number(run_and_get(source, "ref"));
  const auto stats = as_number_list(run_and_get(source, "stats"));

  assert_close(s1, ref, 1e-6);
  assert_close(s2, ref, 1e-6);
  assert(stats.size() == 12);
  assert(stats[5] >= 1.0);  // cache_hit_a
  assert(stats[6] >= 1.0);  // cache_hit_b
}

void test_axpby_epilogue_matches_manual_expression() {
  constexpr auto source = R"(
a = [[1, 2]; [3, 4]]
b = [[5, 6]; [7, 8]]
acc = [[2, 2]; [2, 2]]
ax = a.matmul_axpby(b, 1.25, 0.75, acc)
manual = (a.matmul_f64(b) * 1.25) + (acc * 0.75)
sum_ax = ax.reduce_sum()
sum_manual = manual.reduce_sum()
)";
  const auto sum_ax = as_number(run_and_get(source, "sum_ax"));
  const auto sum_manual = as_number(run_and_get(source, "sum_manual"));
  assert_close(sum_ax, sum_manual, 1e-9);
}

void test_transposed_view_matmul_expected_sum() {
  constexpr auto source = R"(
a = [[1, 2, 3]; [4, 5, 6]; [7, 8, 9]]
b = a.T
expected = matmul_expected_sum(a, b)
tmp = a.matmul_f64(b)
sum_actual = tmp.reduce_sum()
)";
  const auto expected = as_number(run_and_get(source, "expected"));
  const auto sum_actual = as_number(run_and_get(source, "sum_actual"));
  assert_close(expected, sum_actual, 1e-9);
}

}  // namespace

void run_phase8_matmul_extreme_tests() {
  test_medium_matmul_sum_cache_stability();
  test_axpby_epilogue_matches_manual_expression();
  test_transposed_view_matmul_expected_sum();
}

}  // namespace phase8_test
