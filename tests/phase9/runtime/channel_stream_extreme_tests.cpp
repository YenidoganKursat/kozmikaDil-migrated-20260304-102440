#include <cassert>
#include <cmath>

#include "../phase9_support.h"

namespace phase9_test {

namespace {

void assert_close(double lhs, double rhs, double tol = 1e-9) {
  assert(std::fabs(lhs - rhs) <= tol);
}

void test_small_capacity_channel_backpressure_flow() {
  constexpr auto source = R"(
def producer(ch, n):
  i = 0
  while i < n:
    send(ch, i)
    i = i + 1
  close(ch)

n = 128
ch = channel(1)
t = spawn(producer, ch, n)
acc = 0
i = 0
while i < n:
  acc = acc + recv(ch)
  i = i + 1
_ = join(t)
expected = (n * (n - 1)) / 2
)";
  const auto acc = run_and_get(source, "acc");
  const auto expected = run_and_get(source, "expected");
  assert_close(as_number(acc), as_number(expected));
}

void test_async_for_stream_large_sum() {
  constexpr auto source = R"(
def producer(ch, n):
  i = 1
  while i <= n:
    send(ch, i)
    i = i + 1
  close(ch)

n = 64
ch = channel(4)
_ = spawn(producer, ch, n)
acc = 0
async for v in stream(ch):
  acc = acc + v
expected = (n * (n + 1)) / 2
)";
  const auto acc = run_and_get(source, "acc");
  const auto expected = run_and_get(source, "expected");
  assert_close(as_number(acc), as_number(expected));
}

}  // namespace

void run_phase9_channel_stream_extreme_tests() {
  test_small_capacity_channel_backpressure_flow();
  test_async_for_stream_large_sum();
}

}  // namespace phase9_test
