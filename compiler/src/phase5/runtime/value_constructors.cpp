#include <vector>

#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {

Value Value::nil() {
  Value value;
  value.kind = Kind::Nil;
  return value;
}

Value Value::int_value_of(long long v) {
  Value value;
  value.kind = Kind::Int;
  value.int_value = v;
  return value;
}

Value Value::double_value_of(double v) {
  Value value;
  value.kind = Kind::Double;
  value.double_value = v;
  return value;
}

Value Value::bool_value_of(bool v) {
  Value value;
  value.kind = Kind::Bool;
  value.bool_value = v;
  return value;
}

Value Value::list_value_of(std::vector<Value> values) {
  Value value;
  value.kind = Kind::List;
  value.list_value = std::move(values);
  value.list_cache = ListCache{};
  return value;
}

Value Value::function(std::shared_ptr<Function> fn) {
  Value value;
  value.kind = Kind::Function;
  value.function_value = std::move(fn);
  return value;
}

Value Value::builtin(std::string name, std::function<Value(const std::vector<Value>&)> impl) {
  Value value;
  value.kind = Kind::Builtin;
  value.builtin_value = std::make_shared<Builtin>(Builtin{std::move(name), std::move(impl)});
  return value;
}

Value Value::task_value_of(std::shared_ptr<TaskHandle> task) {
  Value value;
  value.kind = Kind::Task;
  value.task_value = std::move(task);
  return value;
}

Value Value::task_group_value_of(std::shared_ptr<TaskGroupHandle> task_group) {
  Value value;
  value.kind = Kind::TaskGroup;
  value.task_group_value = std::move(task_group);
  return value;
}

Value Value::channel_value_of(std::shared_ptr<ChannelHandle> channel) {
  Value value;
  value.kind = Kind::Channel;
  value.channel_value = std::move(channel);
  return value;
}

}  // namespace spark
