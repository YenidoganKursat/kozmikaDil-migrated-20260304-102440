bool CodeGenerator::dispatch_stmt(const Stmt& stmt, FunctionContext& ctx) {
  constexpr std::array<StmtCodegenHandler, static_cast<std::size_t>(Stmt::Kind::WithTaskGroup) + 1> handlers{
      &CodeGenerator::compile_stmt_expression,
      &CodeGenerator::compile_stmt_assign,
      &CodeGenerator::compile_stmt_return,
      &CodeGenerator::compile_stmt_break,
      &CodeGenerator::compile_stmt_continue,
      &CodeGenerator::compile_stmt_if,
      &CodeGenerator::compile_stmt_switch,
      &CodeGenerator::compile_stmt_try_catch,
      &CodeGenerator::compile_stmt_while,
      &CodeGenerator::compile_stmt_for,
      &CodeGenerator::compile_stmt_function_def,
      &CodeGenerator::compile_stmt_class_def,
      &CodeGenerator::compile_stmt_with_task_group,
  };

  const auto index = static_cast<std::size_t>(stmt.kind);
  if (index >= handlers.size() || !handlers[index]) {
    add_error("unsupported statement");
    return false;
  }
  return (this->*handlers[index])(stmt, ctx);
}

bool CodeGenerator::compile_stmt_expression(const Stmt& stmt, FunctionContext& ctx) {
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

bool CodeGenerator::compile_stmt_assign(const Stmt& stmt, FunctionContext& ctx) {
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

bool CodeGenerator::compile_stmt_return(const Stmt& stmt, FunctionContext& ctx) {
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

bool CodeGenerator::compile_stmt_break(const Stmt&, FunctionContext&) {
  add_error("break is unsupported in scalar_runtime codegen");
  return false;
}

bool CodeGenerator::compile_stmt_continue(const Stmt&, FunctionContext&) {
  add_error("continue is unsupported in scalar_runtime codegen");
  return false;
}

bool CodeGenerator::compile_stmt_if(const Stmt& stmt, FunctionContext& ctx) {
  return emit_if_statement(static_cast<const IfStmt&>(stmt), ctx);
}

bool CodeGenerator::compile_stmt_switch(const Stmt&, FunctionContext&) {
  add_error("switch/case is unsupported in scalar_runtime codegen");
  return false;
}

bool CodeGenerator::compile_stmt_try_catch(const Stmt&, FunctionContext&) {
  add_error("try/catch is unsupported in scalar_runtime codegen");
  return false;
}

bool CodeGenerator::compile_stmt_while(const Stmt& stmt, FunctionContext& ctx) {
  return emit_while_statement(static_cast<const WhileStmt&>(stmt), ctx);
}

bool CodeGenerator::compile_stmt_for(const Stmt& stmt, FunctionContext& ctx) {
  return emit_for_statement(static_cast<const ForStmt&>(stmt), ctx);
}

bool CodeGenerator::compile_stmt_function_def(const Stmt& stmt, FunctionContext&) {
  const auto& fn = static_cast<const FunctionDefStmt&>(stmt);
  add_error("nested function definition unsupported in scalar_runtime: " + fn.name);
  return false;
}

bool CodeGenerator::compile_stmt_class_def(const Stmt&, FunctionContext&) {
  add_error("class definitions unsupported in scalar_runtime");
  return false;
}

bool CodeGenerator::compile_stmt_with_task_group(const Stmt&, FunctionContext&) {
  add_error("with task_group is unsupported in scalar_runtime codegen");
  return false;
}
