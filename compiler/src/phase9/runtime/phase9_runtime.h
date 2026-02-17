#pragma once

#include <functional>
#include <future>

#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {
namespace phase9 {

struct SchedulerStats {
  std::size_t threads = 0;
  std::size_t spawned = 0;
  std::size_t executed = 0;
  std::size_t steals = 0;
};

std::shared_future<Value> scheduler_submit(std::function<Value()> fn);
void scheduler_submit_fire_and_forget(std::function<void()> fn);
std::shared_future<Value> scheduler_submit(std::function<Value()> fn, std::size_t* queue_hint);
bool scheduler_assist_one();
bool scheduler_assist_one_from_queue(std::size_t queue_hint);
SchedulerStats scheduler_stats_snapshot();

}  // namespace phase9
}  // namespace spark
