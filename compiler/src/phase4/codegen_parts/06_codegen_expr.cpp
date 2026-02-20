void CodeGenerator::emit_default_return(const FunctionContext& ctx) {
  if (ctx.is_main) {
    emit_line("return 0");
    return;
  }

  if (ctx.return_kind == ScalarKind::Bool) {
    emit_line("return false");
  } else if (ctx.return_kind == ScalarKind::Int) {
    emit_line("return 0");
  } else if (ctx.return_kind == ScalarKind::Float) {
    emit_line("return 0.0");
  } else if (ctx.return_kind == ScalarKind::String) {
    emit_line("return \"\"");
  } else {
    emit_line("return");
  }
}

std::string CodeGenerator::scalar_kind_to_name(ScalarKind kind) const {
  switch (kind) {
    case ScalarKind::Int:
      return "i64";
    case ScalarKind::Float:
      return "f64";
    case ScalarKind::Bool:
      return "bool";
    case ScalarKind::String:
      return "str";
    case ScalarKind::Void:
      return "void";
    case ScalarKind::Unknown:
      return "unknown";
    case ScalarKind::Invalid:
      return "invalid";
    case ScalarKind::ListInt:
      return "list_i64";
    case ScalarKind::ListFloat:
      return "list_f64";
    case ScalarKind::ListAny:
      return "list_i64";
    case ScalarKind::MatrixInt:
      return "matrix_i64";
    case ScalarKind::MatrixFloat:
      return "matrix_f64";
    case ScalarKind::MatrixAny:
      return "matrix_i64";
  }
  return "unknown";
}

std::string CodeGenerator::value_kind_to_name(ValueKind kind) const {
  return scalar_kind_to_name(static_cast<ScalarKind>(kind));
}

std::string CodeGenerator::scalar_to_constant(double value, ScalarKind expected) const {
  if (expected == ScalarKind::Float || expected == ScalarKind::Unknown) {
    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return stream.str();
  }
  return std::to_string(static_cast<long long>(value));
}

std::string CodeGenerator::next_temp() {
  return "%t" + std::to_string(temp_id_++);
}

std::string CodeGenerator::next_label() {
  return "L" + std::to_string(label_id_++);
}

std::string CodeGenerator::indent() const {
  return std::string(indent_level_ * 2, ' ');
}

void CodeGenerator::emit_line(const std::string& line) {
  output_ << indent() << line << "\n";
}

void CodeGenerator::emit_label(const std::string& label) {
  output_ << label << ":\n";
}

void CodeGenerator::begin_indent() {
  ++indent_level_;
}

void CodeGenerator::end_indent() {
  if (indent_level_ > 0) {
    --indent_level_;
  }
}

ScalarKind CodeGenerator::coerce_numeric(ScalarKind left, ScalarKind right) const {
  if (left == ScalarKind::Float || right == ScalarKind::Float) {
    return ScalarKind::Float;
  }
  if (left == ScalarKind::Int && right == ScalarKind::Int) {
    return ScalarKind::Int;
  }
  return ScalarKind::Invalid;
}

ScalarKind CodeGenerator::merge_types(ScalarKind left, ScalarKind right) const {
  if (left == ScalarKind::Invalid || right == ScalarKind::Invalid) {
    return ScalarKind::Invalid;
  }
  if (left == ScalarKind::Unknown) {
    return right;
  }
  if (right == ScalarKind::Unknown) {
    return left;
  }
  if (left == right) {
    return left;
  }
  if ((left == ScalarKind::Int && right == ScalarKind::Float) || (left == ScalarKind::Float && right == ScalarKind::Int)) {
    return ScalarKind::Float;
  }
  if (left == ScalarKind::String && right == ScalarKind::String) {
    return ScalarKind::String;
  }
  return ScalarKind::Invalid;
}

ScalarKind CodeGenerator::finalize_return_type(const std::vector<ScalarKind>& types) const {
  if (types.empty()) {
    return ScalarKind::Void;
  }
  ScalarKind out = ScalarKind::Unknown;
  for (const auto type : types) {
    out = merge_types(out, type);
    if (out == ScalarKind::Invalid) {
      return ScalarKind::Invalid;
    }
  }
  return out == ScalarKind::Unknown ? ScalarKind::Void : out;
}

ScalarKind CodeGenerator::ensure_expected(ScalarKind actual, ExpectedExprContext expected, const std::string& node) {
  const ScalarKind required =
      (expected == ExpectedExprContext::Int)
          ? ScalarKind::Int
          : (expected == ExpectedExprContext::Float)
                ? ScalarKind::Float
                : (expected == ExpectedExprContext::Bool ? ScalarKind::Bool : ScalarKind::Unknown);

  if (expected == ExpectedExprContext::None || required == ScalarKind::Unknown) {
    return actual;
  }

  if (actual == ScalarKind::Unknown) {
    return required;
  }
  if (actual == required) {
    return actual;
  }

  if (required == ScalarKind::Float && actual == ScalarKind::Int) {
    return ScalarKind::Int;
  }
  if ((required == ScalarKind::Int && actual == ScalarKind::Float) && actual == ScalarKind::Float) {
    add_error(node + ": cannot silently narrow float to int");
    return ScalarKind::Invalid;
  }

  if (required == ScalarKind::Bool && (actual == ScalarKind::Int || actual == ScalarKind::Float)) {
    return actual;
  }
  if (required == ScalarKind::Bool && actual == ScalarKind::String) {
    return actual;
  }

  add_error(node + ": type mismatch");
  return ScalarKind::Invalid;
}

ScalarKind CodeGenerator::ensure_bool_for_condition(Code& code, FunctionContext& ctx) {
  (void)ctx;
  if (code.kind == ScalarKind::Bool) {
    return ScalarKind::Bool;
  }

  if (code.kind == ScalarKind::Int) {
    const auto temp = next_temp();
    emit_line(temp + " = cmp.ne.i64 " + code.value + ", 0");
    code = Code{temp, ScalarKind::Bool, true};
    return ScalarKind::Bool;
  }

  if (code.kind == ScalarKind::Float) {
    const auto temp = next_temp();
    emit_line(temp + " = cmp.ne.f64 " + code.value + ", 0.0");
    code = Code{temp, ScalarKind::Bool, true};
    return ScalarKind::Bool;
  }
  if (code.kind == ScalarKind::String) {
    const auto len_temp = next_temp();
    const auto bool_temp = next_temp();
    emit_line(len_temp + " = call @__spark_string_len(" + code.value + ")");
    emit_line(bool_temp + " = cmp.ne.i64 " + len_temp + ", 0");
    code = Code{bool_temp, ScalarKind::Bool, true};
    return ScalarKind::Bool;
  }

  add_error("control-flow condition must be bool/int/float/string");
  return ScalarKind::Invalid;
}

ScalarKind CodeGenerator::lookup_var_type(const FunctionContext& ctx, const std::string& name) const {
  for (const auto& scope : ctx.scopes) {
    auto it = scope.find(name);
    if (it != scope.end()) {
      return it->second;
    }
  }
  return ScalarKind::Invalid;
}

bool CodeGenerator::has_var(const FunctionContext& ctx, const std::string& name) const {
  return lookup_var_type(ctx, name) != ScalarKind::Invalid;
}

void CodeGenerator::set_var_type(FunctionContext& ctx, const std::string& name, ScalarKind kind) {
  if (ctx.scopes.empty()) {
    push_scope(ctx);
  }
  auto& scope = ctx.scopes.back();

  auto it = scope.find(name);
  if (it == scope.end()) {
    scope.emplace(name, kind);
    if (is_container_kind(kind) && kind != ScalarKind::Unknown) {
      const auto default_elem = ValueKind::Unknown;
      ctx.container_element_kinds[name] = default_elem;
    }
    return;
  }

  if (it->second == ScalarKind::Unknown) {
    it->second = kind;
    return;
  }
  if (kind == ScalarKind::Unknown) {
    return;
  }

  it->second = merge_types(it->second, kind);
  if (is_container_kind(kind)) {
    if (it->second != ScalarKind::Invalid) {
      const auto container_it = ctx.container_element_kinds.find(name);
      if (container_it == ctx.container_element_kinds.end()) {
        const auto default_elem = infer_container_scalar_type(kind);
        ctx.container_element_kinds.emplace(name, default_elem);
      }
    }
  }
}

std::string CodeGenerator::lookup_numeric_hint(const FunctionContext& ctx, const std::string& name) const {
  const auto it = ctx.scalar_numeric_hints.find(name);
  if (it == ctx.scalar_numeric_hints.end()) {
    return {};
  }
  return it->second;
}

void CodeGenerator::set_numeric_hint(FunctionContext& ctx, const std::string& name, const std::string& hint) {
  if (hint.empty()) {
    ctx.scalar_numeric_hints.erase(name);
    return;
  }
  ctx.scalar_numeric_hints[name] = hint;
}

void CodeGenerator::clear_numeric_hint(FunctionContext& ctx, const std::string& name) {
  ctx.scalar_numeric_hints.erase(name);
}

ValueKind CodeGenerator::lookup_container_element_type(const FunctionContext& ctx, const std::string& name) const {
  auto it = ctx.container_element_kinds.find(name);
  if (it != ctx.container_element_kinds.end()) {
    return it->second;
  }
  const auto container_kind = lookup_var_type(ctx, name);
  if (container_kind == ValueKind::Invalid || !is_container_kind(container_kind)) {
    return ValueKind::Unknown;
  }
  return infer_container_scalar_type(container_kind);
}

void CodeGenerator::set_container_element_type(FunctionContext& ctx, const std::string& name, ValueKind kind) {
  if (name.empty()) {
    return;
  }
  if (kind == ValueKind::Unknown) {
    return;
  }

  const auto container_kind = lookup_var_type(ctx, name);
  if (container_kind == ValueKind::Invalid || !is_container_kind(container_kind)) {
    return;
  }

  auto it = ctx.container_element_kinds.find(name);
  if (it == ctx.container_element_kinds.end()) {
    ctx.container_element_kinds.emplace(name, kind);
    return;
  }
  if (it->second == ValueKind::Unknown) {
    it->second = kind;
    return;
  }
  if (kind == it->second) {
    return;
  }
  if (is_container_kind(kind) && is_container_kind(it->second)) {
    it->second = is_list_kind(container_kind) ? ValueKind::ListAny : ValueKind::MatrixAny;
    return;
  }
  if (!is_container_kind(kind) && !is_container_kind(it->second) &&
      is_scalar_numeric_kind(kind) && is_scalar_numeric_kind(it->second)) {
    auto merged = merge_types(kind, it->second);
    if (merged == ValueKind::Float) {
      it->second = is_list_kind(container_kind) ? ValueKind::ListFloat : ValueKind::MatrixFloat;
    } else if (merged == ValueKind::Int) {
      it->second = is_list_kind(container_kind) ? ValueKind::ListInt : ValueKind::MatrixInt;
    } else {
      it->second = is_list_kind(container_kind) ? ValueKind::ListAny : ValueKind::MatrixAny;
    }
    return;
  }
  if ((is_container_kind(kind) || is_container_kind(it->second)) && kind != it->second) {
    it->second = is_list_kind(container_kind) ? ValueKind::ListAny : ValueKind::MatrixAny;
    return;
  }
  it->second = is_list_kind(container_kind) ? ValueKind::ListAny : ValueKind::MatrixAny;
}

ValueKind CodeGenerator::infer_container_scalar_type(ValueKind container_kind) const {
  if (container_kind == ValueKind::ListFloat || container_kind == ValueKind::MatrixFloat) {
    return ValueKind::Float;
  }
  if (container_kind == ValueKind::ListInt || container_kind == ValueKind::MatrixInt) {
    return ValueKind::Int;
  }
  if (container_kind == ValueKind::ListAny || container_kind == ValueKind::MatrixAny) {
    return ValueKind::Int;
  }
  return ValueKind::Unknown;
}

ValueKind CodeGenerator::default_container_element_for(const std::string& variable_name, const std::string& container_name) const {
  (void)variable_name;
  (void)container_name;
  return ValueKind::Int;
}

void CodeGenerator::emit_var_decl_if_needed(FunctionContext& ctx, const std::string& name, ScalarKind kind) {
  if (has_var(ctx, name)) {
    return;
  }
  const auto resolved = kind == ScalarKind::Unknown ? ScalarKind::Int : kind;
  if (resolved == ScalarKind::Float) {
    const auto hint = lookup_numeric_hint(ctx, name);
    if (hint == "f8" || hint == "f16" || hint == "bf16" || hint == "f32" || hint == "f64" ||
        hint == "f128" || hint == "f256" || hint == "f512") {
      emit_line("var " + name + ": " + hint);
    } else {
      emit_line("var " + name + ": f64");
    }
  } else if (resolved == ScalarKind::String) {
    emit_line("var " + name + ": str");
  } else {
    emit_line("var " + name + ": " + scalar_kind_to_name(resolved));
  }
  set_var_type(ctx, name, kind == ScalarKind::Unknown ? ScalarKind::Int : kind);
}

void CodeGenerator::push_scope(FunctionContext& ctx) {
  ctx.scopes.emplace_back();
}

void CodeGenerator::pop_scope(FunctionContext& ctx) {
  if (!ctx.scopes.empty()) {
    ctx.scopes.pop_back();
  }
}

void CodeGenerator::merge_scopes(FunctionContext& base, const FunctionContext& branch) {
  if (base.scopes.empty() || branch.scopes.empty()) {
    return;
  }
  const auto max_depth = std::min(base.scopes.size(), branch.scopes.size());
  for (std::size_t depth = 0; depth < max_depth; ++depth) {
    for (const auto& pair : branch.scopes[depth]) {
      auto it = base.scopes[depth].find(pair.first);
      if (it == base.scopes[depth].end()) {
        base.scopes[depth].insert(pair);
      } else {
        it->second = merge_types(it->second, pair.second);
      }
    }
  }
}

void CodeGenerator::add_error(const std::string& message) {
  diagnostics_.push_back(message);
}

const CodeGenerator::FunctionSignature* CodeGenerator::find_function_signature(const std::string& name) const {
  for (const auto& fn : functions_) {
    if (fn.name == name) {
      return &fn;
    }
  }
  return nullptr;
}

CodeGenerator::FunctionSignature* CodeGenerator::find_function_signature_mut(const std::string& name) {
  for (auto& fn : functions_) {
    if (fn.name == name) {
      return &fn;
    }
  }
  return nullptr;
}

}  // namespace spark
