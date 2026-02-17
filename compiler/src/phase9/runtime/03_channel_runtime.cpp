#include <chrono>
#include <optional>
#include <vector>

#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {

Value channel_make_value(const std::optional<long long>& capacity) {
  auto channel = std::make_shared<ChannelHandle>();
  if (capacity.has_value()) {
    if (*capacity < 0) {
      throw EvalException("channel capacity must be non-negative");
    }
    channel->capacity = static_cast<std::size_t>(*capacity);
  }
  return Value::channel_value_of(std::move(channel));
}

Value channel_send_value(Value& channel, const Value& message) {
  if (channel.kind != Value::Kind::Channel || !channel.channel_value) {
    throw EvalException("send() expects channel receiver");
  }
  auto& state = *channel.channel_value;
  std::unique_lock<std::mutex> lock(state.mutex);
  while (!state.closed && state.capacity > 0 && state.queue.size() >= state.capacity) {
    state.wait_count += 1;
    state.cv_send.wait(lock);
  }
  if (state.closed) {
    throw EvalException("send() on closed channel");
  }
  const bool was_empty = state.queue.empty();
  state.queue.push_back(std::make_shared<Value>(message));
  state.send_count += 1;
  lock.unlock();
  if (was_empty) {
    state.cv_recv.notify_one();
  }
  return Value::nil();
}

Value channel_recv_value(Value& channel, const std::optional<long long>& timeout_ms) {
  if (channel.kind != Value::Kind::Channel || !channel.channel_value) {
    throw EvalException("recv() expects channel receiver");
  }
  auto& state = *channel.channel_value;
  std::unique_lock<std::mutex> lock(state.mutex);
  if (timeout_ms.has_value()) {
    if (*timeout_ms < 0) {
      throw EvalException("recv() timeout must be non-negative");
    }
    while (state.queue.empty() && !state.closed) {
      state.wait_count += 1;
      const auto ok = state.cv_recv.wait_for(lock, std::chrono::milliseconds(*timeout_ms)) !=
                      std::cv_status::timeout;
      if (!ok) {
        throw EvalException("recv() timeout");
      }
    }
  } else {
    while (state.queue.empty() && !state.closed) {
      state.wait_count += 1;
      state.cv_recv.wait(lock);
    }
  }

  if (state.queue.empty()) {
    return Value::nil();
  }
  const bool was_full = state.capacity > 0 && state.queue.size() >= state.capacity;
  auto value = state.queue.front();
  state.queue.pop_front();
  state.recv_count += 1;
  lock.unlock();
  if (was_full) {
    state.cv_send.notify_one();
  }
  if (!value) {
    return Value::nil();
  }
  return *value;
}

Value channel_close_value(Value& channel) {
  if (channel.kind != Value::Kind::Channel || !channel.channel_value) {
    throw EvalException("close() expects channel receiver");
  }
  auto& state = *channel.channel_value;
  {
    std::lock_guard<std::mutex> guard(state.mutex);
    state.closed = true;
  }
  state.cv_recv.notify_all();
  state.cv_send.notify_all();
  return Value::nil();
}

Value channel_stats_value(const Value& channel) {
  if (channel.kind != Value::Kind::Channel || !channel.channel_value) {
    throw EvalException("channel.stats() expects channel receiver");
  }
  const auto& state = *channel.channel_value;
  std::lock_guard<std::mutex> guard(state.mutex);
  std::vector<Value> out;
  out.reserve(6);
  out.push_back(Value::int_value_of(static_cast<long long>(state.capacity)));
  out.push_back(Value::int_value_of(static_cast<long long>(state.queue.size())));
  out.push_back(Value::int_value_of(state.closed ? 1 : 0));
  out.push_back(Value::int_value_of(static_cast<long long>(state.send_count)));
  out.push_back(Value::int_value_of(static_cast<long long>(state.recv_count)));
  out.push_back(Value::int_value_of(static_cast<long long>(state.wait_count)));
  return Value::list_value_of(std::move(out));
}

}  // namespace spark
