CodegenResult CodeGenerator::generate(const Program& program, const CodegenOptions& options) {
  diagnostics_.clear();
  output_.str("");
  output_.clear();
  functions_.clear();
  temp_id_ = 0;
  label_id_ = 0;
  indent_level_ = 0;
  verbose_ = options.verbose;

  const bool ok = infer_top_level(program) && compile_top_level(program);

  CodegenResult result;
  result.success = ok && diagnostics_.empty();
  result.output = output_.str();
  result.diagnostics = diagnostics_;
  return result;
}

bool CodeGenerator::infer_top_level(const Program& program) {
  for (const auto& stmt : program.body) {
    if (stmt->kind == Stmt::Kind::FunctionDef) {
      const auto& fn = static_cast<const FunctionDefStmt&>(*stmt);
      if (fn.is_async) {
        add_error("async function definitions are unsupported in phase4 codegen: " + fn.name);
        return false;
      }
      FunctionSignature signature;
      signature.name = fn.name;
      signature.params = fn.params;
      signature.param_kinds.assign(fn.params.size(), ScalarKind::Unknown);
      signature.return_kind = ScalarKind::Unknown;
      functions_.push_back(signature);
      continue;
    }
    if (stmt->kind == Stmt::Kind::ClassDef) {
      const auto& cls = static_cast<const ClassDefStmt&>(*stmt);
      add_error("class definitions are unsupported in phase4 codegen: " + cls.name);
      return false;
    }
  }

  for (std::size_t i = 0; i < functions_.size(); ++i) {
    for (std::size_t j = i + 1; j < functions_.size(); ++j) {
      if (functions_[i].name == functions_[j].name) {
        add_error("duplicate function definition: " + functions_[i].name);
      }
    }
  }

  auto signature_by_name = [&](const std::string& name) -> FunctionSignature* {
    for (auto& fn : functions_) {
      if (fn.name == name) {
        return &fn;
      }
    }
    return nullptr;
  };

  std::unordered_map<std::string, const FunctionDefStmt*> function_defs;
  for (const auto& stmt : program.body) {
    if (stmt->kind == Stmt::Kind::FunctionDef) {
      const auto& fn = static_cast<const FunctionDefStmt&>(*stmt);
      function_defs[fn.name] = &fn;
    }
  }

  auto infer_numeric_ctor_kind = [](const std::string& name) -> ValueKind {
    if (name == "i8" || name == "i16" || name == "i32" || name == "i64" || name == "i128" || name == "i256" ||
        name == "i512") {
      return ValueKind::Int;
    }
    if (name == "f8" || name == "f16" || name == "bf16" || name == "f32" || name == "f64" || name == "f128" ||
        name == "f256" || name == "f512") {
      return ValueKind::Float;
    }
    return ValueKind::Unknown;
  };

  std::function<ValueKind(const Expr&, const std::unordered_map<std::string, ValueKind>&)> infer_expr_kind;
  infer_expr_kind = [&](const Expr& expr, const std::unordered_map<std::string, ValueKind>& env) -> ValueKind {
    switch (expr.kind) {
      case Expr::Kind::Number: {
        const auto& number = static_cast<const NumberExpr&>(expr);
        return number.is_int ? ValueKind::Int : ValueKind::Float;
      }
      case Expr::Kind::String:
        return ValueKind::String;
      case Expr::Kind::Bool:
        return ValueKind::Bool;
      case Expr::Kind::Variable: {
        const auto& variable = static_cast<const VariableExpr&>(expr);
        const auto it = env.find(variable.name);
        return it == env.end() ? ValueKind::Unknown : it->second;
      }
      case Expr::Kind::Unary: {
        const auto& unary = static_cast<const UnaryExpr&>(expr);
        if (unary.op == UnaryOp::Not) {
          return ValueKind::Bool;
        }
        return infer_expr_kind(*unary.operand, env);
      }
      case Expr::Kind::Binary: {
        const auto& binary = static_cast<const BinaryExpr&>(expr);
        const auto left = infer_expr_kind(*binary.left, env);
        const auto right = infer_expr_kind(*binary.right, env);
        switch (binary.op) {
          case BinaryOp::Eq:
          case BinaryOp::Ne:
          case BinaryOp::Lt:
          case BinaryOp::Lte:
          case BinaryOp::Gt:
          case BinaryOp::Gte:
          case BinaryOp::And:
          case BinaryOp::Or:
            return ValueKind::Bool;
          case BinaryOp::Add:
            if (left == ValueKind::String && right == ValueKind::String) {
              return ValueKind::String;
            }
            if (left == ValueKind::Float || right == ValueKind::Float) {
              return ValueKind::Float;
            }
            if ((left == ValueKind::Int || left == ValueKind::Bool) && (right == ValueKind::Int || right == ValueKind::Bool)) {
              return ValueKind::Int;
            }
            return ValueKind::Unknown;
          case BinaryOp::Sub:
          case BinaryOp::Mul:
          case BinaryOp::Mod:
            if (left == ValueKind::Float || right == ValueKind::Float) {
              return ValueKind::Float;
            }
            if ((left == ValueKind::Int || left == ValueKind::Bool) && (right == ValueKind::Int || right == ValueKind::Bool)) {
              return ValueKind::Int;
            }
            return ValueKind::Unknown;
          case BinaryOp::Div:
          case BinaryOp::Pow:
            return ValueKind::Float;
        }
        return ValueKind::Unknown;
      }
      case Expr::Kind::Call: {
        const auto& call = static_cast<const CallExpr&>(expr);
        if (call.callee->kind == Expr::Kind::Variable) {
          const auto& name = static_cast<const VariableExpr&>(*call.callee).name;
          if (name == "len" || name == "utf8_len" || name == "utf16_len" || name == "bench_tick" ||
              name == "bench_tick_raw" || name == "bench_tick_scale_num" ||
              name == "bench_tick_scale_den") {
            return ValueKind::Int;
          }
          if (name == "string") {
            return ValueKind::String;
          }
          if (name == "range") {
            return ValueKind::ListInt;
          }
          if (name == "matrix_i64") {
            return ValueKind::MatrixInt;
          }
          if (name == "matrix_f64") {
            return ValueKind::MatrixFloat;
          }
          const auto ctor_kind = infer_numeric_ctor_kind(name);
          if (ctor_kind != ValueKind::Unknown) {
            return ctor_kind;
          }
          if (auto* signature = signature_by_name(name)) {
            return signature->return_kind;
          }
        }
        return ValueKind::Unknown;
      }
      case Expr::Kind::List: {
        const auto& list = static_cast<const ListExpr&>(expr);
        bool has_float = false;
        for (const auto& element : list.elements) {
          const auto kind = infer_expr_kind(*element, env);
          if (kind == ValueKind::Float) {
            has_float = true;
          }
          if (kind == ValueKind::String || kind == ValueKind::ListAny || kind == ValueKind::MatrixAny) {
            return ValueKind::ListAny;
          }
        }
        return has_float ? ValueKind::ListFloat : ValueKind::ListInt;
      }
      case Expr::Kind::Index: {
        const auto& index = static_cast<const IndexExpr&>(expr);
        const auto target_kind = infer_expr_kind(*index.target, env);
        if (target_kind == ValueKind::String) {
          return ValueKind::String;
        }
        if (target_kind == ValueKind::ListFloat) {
          return ValueKind::Float;
        }
        if (target_kind == ValueKind::ListInt) {
          return ValueKind::Int;
        }
        return ValueKind::Unknown;
      }
      case Expr::Kind::Attribute: {
        const auto& attr = static_cast<const AttributeExpr&>(expr);
        if (attr.attribute == "T" || attr.attribute == "transpose") {
          const auto target_kind = infer_expr_kind(*attr.target, env);
          if (is_matrix_kind(target_kind)) {
            return target_kind;
          }
        }
        return ValueKind::Unknown;
      }
    }
    return ValueKind::Unknown;
  };

  std::function<void(const Expr&, const std::unordered_map<std::string, ValueKind>&)> seed_call_param_types_expr;
  std::function<void(const Stmt&, std::unordered_map<std::string, ValueKind>&)> seed_call_param_types_stmt;
  seed_call_param_types_expr = [&](const Expr& expr, const std::unordered_map<std::string, ValueKind>& env) {
    if (expr.kind == Expr::Kind::Call) {
      const auto& call = static_cast<const CallExpr&>(expr);
      if (call.callee->kind == Expr::Kind::Variable) {
        const auto& name = static_cast<const VariableExpr&>(*call.callee).name;
        if (auto* signature = signature_by_name(name)) {
          for (std::size_t i = 0; i < call.args.size() && i < signature->param_kinds.size(); ++i) {
            const auto kind = infer_expr_kind(*call.args[i], env);
            if (kind == ValueKind::Unknown || is_container_kind(kind)) {
              continue;
            }
            if (signature->param_kinds[i] == ValueKind::Unknown) {
              signature->param_kinds[i] = kind;
            } else {
              signature->param_kinds[i] = merge_types(signature->param_kinds[i], kind);
            }
          }
        }
      }
      seed_call_param_types_expr(*call.callee, env);
      for (const auto& arg : call.args) {
        seed_call_param_types_expr(*arg, env);
      }
      return;
    }
    if (expr.kind == Expr::Kind::Binary) {
      const auto& binary = static_cast<const BinaryExpr&>(expr);
      seed_call_param_types_expr(*binary.left, env);
      seed_call_param_types_expr(*binary.right, env);
      return;
    }
    if (expr.kind == Expr::Kind::Unary) {
      const auto& unary = static_cast<const UnaryExpr&>(expr);
      seed_call_param_types_expr(*unary.operand, env);
      return;
    }
    if (expr.kind == Expr::Kind::List) {
      const auto& list = static_cast<const ListExpr&>(expr);
      for (const auto& element : list.elements) {
        seed_call_param_types_expr(*element, env);
      }
      return;
    }
    if (expr.kind == Expr::Kind::Index) {
      const auto& index = static_cast<const IndexExpr&>(expr);
      seed_call_param_types_expr(*index.target, env);
      for (const auto& item : index.indices) {
        if (item.index) {
          seed_call_param_types_expr(*item.index, env);
        }
        if (item.slice_start) {
          seed_call_param_types_expr(*item.slice_start, env);
        }
        if (item.slice_stop) {
          seed_call_param_types_expr(*item.slice_stop, env);
        }
        if (item.slice_step) {
          seed_call_param_types_expr(*item.slice_step, env);
        }
      }
      return;
    }
    if (expr.kind == Expr::Kind::Attribute) {
      const auto& attr = static_cast<const AttributeExpr&>(expr);
      seed_call_param_types_expr(*attr.target, env);
    }
  };

  seed_call_param_types_stmt = [&](const Stmt& stmt, std::unordered_map<std::string, ValueKind>& env) {
    switch (stmt.kind) {
      case Stmt::Kind::Expression: {
        const auto& expression_stmt = static_cast<const ExpressionStmt&>(stmt);
        seed_call_param_types_expr(*expression_stmt.expression, env);
        break;
      }
      case Stmt::Kind::Assign: {
        const auto& assign = static_cast<const AssignStmt&>(stmt);
        seed_call_param_types_expr(*assign.value, env);
        if (assign.target->kind == Expr::Kind::Variable) {
          const auto& name = static_cast<const VariableExpr&>(*assign.target).name;
          env[name] = infer_expr_kind(*assign.value, env);
        }
        break;
      }
      case Stmt::Kind::Return: {
        const auto& ret = static_cast<const ReturnStmt&>(stmt);
        if (ret.value) {
          seed_call_param_types_expr(*ret.value, env);
        }
        break;
      }
      case Stmt::Kind::If: {
        const auto& if_stmt = static_cast<const IfStmt&>(stmt);
        seed_call_param_types_expr(*if_stmt.condition, env);
        auto then_env = env;
        for (const auto& body_stmt : if_stmt.then_body) {
          seed_call_param_types_stmt(*body_stmt, then_env);
        }
        auto else_env = env;
        for (const auto& body_stmt : if_stmt.else_body) {
          seed_call_param_types_stmt(*body_stmt, else_env);
        }
        break;
      }
      case Stmt::Kind::While: {
        const auto& while_stmt = static_cast<const WhileStmt&>(stmt);
        seed_call_param_types_expr(*while_stmt.condition, env);
        auto body_env = env;
        for (const auto& body_stmt : while_stmt.body) {
          seed_call_param_types_stmt(*body_stmt, body_env);
        }
        break;
      }
      case Stmt::Kind::For: {
        const auto& for_stmt = static_cast<const ForStmt&>(stmt);
        seed_call_param_types_expr(*for_stmt.iterable, env);
        auto body_env = env;
        for (const auto& body_stmt : for_stmt.body) {
          seed_call_param_types_stmt(*body_stmt, body_env);
        }
        break;
      }
      case Stmt::Kind::FunctionDef:
      case Stmt::Kind::ClassDef:
      case Stmt::Kind::WithTaskGroup:
        break;
    }
  };

  // Pass 1: seed function parameter kinds from observable callsites.
  std::unordered_map<std::string, ValueKind> top_level_env;
  for (const auto& stmt : program.body) {
    if (stmt->kind == Stmt::Kind::FunctionDef) {
      continue;
    }
    seed_call_param_types_stmt(*stmt, top_level_env);
  }
  for (const auto& [name, fn] : function_defs) {
    auto* signature = signature_by_name(name);
    if (!signature || !fn) {
      continue;
    }
    std::unordered_map<std::string, ValueKind> env;
    for (std::size_t i = 0; i < fn->params.size() && i < signature->param_kinds.size(); ++i) {
      env[fn->params[i]] = signature->param_kinds[i];
    }
    for (const auto& stmt : fn->body) {
      seed_call_param_types_stmt(*stmt, env);
    }
  }

  // Pass 2: infer function return kinds with seeded parameter kinds.
  for (const auto& [name, fn] : function_defs) {
    auto* signature = signature_by_name(name);
    if (!signature || !fn) {
      continue;
    }
    std::unordered_map<std::string, ValueKind> env;
    for (std::size_t i = 0; i < fn->params.size() && i < signature->param_kinds.size(); ++i) {
      env[fn->params[i]] = signature->param_kinds[i];
    }
    ValueKind merged_return = ValueKind::Unknown;
    std::function<void(const Stmt&, std::unordered_map<std::string, ValueKind>&)> visit_stmt;
    visit_stmt = [&](const Stmt& stmt, std::unordered_map<std::string, ValueKind>& local_env) {
      switch (stmt.kind) {
        case Stmt::Kind::Assign: {
          const auto& assign = static_cast<const AssignStmt&>(stmt);
          if (assign.target->kind == Expr::Kind::Variable) {
            const auto& var = static_cast<const VariableExpr&>(*assign.target).name;
            local_env[var] = infer_expr_kind(*assign.value, local_env);
          }
          break;
        }
        case Stmt::Kind::Return: {
          const auto& ret = static_cast<const ReturnStmt&>(stmt);
          const auto ret_kind = ret.value ? infer_expr_kind(*ret.value, local_env) : ValueKind::Void;
          merged_return = merge_types(merged_return, ret_kind);
          break;
        }
        case Stmt::Kind::If: {
          const auto& if_stmt = static_cast<const IfStmt&>(stmt);
          auto then_env = local_env;
          for (const auto& body_stmt : if_stmt.then_body) {
            visit_stmt(*body_stmt, then_env);
          }
          auto else_env = local_env;
          for (const auto& body_stmt : if_stmt.else_body) {
            visit_stmt(*body_stmt, else_env);
          }
          break;
        }
        case Stmt::Kind::While: {
          const auto& while_stmt = static_cast<const WhileStmt&>(stmt);
          auto body_env = local_env;
          for (const auto& body_stmt : while_stmt.body) {
            visit_stmt(*body_stmt, body_env);
          }
          break;
        }
        case Stmt::Kind::For: {
          const auto& for_stmt = static_cast<const ForStmt&>(stmt);
          auto body_env = local_env;
          for (const auto& body_stmt : for_stmt.body) {
            visit_stmt(*body_stmt, body_env);
          }
          break;
        }
        case Stmt::Kind::Expression:
        case Stmt::Kind::FunctionDef:
        case Stmt::Kind::ClassDef:
        case Stmt::Kind::WithTaskGroup:
          break;
      }
    };
    for (const auto& stmt : fn->body) {
      visit_stmt(*stmt, env);
    }
    if (merged_return == ValueKind::Unknown) {
      merged_return = ValueKind::Void;
    }
    signature->return_kind = merged_return;
  }

  return true;
}

bool CodeGenerator::compile_top_level(const Program& program) {
  emit_line("module:");
  begin_indent();

  for (const auto& stmt : program.body) {
    if (stmt->kind == Stmt::Kind::FunctionDef) {
      const auto& fn = static_cast<const FunctionDefStmt&>(*stmt);
      if (!emit_function(fn)) {
        return false;
      }
    }
  }

  std::vector<const Stmt*> main_body;
  for (const auto& stmt : program.body) {
    if (stmt->kind != Stmt::Kind::FunctionDef && stmt->kind != Stmt::Kind::ClassDef) {
      main_body.push_back(stmt.get());
    }
  }
  if (!emit_main_body(main_body)) {
    return false;
  }

  end_indent();
  return true;
}

bool CodeGenerator::emit_main_body(const std::vector<const Stmt*>& body) {
  FunctionContext ctx;
  ctx.name = "__main__";
  ctx.is_main = true;
  ctx.return_kind = ScalarKind::Void;
  push_scope(ctx);

  emit_line("function @__main__() -> void {");
  begin_indent();
  compile_block(body, ctx);
  if (!ctx.has_terminated) {
    emit_default_return(ctx);
  }
  end_indent();
  emit_line("}");
  emit_line("");

  pop_scope(ctx);
  return true;
}

bool CodeGenerator::emit_function(const FunctionDefStmt& fn) {
  auto* signature = find_function_signature_mut(fn.name);
  if (!signature) {
    add_error("internal: missing function signature for " + fn.name);
    return false;
  }

  FunctionContext ctx;
  ctx.name = fn.name;
  ctx.is_main = false;
  ctx.return_kind = signature->return_kind;
  push_scope(ctx);

  for (std::size_t i = 0; i < fn.params.size(); ++i) {
    const auto inferred = i < signature->param_kinds.size() ? signature->param_kinds[i] : ScalarKind::Int;
    set_var_type(ctx, fn.params[i], inferred == ScalarKind::Unknown ? ScalarKind::Int : inferred);
  }

  std::string decl = "function @" + fn.name + "(";
  for (std::size_t i = 0; i < fn.params.size(); ++i) {
    const auto inferred = i < signature->param_kinds.size() ? signature->param_kinds[i] : ScalarKind::Int;
    const auto type_name = scalar_kind_to_name(inferred == ScalarKind::Unknown ? ScalarKind::Int : inferred);
    decl += fn.params[i] + ": " + type_name;
    if (i + 1 < fn.params.size()) {
      decl += ", ";
    }
  }
  decl += ") -> " + scalar_kind_to_name(signature->return_kind == ScalarKind::Unknown ? ScalarKind::Unknown : signature->return_kind);
  decl += " {";
  emit_line(decl);

  begin_indent();
  for (const auto& param : fn.params) {
    emit_var_decl_if_needed(ctx, param, lookup_var_type(ctx, param));
  }
  compile_block(to_stmt_refs(fn.body), ctx);
  if (!ctx.has_terminated) {
    emit_default_return(ctx);
  }

  signature->return_kind = finalize_return_type(ctx.return_types);

  end_indent();
  emit_line("}");
  emit_line("");
  pop_scope(ctx);

  return diagnostics_.empty();
}

bool CodeGenerator::compile_block(const std::vector<const Stmt*>& block, FunctionContext& ctx) {
  for (const auto& stmt : block) {
    if (!compile_stmt(*stmt, ctx)) {
      return false;
    }
    if (ctx.has_terminated) {
      return true;
    }
  }
  return true;
}

bool CodeGenerator::compile_stmt(const Stmt& stmt, FunctionContext& ctx) {
  switch (stmt.kind) {
    case Stmt::Kind::Expression: {
      const auto& expression_stmt = static_cast<const ExpressionStmt&>(stmt);
      const auto value = emit_expr(*expression_stmt.expression, ctx, ExpectedExprContext::None);
      if (!value.has_value || ctx.has_terminated) {
        return true;
      }
      if (verbose_ && value.has_value && value.kind != ScalarKind::Void && value.kind != ScalarKind::Invalid) {
        emit_line("; expr: " + value.value);
      }
      return true;
    }
    case Stmt::Kind::Assign: {
      const auto& assign = static_cast<const AssignStmt&>(stmt);
      if (assign.target->kind == Expr::Kind::Variable) {
        const auto& target_name = static_cast<const VariableExpr&>(*assign.target).name;
        const auto value = emit_expr(*assign.value, ctx, ExpectedExprContext::None);
        if (!value.has_value) {
          add_error("cannot compile assignment RHS for '" + target_name + "'");
          return false;
        }
        auto next = ensure_expected(value.kind, ExpectedExprContext::None, "assignment '" + target_name + "'");
        if (next == ScalarKind::Invalid) {
          return false;
        }
        set_var_type(ctx, target_name, next == ScalarKind::Unknown ? ScalarKind::Int : next);
        if (next == ScalarKind::Float) {
          set_numeric_hint(ctx, target_name, value.numeric_hint);
        } else {
          clear_numeric_hint(ctx, target_name);
        }
        emit_var_decl_if_needed(ctx, target_name, lookup_var_type(ctx, target_name));
        emit_line(target_name + " = " + value.value);
        return true;
      }

      if (assign.target->kind == Expr::Kind::Index) {
        const auto& index_target = static_cast<const IndexExpr&>(*assign.target);
        const auto value = emit_expr(*assign.value, ctx, ExpectedExprContext::None);
        if (!value.has_value) {
          add_error("cannot compile assignment RHS for indexed target");
          return false;
        }
        return emit_indexed_assignment(index_target, value, ctx);
      }

      add_error("assignment target must be a variable or index target");
      return false;
    }
    case Stmt::Kind::Return: {
      const auto& ret = static_cast<const ReturnStmt&>(stmt);
      if (!ret.value) {
        ctx.has_terminated = true;
        emit_line("return");
        ctx.return_types.push_back(ScalarKind::Void);
        return true;
      }

      const auto value = emit_expr(*ret.value, ctx, ExpectedExprContext::None);
      if (!value.has_value) {
        add_error("cannot compile return expression");
        return false;
      }
      ctx.has_terminated = true;
      ctx.return_types.push_back(value.kind == ScalarKind::Unknown ? ScalarKind::Int : value.kind);
      emit_line("return " + value.value);
      return true;
    }
    case Stmt::Kind::If:
      return emit_if_statement(static_cast<const IfStmt&>(stmt), ctx);
    case Stmt::Kind::While:
      return emit_while_statement(static_cast<const WhileStmt&>(stmt), ctx);
    case Stmt::Kind::For:
      return emit_for_statement(static_cast<const ForStmt&>(stmt), ctx);
    case Stmt::Kind::FunctionDef: {
      const auto& fn = static_cast<const FunctionDefStmt&>(stmt);
      add_error("nested function definition unsupported in phase4: " + fn.name);
      return false;
    }
    case Stmt::Kind::ClassDef:
      add_error("class definitions unsupported in phase4");
      return false;
    case Stmt::Kind::WithTaskGroup:
      add_error("with task_group is unsupported in phase4 codegen");
      return false;
  }
  return true;
}

bool CodeGenerator::emit_indexed_assignment(const IndexExpr& target, const Code& value, FunctionContext& ctx) {
  const auto& value_for_store = value;
  if (value_for_store.kind != ValueKind::Int && value_for_store.kind != ValueKind::Float &&
      value_for_store.kind != ValueKind::Bool) {
    add_error("unsupported value type for indexed assignment");
    return false;
  }

  // Flatten chained index assignment target into base + index list
  // while preserving index order for multi-index forms:
  // a[0][1] -> [0, 1], m[r, c] -> [r, c].
  const auto flattened = flatten_index_chain(target);
  std::vector<const IndexExpr::IndexItem*> items = flattened.indices;
  const Expr* current = flattened.base;

  if (items.empty()) {
    add_error("empty index target in assignment");
    return false;
  }
  if (items.size() > 2) {
    add_error("index chain too deep for assignment");
    return false;
  }

  if (!current || current->kind != Expr::Kind::Variable) {
    add_error("indexed assignment target must ultimately be a variable");
    return false;
  }

  const auto& base = static_cast<const VariableExpr&>(*current);
  const auto container_kind = lookup_var_type(ctx, base.name);
  if (!is_container_index_assignable(container_kind)) {
    add_error("target '" + base.name + "' is not index-assignable");
    return false;
  }

  std::vector<Code> index_codes;
  for (const auto* item : items) {
    if (!item || item->is_slice) {
      add_error("slice assignment unsupported in this phase");
      return false;
    }
    if (!item->index) {
      add_error("invalid index item");
      return false;
    }
    auto idx = emit_expr(*item->index, ctx, ExpectedExprContext::Int);
    if (!idx.has_value) {
      return false;
    }
    if (idx.kind != ValueKind::Int && idx.kind != ValueKind::Bool) {
      add_error("index must be integer");
      return false;
    }
    index_codes.push_back(idx);
  }

  Code stored_value = value_for_store;
  const auto element_kind = container_element_kind(container_kind);
  if (element_kind == ValueKind::Int && stored_value.kind == ValueKind::Float) {
    const auto casted = next_temp();
    emit_line(casted + " = cast.f64_to_i64 " + stored_value.value);
    stored_value.value = casted;
    stored_value.kind = ValueKind::Int;
  } else if (element_kind == ValueKind::Float && stored_value.kind == ValueKind::Int) {
    const auto casted = next_temp();
    emit_line(casted + " = cast.i64_to_f64 " + stored_value.value);
    stored_value.value = casted;
    stored_value.kind = ValueKind::Float;
  }

  if (is_list_kind(container_kind)) {
    if (index_codes.size() != 1) {
      add_error("list assignment supports one index");
      return false;
    }
    emit_line("call @" + container_index_set_fn(container_kind) + "(" + base.name + ", " + index_codes[0].value + ", " +
              stored_value.value + ")");
    return true;
  }

  if (is_matrix_kind(container_kind)) {
    if (index_codes.size() == 1) {
      if (stored_value.kind != (is_float_kind(container_kind) ? ValueKind::ListFloat : ValueKind::ListInt)) {
        add_error("matrix row assignment requires a matching typed row list");
        return false;
      }

      emit_line("call @" + matrix_set_row_fn_for(container_kind) + "(" + base.name + ", " + index_codes[0].value + ", " +
                stored_value.value + ")");
      return true;
    }

    if (index_codes.size() != 2) {
      add_error("matrix assignment requires row/column indexes or row list value");
      return false;
    }
    emit_line("call @" + container_index_set_fn(container_kind) + "(" + base.name + ", " + index_codes[0].value + ", " +
              index_codes[1].value + ", " + stored_value.value + ")");
    return true;
  }

  add_error("unsupported indexed assignment target");
  return false;
}

bool CodeGenerator::emit_if_statement(const IfStmt& if_stmt, FunctionContext& ctx) {
  const std::size_t branch_count = 1 + if_stmt.elif_branches.size();
  const std::string end_label = next_label();

  std::vector<std::string> test_labels;
  std::vector<std::string> body_labels;
  test_labels.reserve(branch_count);
  body_labels.reserve(branch_count);
  for (std::size_t i = 0; i < branch_count; ++i) {
    test_labels.push_back(next_label());
    body_labels.push_back(next_label());
  }

  const std::string else_label = if_stmt.else_body.empty() ? end_label : next_label();
  bool all_paths_return = true;

  emit_label(test_labels[0]);

  for (std::size_t i = 0; i < branch_count; ++i) {
    const Expr* cond = (i == 0) ? if_stmt.condition.get() : if_stmt.elif_branches[i - 1].first.get();
    const auto& body = (i == 0) ? if_stmt.then_body : if_stmt.elif_branches[i - 1].second;

    const std::string false_target =
        (i + 1 < branch_count) ? test_labels[i + 1] : (if_stmt.else_body.empty() ? end_label : else_label);

    auto cond_code = emit_expr(*cond, ctx, ExpectedExprContext::None);
    if (!cond_code.has_value) {
      return false;
    }
    if (ensure_bool_for_condition(cond_code, ctx) == ScalarKind::Invalid) {
      return false;
    }

    emit_line("br_if " + cond_code.value + ", " + body_labels[i] + ", " + false_target);

    emit_label(body_labels[i]);
    FunctionContext branch_ctx = ctx;
    if (!compile_block(to_stmt_refs(body), branch_ctx)) {
      return false;
    }
    merge_scopes(ctx, branch_ctx);
    if (!branch_ctx.has_terminated) {
      all_paths_return = false;
      emit_line("goto " + end_label);
    }

    if (i + 1 < branch_count) {
      emit_label(test_labels[i + 1]);
    }
  }

  if (!if_stmt.else_body.empty()) {
    emit_label(else_label);
    FunctionContext else_ctx = ctx;
    if (!compile_block(to_stmt_refs(if_stmt.else_body), else_ctx)) {
      return false;
    }
    merge_scopes(ctx, else_ctx);
    if (!else_ctx.has_terminated) {
      all_paths_return = false;
      emit_line("goto " + end_label);
    }
  }

  emit_label(end_label);
  if (all_paths_return) {
    ctx.has_terminated = true;
  }

  return true;
}

bool CodeGenerator::emit_while_statement(const WhileStmt& while_stmt, FunctionContext& ctx) {
  // Canonical numeric recurrence fast path:
  // while i < N:
  //   acc = acc <op> rhs
  //   i = i + 1
  //
  // Lower to repeat kernel call so hot-loop dispatch/branch overhead is removed.
  // Strict behavior remains available in helper kernels; this pass only rewrites shape-stable loops.
  const auto contains_variable = [](const Expr& expr, const std::string& name) {
    std::function<bool(const Expr&)> visit = [&](const Expr& node) -> bool {
      switch (node.kind) {
        case Expr::Kind::Variable:
          return static_cast<const VariableExpr&>(node).name == name;
        case Expr::Kind::Unary:
          return visit(*static_cast<const UnaryExpr&>(node).operand);
        case Expr::Kind::Binary: {
          const auto& binary = static_cast<const BinaryExpr&>(node);
          return visit(*binary.left) || visit(*binary.right);
        }
        case Expr::Kind::Call: {
          const auto& call = static_cast<const CallExpr&>(node);
          if (visit(*call.callee)) {
            return true;
          }
          for (const auto& arg : call.args) {
            if (visit(*arg)) {
              return true;
            }
          }
          return false;
        }
        case Expr::Kind::List: {
          const auto& list = static_cast<const ListExpr&>(node);
          for (const auto& element : list.elements) {
            if (visit(*element)) {
              return true;
            }
          }
          return false;
        }
        case Expr::Kind::Index: {
          const auto& index = static_cast<const IndexExpr&>(node);
          if (visit(*index.target)) {
            return true;
          }
          for (const auto& idx_item : index.indices) {
            if (idx_item.is_slice) {
              if (idx_item.slice_start && visit(*idx_item.slice_start)) {
                return true;
              }
              if (idx_item.slice_stop && visit(*idx_item.slice_stop)) {
                return true;
              }
              if (idx_item.slice_step && visit(*idx_item.slice_step)) {
                return true;
              }
              continue;
            }
            if (idx_item.index && visit(*idx_item.index)) {
              return true;
            }
          }
          return false;
        }
        case Expr::Kind::Attribute:
          return visit(*static_cast<const AttributeExpr&>(node).target);
        default:
          return false;
      }
    };
    return visit(expr);
  };

  struct NumericRepeatPattern {
    std::string index_name;
    const Expr* bound_expr = nullptr;
    std::string target_name;
    BinaryOp op = BinaryOp::Add;
    const Expr* rhs_expr = nullptr;
  };

  const auto detect_numeric_repeat_pattern = [&](NumericRepeatPattern& out) -> bool {
    if (!while_stmt.condition || while_stmt.condition->kind != Expr::Kind::Binary) {
      return false;
    }
    const auto& cond = static_cast<const BinaryExpr&>(*while_stmt.condition);
    if (cond.op != BinaryOp::Lt || cond.left->kind != Expr::Kind::Variable) {
      return false;
    }
    if (cond.right->kind != Expr::Kind::Variable && cond.right->kind != Expr::Kind::Number) {
      return false;
    }

    const auto& index_name = static_cast<const VariableExpr&>(*cond.left).name;
    bool saw_step = false;
    bool saw_update = false;
    std::string update_target;
    BinaryOp update_op = BinaryOp::Add;
    const Expr* update_rhs = nullptr;

    for (const auto& stmt : while_stmt.body) {
      if (!stmt || stmt->kind != Stmt::Kind::Assign) {
        return false;
      }
      const auto& assign = static_cast<const AssignStmt&>(*stmt);
      if (!assign.target || assign.target->kind != Expr::Kind::Variable || !assign.value) {
        return false;
      }
      const auto& assign_name = static_cast<const VariableExpr&>(*assign.target).name;
      if (assign_name == index_name) {
        if (saw_step || assign.value->kind != Expr::Kind::Binary) {
          return false;
        }
        const auto& step_rhs = static_cast<const BinaryExpr&>(*assign.value);
        if (step_rhs.op != BinaryOp::Add) {
          return false;
        }
        long long cst = 0;
        const bool left_match = step_rhs.left->kind == Expr::Kind::Variable &&
                                static_cast<const VariableExpr&>(*step_rhs.left).name == index_name;
        const bool right_match = step_rhs.right->kind == Expr::Kind::Variable &&
                                 static_cast<const VariableExpr&>(*step_rhs.right).name == index_name;
        const bool left_inc = is_integer_constant_expr(*step_rhs.left, cst) && cst == 1;
        const bool right_inc = is_integer_constant_expr(*step_rhs.right, cst) && cst == 1;
        if (!((left_match && right_inc) || (right_match && left_inc))) {
          return false;
        }
        saw_step = true;
        continue;
      }

      if (saw_update || assign.value->kind != Expr::Kind::Binary) {
        return false;
      }
      const auto& update = static_cast<const BinaryExpr&>(*assign.value);
      if (update.op != BinaryOp::Add && update.op != BinaryOp::Sub && update.op != BinaryOp::Mul &&
          update.op != BinaryOp::Div && update.op != BinaryOp::Mod && update.op != BinaryOp::Pow) {
        return false;
      }

      // Keep semantics predictable: require acc to be the left operand in recurrence.
      if (update.left->kind != Expr::Kind::Variable ||
          static_cast<const VariableExpr&>(*update.left).name != assign_name) {
        return false;
      }

      if (update.right->kind != Expr::Kind::Variable && update.right->kind != Expr::Kind::Number) {
        return false;
      }
      if (contains_variable(*update.right, index_name) || contains_variable(*update.right, assign_name)) {
        return false;
      }

      saw_update = true;
      update_target = assign_name;
      update_op = update.op;
      update_rhs = update.right.get();
    }

    if (!saw_step || !saw_update || update_target.empty() || update_rhs == nullptr) {
      return false;
    }
    if (contains_variable(*cond.right, update_target)) {
      return false;
    }

    out.index_name = index_name;
    out.bound_expr = cond.right.get();
    out.target_name = update_target;
    out.op = update_op;
    out.rhs_expr = update_rhs;
    return true;
  };

  const auto try_emit_numeric_repeat_fastpath = [&]() -> bool {
    NumericRepeatPattern pattern;
    if (!detect_numeric_repeat_pattern(pattern)) {
      return false;
    }

    const auto target_kind = lookup_var_type(ctx, pattern.target_name);
    if (target_kind != ValueKind::Float) {
      return false;
    }
    std::string kind_suffix;
    const auto hint = lookup_numeric_hint(ctx, pattern.target_name);
    if (hint == "f8" || hint == "f16" || hint == "bf16" || hint == "f32" || hint == "f64" || hint == "f128" ||
        hint == "f256" || hint == "f512") {
      kind_suffix = hint;
    } else {
      kind_suffix = "f64";
    }

    auto rhs_code = emit_expr(*pattern.rhs_expr, ctx, ExpectedExprContext::None);
    if (!rhs_code.has_value || rhs_code.kind == ValueKind::Invalid) {
      return false;
    }
    auto bound_code = emit_expr(*pattern.bound_expr, ctx, ExpectedExprContext::None);
    if (!bound_code.has_value || bound_code.kind == ValueKind::Invalid) {
      return false;
    }

    if (bound_code.kind == ValueKind::Float) {
      const auto cast = next_temp();
      emit_line(cast + " = cast.f64_to_i64 " + bound_code.value);
      bound_code.value = cast;
      bound_code.kind = ValueKind::Int;
    }

    const auto iterations = next_temp();
    emit_line(iterations + " = sub.i64 " + bound_code.value + ", " + pattern.index_name);

    const auto positive = next_temp();
    emit_line(positive + " = cmp.gt.i64 " + iterations + ", 0");

    const std::string fast_label = next_label();
    const std::string end_label = next_label();
    emit_line("br_if " + positive + ", " + fast_label + ", " + end_label);
    emit_label(fast_label);

    const auto op_name = (pattern.op == BinaryOp::Add   ? "add"
                         : pattern.op == BinaryOp::Sub ? "sub"
                         : pattern.op == BinaryOp::Mul ? "mul"
                         : pattern.op == BinaryOp::Div ? "div"
                         : pattern.op == BinaryOp::Mod ? "mod"
                                                       : "pow");
    emit_line(pattern.target_name + " = call @__spark_num_repeat_" + op_name + "_" + kind_suffix + "(" +
              pattern.target_name + ", " + rhs_code.value + ", " + iterations + ")");
    emit_line(pattern.index_name + " = " + bound_code.value);
    emit_line("goto " + end_label);
    emit_label(end_label);
    return true;
  };

  if (try_emit_numeric_repeat_fastpath()) {
    return true;
  }

  // Reserve list capacity on canonical append loops:
  // while i < N: list.append(...)
  // This keeps append amortization costs out of hot loops without changing semantics.
  const auto extract_index_and_bound = [](const Expr& condition, std::string& index_name, const Expr*& bound_expr) {
    if (condition.kind != Expr::Kind::Binary) {
      return false;
    }
    const auto& binary = static_cast<const BinaryExpr&>(condition);
    if (binary.op != BinaryOp::Lt) {
      return false;
    }
    if (binary.left->kind != Expr::Kind::Variable) {
      return false;
    }
    if (binary.right->kind != Expr::Kind::Variable && binary.right->kind != Expr::Kind::Number) {
      return false;
    }
    index_name = static_cast<const VariableExpr&>(*binary.left).name;
    bound_expr = binary.right.get();
    return true;
  };

  const auto find_single_append_target = [](const StmtList& body) -> std::optional<std::string> {
    std::string target;
    bool found = false;
    for (const auto& stmt : body) {
      if (!stmt || stmt->kind != Stmt::Kind::Expression) {
        continue;
      }
      const auto& expr_stmt = static_cast<const ExpressionStmt&>(*stmt);
      if (!expr_stmt.expression || expr_stmt.expression->kind != Expr::Kind::Call) {
        continue;
      }
      const auto& call = static_cast<const CallExpr&>(*expr_stmt.expression);
      if (!call.callee || call.callee->kind != Expr::Kind::Attribute) {
        continue;
      }
      const auto& attr = static_cast<const AttributeExpr&>(*call.callee);
      if (attr.attribute != "append" || !attr.target || attr.target->kind != Expr::Kind::Variable) {
        continue;
      }
      const auto& list_name = static_cast<const VariableExpr&>(*attr.target).name;
      if (!found) {
        target = list_name;
        found = true;
        continue;
      }
      if (target != list_name) {
        return std::nullopt;
      }
    }
    if (!found) {
      return std::nullopt;
    }
    return target;
  };

  const auto has_unit_positive_step = [](const StmtList& body, const std::string& index_name) {
    for (const auto& stmt : body) {
      if (!stmt || stmt->kind != Stmt::Kind::Assign) {
        continue;
      }
      const auto& assign = static_cast<const AssignStmt&>(*stmt);
      if (!assign.target || assign.target->kind != Expr::Kind::Variable || !assign.value ||
          assign.value->kind != Expr::Kind::Binary) {
        continue;
      }
      const auto& target = static_cast<const VariableExpr&>(*assign.target);
      if (target.name != index_name) {
        continue;
      }
      const auto& rhs = static_cast<const BinaryExpr&>(*assign.value);
      if (rhs.op != BinaryOp::Add) {
        continue;
      }
      long long constant = 0;
      if (rhs.left->kind == Expr::Kind::Variable &&
          static_cast<const VariableExpr&>(*rhs.left).name == index_name &&
          is_integer_constant_expr(*rhs.right, constant) && constant == 1) {
        return true;
      }
      if (rhs.right->kind == Expr::Kind::Variable &&
          static_cast<const VariableExpr&>(*rhs.right).name == index_name &&
          is_integer_constant_expr(*rhs.left, constant) && constant == 1) {
        return true;
      }
    }
    return false;
  };

  std::optional<std::string> unchecked_append_target;
  std::string loop_index_name;
  const Expr* loop_bound_expr = nullptr;
  if (extract_index_and_bound(*while_stmt.condition, loop_index_name, loop_bound_expr)) {
    const auto append_target = find_single_append_target(while_stmt.body);
    if (append_target.has_value() && has_unit_positive_step(while_stmt.body, loop_index_name)) {
      const auto list_kind = lookup_var_type(ctx, *append_target);
      if (is_list_kind(list_kind) && loop_bound_expr) {
        const auto bound_code = emit_expr(*loop_bound_expr, ctx, ExpectedExprContext::None);
        if (bound_code.has_value &&
            (bound_code.kind == ValueKind::Int || bound_code.kind == ValueKind::Bool || bound_code.kind == ValueKind::Unknown)) {
          emit_line("call @" + container_reserve_fn(list_kind) + "(" + *append_target + ", " + bound_code.value + ")");
          unchecked_append_target = *append_target;
        }
      }
    }
  }

  const std::string cond_label = next_label();
  const std::string body_label = next_label();
  const std::string end_label = next_label();

  emit_line("goto " + cond_label);
  emit_label(cond_label);

  auto cond_code = emit_expr(*while_stmt.condition, ctx, ExpectedExprContext::None);
  if (!cond_code.has_value) {
    return false;
  }
  if (ensure_bool_for_condition(cond_code, ctx) == ScalarKind::Invalid) {
    return false;
  }

  emit_line("br_if " + cond_code.value + ", " + body_label + ", " + end_label);

  emit_label(body_label);
  const auto saved_unchecked_targets = ctx.unchecked_append_targets;
  if (unchecked_append_target.has_value()) {
    ctx.unchecked_append_targets.insert(*unchecked_append_target);
  }
  const auto before = ctx;
  const bool loop_ok = compile_block(to_stmt_refs(while_stmt.body), ctx);
  ctx.unchecked_append_targets = saved_unchecked_targets;
  if (!loop_ok) {
    return false;
  }
  if (!ctx.has_terminated) {
    emit_line("goto " + cond_label);
  }

  if (before.has_terminated) {
    ctx.has_terminated = true;
  }
  emit_label(end_label);
  return true;
}

bool CodeGenerator::emit_range_loop_setup(const Expr& iterable, FunctionContext& ctx, Code& start_code,
                                         Code& stop_code, long long& step_value) {
  const auto* call = dynamic_cast<const CallExpr*>(&iterable);
  if (!call || call->callee->kind != Expr::Kind::Variable) {
    add_error("for loop requires range(...) call");
    return false;
  }

  const auto& callee = static_cast<const VariableExpr&>(*call->callee);
  if (callee.name != "range") {
    add_error("for loop only supports range() iterable");
    return false;
  }
  if (call->args.empty() || call->args.size() > 3) {
    add_error("range() expects 1 to 3 arguments");
    return false;
  }

  if (call->args.size() == 1) {
    start_code = Code{"0", ScalarKind::Int, true};
    stop_code = emit_expr(*call->args[0], ctx, ExpectedExprContext::Int);
    step_value = 1;
  } else if (call->args.size() == 2) {
    start_code = emit_expr(*call->args[0], ctx, ExpectedExprContext::Int);
    stop_code = emit_expr(*call->args[1], ctx, ExpectedExprContext::Int);
    step_value = 1;
  } else {
    start_code = emit_expr(*call->args[0], ctx, ExpectedExprContext::Int);
    stop_code = emit_expr(*call->args[1], ctx, ExpectedExprContext::Int);
    auto step_code = emit_expr(*call->args[2], ctx, ExpectedExprContext::Int);
    if (!step_code.has_value) {
      return false;
    }
    if (!is_integer_constant_expr(*call->args[2], step_value)) {
      add_error("range step must be compile-time integer constant in phase4");
      return false;
    }
  }

  if (start_code.kind == ScalarKind::Invalid || stop_code.kind == ScalarKind::Invalid ||
      !start_code.has_value || !stop_code.has_value) {
    return false;
  }

  if (start_code.kind == ScalarKind::Unknown) {
    start_code.kind = ScalarKind::Int;
  }
  if (stop_code.kind == ScalarKind::Unknown) {
    stop_code.kind = ScalarKind::Int;
  }
  if (start_code.kind != ScalarKind::Int || stop_code.kind != ScalarKind::Int) {
    add_error("range bounds must be integer expressions in phase4");
    return false;
  }
  if (step_value == 0) {
    add_error("range step cannot be zero");
    return false;
  }

  return true;
}

bool CodeGenerator::emit_for_statement(const ForStmt& for_stmt, FunctionContext& ctx) {
  if (for_stmt.is_async) {
    add_error("async for is unsupported in phase4 codegen");
    return false;
  }
  if (!for_stmt.iterable) {
    add_error("for loop requires an iterable expression");
    return false;
  }

  if (const auto* call = dynamic_cast<const CallExpr*>(for_stmt.iterable.get())) {
    if (call->callee && call->callee->kind == Expr::Kind::Variable) {
      const auto& callee = static_cast<const VariableExpr&>(*call->callee);
      if (callee.name == "range") {
        Code start_code;
        Code stop_code;
        long long step = 1;
        if (!emit_range_loop_setup(*for_stmt.iterable, ctx, start_code, stop_code, step)) {
          return false;
        }

        emit_var_decl_if_needed(ctx, for_stmt.name, ScalarKind::Int);
        set_var_type(ctx, for_stmt.name, ScalarKind::Int);

        const bool step_positive = step > 0;

        emit_line(for_stmt.name + " = " + start_code.value);
        const std::string cond_label = next_label();
        const std::string body_label = next_label();
        const std::string end_label = next_label();

        emit_line("goto " + cond_label);

        emit_label(cond_label);
        if (step_positive) {
          const auto cond = next_temp();
          emit_line(cond + " = cmp.lt.i64 " + for_stmt.name + ", " + stop_code.value);
          emit_line("br_if " + cond + ", " + body_label + ", " + end_label);
        } else {
          const auto cond = next_temp();
          emit_line(cond + " = cmp.gt.i64 " + for_stmt.name + ", " + stop_code.value);
          emit_line("br_if " + cond + ", " + body_label + ", " + end_label);
        }

        emit_label(body_label);
        if (!compile_block(to_stmt_refs(for_stmt.body), ctx)) {
          return false;
        }
        if (!ctx.has_terminated) {
          emit_line(for_stmt.name + " = add.i64 " + for_stmt.name + ", " + std::to_string(step));
          emit_line("goto " + cond_label);
        }

        emit_label(end_label);
        return true;
      }
    }
  }

  auto iterable = emit_expr(*for_stmt.iterable, ctx, ExpectedExprContext::None);
  if (!iterable.has_value) {
    return false;
  }

  if (iterable.kind == ScalarKind::Unknown) {
    iterable.kind = ScalarKind::ListInt;
  }
  if (iterable.kind == ScalarKind::ListInt || iterable.kind == ScalarKind::ListFloat) {
    return emit_for_list_statement(for_stmt, iterable, ctx);
  }
  if (iterable.kind == ScalarKind::MatrixInt || iterable.kind == ScalarKind::MatrixFloat) {
    return emit_for_matrix_statement(for_stmt, iterable, ctx);
  }

  Code start_code;
  Code stop_code;
  long long step = 1;

  if (!emit_range_loop_setup(*for_stmt.iterable, ctx, start_code, stop_code, step)) {
    return false;
  }

  emit_var_decl_if_needed(ctx, for_stmt.name, ScalarKind::Int);
  set_var_type(ctx, for_stmt.name, ScalarKind::Int);

  const bool step_positive = step > 0;

  emit_line(for_stmt.name + " = " + start_code.value);
  const std::string cond_label = next_label();
  const std::string body_label = next_label();
  const std::string end_label = next_label();

  emit_line("goto " + cond_label);

  emit_label(cond_label);
  if (step_positive) {
    const auto cond = next_temp();
    emit_line(cond + " = cmp.lt.i64 " + for_stmt.name + ", " + stop_code.value);
    emit_line("br_if " + cond + ", " + body_label + ", " + end_label);
  } else {
    const auto cond = next_temp();
    emit_line(cond + " = cmp.gt.i64 " + for_stmt.name + ", " + stop_code.value);
    emit_line("br_if " + cond + ", " + body_label + ", " + end_label);
  }

  emit_label(body_label);
  if (!compile_block(to_stmt_refs(for_stmt.body), ctx)) {
    return false;
  }
  if (!ctx.has_terminated) {
    emit_line(for_stmt.name + " = add.i64 " + for_stmt.name + ", " + std::to_string(step));
    emit_line("goto " + cond_label);
  }

  emit_label(end_label);
  return true;
}

bool CodeGenerator::emit_for_list_statement(const ForStmt& for_stmt, const Code& iterable, FunctionContext& ctx) {
  if (iterable.kind != ScalarKind::ListInt && iterable.kind != ScalarKind::ListFloat) {
    add_error("for loop list iterable must be a list");
    return false;
  }

  const auto element_kind = (iterable.kind == ScalarKind::ListFloat) ? ScalarKind::Float : ScalarKind::Int;
  emit_var_decl_if_needed(ctx, for_stmt.name, element_kind);
  set_var_type(ctx, for_stmt.name, element_kind);

  const auto container = next_temp();
  const auto len_temp = next_temp();
  const auto idx_temp = next_temp();
  const auto row_fn = container_len_rows_fn(iterable.kind == ScalarKind::ListFloat ? ScalarKind::Float : ScalarKind::Int);

  emit_line("var " + container + ": list_" + (iterable.kind == ScalarKind::ListFloat ? "f64" : "i64") + ";");
  set_var_type(ctx, container, iterable.kind);
  emit_line(container + " = " + iterable.value);
  emit_line(len_temp + " = call @" + row_fn + "(" + container + ")");
  emit_line(idx_temp + " = 0");

  const auto cond_label = next_label();
  const auto body_label = next_label();
  const auto end_label = next_label();
  const auto elem_fn = (iterable.kind == ScalarKind::ListFloat) ? "__spark_list_get_f64" : "__spark_list_get_i64";

  emit_line("goto " + cond_label);

  emit_label(cond_label);
  const auto cond = next_temp();
  emit_line(cond + " = cmp.lt.i64 " + idx_temp + ", " + len_temp);
  emit_line("br_if " + cond + ", " + body_label + ", " + end_label);

  emit_label(body_label);
  emit_line(for_stmt.name + " = call @" + elem_fn + "(" + container + ", " + idx_temp + ")");

  if (!compile_block(to_stmt_refs(for_stmt.body), ctx)) {
    return false;
  }
  if (!ctx.has_terminated) {
    emit_line(idx_temp + " = add.i64 " + idx_temp + ", 1");
    emit_line("goto " + cond_label);
  }

  emit_label(end_label);
  return true;
}

ValueKind CodeGenerator::infer_list_expression_kind(const ListExpr& list) const {
  if (list.elements.empty()) {
    return ValueKind::ListInt;
  }

  bool all_rows = true;
  std::vector<ValueKind> row_element_kinds;
  row_element_kinds.reserve(list.elements.size());
  std::vector<std::size_t> row_lengths;
  row_lengths.reserve(list.elements.size());

  for (const auto& element : list.elements) {
    if (!element || element->kind != Expr::Kind::List) {
      all_rows = false;
      break;
    }

    const auto& row = static_cast<const ListExpr&>(*element);
    const auto kinds = infer_matrix_row_kinds(row);
    row_element_kinds.push_back(infer_matrix_row_element_kind({}, kinds));
    row_lengths.push_back(kinds.size());
  }

  if (all_rows && !list.elements.empty()) {
    auto element_kind = row_element_kinds.empty() ? ValueKind::Unknown : row_element_kinds.front();
    for (std::size_t i = 1; i < row_element_kinds.size(); ++i) {
      element_kind = merge_types(element_kind, row_element_kinds[i]);
      if (element_kind == ValueKind::Invalid) {
        return ValueKind::MatrixAny;
      }
    }
    if (element_kind == ValueKind::Bool) {
      element_kind = ValueKind::Int;
    }

    return is_float_kind(element_kind) ? ValueKind::MatrixFloat : ValueKind::MatrixInt;
  }

  ValueKind element_kind = ValueKind::Unknown;
  for (const auto& element : list.elements) {
    const auto current_kind = infer_ast_expr_kind(*element);
    if (element_kind == ValueKind::Unknown) {
      element_kind = current_kind;
      continue;
    }
    element_kind = merge_types(element_kind, current_kind);
    if (element_kind == ValueKind::Invalid) {
      return ValueKind::ListAny;
    }
  }

  if (element_kind == ValueKind::Bool || element_kind == ValueKind::Float || element_kind == ValueKind::Int) {
    return is_float_kind(element_kind) ? ValueKind::ListFloat : ValueKind::ListInt;
  }
  if (element_kind == ValueKind::MatrixAny || element_kind == ValueKind::MatrixFloat || element_kind == ValueKind::MatrixInt) {
    return ValueKind::ListAny;
  }
  return ValueKind::ListInt;
}

ValueKind CodeGenerator::infer_matrix_row_element_kind(const std::vector<std::string>& row_values,
                                                     const std::vector<ValueKind>& kinds) const {
  (void)row_values;
  if (kinds.empty()) {
    return ValueKind::Unknown;
  }

  ValueKind element_kind = kinds.front();
  if (element_kind == ValueKind::Bool) {
    element_kind = ValueKind::Int;
  }

  for (std::size_t i = 1; i < kinds.size(); ++i) {
    auto next = kinds[i];
    if (next == ValueKind::Bool) {
      next = ValueKind::Int;
    }
    element_kind = merge_types(element_kind, next);
    if (element_kind == ValueKind::Invalid) {
      return ValueKind::MatrixAny;
    }
  }

  if (element_kind == ValueKind::Invalid || element_kind == ValueKind::Unknown) {
    return ValueKind::Unknown;
  }
  return element_kind;
}
