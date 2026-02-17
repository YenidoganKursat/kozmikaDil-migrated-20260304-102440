#include <cassert>
#include <string>

#include "../phase8_support.h"

namespace phase8_test {

namespace {

void test_type_dump_contains_matmul_result() {
  constexpr auto source = R"(
a = [[1, 2, 3]; [4, 5, 6]]
b = [[7, 8]; [9, 10]; [11, 12]]
c = a.matmul(b)
stats = c.matmul_stats()
)";
  const auto dump = analyze_dump(source, "types");
  assert(dump.find("c|global|global|Matrix[Float(f64)][2,2]") != std::string::npos);
  assert(dump.find("stats|global|global|List[Int]") != std::string::npos);
}

void test_tier_dump_contains_matmul_reason() {
  constexpr auto source = R"(
a = [[1, 2]; [3, 4]]
b = [[5, 6]; [7, 8]]
c = a.matmul_f64(b)
)";
  const auto dump = analyze_dump(source, "tiers");
  assert(dump.find("matmul eligible for phase8 kernel scheduling path") != std::string::npos);
}

}  // namespace

void run_phase8_analyze_tests() {
  test_type_dump_contains_matmul_result();
  test_tier_dump_contains_matmul_reason();
}

}  // namespace phase8_test
