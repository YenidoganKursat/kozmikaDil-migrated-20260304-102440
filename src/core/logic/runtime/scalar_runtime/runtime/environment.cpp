#include <atomic>
#include <memory>
#include <utility>
#include <unordered_map>
#include <vector>

#include "../../semantic_runtime/evaluator_parts/internal_helpers.h"

namespace spark {

namespace {

std::atomic<std::uint64_t> g_environment_id_counter{1};

const Environment* resolve_owner_cached(const Environment& env, const std::string& name);

template <typename TValue>
inline void assign_env_value(Value& target, TValue&& value) {
  if (!copy_numeric_value_inplace(target, value)) {
    target = std::forward<TValue>(value);
  }
}

template <typename TValue>
bool set_env_impl(Environment& self, std::string name, TValue&& value) {
  if (!self.parent) {
    auto it = self.values.find(name);
    if (it == self.values.end()) {
      return false;
    }
    if (self.frozen) {
      return false;
    }
    assign_env_value(it->second, std::forward<TValue>(value));
    return true;
  }

  if (const auto* owner = resolve_owner_cached(self, name); owner != nullptr) {
    auto* mutable_owner = const_cast<Environment*>(owner);
    auto it = mutable_owner->values.find(name);
    if (it != mutable_owner->values.end()) {
      if (mutable_owner->frozen) {
        return false;
      }
      assign_env_value(it->second, std::forward<TValue>(value));
      return true;
    }
  }

  auto* current = &self;
  while (current) {
    auto it = current->values.find(name);
    if (it != current->values.end()) {
      if (current->frozen) {
        return false;
      }
      assign_env_value(it->second, std::forward<TValue>(value));
      return true;
    }
    current = current->parent.get();
  }
  return false;
}

const Environment* resolve_owner_cached(const Environment& env, const std::string& name) {
  if (auto it = env.lookup_owner_cache.find(name); it != env.lookup_owner_cache.end()) {
    const auto* owner = it->second;
    if (owner) {
      const auto owner_it = owner->values.find(name);
      if (owner_it != owner->values.end()) {
        return owner;
      }
    }
    env.lookup_owner_cache.erase(it);
  }

  const auto* current = &env;
  while (current) {
    if (current->values.find(name) != current->values.end()) {
      env.lookup_owner_cache.emplace(name, current);
      return current;
    }
    current = current->parent.get();
  }
  return nullptr;
}

}  // namespace

Environment::Environment(std::shared_ptr<Environment> parent_env, bool is_frozen)
    : parent(std::move(parent_env)),
      frozen(is_frozen),
      stable_id(g_environment_id_counter.fetch_add(1, std::memory_order_relaxed)) {}

void Environment::define(std::string name, const Value& value) {
  auto [it, inserted] = values.try_emplace(std::move(name), value);
  if (!inserted) {
    assign_env_value(it->second, value);
  } else {
    // Map inserts can trigger rehash and invalidate cached Value* pointers.
    ++values_epoch;
  }
  lookup_owner_cache[it->first] = this;
}

void Environment::define(std::string name, Value&& value) {
  auto [it, inserted] = values.try_emplace(std::move(name), std::move(value));
  if (!inserted) {
    assign_env_value(it->second, std::move(value));
  } else {
    ++values_epoch;
  }
  lookup_owner_cache[it->first] = this;
}

bool Environment::set(std::string name, const Value& value) {
  return set_env_impl(*this, std::move(name), value);
}

bool Environment::set(std::string name, Value&& value) {
  return set_env_impl(*this, std::move(name), std::move(value));
}

bool Environment::contains(const std::string& name) const {
  if (!parent) {
    return values.find(name) != values.end();
  }
  return resolve_owner_cached(*this, name) != nullptr;
}

Value Environment::get(const std::string& name) const {
  if (!parent) {
    const auto it = values.find(name);
    if (it != values.end()) {
      return it->second;
    }
    throw EvalException("undefined variable: " + name);
  }

  if (const auto* owner = resolve_owner_cached(*this, name); owner != nullptr) {
    const auto it = owner->values.find(name);
    if (it != owner->values.end()) {
      return it->second;
    }
  }
  throw EvalException("undefined variable: " + name);
}

Value* Environment::get_ptr(const std::string& name) {
  if (!parent) {
    auto it = values.find(name);
    if (it != values.end()) {
      return &it->second;
    }
    return nullptr;
  }

  if (const auto* owner = resolve_owner_cached(*this, name); owner != nullptr) {
    auto* mutable_owner = const_cast<Environment*>(owner);
    const auto it = mutable_owner->values.find(name);
    if (it != mutable_owner->values.end()) {
      return &it->second;
    }
  }

  auto* current = this;
  while (current) {
    auto it = current->values.find(name);
    if (it != current->values.end()) {
      return &it->second;
    }
    current = current->parent.get();
  }
  return nullptr;
}

const Value* Environment::get_ptr(const std::string& name) const {
  if (!parent) {
    const auto it = values.find(name);
    if (it != values.end()) {
      return &it->second;
    }
    return nullptr;
  }

  if (const auto* owner = resolve_owner_cached(*this, name); owner != nullptr) {
    const auto it = owner->values.find(name);
    if (it != owner->values.end()) {
      return &it->second;
    }
  }
  return nullptr;
}

std::vector<std::string> Environment::keys() const {
  std::vector<std::string> out;
  const auto* current = this;
  while (current) {
    for (const auto& pair : current->values) {
      out.push_back(pair.first);
    }
    current = current->parent.get();
  }
  return out;
}

}  // namespace spark
