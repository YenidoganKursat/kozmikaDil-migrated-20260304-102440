#include <cassert>
#include <string>
#include <vector>

#include "../phase9_support.h"

namespace phase9_test {

namespace {

bool contains_fragment(const std::vector<std::string>& lines, const std::string& needle) {
  for (const auto& line : lines) {
    if (line.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void test_spawn_requires_named_function_for_sendable_check() {
  constexpr auto source = R"(
def inc(x):
  return x + 1

t = spawn(inc(1))
)";
  const auto diagnostics = analyze_diagnostics(source);
  assert(contains_fragment(diagnostics, "spawn() callable must be named function"));
}

void test_parallel_for_rejects_non_sendable_capture() {
  constexpr auto source = R"(
def worker(i, buf):
  return i

buf = [1, 2, 3]
parallel_for(0, 10, worker, buf)
)";
  const auto diagnostics = analyze_diagnostics(source);
  assert(contains_fragment(diagnostics, "parallel_for() capture arg"));
  assert(contains_fragment(diagnostics, "not Sendable/Shareable"));
}

void test_tier_dump_mentions_phase9_scheduler_path() {
  constexpr auto source = R"(
def add(a, b):
  return a + b

xs = [1, 2, 3, 4]
out = par_reduce(xs, 0, add)
)";
  const auto tiers = analyze_dump(source, "tiers");
  assert(tiers.find("phase9 work-stealing scheduler") != std::string::npos);
}

void test_async_lowering_dump_has_state_machine_shape() {
  constexpr auto source = R"(
async fn f(x):
  a = await x
  return a
)";
  const auto dump = analyze_dump(source, "async");
  assert(dump.find("f|await_points=1|states=2") != std::string::npos);
  assert(dump.find("state|0|suspend_on=await") != std::string::npos);
  assert(dump.find("state|1|terminal=return") != std::string::npos);
}

void test_deadline_alias_typechecks_for_join() {
  constexpr auto source = R"(
async fn add1(x):
  return x + 1
t = add1(4)
out = join(t, deadline(10))
)";
  const auto diagnostics = analyze_diagnostics(source);
  assert(diagnostics.empty());
}

void test_task_group_binding_visible_after_scope() {
  constexpr auto source = R"(
def sq(x):
  return x * x

with task_group(deadline(25)) as g:
  _ = g.spawn(sq, 2)

vals = g.join_all()
out = vals[0]
)";
  const auto diagnostics = analyze_diagnostics(source);
  assert(diagnostics.empty());
}

}  // namespace

void run_phase9_safety_diagnostics_tests() {
  test_spawn_requires_named_function_for_sendable_check();
  test_parallel_for_rejects_non_sendable_capture();
  test_tier_dump_mentions_phase9_scheduler_path();
  test_async_lowering_dump_has_state_machine_shape();
  test_deadline_alias_typechecks_for_join();
  test_task_group_binding_visible_after_scope();
}

}  // namespace phase9_test
