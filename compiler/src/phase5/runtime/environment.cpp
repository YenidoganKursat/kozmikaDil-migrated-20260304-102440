#include <memory>
#include <unordered_map>
#include <vector>

#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {

Environment::Environment(std::shared_ptr<Environment> parent_env)
    : parent(std::move(parent_env)) {}

void Environment::define(std::string name, const Value& value) {
  values.emplace(std::move(name), value);
}

bool Environment::set(std::string name, const Value& value) {
  auto current = this;
  while (current) {
    auto it = current->values.find(name);
    if (it != current->values.end()) {
      it->second = value;
      return true;
    }
    current = current->parent.get();
  }
  return false;
}

bool Environment::contains(const std::string& name) const {
  const auto* current = this;
  while (current) {
    if (current->values.find(name) != current->values.end()) {
      return true;
    }
    current = current->parent.get();
  }
  return false;
}

Value Environment::get(const std::string& name) const {
  const auto* current = this;
  while (current) {
    auto it = current->values.find(name);
    if (it != current->values.end()) {
      return it->second;
    }
    current = current->parent.get();
  }
  throw EvalException("undefined variable: " + name);
}

Value* Environment::get_ptr(const std::string& name) {
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
