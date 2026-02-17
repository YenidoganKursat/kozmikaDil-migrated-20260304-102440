#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <exception>
#include <thread>
#include <vector>

#include "phase9_runtime.h"

namespace spark {
namespace phase9 {

namespace {

std::size_t env_thread_count() {
  if (const auto* value = std::getenv("SPARK_PHASE9_THREADS")) {
    const auto parsed = std::strtoull(value, nullptr, 10);
    if (parsed > 0) {
      return static_cast<std::size_t>(parsed);
    }
  }
  const auto hw = std::thread::hardware_concurrency();
  return hw == 0 ? 4u : static_cast<std::size_t>(hw);
}

class WorkStealingScheduler {
 public:
  WorkStealingScheduler()
      : done_(false), rr_index_(0), pending_(0), spawned_(0), executed_(0), steals_(0) {
    const auto count = env_thread_count();
    workers_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      workers_.push_back(std::make_unique<Worker>());
    }
    for (std::size_t i = 0; i < workers_.size(); ++i) {
      workers_[i]->thread = std::thread([this, i]() { worker_loop(i); });
    }
  }

  ~WorkStealingScheduler() {
    done_.store(true, std::memory_order_relaxed);
    cv_.notify_all();
    for (auto& worker : workers_) {
      if (worker->thread.joinable()) {
        worker->thread.join();
      }
    }
  }

  std::shared_future<Value> submit(std::function<Value()> fn, std::size_t* queue_hint) {
    auto promise = std::make_shared<std::promise<Value>>();
    auto future = promise->get_future().share();
    TaskItem item;
    item.run = [fn = std::move(fn), promise]() mutable {
      try {
        promise->set_value(fn());
      } catch (...) {
        promise->set_exception(std::current_exception());
      }
    };
    enqueue_item(std::move(item), queue_hint);
    return future;
  }

  void submit_fire_and_forget(std::function<void()> fn) {
    TaskItem item;
    item.run = std::move(fn);
    enqueue_item(std::move(item), nullptr);
  }

  bool assist_one() {
    TaskItem item;
    for (std::size_t i = 0; i < workers_.size(); ++i) {
      auto& worker = workers_[i];
      std::lock_guard<std::mutex> guard(worker->mutex);
      if (worker->queue.empty()) {
        continue;
      }
      item = std::move(worker->queue.back());
      worker->queue.pop_back();
      run_item(item);
      return true;
    }
    return false;
  }

  bool assist_one_from_queue(std::size_t queue_hint) {
    if (workers_.empty()) {
      return false;
    }
    TaskItem item;
    auto& worker = workers_[queue_hint % workers_.size()];
    {
      std::lock_guard<std::mutex> guard(worker->mutex);
      if (worker->queue.empty()) {
        return false;
      }
      item = std::move(worker->queue.back());
      worker->queue.pop_back();
    }
    run_item(item);
    return true;
  }

  SchedulerStats stats() const {
    SchedulerStats out;
    out.threads = workers_.size();
    out.spawned = spawned_.load(std::memory_order_relaxed);
    out.executed = executed_.load(std::memory_order_relaxed);
    out.steals = steals_.load(std::memory_order_relaxed);
    return out;
  }

 private:
  struct TaskItem {
    std::function<void()> run;
  };

  struct Worker {
    std::mutex mutex;
    std::deque<TaskItem> queue;
    std::thread thread;
  };

  void enqueue_item(TaskItem item, std::size_t* queue_hint) {
    const auto index = rr_index_.fetch_add(1, std::memory_order_relaxed) % workers_.size();
    if (queue_hint) {
      *queue_hint = index;
    }
    {
      std::lock_guard<std::mutex> guard(workers_[index]->mutex);
      workers_[index]->queue.push_back(std::move(item));
    }
    pending_.fetch_add(1, std::memory_order_relaxed);
    spawned_.fetch_add(1, std::memory_order_relaxed);
    cv_.notify_one();
  }

  bool pop_local(std::size_t worker_index, TaskItem& out) {
    auto& worker = workers_[worker_index];
    std::lock_guard<std::mutex> guard(worker->mutex);
    if (worker->queue.empty()) {
      return false;
    }
    out = std::move(worker->queue.back());
    worker->queue.pop_back();
    return true;
  }

  bool steal_remote(std::size_t worker_index, TaskItem& out) {
    for (std::size_t offset = 1; offset < workers_.size(); ++offset) {
      const auto victim_index = (worker_index + offset) % workers_.size();
      auto& victim = workers_[victim_index];
      std::lock_guard<std::mutex> guard(victim->mutex);
      if (victim->queue.empty()) {
        continue;
      }
      out = std::move(victim->queue.front());
      victim->queue.pop_front();
      steals_.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    return false;
  }

  void run_item(TaskItem& item) {
    pending_.fetch_sub(1, std::memory_order_relaxed);
    item.run();
    executed_.fetch_add(1, std::memory_order_relaxed);
  }

  void worker_loop(std::size_t worker_index) {
    while (!done_.load(std::memory_order_relaxed)) {
      TaskItem item;
      if (pop_local(worker_index, item) || steal_remote(worker_index, item)) {
        run_item(item);
        continue;
      }

      std::unique_lock<std::mutex> lock(cv_mutex_);
      cv_.wait(lock, [this]() {
        return done_.load(std::memory_order_relaxed) ||
               pending_.load(std::memory_order_relaxed) > 0;
      });
    }
  }

  std::vector<std::unique_ptr<Worker>> workers_;
  std::atomic<bool> done_;
  std::atomic<std::size_t> rr_index_;
  std::atomic<std::size_t> pending_;
  std::atomic<std::size_t> spawned_;
  std::atomic<std::size_t> executed_;
  std::atomic<std::size_t> steals_;
  mutable std::mutex cv_mutex_;
  std::condition_variable cv_;
};

WorkStealingScheduler& scheduler_instance() {
  static WorkStealingScheduler scheduler;
  return scheduler;
}

}  // namespace

std::shared_future<Value> scheduler_submit(std::function<Value()> fn) {
  return scheduler_instance().submit(std::move(fn), nullptr);
}

std::shared_future<Value> scheduler_submit(std::function<Value()> fn, std::size_t* queue_hint) {
  return scheduler_instance().submit(std::move(fn), queue_hint);
}

void scheduler_submit_fire_and_forget(std::function<void()> fn) {
  scheduler_instance().submit_fire_and_forget(std::move(fn));
}

bool scheduler_assist_one() {
  return scheduler_instance().assist_one();
}

bool scheduler_assist_one_from_queue(std::size_t queue_hint) {
  return scheduler_instance().assist_one_from_queue(queue_hint);
}

SchedulerStats scheduler_stats_snapshot() { return scheduler_instance().stats(); }

}  // namespace phase9
}  // namespace spark
