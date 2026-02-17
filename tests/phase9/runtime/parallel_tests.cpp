#include <cassert>
#include <cmath>

#include "../phase9_support.h"

namespace phase9_test {

namespace {

void assert_close(double lhs, double rhs, double tol = 1e-9) {
  assert(std::fabs(lhs - rhs) <= tol);
}

void test_par_map_and_par_reduce() {
  constexpr auto source = R"(
def sq(x):
  return x * x
def add(a, b):
  return a + b

xs = [1, 2, 3, 4, 5, 6, 7, 8]
ys = par_map(xs, sq)
out = par_reduce(ys, 0, add)
)";
  const auto out = run_and_get(source, "out");
  assert_close(as_number(out), 204.0);
}

void test_parallel_for_with_channel_sink() {
  constexpr auto source = R"(
def emit(i, ch):
  send(ch, i + 1)

n = 64
ch = channel(64)
parallel_for(0, n, emit, ch)

acc = 0
i = 0
while i < n:
  acc = acc + recv(ch)
  i = i + 1
close(ch)
)";
  const auto acc = run_and_get(source, "acc");
  assert_close(as_number(acc), 2080.0);
}

}  // namespace

void run_phase9_parallel_tests() {
  test_par_map_and_par_reduce();
  test_parallel_for_with_channel_sink();
}

}  // namespace phase9_test
