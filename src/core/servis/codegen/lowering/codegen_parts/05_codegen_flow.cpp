::std::string escape_string_for_ir_literal(const ::std::string& input) {
  ::std::string out;
  out.reserve(input.size() + 8);
  for (const auto ch : input) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

const CallExpr* as_call_expr_codegen_flow(const Expr* expr) {
  if (!expr || expr->kind != Expr::Kind::Call) {
    return nullptr;
  }
  return static_cast<const CallExpr*>(expr);
}

bool parse_string_ctor_arg_codegen_flow(const Expr* expr, const Expr*& out_arg) {
  const auto* call = as_call_expr_codegen_flow(expr);
  if (!call || !call->callee || call->callee->kind != Expr::Kind::Variable ||
      call->args.size() != 1 || !call->args[0]) {
    return false;
  }
  const auto& callee = static_cast<const VariableExpr&>(*call->callee);
  if (callee.name != "string") {
    return false;
  }
  out_arg = call->args[0].get();
  return true;
}

bool parse_ascii1_string_ctor_add_pattern_codegen_flow(const BinaryExpr& binary,
                                                       long long& out_ascii_code,
                                                       const Expr*& out_numeric_arg,
                                                       bool& out_literal_on_left) {
  if (binary.op != BinaryOp::Add || !binary.left || !binary.right) {
    return false;
  }
  if (binary.left->kind == Expr::Kind::String) {
    const auto& literal = static_cast<const StringExpr&>(*binary.left);
    const Expr* arg = nullptr;
    if (literal.value.size() == 1 &&
        static_cast<unsigned char>(literal.value[0]) < 0x80u &&
        parse_string_ctor_arg_codegen_flow(binary.right.get(), arg)) {
      out_ascii_code = static_cast<long long>(
          static_cast<unsigned char>(literal.value[0]));
      out_numeric_arg = arg;
      out_literal_on_left = true;
      return true;
    }
  }
  if (binary.right->kind == Expr::Kind::String) {
    const auto& literal = static_cast<const StringExpr&>(*binary.right);
    const Expr* arg = nullptr;
    if (literal.value.size() == 1 &&
        static_cast<unsigned char>(literal.value[0]) < 0x80u &&
        parse_string_ctor_arg_codegen_flow(binary.left.get(), arg)) {
      out_ascii_code = static_cast<long long>(
          static_cast<unsigned char>(literal.value[0]));
      out_numeric_arg = arg;
      out_literal_on_left = false;
      return true;
    }
  }
  return false;
}

bool try_pow2_mod_mask_from_expr_codegen_flow(const Expr* expr,
                                              unsigned long long& out_mask) {
  out_mask = 0ULL;
  if (!expr || expr->kind != Expr::Kind::Number) {
    return false;
  }
  const auto& number = static_cast<const NumberExpr&>(*expr);
  if (!number.is_int || number.raw_text.empty()) {
    return false;
  }

  std::string token = number.raw_text;
  if (token.empty()) {
    return false;
  }
  if (token.front() == '+') {
    token.erase(token.begin());
  }
  if (token.empty() || token.front() == '-') {
    return false;
  }

  ::std::size_t consumed = 0;
  unsigned long long value = 0ULL;
  try {
    value = static_cast<unsigned long long>(::std::stoull(token, &consumed, 10));
  } catch (...) {
    return false;
  }
  if (consumed != token.size()) {
    return false;
  }
  if (value == 0ULL || (value & (value - 1ULL)) != 0ULL) {
    return false;
  }
  out_mask = value - 1ULL;
  return true;
}

Code CodeGenerator::emit_list_expression(const ListExpr& list, FunctionContext& ctx, ExpectedExprContext expected) {
  (void)ctx;
  (void)expected;
  const auto kind = infer_list_expression_kind(list);
  if (kind == ValueKind::ListFloat) {
    const auto target = next_temp();
    emit_line(target + " = call @" + container_initializer_fn(ValueKind::ListFloat) + "(0)");
    for (const auto& element : list.elements) {
      const auto value = emit_expr(*element, ctx, ExpectedExprContext::Float);
      if (!value.has_value) {
        return {"", ScalarKind::Invalid, false};
      }
      if (value.kind != ValueKind::Float) {
        const auto casted = next_temp();
        emit_line(casted + " = cast.i64_to_f64 " + value.value);
        emit_line("call @" + container_append_fn(ValueKind::ListFloat) + "(" + target + ", " + casted + ")");
      } else {
        emit_line("call @" + container_append_fn(ValueKind::ListFloat) + "(" + target + ", " + value.value + ")");
      }
    }
    return {target, ValueKind::ListFloat, true};
  }

  if (kind == ValueKind::ListInt || kind == ValueKind::ListAny) {
    const auto target = next_temp();
    emit_line(target + " = call @" + container_initializer_fn(ValueKind::ListInt) + "(0)");
    for (const auto& element : list.elements) {
      const auto value = emit_expr(*element, ctx, ExpectedExprContext::Int);
      if (!value.has_value) {
        return {"", ScalarKind::Invalid, false};
      }
      emit_line("call @" + container_append_fn(ValueKind::ListInt) + "(" + target + ", " + value.value + ")");
    }
    return {target, ValueKind::ListInt, true};
  }

  if (kind == ValueKind::MatrixFloat || kind == ValueKind::MatrixInt || kind == ValueKind::MatrixAny) {
    auto has_rows = !list.elements.empty();
    if (!has_rows) {
      const auto target = next_temp();
      emit_line(target + " = call @" + matrix_new_fn(ValueKind::MatrixInt) + "(0, 0)");
      return {target, ValueKind::MatrixInt, true};
    }

    const auto matrix_kind = is_float_kind(kind) ? ValueKind::MatrixFloat : ValueKind::MatrixInt;
    const ::std::size_t rows = list.elements.size();
    const auto* first_row = static_cast<const ListExpr*>(list.elements[0].get());
    const ::std::size_t cols = first_row ? first_row->elements.size() : 0;
    const auto container = next_temp();
    emit_line(container + " = call @" + matrix_new_fn(matrix_kind) + "(" + ::std::to_string(rows) + ", " +
              ::std::to_string(cols) + ")");

    for (::std::size_t row = 0; row < list.elements.size(); ++row) {
      const auto* row_expr = static_cast<const ListExpr*>(list.elements[row].get());
      if (!row_expr) {
        add_error("matrix literal row must be a list");
        return {"", ValueKind::Invalid, false};
      }
      const auto expected_row_expected = is_float_kind(matrix_kind) ? ExpectedExprContext::Float : ExpectedExprContext::Int;
      for (::std::size_t col = 0; col < row_expr->elements.size(); ++col) {
        const auto value = emit_expr(*row_expr->elements[col], ctx, expected_row_expected);
        if (!value.has_value) {
          return {"", ScalarKind::Invalid, false};
        }
        auto value_code = value.value;
        auto value_kind = value.kind;
        if (matrix_kind == ValueKind::MatrixFloat && value_kind != ValueKind::Float) {
          const auto casted = next_temp();
          if (value_kind == ValueKind::Int) {
            emit_line(casted + " = cast.i64_to_f64 " + value_code);
            value_code = casted;
          }
        } else if (matrix_kind == ValueKind::MatrixInt && value_kind == ValueKind::Float) {
          const auto casted = next_temp();
          emit_line(casted + " = cast.f64_to_i64 " + value_code);
          value_code = casted;
        }
        emit_line("call @" + container_index_set_fn(matrix_kind) + "(" + container + ", " + ::std::to_string(row) + ", " +
                  ::std::to_string(col) + ", " + value_code + ")");
      }
    }
    return {container, matrix_kind, true};
  }

  const auto target = next_temp();
  emit_line(target + " = call @" + container_initializer_fn(ValueKind::ListInt) + "(0)");
  for (const auto& element : list.elements) {
    const auto value = emit_expr(*element, ctx, ExpectedExprContext::Int);
    if (!value.has_value) {
      return {"", ScalarKind::Invalid, false};
    }
    emit_line("call @" + container_append_fn(ValueKind::ListInt) + "(" + target + ", " + value.value + ")");
  }
  return {target, ValueKind::ListInt, true};
}

Code CodeGenerator::emit_index_expression(const IndexExpr& index, FunctionContext& ctx, ExpectedExprContext expected) {
  (void)expected;
  const auto flattened = flatten_index_chain(index);
  if (!flattened.base) {
    add_error("invalid index expression");
    return {"", ScalarKind::Invalid, false};
  }

  auto current = emit_expr(*flattened.base, ctx, ExpectedExprContext::None);
  if (!current.has_value) {
    return {"", ScalarKind::Invalid, false};
  }

  if (flattened.indices.empty()) {
    return current;
  }

  auto current_code = current;
  auto current_kind = current.kind;
  if (!is_container_kind(current_kind) && current_kind != ScalarKind::Unknown && current_kind != ValueKind::String) {
    add_error("indexing target is not indexable");
    return {"", ScalarKind::Invalid, false};
  }

  const auto* base_variable = root_variable_expr(*flattened.base);
  const auto base_name = base_variable ? base_variable->name : ::std::string{};
  ValueKind current_element_kind =
      base_name.empty() ? infer_container_scalar_type(current_kind) : lookup_container_element_type(ctx, base_name);

  const auto cast_ptr_from_i64 = [](ValueKind container_kind, const ::std::string& value) {
    switch (container_kind) {
      case ValueKind::ListInt:
      case ValueKind::ListAny:
        return "cast.i64_to_list_i64 " + value;
      case ValueKind::ListFloat:
        return "cast.i64_to_list_f64 " + value;
      case ValueKind::MatrixInt:
      case ValueKind::MatrixAny:
        return "cast.i64_to_matrix_i64 " + value;
      case ValueKind::MatrixFloat:
        return "cast.i64_to_matrix_f64 " + value;
      default:
        return "cast.i64_to_i64 " + value;
    }
  };
  auto scalar_from_container = [](ValueKind kind) {
    return is_float_kind(kind) ? ValueKind::Float : ValueKind::Int;
  };

  for (::std::size_t i = 0; i < flattened.indices.size(); ++i) {
    const auto& item = *flattened.indices[i];
    if (is_matrix_kind(current_kind)) {
      if (item.is_slice) {
        const auto row_start =
            item.slice_start ? emit_expr(*item.slice_start, ctx, ExpectedExprContext::Int) : Code{"0", ValueKind::Int, true};
        Code row_stop;
        if (item.slice_stop) {
          row_stop = emit_expr(*item.slice_stop, ctx, ExpectedExprContext::Int);
        } else {
          const auto row_len = next_temp();
          emit_line(row_len + " = call @" + matrix_len_rows_fn(current_kind) + "(" + current_code.value + ")");
          row_stop = {row_len, ValueKind::Int, true};
        }
        const auto row_step =
            item.slice_step ? emit_expr(*item.slice_step, ctx, ExpectedExprContext::Int) : Code{"1", ValueKind::Int, true};
        if (!row_start.has_value || !row_stop.has_value || !row_step.has_value) {
          return {"", ScalarKind::Invalid, false};
        }

        if (i + 1 < flattened.indices.size()) {
          const auto* next_item = flattened.indices[i + 1];
          if (!next_item) {
            add_error("invalid matrix slice continuation");
            return {"", ScalarKind::Invalid, false};
          }

          if (next_item->is_slice) {
            const auto col_start = next_item->slice_start ? emit_expr(*next_item->slice_start, ctx, ExpectedExprContext::Int)
                                                          : Code{"0", ValueKind::Int, true};
            Code col_stop;
            if (next_item->slice_stop) {
              col_stop = emit_expr(*next_item->slice_stop, ctx, ExpectedExprContext::Int);
            } else {
              const auto col_len = next_temp();
              emit_line(col_len + " = call @" + matrix_len_cols_fn(current_kind) + "(" + current_code.value + ")");
              col_stop = {col_len, ValueKind::Int, true};
            }
            const auto col_step = next_item->slice_step ? emit_expr(*next_item->slice_step, ctx, ExpectedExprContext::Int)
                                                        : Code{"1", ValueKind::Int, true};
            if (!col_start.has_value || !col_stop.has_value || !col_step.has_value) {
              return {"", ScalarKind::Invalid, false};
            }

            const auto out = next_temp();
            emit_line(out + " = call @" + matrix_slice_block_fn_for(current_kind) + "(" + current_code.value + ", " +
                      row_start.value + ", " + row_stop.value + ", " + row_step.value + ", " + col_start.value + ", " +
                      col_stop.value + ", " + col_step.value + ")");
            current_code = {out, current_kind, true};
            current_element_kind = infer_container_scalar_type(current_kind);
            ++i;
            continue;
          }

          if (!next_item->index) {
            add_error("invalid matrix column index");
            return {"", ScalarKind::Invalid, false};
          }
          const auto col_index = emit_expr(*next_item->index, ctx, ExpectedExprContext::Int);
          if (!col_index.has_value) {
            return {"", ScalarKind::Invalid, false};
          }
          const auto out = next_temp();
          emit_line(out + " = call @" + matrix_rows_col_fn_for(current_kind) + "(" + current_code.value + ", " +
                    row_start.value + ", " + row_stop.value + ", " + row_step.value + ", " + col_index.value + ")");
          current_code = {out, is_float_kind(current_kind) ? ValueKind::ListFloat : ValueKind::ListInt, true};
          current_kind = current_code.kind;
          current_element_kind = infer_container_scalar_type(current_kind);
          ++i;
          continue;
        }

        const auto out = next_temp();
        emit_line(out + " = call @" + matrix_slice_fn_for(current_kind) + "(" + current_code.value + ", " + row_start.value +
                  ", " + row_stop.value + ", " + row_step.value + ")");
        current_code = {out, current_kind, true};
        current_element_kind = infer_container_scalar_type(current_kind);
        continue;
      }

      if (!item.index) {
        add_error("invalid index item");
        return {"", ScalarKind::Invalid, false};
      }
      const auto row_index = emit_expr(*item.index, ctx, ExpectedExprContext::Int);
      if (!row_index.has_value) {
        return {"", ScalarKind::Invalid, false};
      }

      if (i + 1 < flattened.indices.size()) {
        const auto* next_item = flattened.indices[i + 1];
        if (!next_item) {
          add_error("invalid matrix index item");
          return {"", ScalarKind::Invalid, false};
        }
        if (!next_item->is_slice && next_item->index) {
          const auto col_index = emit_expr(*next_item->index, ctx, ExpectedExprContext::Int);
          if (!col_index.has_value) {
            return {"", ScalarKind::Invalid, false};
          }
          const auto out = next_temp();
          const auto getter = is_float_kind(current_kind) ? "__spark_matrix_get_f64" : "__spark_matrix_get_i64";
          emit_line(out + " = call @" + getter + "(" + current_code.value + ", " + row_index.value + ", " + col_index.value + ")");
          current_code = {out, is_float_kind(current_kind) ? ValueKind::Float : ValueKind::Int, true};
          current_kind = current_code.kind;
          current_element_kind = ValueKind::Unknown;
          ++i;
          continue;
        }
      }

      const auto out = next_temp();
      const auto row_getter = is_float_kind(current_kind) ? "__spark_matrix_row_f64" : "__spark_matrix_row_i64";
      emit_line(out + " = call @" + row_getter + "(" + current_code.value + ", " + row_index.value + ")");
      current_code = {out, is_float_kind(current_kind) ? ValueKind::ListFloat : ValueKind::ListInt, true};
      current_kind = current_code.kind;
      current_element_kind = infer_container_scalar_type(current_kind);
      continue;
    }

    if (current_kind == ValueKind::String) {
      if (item.is_slice) {
        const auto start =
            item.slice_start ? emit_expr(*item.slice_start, ctx, ExpectedExprContext::Int) : Code{"0", ValueKind::Int, true};
        Code stop;
        if (item.slice_stop) {
          stop = emit_expr(*item.slice_stop, ctx, ExpectedExprContext::Int);
        } else {
          const auto string_len = next_temp();
          emit_line(string_len + " = call @__spark_string_len(" + current_code.value + ")");
          stop = {string_len, ValueKind::Int, true};
        }
        const auto step =
            item.slice_step ? emit_expr(*item.slice_step, ctx, ExpectedExprContext::Int) : Code{"1", ValueKind::Int, true};
        if (!start.has_value || !stop.has_value || !step.has_value) {
          return {"", ScalarKind::Invalid, false};
        }

        const auto out = next_temp();
        emit_line(out + " = call @__spark_string_slice(" + current_code.value + ", " + start.value + ", " + stop.value + ", " +
                  step.value + ")");
        current_code = {out, ValueKind::String, true};
        current_kind = current_code.kind;
        current_element_kind = ValueKind::Unknown;
        continue;
      }

      if (!item.index) {
        add_error("invalid string index item");
        return {"", ScalarKind::Invalid, false};
      }
      const auto index_code = emit_expr(*item.index, ctx, ExpectedExprContext::Int);
      if (!index_code.has_value) {
        return {"", ScalarKind::Invalid, false};
      }
      const auto out = next_temp();
      emit_line(out + " = call @__spark_string_index(" + current_code.value + ", " + index_code.value + ")");
      current_code = {out, ValueKind::String, true};
      current_kind = current_code.kind;
      current_element_kind = ValueKind::Unknown;
      continue;
    }

    if (item.is_slice) {
      if (!is_list_kind(current_kind)) {
        add_error("slice indexing target is not list");
        return {"", ScalarKind::Invalid, false};
      }
      const auto start =
          item.slice_start ? emit_expr(*item.slice_start, ctx, ExpectedExprContext::Int) : Code{"0", ValueKind::Int, true};
      Code stop;
      if (item.slice_stop) {
        stop = emit_expr(*item.slice_stop, ctx, ExpectedExprContext::Int);
      } else {
        const auto list_len = next_temp();
        emit_line(list_len + " = call @" + container_len_fn(current_kind) + "(" + current_code.value + ")");
        stop = {list_len, ValueKind::Int, true};
      }
      const auto step =
          item.slice_step ? emit_expr(*item.slice_step, ctx, ExpectedExprContext::Int) : Code{"1", ValueKind::Int, true};
      if (!start.has_value || !stop.has_value || !step.has_value) {
        return {"", ScalarKind::Invalid, false};
      }

      const auto out = next_temp();
      emit_line(out + " = call @" + list_slice_fn_for(current_kind) + "(" + current_code.value + ", " + start.value + ", " +
                stop.value + ", " + step.value + ")");
      current_code = {out, is_float_kind(current_kind) ? ValueKind::ListFloat : ValueKind::ListInt, true};
      current_kind = current_code.kind;
      current_element_kind = infer_container_scalar_type(current_kind);
      continue;
    }

    if (!item.index) {
      add_error("invalid index item");
      return {"", ScalarKind::Invalid, false};
    }
    const auto index_code = emit_expr(*item.index, ctx, ExpectedExprContext::Int);
    if (!index_code.has_value) {
      return {"", ScalarKind::Invalid, false};
    }

    if (is_list_kind(current_kind)) {
      const auto out = next_temp();
      const auto getter = is_float_kind(current_kind) ? "__spark_list_get_f64" : "__spark_list_get_i64";
      emit_line(out + " = call @" + getter + "(" + current_code.value + ", " + index_code.value + ")");

      if (is_container_kind(current_element_kind)) {
        if (is_float_kind(current_kind)) {
          add_error("cannot read nested container from float list");
          return {"", ScalarKind::Invalid, false};
        }
        const auto casted = next_temp();
        emit_line(casted + " = " + cast_ptr_from_i64(current_element_kind, out) + "");
        current_code = {casted, current_element_kind, true};
        current_kind = current_element_kind;
        current_element_kind = infer_container_scalar_type(current_kind);
        continue;
      }

      auto effective = scalar_from_container(current_kind);
      if (is_float_kind(current_kind) && current_element_kind == ValueKind::Int) {
        const auto casted = next_temp();
        emit_line(casted + " = cast.f64_to_i64 " + out);
        current_code = {casted, ValueKind::Int, true};
        effective = ValueKind::Int;
      } else if (!is_float_kind(current_kind) && current_element_kind == ValueKind::Float) {
        const auto casted = next_temp();
        emit_line(casted + " = cast.i64_to_f64 " + out);
        current_code = {casted, ValueKind::Float, true};
        effective = ValueKind::Float;
      } else {
        current_code = {out, effective, true};
      }
      current_kind = effective;
      current_element_kind = ValueKind::Unknown;
      continue;
    }

    add_error("indexing target is not indexable");
    return {"", ScalarKind::Invalid, false};
  }

  return current_code;
}

bool CodeGenerator::emit_for_matrix_statement(const ForStmt& for_stmt, const Code& iterable, FunctionContext& ctx) {
  if (iterable.kind != ScalarKind::MatrixInt && iterable.kind != ScalarKind::MatrixFloat) {
    add_error("for loop matrix iterable must be a matrix");
    return false;
  }

  const auto row_list_kind = (iterable.kind == ValueKind::MatrixFloat) ? ValueKind::ListFloat : ValueKind::ListInt;
  emit_var_decl_if_needed(ctx, for_stmt.name, row_list_kind);
  set_var_type(ctx, for_stmt.name, row_list_kind);

  const auto container = next_temp();
  emit_line("var " + container + ": " + ::std::string(iterable.kind == ValueKind::MatrixFloat ? "matrix_f64" : "matrix_i64") + ";");
  set_var_type(ctx, container, iterable.kind);
  emit_line(container + " = " + iterable.value);

  const auto rows = next_temp();
  const auto index = next_temp();

  emit_line(rows + " = call @" + matrix_len_rows_fn(iterable.kind) + "(" + container + ")");
  emit_line(index + " = 0");

  const auto cond_label = next_label();
  const auto body_label = next_label();
  const auto end_label = next_label();

  emit_line("goto " + cond_label);

  emit_label(cond_label);
  const auto cond = next_temp();
  emit_line(cond + " = cmp.lt.i64 " + index + ", " + rows);
  emit_line("br_if " + cond + ", " + body_label + ", " + end_label);

  emit_label(body_label);
  const auto row_getter = is_float_kind(iterable.kind) ? "__spark_matrix_row_f64" : "__spark_matrix_row_i64";
  emit_line(for_stmt.name + " = call @" + row_getter + "(" + container + ", " + index + ")");

  if (!compile_block(to_stmt_refs(for_stmt.body), ctx)) {
    return false;
  }
  if (!ctx.has_terminated) {
    emit_line(index + " = add.i64 " + index + ", 1");
    emit_line("goto " + cond_label);
  }

  emit_label(end_label);
  return true;
}

Code CodeGenerator::emit_expr(const Expr& expr, FunctionContext& ctx, ExpectedExprContext expected) {
  if (ctx.has_terminated) {
    return {"", ScalarKind::Invalid, false};
  }

  switch (expr.kind) {
    case Expr::Kind::Number: {
      const auto& number_expr = static_cast<const NumberExpr&>(expr);
      auto kind = number_expr.is_int ? ScalarKind::Int : ScalarKind::Float;
      if (expected == ExpectedExprContext::Float) {
        if (number_expr.is_int) {
          kind = ScalarKind::Float;
        }
      } else if (expected == ExpectedExprContext::Int) {
        kind = ScalarKind::Int;
      }
      const auto tmp = next_temp();
      emit_line(tmp + " = " + scalar_kind_to_name(kind) + ".const " + scalar_to_constant(number_expr.value, kind));
      return {tmp, kind, true};
    }
    case Expr::Kind::String: {
      const auto& string_expr = static_cast<const StringExpr&>(expr);
      const auto tmp = next_temp();
      emit_line(tmp + " = str.const \"" + escape_string_for_ir_literal(string_expr.value) + "\"");
      return {tmp, ScalarKind::String, true};
    }
    case Expr::Kind::Bool: {
      const auto& bool_expr = static_cast<const BoolExpr&>(expr);
      const auto tmp = next_temp();
      emit_line(tmp + " = bool.const " + ::std::string(bool_expr.value ? "true" : "false"));
      return {tmp, ScalarKind::Bool, true};
    }
    case Expr::Kind::Variable: {
      const auto& variable = static_cast<const VariableExpr&>(expr);
      const auto known = lookup_var_type(ctx, variable.name);
      if (known == ScalarKind::Invalid) {
        add_error("undefined variable: " + variable.name + " in scope depth " + ::std::to_string(ctx.scopes.size()));
        return {"", ScalarKind::Invalid, false};
      }
      if (known == ScalarKind::Unknown) {
        const auto inferred = expected == ExpectedExprContext::Float ? ScalarKind::Float
                              : expected == ExpectedExprContext::Bool ? ScalarKind::Bool
                                                                     : ScalarKind::Int;
        set_var_type(ctx, variable.name, inferred);
        return {variable.name, inferred, true, lookup_numeric_hint(ctx, variable.name)};
      }
      return {variable.name, known, true, lookup_numeric_hint(ctx, variable.name)};
    }
    case Expr::Kind::Unary: {
      const auto& unary = static_cast<const UnaryExpr&>(expr);
      if (unary.op == UnaryOp::Await) {
        add_error("await is unsupported in scalar_runtime codegen");
        return {"", ScalarKind::Invalid, false};
      }
      if (unary.op == UnaryOp::Not) {
        auto operand = emit_expr(*unary.operand, ctx, ExpectedExprContext::Bool);
        if (operand.kind == ScalarKind::Invalid) {
          return {"", ScalarKind::Invalid, false};
        }
        if (ensure_bool_for_condition(operand, ctx) == ScalarKind::Invalid) {
          add_error("unary 'not' expects boolean-like operand");
          return {"", ScalarKind::Invalid, false};
        }
        const auto tmp = next_temp();
        emit_line(tmp + " = not " + operand.value);
        return {tmp, ScalarKind::Bool, true};
      }

      auto operand = emit_expr(*unary.operand, ctx,
                              expected == ExpectedExprContext::Float ? ExpectedExprContext::Float : ExpectedExprContext::Int);
      if (operand.kind != ScalarKind::Int && operand.kind != ScalarKind::Float) {
        add_error("unary '-' expects numeric operand");
        return {"", ScalarKind::Invalid, false};
      }
      const auto tmp = next_temp();
      emit_line(tmp + " = neg." + scalar_kind_to_name(operand.kind) + " " + operand.value);
      return {tmp, operand.kind, true, operand.numeric_hint};
    }
    case Expr::Kind::Binary: {
      const auto& binary = static_cast<const BinaryExpr&>(expr);

      if (binary.op == BinaryOp::Add) {
        if (binary.left->kind == Expr::Kind::String && binary.right->kind == Expr::Kind::String) {
          const auto& left_literal = static_cast<const StringExpr&>(*binary.left);
          const auto& right_literal = static_cast<const StringExpr&>(*binary.right);
          return {"\"" + escape_string_for_ir_literal(left_literal.value + right_literal.value) + "\"",
                  ScalarKind::String, true};
        }

        if (binary.left->kind == Expr::Kind::String) {
          const auto& left_literal = static_cast<const StringExpr&>(*binary.left);
          if (left_literal.value.empty()) {
            auto right_passthrough = emit_expr(*binary.right, ctx, ExpectedExprContext::None);
            if (right_passthrough.has_value && right_passthrough.kind == ScalarKind::String) {
              return right_passthrough;
            }
          }
        }
        if (binary.right->kind == Expr::Kind::String) {
          const auto& right_literal = static_cast<const StringExpr&>(*binary.right);
          if (right_literal.value.empty()) {
            auto left_passthrough = emit_expr(*binary.left, ctx, ExpectedExprContext::None);
            if (left_passthrough.has_value && left_passthrough.kind == ScalarKind::String) {
              return left_passthrough;
            }
          }
        }

        long long ascii_code = 0;
        const Expr* numeric_arg = nullptr;
        bool literal_on_left = true;
        if (parse_ascii1_string_ctor_add_pattern_codegen_flow(
                binary, ascii_code, numeric_arg, literal_on_left) &&
            numeric_arg != nullptr) {
          auto arg = emit_expr(*numeric_arg, ctx, ExpectedExprContext::None);
          if (arg.has_value && arg.kind == ScalarKind::Int) {
            const auto out = next_temp();
            const auto fn = literal_on_left
                                ? "__spark_string_from_ascii1_i64_prefix"
                                : "__spark_string_from_ascii1_i64_suffix";
            emit_line(out + " = call @" + ::std::string(fn) + "(" +
                      ::std::to_string(ascii_code) + ", " + arg.value + ")");
            return {out, ScalarKind::String, true};
          }
        }
      }

      if (binary.op == BinaryOp::And || binary.op == BinaryOp::Or) {
        auto left = emit_expr(*binary.left, ctx, ExpectedExprContext::Bool);
        auto right = emit_expr(*binary.right, ctx, ExpectedExprContext::Bool);
        if (left.kind == ScalarKind::Invalid || right.kind == ScalarKind::Invalid) {
          return {"", ScalarKind::Invalid, false};
        }
        if (ensure_bool_for_condition(left, ctx) == ScalarKind::Invalid ||
            ensure_bool_for_condition(right, ctx) == ScalarKind::Invalid) {
          add_error("logical operators require boolean-like operands");
          return {"", ScalarKind::Invalid, false};
        }

        const auto tmp = next_temp();
        emit_line(tmp + " = " + (binary.op == BinaryOp::And ? "and " : "or ") + left.value + ", " + right.value);
        return {tmp, ScalarKind::Bool, true};
      }

      if (binary.op == BinaryOp::Eq || binary.op == BinaryOp::Ne ||
          binary.op == BinaryOp::Lt || binary.op == BinaryOp::Lte || binary.op == BinaryOp::Gt ||
          binary.op == BinaryOp::Gte) {
        auto left = emit_expr(*binary.left, ctx, ExpectedExprContext::None);
        auto right = emit_expr(*binary.right, ctx, ExpectedExprContext::None);
        if (left.kind == ScalarKind::Invalid || right.kind == ScalarKind::Invalid) {
          return {"", ScalarKind::Invalid, false};
        }

        if (left.kind == ScalarKind::String || right.kind == ScalarKind::String) {
          if (left.kind != ScalarKind::String || right.kind != ScalarKind::String) {
            add_error("string comparison requires both operands to be string");
            return {"", ScalarKind::Invalid, false};
          }
          if (left.value == right.value) {
            if (binary.op == BinaryOp::Eq || binary.op == BinaryOp::Lte || binary.op == BinaryOp::Gte) {
              return {"true", ScalarKind::Bool, true};
            }
            if (binary.op == BinaryOp::Ne || binary.op == BinaryOp::Lt || binary.op == BinaryOp::Gt) {
              return {"false", ScalarKind::Bool, true};
            }
          }
          if (binary.op == BinaryOp::Eq || binary.op == BinaryOp::Ne) {
            const auto eq_value = next_temp();
            emit_line(eq_value + " = call @__spark_string_eq(" + left.value + ", " + right.value + ")");
            if (binary.op == BinaryOp::Eq) {
              return {eq_value, ScalarKind::Bool, true};
            }
            const auto out = next_temp();
            emit_line(out + " = not " + eq_value);
            return {out, ScalarKind::Bool, true};
          }
          const auto cmp_value = next_temp();
          emit_line(cmp_value + " = call @__spark_string_cmp(" + left.value + ", " + right.value + ")");
          const auto out = next_temp();
          const auto op =
              (binary.op == BinaryOp::Lt ? "lt" : binary.op == BinaryOp::Lte ? "le" : binary.op == BinaryOp::Gt ? "gt" : "ge");
          emit_line(out + " = cmp." + op + ".i64 " + cmp_value + ", 0");
          return {out, ScalarKind::Bool, true};
        }

        if (left.kind == ScalarKind::Unknown) {
          left.kind = ScalarKind::Int;
        }
        if (right.kind == ScalarKind::Unknown) {
          right.kind = ScalarKind::Int;
        }

        ScalarKind cmp_kind = ScalarKind::Invalid;
        if (left.kind == ScalarKind::Bool || right.kind == ScalarKind::Bool) {
          if (left.kind != right.kind) {
            add_error("equality/comparison type mismatch for boolean operands");
            return {"", ScalarKind::Invalid, false};
          }
          cmp_kind = left.kind;
        } else {
          if (binary.op == BinaryOp::Lt || binary.op == BinaryOp::Lte || binary.op == BinaryOp::Gt || binary.op == BinaryOp::Gte) {
            cmp_kind = coerce_numeric(left.kind, right.kind);
            if (cmp_kind == ScalarKind::Invalid) {
              add_error("comparison requires numeric operands");
              return {"", ScalarKind::Invalid, false};
            }
          } else {
            cmp_kind = left.kind == ScalarKind::Float || right.kind == ScalarKind::Float ? ScalarKind::Float : ScalarKind::Int;
          }
        }

        if (left.kind != cmp_kind && cmp_kind != ScalarKind::Bool) {
          if (left.kind == ScalarKind::Int) {
            const auto cast = next_temp();
            emit_line(cast + " = cast.i64_to_f64 " + left.value);
            left.value = cast;
            left.kind = cmp_kind;
          }
        }
        if (right.kind != cmp_kind && cmp_kind != ScalarKind::Bool) {
          if (right.kind == ScalarKind::Int) {
            const auto cast = next_temp();
            emit_line(cast + " = cast.i64_to_f64 " + right.value);
            right.value = cast;
            right.kind = cmp_kind;
          }
        }

        const ::std::string op =
            (binary.op == BinaryOp::Lt ? "lt" : binary.op == BinaryOp::Lte ? "le" : binary.op == BinaryOp::Gt ? "gt"
                                                                                                     : binary.op == BinaryOp::Gte
                                                                                                           ? "ge"
                                                                                                           : (binary.op == BinaryOp::Eq ? "eq"
                                                                                                                                         : "ne"));
        const auto tmp = next_temp();
        const ::std::string cmp_type = cmp_kind == ScalarKind::Bool ? "bool" : (cmp_kind == ScalarKind::Float ? "f64" : "i64");
        emit_line(tmp + " = cmp." + op + "." + cmp_type + " " + left.value + ", " + right.value);
        return {tmp, ScalarKind::Bool, true};
      }

      if (binary.op == BinaryOp::Mod) {
        unsigned long long mask = 0ULL;
        if (try_pow2_mod_mask_from_expr_codegen_flow(binary.right.get(), mask)) {
          auto left_pow2 = emit_expr(*binary.left, ctx, ExpectedExprContext::Int);
          if (left_pow2.kind == ScalarKind::Invalid) {
            return {"", ScalarKind::Invalid, false};
          }
          if (left_pow2.kind == ScalarKind::Unknown) {
            left_pow2.kind = ScalarKind::Int;
          }
          if (left_pow2.kind == ScalarKind::Int) {
            const auto out = next_temp();
            emit_line(out + " = call @__spark_mod_i64_pow2(" + left_pow2.value + ", " +
                      ::std::to_string(mask) + ")");
            return {out, ScalarKind::Int, true};
          }
        }
      }

      auto left = emit_expr(*binary.left, ctx, ExpectedExprContext::None);
      auto right = emit_expr(*binary.right, ctx, ExpectedExprContext::None);
      if (left.kind == ScalarKind::Invalid || right.kind == ScalarKind::Invalid) {
        return {"", ScalarKind::Invalid, false};
      }
      if (left.kind == ScalarKind::Unknown) {
        left.kind = ScalarKind::Int;
      }
      if (right.kind == ScalarKind::Unknown) {
        right.kind = ScalarKind::Int;
      }
      if (left.kind == ScalarKind::String || right.kind == ScalarKind::String) {
        if (binary.op == BinaryOp::Add && left.kind == ScalarKind::String && right.kind == ScalarKind::String) {
          const auto out = next_temp();
          emit_line(out + " = call @__spark_string_concat(" + left.value + ", " + right.value + ")");
          return {out, ScalarKind::String, true};
        }
        add_error("string arithmetic only supports '+' between two strings");
        return {"", ScalarKind::Invalid, false};
      }
      if (left.kind == ScalarKind::Bool || right.kind == ScalarKind::Bool) {
        add_error("arithmetic expects numeric operands");
        return {"", ScalarKind::Invalid, false};
      }

      ScalarKind result_kind = ScalarKind::Int;
      if (binary.op == BinaryOp::Div || binary.op == BinaryOp::Pow) {
        result_kind = ScalarKind::Float;
      } else if (binary.op == BinaryOp::Mod) {
        result_kind = coerce_numeric(left.kind, right.kind);
        if (result_kind == ScalarKind::Invalid) {
          add_error("modulo requires numeric operands");
          return {"", ScalarKind::Invalid, false};
        }
      } else {
        result_kind = coerce_numeric(left.kind, right.kind);
        if (result_kind == ScalarKind::Invalid) {
          add_error("arithmetic expects numeric operands");
          return {"", ScalarKind::Invalid, false};
        }
      }

      if (result_kind == ScalarKind::Float) {
        if (left.kind == ScalarKind::Int) {
          auto cast_left = next_temp();
          emit_line(cast_left + " = cast.i64_to_f64 " + left.value);
          left.value = cast_left;
          left.kind = ScalarKind::Float;
          left.numeric_hint.clear();
        }
        if (right.kind == ScalarKind::Int) {
          auto cast_right = next_temp();
          emit_line(cast_right + " = cast.i64_to_f64 " + right.value);
          right.value = cast_right;
          right.kind = ScalarKind::Float;
          right.numeric_hint.clear();
        }
      }

      const ::std::string opcode = (binary.op == BinaryOp::Add   ? "add"
                                 : binary.op == BinaryOp::Sub ? "sub"
                                 : binary.op == BinaryOp::Mul ? "mul"
                                 : binary.op == BinaryOp::Div ? "div"
                                 : binary.op == BinaryOp::Mod ? "mod"
                                                               : "pow");
      const auto is_supported_float_hint = [](const ::std::string& hint) {
        return hint == "f8" || hint == "f16" || hint == "bf16" || hint == "f32" || hint == "f64" ||
               hint == "f128" || hint == "f256" || hint == "f512";
      };
      ::std::string opcode_kind = scalar_kind_to_name(result_kind);
      ::std::string result_hint;
      if (result_kind == ScalarKind::Float) {
        if (is_supported_float_hint(left.numeric_hint) && left.numeric_hint == right.numeric_hint) {
          opcode_kind = left.numeric_hint;
          result_hint = left.numeric_hint;
        } else if (is_supported_float_hint(left.numeric_hint) && is_supported_float_hint(right.numeric_hint)) {
          opcode_kind = "f64";
          result_hint = "f64";
        }
      }
      const auto tmp = next_temp();
      emit_line(tmp + " = " + opcode + "." + opcode_kind + " " + left.value + ", " + right.value);
      return {tmp, result_kind, true, result_hint};
    }
    case Expr::Kind::Call:
      return emit_function_call(static_cast<const CallExpr&>(expr), ctx, expected);
    case Expr::Kind::List:
      return emit_list_expression(static_cast<const ListExpr&>(expr), ctx, expected);
    case Expr::Kind::Attribute:
      {
        const auto& attr = static_cast<const AttributeExpr&>(expr);
        const auto is_transpose = (attr.attribute == "T" || attr.attribute == "transpose");
        if (!is_transpose) {
          add_error("attribute access is only supported in method calls or transpose in scalar_runtime");
          return {"", ScalarKind::Invalid, false};
        }

        auto target = emit_expr(*attr.target, ctx, ExpectedExprContext::None);
        if (!target.has_value) {
          return {"", ScalarKind::Invalid, false};
        }

        auto inferred = target.kind;
        if (!is_matrix_kind(inferred)) {
          const auto* variable = root_variable_expr(*attr.target);
          if (variable != nullptr) {
            inferred = lookup_var_type(ctx, variable->name);
          }
          if (!is_matrix_kind(inferred)) {
            add_error("transpose only supported on matrix values");
            return {"", ScalarKind::Invalid, false};
          }
        }

        const auto output = next_temp();
        emit_line(output + " = call @" + matrix_transpose_fn_for(inferred) + "(" + target.value + ")");
        return {output, is_float_kind(inferred) ? ValueKind::MatrixFloat : ValueKind::MatrixInt, true};
      }
    case Expr::Kind::Index:
      return emit_index_expression(static_cast<const IndexExpr&>(expr), ctx, expected);
  }

  return {"", ScalarKind::Invalid, false};
}

Code CodeGenerator::emit_function_call(const CallExpr& call, FunctionContext& ctx, ExpectedExprContext expected) {
  const bool callee_is_attr = call.callee->kind == Expr::Kind::Attribute;
  const bool callee_is_name = call.callee->kind == Expr::Kind::Variable;

  if (!callee_is_attr && !callee_is_name) {
    add_error("call target must be a variable or attribute in scalar_runtime");
    return Code{"", ScalarKind::Invalid, false};
  }

  if (callee_is_attr) {
    const auto& attribute = static_cast<const AttributeExpr&>(*call.callee);
    const auto& attr_name = attribute.attribute;

    const auto target = emit_expr(*attribute.target, ctx, ExpectedExprContext::None);
    if (!target.has_value) {
      return {"", ScalarKind::Invalid, false};
    }
    if (!is_list_kind(target.kind)) {
      add_error(attr_name + "() requires a list receiver");
      return {"", ScalarKind::Invalid, false};
    }
    const auto* target_var = root_variable_expr(*attribute.target);
    const ::std::string target_name = target_var ? target_var->name : ::std::string{};
    const auto target_kind = target.kind;

    if (attr_name == "append") {
      if (call.args.size() != 1) {
        add_error("append() expects exactly one argument");
        return {"", ScalarKind::Invalid, false};
      }

      auto value = emit_expr(*call.args[0], ctx, ExpectedExprContext::None);
      if (!value.has_value) {
        return {"", ScalarKind::Invalid, false};
      }

      const auto append_payload_kind = value.kind;
      auto emit_payload = value;

      const auto cast_payload_to_i64 = [](ValueKind payload_kind, const ::std::string& value_name) {
        switch (payload_kind) {
          case ValueKind::ListInt:
          case ValueKind::ListAny:
            return ::std::string("cast.list_i64_to_i64 ") + value_name;
          case ValueKind::ListFloat:
            return ::std::string("cast.list_f64_to_i64 ") + value_name;
          case ValueKind::MatrixInt:
          case ValueKind::MatrixAny:
            return ::std::string("cast.matrix_i64_to_i64 ") + value_name;
          case ValueKind::MatrixFloat:
            return ::std::string("cast.matrix_f64_to_i64 ") + value_name;
          default:
            return value_name;
        }
      };

      if (target_kind == ValueKind::ListInt && append_payload_kind == ValueKind::Float) {
        const auto value_as_int = next_temp();
        emit_line(value_as_int + " = cast.f64_to_i64 " + value.value);
        emit_payload.value = value_as_int;
        emit_payload.kind = ValueKind::Int;
        value.kind = ValueKind::Int;
      } else if (target_kind == ValueKind::ListFloat && append_payload_kind == ValueKind::Int) {
        const auto value_as_float = next_temp();
        emit_line(value_as_float + " = cast.i64_to_f64 " + value.value);
        emit_payload.value = value_as_float;
        emit_payload.kind = ValueKind::Float;
        value.kind = ValueKind::Float;
      } else if (is_container_kind(append_payload_kind) && target_kind == ValueKind::ListInt) {
        const auto payload_as_i64 = next_temp();
        emit_line(payload_as_i64 + " = " + cast_payload_to_i64(append_payload_kind, value.value));
        emit_payload.value = payload_as_i64;
        emit_payload.kind = ValueKind::Int;
      } else if (is_container_kind(append_payload_kind) && target_kind == ValueKind::ListFloat) {
        add_error("cannot append container into float list");
        return {"", ScalarKind::Invalid, false};
      }

      if (!target_name.empty()) {
        set_container_element_type(ctx, target_name, value.kind);
      }
      auto append_fn = container_append_fn(target_kind);
      if (!target_name.empty() && ctx.unchecked_append_targets.count(target_name) > 0) {
        append_fn = container_append_unchecked_fn(target_kind);
      }
      emit_line("call @" + append_fn + "(" + target.value + ", " + emit_payload.value + ")");
      return {"", ScalarKind::Void, false};
    }

    auto cast_payload_for_list = [&](Code value, const ::std::string& method_name) -> Code {
      if (is_container_kind(value.kind)) {
        add_error(method_name + "() does not accept container arguments");
        return {"", ScalarKind::Invalid, false};
      }
      auto payload = value;
      if (target_kind == ValueKind::ListFloat && payload.kind == ValueKind::Int) {
        const auto casted = next_temp();
        emit_line(casted + " = cast.i64_to_f64 " + payload.value);
        payload.value = casted;
        payload.kind = ValueKind::Float;
      } else if ((target_kind == ValueKind::ListInt || target_kind == ValueKind::ListAny) &&
                 payload.kind == ValueKind::Float) {
        const auto casted = next_temp();
        emit_line(casted + " = cast.f64_to_i64 " + payload.value);
        payload.value = casted;
        payload.kind = ValueKind::Int;
      }
      return payload;
    };

    if (attr_name == "pop") {
      if (call.args.size() > 1) {
        add_error("pop() expects zero or one argument");
        return {"", ScalarKind::Invalid, false};
      }
      Code pop_index;
      if (call.args.empty()) {
        const auto len_temp = next_temp();
        const auto last_temp = next_temp();
        emit_line(len_temp + " = call @" + container_len_fn(target_kind) + "(" + target.value + ")");
        emit_line(last_temp + " = sub.i64 " + len_temp + ", 1");
        pop_index = {last_temp, ValueKind::Int, true};
      } else {
        pop_index = emit_expr(*call.args[0], ctx, ExpectedExprContext::Int);
      }
      if (!pop_index.has_value) {
        return {"", ScalarKind::Invalid, false};
      }
      const auto out = next_temp();
      const auto pop_fn = is_float_kind(target_kind) ? "__spark_list_pop_f64" : "__spark_list_pop_i64";
      emit_line(out + " = call @" + pop_fn + "(" + target.value + ", " + pop_index.value + ")");
      return {out, is_float_kind(target_kind) ? ValueKind::Float : ValueKind::Int, true};
    }

    if (attr_name == "insert") {
      if (call.args.size() != 2) {
        add_error("insert() expects exactly two arguments");
        return {"", ScalarKind::Invalid, false};
      }
      const auto index_arg = emit_expr(*call.args[0], ctx, ExpectedExprContext::Int);
      if (!index_arg.has_value) {
        return {"", ScalarKind::Invalid, false};
      }
      auto value_arg = emit_expr(*call.args[1], ctx, ExpectedExprContext::None);
      if (!value_arg.has_value) {
        return {"", ScalarKind::Invalid, false};
      }
      value_arg = cast_payload_for_list(value_arg, "insert");
      if (!value_arg.has_value) {
        return {"", ScalarKind::Invalid, false};
      }
      if (!target_name.empty()) {
        set_container_element_type(ctx, target_name, value_arg.kind);
      }
      const auto insert_fn = is_float_kind(target_kind) ? "__spark_list_insert_f64" : "__spark_list_insert_i64";
      emit_line("call @" + ::std::string(insert_fn) + "(" + target.value + ", " + index_arg.value + ", " + value_arg.value + ")");
      return {"", ScalarKind::Void, false};
    }

    if (attr_name == "remove") {
      if (call.args.size() != 1) {
        add_error("remove() expects exactly one argument");
        return {"", ScalarKind::Invalid, false};
      }
      auto value_arg = emit_expr(*call.args[0], ctx, ExpectedExprContext::None);
      if (!value_arg.has_value) {
        return {"", ScalarKind::Invalid, false};
      }
      value_arg = cast_payload_for_list(value_arg, "remove");
      if (!value_arg.has_value) {
        return {"", ScalarKind::Invalid, false};
      }
      const auto remove_fn = is_float_kind(target_kind) ? "__spark_list_remove_f64" : "__spark_list_remove_i64";
      emit_line("call @" + ::std::string(remove_fn) + "(" + target.value + ", " + value_arg.value + ")");
      return {"", ScalarKind::Void, false};
    }

    add_error("unsupported method call: " + attr_name);
    return Code{"", ScalarKind::Invalid, false};
  }

  const auto& callee = static_cast<const VariableExpr&>(*call.callee);
  const auto callee_kind = lookup_var_type(ctx, callee.name);

  if (is_container_kind(callee_kind) || callee_kind == ValueKind::String) {
    if (call.args.empty()) {
      add_error("index-call expects at least one index");
      return {"", ScalarKind::Invalid, false};
    }

    auto current_code = Code{callee.name, callee_kind, true};
    auto current_kind = callee_kind;
    auto current_element_kind = lookup_container_element_type(ctx, callee.name);
    if (current_element_kind == ValueKind::Unknown) {
      current_element_kind = infer_container_scalar_type(current_kind);
    }

    const auto cast_ptr_from_i64 = [](ValueKind container_kind, const ::std::string& value) {
      switch (container_kind) {
        case ValueKind::ListInt:
        case ValueKind::ListAny:
          return ::std::string("cast.i64_to_list_i64 ") + value;
        case ValueKind::ListFloat:
          return ::std::string("cast.i64_to_list_f64 ") + value;
        case ValueKind::MatrixInt:
        case ValueKind::MatrixAny:
          return ::std::string("cast.i64_to_matrix_i64 ") + value;
        case ValueKind::MatrixFloat:
          return ::std::string("cast.i64_to_matrix_f64 ") + value;
        default:
          return ::std::string("cast.i64_to_i64 ") + value;
      }
    };
    auto scalar_from_container = [](ValueKind kind) {
      return is_float_kind(kind) ? ValueKind::Float : ValueKind::Int;
    };

    for (const auto& arg_expr : call.args) {
      const auto index_code = emit_expr(*arg_expr, ctx, ExpectedExprContext::Int);
      if (!index_code.has_value) {
        return {"", ScalarKind::Invalid, false};
      }

      if (is_matrix_kind(current_kind)) {
        const auto out = next_temp();
        const auto row_getter = is_float_kind(current_kind) ? "__spark_matrix_row_f64" : "__spark_matrix_row_i64";
        emit_line(out + " = call @" + row_getter + "(" + current_code.value + ", " + index_code.value + ")");
        current_code = {out, is_float_kind(current_kind) ? ValueKind::ListFloat : ValueKind::ListInt, true};
        current_kind = current_code.kind;
        current_element_kind = infer_container_scalar_type(current_kind);
        continue;
      }

      if (current_kind == ValueKind::String) {
        const auto out = next_temp();
        emit_line(out + " = call @__spark_string_index(" + current_code.value + ", " + index_code.value + ")");
        current_code = {out, ValueKind::String, true};
        current_kind = current_code.kind;
        current_element_kind = ValueKind::Unknown;
        continue;
      }

      if (!is_list_kind(current_kind)) {
        add_error("index-call target is not indexable");
        return {"", ScalarKind::Invalid, false};
      }

      const auto out = next_temp();
      const auto getter = is_float_kind(current_kind) ? "__spark_list_get_f64" : "__spark_list_get_i64";
      emit_line(out + " = call @" + getter + "(" + current_code.value + ", " + index_code.value + ")");

      if (is_container_kind(current_element_kind)) {
        if (is_float_kind(current_kind)) {
          add_error("cannot read nested container from float list");
          return {"", ScalarKind::Invalid, false};
        }
        const auto casted = next_temp();
        emit_line(casted + " = " + cast_ptr_from_i64(current_element_kind, out));
        current_code = {casted, current_element_kind, true};
        current_kind = current_element_kind;
        current_element_kind = infer_container_scalar_type(current_kind);
        continue;
      }

      auto effective = scalar_from_container(current_kind);
      if (is_float_kind(current_kind) && current_element_kind == ValueKind::Int) {
        const auto casted = next_temp();
        emit_line(casted + " = cast.f64_to_i64 " + out);
        current_code = {casted, ValueKind::Int, true};
        effective = ValueKind::Int;
      } else if (!is_float_kind(current_kind) && current_element_kind == ValueKind::Float) {
        const auto casted = next_temp();
        emit_line(casted + " = cast.i64_to_f64 " + out);
        current_code = {casted, ValueKind::Float, true};
        effective = ValueKind::Float;
      } else {
        current_code = {out, effective, true};
      }
      current_kind = effective;
      current_element_kind = ValueKind::Unknown;
    }

    return current_code;
  }

  if (callee.name == "matrix_i64" || callee.name == "matrix_f64") {
    if (call.args.size() != 2) {
      add_error(callee.name + "() expects exactly two integer arguments");
      return {"", ScalarKind::Invalid, false};
    }
    const auto rows = emit_expr(*call.args[0], ctx, ExpectedExprContext::Int);
    const auto cols = emit_expr(*call.args[1], ctx, ExpectedExprContext::Int);
    if (!rows.has_value || !cols.has_value) {
      return {"", ScalarKind::Invalid, false};
    }
    const auto matrix_kind = (callee.name == "matrix_f64") ? ValueKind::MatrixFloat : ValueKind::MatrixInt;
    const auto out = next_temp();
    emit_line(out + " = call @" + matrix_new_fn(matrix_kind) + "(" + rows.value + ", " + cols.value + ")");
    return {out, matrix_kind, true};
  }

  if (callee.name == "print") {
    if (call.args.size() != 1) {
      add_error("print expects exactly one argument");
      return {"", ScalarKind::Invalid, false};
    }
    const auto arg = emit_expr(*call.args[0], ctx, ExpectedExprContext::None);
    if (!arg.has_value) {
      return {"", ScalarKind::Invalid, false};
    }
    emit_line("call @print(" + arg.value + ")");
    return {"", ScalarKind::Void, false};
  }

  if (callee.name == "string") {
    if (call.args.size() > 1) {
      add_error("string() expects zero or one argument");
      return {"", ScalarKind::Invalid, false};
    }
    if (call.args.empty()) {
      const auto out = next_temp();
      emit_line(out + " = str.const \"\"");
      return {out, ScalarKind::String, true};
    }
    const auto arg = emit_expr(*call.args[0], ctx, ExpectedExprContext::None);
    if (!arg.has_value) {
      return {"", ScalarKind::Invalid, false};
    }
    if (arg.kind == ScalarKind::String) {
      return {arg.value, ScalarKind::String, true};
    }
    const auto out = next_temp();
    if (arg.kind == ScalarKind::Int) {
      emit_line(out + " = call @__spark_string_from_i64(" + arg.value + ")");
    } else if (arg.kind == ScalarKind::Float) {
      emit_line(out + " = call @__spark_string_from_f64(" + arg.value + ")");
    } else if (arg.kind == ScalarKind::Bool) {
      emit_line(out + " = call @__spark_string_from_bool(" + arg.value + ")");
    } else {
      add_error("string() expects scalar/string input");
      return {"", ScalarKind::Invalid, false};
    }
    return {out, ScalarKind::String, true};
  }

  if (callee.name == "len") {
    if (call.args.size() != 1) {
      add_error("len() expects exactly one argument");
      return {"", ScalarKind::Invalid, false};
    }
    const auto arg = emit_expr(*call.args[0], ctx, ExpectedExprContext::None);
    if (!arg.has_value) {
      return {"", ScalarKind::Invalid, false};
    }
    const auto target_kind = arg.kind == ValueKind::Unknown ? ValueKind::ListInt : arg.kind;
    ::std::string callee_name = "__spark_list_len_i64";
    if (target_kind == ValueKind::String) {
      callee_name = "__spark_string_len";
    } else if (is_matrix_kind(target_kind)) {
      callee_name = matrix_len_rows_fn(target_kind);
    } else if (is_list_kind(target_kind)) {
      callee_name = "__spark_list_len_i64";
    }
    const auto temp = next_temp();
    emit_line(temp + " = call @" + callee_name + "(" + arg.value + ")");
    return {temp, ValueKind::Int, true};
  }

  if (callee.name == "utf8_len" || callee.name == "utf16_len") {
    if (call.args.size() != 1) {
      add_error(callee.name + "() expects exactly one string argument");
      return {"", ScalarKind::Invalid, false};
    }
    const auto arg = emit_expr(*call.args[0], ctx, ExpectedExprContext::None);
    if (!arg.has_value) {
      return {"", ScalarKind::Invalid, false};
    }
    if (arg.kind != ValueKind::String) {
      add_error(callee.name + "() expects string argument");
      return {"", ScalarKind::Invalid, false};
    }
    const auto out = next_temp();
    const auto fn = (callee.name == "utf8_len") ? "__spark_string_utf8_len" : "__spark_string_utf16_len";
    emit_line(out + " = call @" + ::std::string(fn) + "(" + arg.value + ")");
    return {out, ValueKind::Int, true};
  }

  if (callee.name == "range") {
    add_error("range() can only be used in for-loop iterable position in scalar_runtime");
    return {"", ScalarKind::Invalid, false};
  }

  if (callee.name == "bench_tick") {
    if (!call.args.empty()) {
      add_error("bench_tick() expects no arguments");
      return {"", ScalarKind::Invalid, false};
    }
    const auto out = next_temp();
    emit_line(out + " = call @__spark_bench_tick_i64()");
    return {out, ScalarKind::Int, true};
  }

  if (callee.name == "bench_tick_raw") {
    if (!call.args.empty()) {
      add_error("bench_tick_raw() expects no arguments");
      return {"", ScalarKind::Invalid, false};
    }
    const auto out = next_temp();
    emit_line(out + " = call @__spark_bench_tick_raw_i64()");
    return {out, ScalarKind::Int, true};
  }

  if (callee.name == "bench_tick_scale_num") {
    if (!call.args.empty()) {
      add_error("bench_tick_scale_num() expects no arguments");
      return {"", ScalarKind::Invalid, false};
    }
    const auto out = next_temp();
    emit_line(out + " = call @__spark_bench_tick_scale_num_i64()");
    return {out, ScalarKind::Int, true};
  }

  if (callee.name == "bench_tick_scale_den") {
    if (!call.args.empty()) {
      add_error("bench_tick_scale_den() expects no arguments");
      return {"", ScalarKind::Invalid, false};
    }
    const auto out = next_temp();
    emit_line(out + " = call @__spark_bench_tick_scale_den_i64()");
    return {out, ScalarKind::Int, true};
  }

  const auto is_int_ctor = [](const ::std::string& name) {
    return name == "i8" || name == "i16" || name == "i32" || name == "i64" || name == "i128" ||
           name == "i256" || name == "i512";
  };
  const auto is_float_ctor = [](const ::std::string& name) {
    return name == "f8" || name == "f16" || name == "bf16" || name == "f32" || name == "f64" ||
           name == "f128" || name == "f256" || name == "f512";
  };

  if (is_int_ctor(callee.name) || is_float_ctor(callee.name)) {
    if (call.args.size() != 1) {
      add_error(callee.name + "() expects exactly one numeric argument");
      return {"", ScalarKind::Invalid, false};
    }
    auto arg = emit_expr(*call.args[0], ctx, ExpectedExprContext::None);
    if (!arg.has_value) {
      return {"", ScalarKind::Invalid, false};
    }
    if (is_container_kind(arg.kind)) {
      add_error(callee.name + "() does not accept container arguments in scalar_runtime codegen");
      return {"", ScalarKind::Invalid, false};
    }
    if (arg.kind != ScalarKind::Int && arg.kind != ScalarKind::Float && arg.kind != ScalarKind::Bool) {
      add_error(callee.name + "() expects numeric argument");
      return {"", ScalarKind::Invalid, false};
    }
    if (is_int_ctor(callee.name)) {
      if (arg.kind == ScalarKind::Float) {
        const auto casted = next_temp();
        emit_line(casted + " = cast.f64_to_i64 " + arg.value);
        return {casted, ScalarKind::Int, true};
      }
      return {arg.value, ScalarKind::Int, true};
    }
    auto base_value = arg.value;
    if (arg.kind == ScalarKind::Int) {
      const auto casted = next_temp();
      emit_line(casted + " = cast.i64_to_f64 " + arg.value);
      base_value = casted;
    }
    const auto quantized = next_temp();
    emit_line(quantized + " = add." + callee.name + " " + base_value + ", 0.0");
    return {quantized, ScalarKind::Float, true, callee.name};
  }

  const auto* signature = find_function_signature(callee.name);
  if (!signature) {
    add_error("unknown callee: " + callee.name);
    return {"", ScalarKind::Invalid, false};
  }
  if (call.args.size() != signature->params.size()) {
    add_error("call to '" + callee.name + "' has wrong arity");
    return {"", ScalarKind::Invalid, false};
  }

  ::std::vector<::std::string> arg_values;
  arg_values.reserve(call.args.size());
  for (::std::size_t i = 0; i < call.args.size(); ++i) {
    const auto arg = emit_expr(*call.args[i], ctx, ExpectedExprContext::None);
    if (!arg.has_value) {
      return {"", ScalarKind::Invalid, false};
    }

    const auto expected_kind = (i < signature->param_kinds.size() ? signature->param_kinds[i] : ScalarKind::Unknown);
    auto incoming = arg.kind == ScalarKind::Unknown ? ScalarKind::Int : arg.kind;

    if (expected_kind == ScalarKind::Unknown) {
      const_cast<CodeGenerator::FunctionSignature*>(signature)->param_kinds[i] = incoming;
    } else if (!((expected_kind == ScalarKind::Float && incoming == ScalarKind::Int) ||
                 (expected_kind == ScalarKind::Int && incoming == ScalarKind::Float &&
                  signature->param_kinds[i] == ScalarKind::Unknown) ||
                 expected_kind == incoming)) {
      add_error("argument " + ::std::to_string(i + 1) + " for '" + callee.name + "' has type mismatch");
      return {"", ScalarKind::Invalid, false};
    }

    arg_values.push_back(arg.value);
  }

  ::std::string joined;
  for (::std::size_t i = 0; i < arg_values.size(); ++i) {
    if (i > 0) {
      joined += ", ";
    }
    joined += arg_values[i];
  }

  const auto call_return = signature->return_kind == ScalarKind::Unknown ? ScalarKind::Unknown : signature->return_kind;
  if (call_return == ScalarKind::Void) {
    if (expected != ExpectedExprContext::None) {
      add_error("cannot use void call in expression context: " + callee.name);
      return {"", ScalarKind::Invalid, false};
    }
    emit_line("call @" + callee.name + "(" + joined + ")");
    return {"", ScalarKind::Void, false};
  }

  const auto temp = next_temp();
  emit_line(temp + " = call @" + callee.name + "(" + joined + ")");
  return {temp, call_return == ScalarKind::Unknown ? ScalarKind::Int : call_return, true};
}
