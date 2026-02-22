#include <cassert>
#include <cmath>

#include "../phase9_support.h"

namespace phase9_test {

namespace {

void assert_close(double lhs, double rhs, double tol = 1e-9) {
  assert(std::fabs(lhs - rhs) <= tol);
}

void test_parallel_for_square_sum() {
  constexpr auto source = R"(
def emit_square(i, ch):
  send(ch, i * i)

n = 256
ch = channel(256)
parallel_for(0, n, emit_square, ch)

acc = 0
i = 0
while i < n:
  acc = acc + recv(ch)
  i = i + 1
close(ch)
expected = (n * (n - 1) * (2 * n - 1)) / 6
)";
  const auto acc = run_and_get(source, "acc");
  const auto expected = run_and_get(source, "expected");
  assert_close(as_number(acc), as_number(expected));
}

void test_par_map_reduce_large_input() {
  constexpr auto source = R"(
def affine(x):
  return x * 3 + 1

def add(a, b):
  return a + b

xs = []
i = 0
while i < 512:
  xs.append(i)
  i = i + 1

ys = par_map(xs, affine)
sum_parallel = par_reduce(ys, 0, add)
sum_serial = 0
for v in ys:
  sum_serial = sum_serial + v
)";
  const auto sum_parallel = run_and_get(source, "sum_parallel");
  const auto sum_serial = run_and_get(source, "sum_serial");
  assert_close(as_number(sum_parallel), as_number(sum_serial));
}

}  // namespace

void run_phase9_parallel_extreme_tests() {
  test_parallel_for_square_sum();
  test_par_map_reduce_large_input();
}

}  // namespace phase9_test
