#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {

Value stream_value(Value& channel) {
  if (channel.kind != Value::Kind::Channel || !channel.channel_value) {
    throw EvalException("stream() expects channel value");
  }
  // Stream v1 is channel-backed. This keeps event-driven consumption zero-copy.
  return channel;
}

Value stream_next_value(Value& stream, const std::optional<long long>& timeout_ms) {
  return channel_recv_value(stream, timeout_ms);
}

Value stream_has_next_value(const Value& stream) {
  if (stream.kind != Value::Kind::Channel || !stream.channel_value) {
    throw EvalException("has_next() expects channel-backed stream");
  }
  const auto& state = *stream.channel_value;
  std::lock_guard<std::mutex> guard(state.mutex);
  const bool has_next = !state.queue.empty() || !state.closed;
  return Value::bool_value_of(has_next);
}

}  // namespace spark
