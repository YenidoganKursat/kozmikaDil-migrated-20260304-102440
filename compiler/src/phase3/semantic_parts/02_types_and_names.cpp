
TypeChecker::TypeChecker() {
  next_shape_id_ = 0;
}

void TypeChecker::check(const Program& program) {
  errors_.clear();
  scopes_.clear();
  scope_names_.clear();
  symbols_.clear();
  symbol_key_to_index_.clear();
  shapes_.clear();
  function_reports_.clear();
  loop_reports_.clear();
  pipelines_.clear();
  async_lowerings_.clear();
  function_stack_.clear();
  loop_stack_.clear();
  next_shape_id_ = 0;
  suppress_side_effects_ = false;

  push_scope("global");
  define_name("print", builtin_type({any_type()}, nil_type()));
  define_name("range", builtin_type({int_type(), int_type(), int_type()}, list_type(int_type()), 1));
  define_name("append", builtin_type({list_type(any_type()), any_type()}, nil_type(), 2));
  define_name("len", builtin_type({any_type()}, int_type(), 1));
  define_name("bench_tick", builtin_type({}, int_type(), 0));
  define_name("bench_tick_raw", builtin_type({}, int_type(), 0));
  define_name("bench_tick_scale_num", builtin_type({}, int_type(), 0));
  define_name("bench_tick_scale_den", builtin_type({}, int_type(), 0));
  define_name("string", builtin_type({any_type()}, string_type(), 0));
  define_name("utf8_len", builtin_type({string_type()}, int_type(), 1));
  define_name("utf16_len", builtin_type({string_type()}, int_type(), 1));
  define_name("matrix_i64", builtin_type({int_type(), int_type()}, matrix_type(int_type(), 0, 0), 2));
  define_name("matrix_f64", builtin_type({int_type(), int_type()}, matrix_type(float_type(Type::FloatKind::F64), 0, 0), 2));
  define_name("i8", builtin_type({any_type()}, int_type(), 1));
  define_name("i16", builtin_type({any_type()}, int_type(), 1));
  define_name("i32", builtin_type({any_type()}, int_type(), 1));
  define_name("i64", builtin_type({any_type()}, int_type(), 1));
  define_name("i128", builtin_type({any_type()}, int_type(), 1));
  define_name("i256", builtin_type({any_type()}, int_type(), 1));
  define_name("i512", builtin_type({any_type()}, int_type(), 1));
  define_name("f8", builtin_type({any_type()}, float_type(Type::FloatKind::F8), 1));
  define_name("f16", builtin_type({any_type()}, float_type(Type::FloatKind::F16), 1));
  define_name("bf16", builtin_type({any_type()}, float_type(Type::FloatKind::BF16), 1));
  define_name("f32", builtin_type({any_type()}, float_type(Type::FloatKind::F32), 1));
  define_name("f64", builtin_type({any_type()}, float_type(Type::FloatKind::F64), 1));
  define_name("f128", builtin_type({any_type()}, float_type(Type::FloatKind::F128), 1));
  define_name("f256", builtin_type({any_type()}, float_type(Type::FloatKind::F256), 1));
  define_name("f512", builtin_type({any_type()}, float_type(Type::FloatKind::F512), 1));
  define_name("matrix_fill_affine",
              builtin_type({int_type(), int_type(), int_type(), int_type(), int_type(),
                            float_type(Type::FloatKind::F64), float_type(Type::FloatKind::F64)},
                           matrix_type(float_type(Type::FloatKind::F64), 0, 0), 6));
  define_name("matmul_expected_sum",
              builtin_type({matrix_type(any_type(), 0, 0), matrix_type(any_type(), 0, 0)},
                           float_type(Type::FloatKind::F64), 2));
  define_name("matmul_sum",
              builtin_type({matrix_type(any_type(), 0, 0), matrix_type(any_type(), 0, 0)},
                           float_type(Type::FloatKind::F64), 2));
  define_name("matmul_sum_f32",
              builtin_type({matrix_type(any_type(), 0, 0), matrix_type(any_type(), 0, 0)},
                           float_type(Type::FloatKind::F64), 2));
  define_name("matmul4_sum",
              builtin_type({matrix_type(any_type(), 0, 0), matrix_type(any_type(), 0, 0),
                            matrix_type(any_type(), 0, 0), matrix_type(any_type(), 0, 0)},
                           float_type(Type::FloatKind::F64), 4));
  define_name("matmul4_sum_f32",
              builtin_type({matrix_type(any_type(), 0, 0), matrix_type(any_type(), 0, 0),
                            matrix_type(any_type(), 0, 0), matrix_type(any_type(), 0, 0)},
                           float_type(Type::FloatKind::F64), 4));
  define_name("accumulate_sum",
              builtin_type({float_type(Type::FloatKind::F64), any_type()},
                           float_type(Type::FloatKind::F64), 2));
  define_name("spawn", builtin_type({any_type(), any_type()}, task_type(any_type()), 1));
  define_name("join", builtin_type({task_type(any_type()), int_type()}, any_type(), 1));
  define_name("deadline", builtin_type({int_type()}, int_type(), 1));
  define_name("cancel", builtin_type({task_type(any_type())}, nil_type(), 1));
  define_name("task_group", builtin_type({int_type()}, task_group_type(), 0));
  define_name("parallel_for", builtin_type({int_type(), int_type(), any_type(), any_type()}, nil_type(), 3));
  define_name("par_map", builtin_type({list_type(any_type()), any_type()}, list_type(any_type()), 2));
  define_name("par_reduce", builtin_type({list_type(any_type()), any_type(), any_type()}, any_type(), 3));
  define_name("scheduler_stats", builtin_type({}, list_type(int_type()), 0));
  define_name("channel", builtin_type({int_type()}, channel_type(any_type(), 0), 0));
  define_name("send", builtin_type({channel_type(any_type(), 0), any_type()}, nil_type(), 2));
  define_name("recv", builtin_type({channel_type(any_type(), 0), int_type()}, any_type(), 1));
  define_name("close", builtin_type({channel_type(any_type(), 0)}, nil_type(), 1));
  define_name("stream", builtin_type({channel_type(any_type(), 0)}, channel_type(any_type(), 0), 1));
  define_name("anext", builtin_type({channel_type(any_type(), 0), int_type()}, any_type(), 1));
  define_name("None", nil_type());

  push_function_context("__main__");
  check_program_body(program.body);
  if (!function_stack_.empty()) {
    pop_function_context();
  } else {
    auto record = TierRecord{"__main__", "", TierLevel::T8, "function", {}};
    record.tier = TierLevel::T4;
    function_reports_.push_back(record);
  }
  pop_scope();
}

const std::vector<std::string>& TypeChecker::diagnostics() const {
  return errors_;
}

bool TypeChecker::has_errors() const {
  return !errors_.empty();
}

bool TypeChecker::has_fatal_errors() const {
  return has_errors();
}

const std::vector<SymbolRecord>& TypeChecker::symbols() const {
  return symbols_;
}

const std::vector<ShapeRecord>& TypeChecker::shapes() const {
  return shapes_;
}

const std::vector<TierRecord>& TypeChecker::function_reports() const {
  return function_reports_;
}

const std::vector<TierRecord>& TypeChecker::loop_reports() const {
  return loop_reports_;
}

const std::vector<PipelineRecord>& TypeChecker::pipelines() const {
  return pipelines_;
}

const std::vector<AsyncLoweringRecord>& TypeChecker::async_lowerings() const {
  return async_lowerings_;
}

std::string TypeChecker::tier_to_string(TierLevel tier) {
  switch (tier) {
    case TierLevel::T4:
      return "T4";
    case TierLevel::T5:
      return "T5";
    case TierLevel::T8:
      return "T8";
  }
  return "T8";
}

TypePtr TypeChecker::bool_type() {
  return std::make_shared<Type>(Type{.kind = Type::Kind::Bool});
}

TypePtr TypeChecker::int_type() {
  return std::make_shared<Type>(Type{.kind = Type::Kind::Int});
}

TypePtr TypeChecker::float_type(Type::FloatKind kind) {
  return std::make_shared<Type>(Type{.kind = Type::Kind::Float, .float_kind = kind});
}

TypePtr TypeChecker::string_type() {
  return std::make_shared<Type>(Type{.kind = Type::Kind::String});
}

TypePtr TypeChecker::nil_type() {
  return std::make_shared<Type>(Type{.kind = Type::Kind::Nil});
}

TypePtr TypeChecker::unknown_type() {
  return std::make_shared<Type>(Type{.kind = Type::Kind::Unknown});
}

TypePtr TypeChecker::any_type() const {
  return std::make_shared<Type>(Type{.kind = Type::Kind::Any});
}

TypePtr TypeChecker::error_type() const {
  return std::make_shared<Type>(Type{.kind = Type::Kind::Error});
}

TypePtr TypeChecker::list_type(TypePtr element_type) {
  auto type = std::make_shared<Type>();
  type->kind = Type::Kind::List;
  type->list_element = std::move(element_type);
  return type;
}

TypePtr TypeChecker::matrix_type(TypePtr element_type, std::size_t rows, std::size_t cols) {
  auto type = std::make_shared<Type>();
  type->kind = Type::Kind::Matrix;
  type->list_element = std::move(element_type);
  type->matrix_rows = rows;
  type->matrix_cols = cols;
  return type;
}

TypePtr TypeChecker::task_type(TypePtr result_type) {
  auto type = std::make_shared<Type>();
  type->kind = Type::Kind::Task;
  type->task_result = std::move(result_type);
  return type;
}

TypePtr TypeChecker::task_group_type() {
  auto type = std::make_shared<Type>();
  type->kind = Type::Kind::TaskGroup;
  return type;
}

TypePtr TypeChecker::channel_type(TypePtr element_type, std::size_t capacity) {
  auto type = std::make_shared<Type>();
  type->kind = Type::Kind::Channel;
  type->channel_element = std::move(element_type);
  type->channel_capacity = capacity;
  return type;
}

TypePtr TypeChecker::function_type(std::vector<TypePtr> params, TypePtr return_type) {
  auto type = std::make_shared<Type>();
  type->kind = Type::Kind::Function;
  type->function_params = std::move(params);
  type->function_return = std::move(return_type);
  type->arity_min = type->function_params.size();
  type->arity_max = type->function_params.size();
  return type;
}

TypePtr TypeChecker::builtin_type(std::vector<TypePtr> params, TypePtr return_type,
                                 std::optional<std::size_t> min_arity) {
  auto type = std::make_shared<Type>();
  type->kind = Type::Kind::Builtin;
  type->function_params = std::move(params);
  type->function_return = std::move(return_type);
  type->arity_max = type->function_params.size();
  type->arity_min = min_arity.value_or(type->arity_max);
  return type;
}

TypePtr TypeChecker::class_type(std::string_view name, bool open, std::size_t shape_id) {
  Type type;
  type.kind = Type::Kind::Class;
  type.class_name = std::string(name);
  type.class_open = open;
  type.class_shape_id = shape_id;
  return std::make_shared<Type>(std::move(type));
}

void TypeChecker::push_scope(std::string_view name) {
  scopes_.push_back({});
  scope_names_.push_back(std::string(name));
}

void TypeChecker::pop_scope() {
  if (!scopes_.empty()) {
    scopes_.pop_back();
    scope_names_.pop_back();
  }
}

bool TypeChecker::define_name(const std::string& name, TypePtr type) {
  if (scopes_.empty()) {
    push_scope("global");
  }
  auto& scope = scopes_.back();
  bool first_seen = !scope.count(name);
  scope[name] = type;
  const std::string owner = scope_names_.empty() ? "" : scope_names_.back();
  const std::string scope_key = owner.empty() ? "global" : owner;
  register_symbol(name, owner, scope_key, *type);
  return first_seen;
}

bool TypeChecker::set_name(const std::string& name, TypePtr type) {
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    const auto pos = it->find(name);
    if (pos != it->end()) {
      pos->second = type;
      const auto distance_from_top = static_cast<std::size_t>(std::distance(scopes_.rbegin(), it));
      const auto index = scope_names_.size() - 1 - distance_from_top;
      const std::string owner = scope_names_[index];
      const std::string scope_key = owner.empty() ? "global" : owner;
      register_symbol(name, owner, scope_key, *type);
      return true;
    }
  }
  return false;
}

void TypeChecker::register_symbol(const std::string& name, const std::string& owner, const std::string& scope,
                                 const Type& type) {
  const std::string key = owner + "::" + name;
  const std::string key2 = scope + "::" + name;
  const std::string resolved_key = !key.empty() ? key : key2;
  const auto it = symbol_key_to_index_.find(resolved_key);
  const std::string rendered = type_to_string(type);
  if (it == symbol_key_to_index_.end()) {
    const auto index = symbols_.size();
    symbols_.push_back(SymbolRecord{.name = name, .owner = owner, .scope = scope, .type = rendered});
    symbol_key_to_index_[resolved_key] = index;
    return;
  }
  symbols_[it->second].type = rendered;
}

bool TypeChecker::has_name(const std::string& name) const {
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    if (it->find(name) != it->end()) {
      return true;
    }
  }
  return false;
}

TypePtr TypeChecker::get_name(const std::string& name) const {
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    auto found = it->find(name);
    if (found != it->end()) {
      return found->second;
    }
  }
  return error_type();
}

void TypeChecker::add_error(const std::string& message) {
  errors_.push_back(message);
}

bool TypeChecker::is_numeric_type(const Type& type) const {
  return type.kind == Type::Kind::Int || type.kind == Type::Kind::Float;
}

bool TypeChecker::is_bool_like(const Type& type) const {
  return type.kind == Type::Kind::Bool || type.kind == Type::Kind::String ||
         is_numeric_type(type) || type.kind == Type::Kind::Unknown;
}

bool TypeChecker::same_or_unknown(const Type& a, const Type& b) const {
  return same_kind_or_unknown(a, b);
}

bool TypeChecker::is_assignable(const Type& source, const Type& target) const {
  if (target.kind == Type::Kind::Unknown || source.kind == Type::Kind::Unknown) {
    return true;
  }
  if (source.kind == Type::Kind::Error || target.kind == Type::Kind::Error) {
    return true;
  }
  if (source.kind == Type::Kind::Any || target.kind == Type::Kind::Any) {
    return true;
  }

  if (target.kind == Type::Kind::Float && is_numeric_type(source)) {
    return true;
  }

  if (target.kind == Type::Kind::List && source.kind == Type::Kind::List) {
    if (!target.list_element || !source.list_element) {
      return true;
    }
    return is_assignable(*source.list_element, *target.list_element);
  }
  if (target.kind == Type::Kind::Matrix && source.kind == Type::Kind::Matrix) {
    if (!target.list_element || !source.list_element) {
      return true;
    }
    if (!is_assignable(*source.list_element, *target.list_element)) {
      return false;
    }
    if (target.matrix_cols == 0 || source.matrix_cols == 0) {
      return true;
    }
    return target.matrix_cols == source.matrix_cols;
  }
  if (target.kind == Type::Kind::Task && source.kind == Type::Kind::Task) {
    if (!target.task_result || !source.task_result) {
      return true;
    }
    return is_assignable(*source.task_result, *target.task_result);
  }
  if (target.kind == Type::Kind::Channel && source.kind == Type::Kind::Channel) {
    if (!target.channel_element || !source.channel_element) {
      return true;
    }
    return is_assignable(*source.channel_element, *target.channel_element);
  }
  if (target.kind == Type::Kind::TaskGroup && source.kind == Type::Kind::TaskGroup) {
    return true;
  }
  return same_or_unknown(source, target);
}

TypePtr TypeChecker::normalize_list_elements(const TypePtr& current, const TypePtr& next) {
  if (current->kind == Type::Kind::Unknown) {
    return next;
  }
  if (next->kind == Type::Kind::Unknown) {
    return current;
  }
  if (current->kind == Type::Kind::Any || next->kind == Type::Kind::Any) {
    return any_type();
  }
  if ((current->kind == Type::Kind::Int || current->kind == Type::Kind::Float) &&
      (next->kind == Type::Kind::Int || next->kind == Type::Kind::Float)) {
    if (current->kind == Type::Kind::Int && next->kind == Type::Kind::Int) {
      return std::make_shared<Type>(Type{.kind = Type::Kind::Int});
    }
    const auto wide = widest_float(
        current->kind == Type::Kind::Float ? current->float_kind : Type::FloatKind::F64,
        next->kind == Type::Kind::Float ? next->float_kind : Type::FloatKind::F64);
    return float_type(wide);
  }
  if (current->kind == next->kind) {
    if (current->kind == Type::Kind::List && current->list_element && next->list_element) {
      return list_type(normalize_list_elements(current->list_element, next->list_element));
    }
