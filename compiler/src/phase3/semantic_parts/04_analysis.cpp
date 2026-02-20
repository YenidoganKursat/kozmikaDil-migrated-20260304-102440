    }
    if (call.args.size() < arity_min) {
      add_error("function called with too few arguments");
      add_context_reason({.message = "function call arity mismatch", .normalizable = false});
    }

    for (std::size_t i = 0; i < call.args.size() && i < callee_expr->function_params.size(); ++i) {
      auto arg = infer_expr(*call.args[i]);
      auto target = callee_expr->function_params[i];
      if (target->kind == Type::Kind::Unknown) {
        continue;
      }
      if (!is_assignable(*arg, *target)) {
        add_error("argument " + std::to_string(i + 1) + " type mismatch");
        add_context_reason({.message = "argument " + std::to_string(i + 1) + " incompatible", .normalizable = false});
      }
      analyze_expr(*call.args[i], arg);
    }

    if (callee_expr->kind == Type::Kind::Builtin) {
      if (call.callee->kind == Expr::Kind::Variable) {
        const auto& callee_var = static_cast<const VariableExpr&>(*call.callee).name;
        if (callee_var == "append") {
          add_context_reason({.message = "global append() call is mutating", .normalizable = true});
        } else if (callee_var == "spawn") {
          add_context_reason(
              {.message = "spawn() lowers to phase9 scheduler task enqueue path", .normalizable = true});
          if (call.args.empty()) {
            add_error("spawn() expects callable first argument");
          } else if (call.args[0]->kind != Expr::Kind::Variable) {
            add_error("spawn() callable must be named function for sendable analysis");
            add_context_reason(
                {.message = "non-sendable capture risk: spawn() callable is not a named function",
                 .normalizable = false});
          }
          for (std::size_t i = 1; i < call.args.size(); ++i) {
            auto arg_type = infer_expr(*call.args[i]);
            analyze_expr(*call.args[i], arg_type);
            if (arg_type->kind != Type::Kind::Error && !is_sendable_for_phase9(*arg_type)) {
              report_non_sendable_capture("spawn()", i, arg_type);
            }
          }
        } else if (callee_var == "parallel_for" || callee_var == "par_map" || callee_var == "par_reduce") {
          add_context_reason(
              {.message = callee_var + "() lowers to phase9 work-stealing scheduler", .normalizable = true});
          const std::size_t fn_index = (callee_var == "parallel_for") ? 2 : (callee_var == "par_map" ? 1 : 2);
          if (call.args.size() > fn_index && call.args[fn_index]->kind != Expr::Kind::Variable) {
            add_error(callee_var + "() callable must be named function for sendable analysis");
            add_context_reason(
                {.message = "non-sendable capture risk in " + callee_var + "() callable",
                 .normalizable = false});
          }
          const std::size_t capture_start = (callee_var == "parallel_for") ? 3 : call.args.size();
          for (std::size_t i = capture_start; i < call.args.size(); ++i) {
            auto arg_type = infer_expr(*call.args[i]);
            analyze_expr(*call.args[i], arg_type);
            if (arg_type->kind != Type::Kind::Error && !is_sendable_for_phase9(*arg_type)) {
              report_non_sendable_capture(callee_var + "()", i, arg_type);
            }
          }
        } else if (callee_var == "task_group" || callee_var == "join" || callee_var == "cancel") {
          add_context_reason(
              {.message = "structured concurrency runtime primitive invoked (" + callee_var + ")",
               .normalizable = true});
        } else if (callee_var == "channel" || callee_var == "send" || callee_var == "recv" ||
                   callee_var == "close" || callee_var == "stream" || callee_var == "anext") {
          add_context_reason(
              {.message = "event-driven channel primitive invoked (" + callee_var + ")",
               .normalizable = true});
        }
      }
    }
    return callee_expr->function_return ? callee_expr->function_return : unknown_type();
  }

  add_error("attempted to call non-callable value");
  add_context_reason({.message = "call to non-callable value", .normalizable = false});
  return unknown_type();
}

void TypeChecker::check_unary(const UnaryExpr& unary) {
  auto value = infer_expr(*unary.operand);
  if (unary.op == UnaryOp::Neg) {
    if (!is_numeric_type(*value) && value->kind != Type::Kind::Unknown) {
      add_error("unary - expects numeric value");
      add_context_reason({.message = "invalid unary operator usage", .normalizable = false});
    }
  } else if (unary.op == UnaryOp::Not) {
    if (!is_bool_like(*value)) {
      add_error("unary not expects bool-like value");
      add_context_reason({.message = "invalid unary operator usage", .normalizable = false});
    }
  } else if (unary.op == UnaryOp::Await) {
    if (value->kind != Type::Kind::Task && value->kind != Type::Kind::Unknown) {
      add_error("await expects task value");
      add_context_reason({.message = "await on non-task value", .normalizable = false});
    }
  }
}

void TypeChecker::check_binary(const BinaryExpr& binary) {
  auto left = infer_expr(*binary.left);
  auto right = infer_expr(*binary.right);
  analyze_expr(*binary.left, left);
  analyze_expr(*binary.right, right);

  if (binary.op == BinaryOp::Add || binary.op == BinaryOp::Sub ||
      binary.op == BinaryOp::Mul || binary.op == BinaryOp::Div ||
      binary.op == BinaryOp::Mod || binary.op == BinaryOp::Pow) {
    if (left->kind == Type::Kind::Unknown || right->kind == Type::Kind::Unknown) {
      return;
    }

    if (left->kind == Type::Kind::Matrix || right->kind == Type::Kind::Matrix) {
      auto matrix = (left->kind == Type::Kind::Matrix) ? left : right;
      auto other = (left->kind == Type::Kind::Matrix) ? right : left;
      const bool allow_hetero_matrix = (binary.op == BinaryOp::Add || binary.op == BinaryOp::Mul);
      if (other->kind == Type::Kind::Matrix) {
        if (binary.op == BinaryOp::Mul) {
          if (left->matrix_cols != 0 && right->matrix_rows != 0 &&
              left->matrix_cols != right->matrix_rows) {
            add_error("matrix multiplication shape mismatch: lhs.cols must equal rhs.rows");
            add_context_reason({.message = "matrix matmul shape mismatch", .normalizable = false});
          } else {
            add_context_reason(
                {.message = "matrix '*' lowers to matmul kernel path when eligible", .normalizable = true});
          }
        } else {
          if (matrix->matrix_cols != 0 && other->matrix_cols != 0 && matrix->matrix_cols != other->matrix_cols) {
            add_error("matrix shapes differ in column count");
            add_context_reason({.message = "matrix shape mismatch in arithmetic", .normalizable = false});
          }
          if (matrix->matrix_rows != 0 && other->matrix_rows != 0 && matrix->matrix_rows != other->matrix_rows) {
            add_error("matrix shapes differ in row count");
            add_context_reason({.message = "matrix shape mismatch in arithmetic", .normalizable = false});
          }
        }
        if (other->list_element && matrix->list_element &&
            other->list_element->kind != Type::Kind::Unknown &&
            matrix->list_element->kind != Type::Kind::Unknown &&
            !(is_numeric_type(*other->list_element) && is_numeric_type(*matrix->list_element)) &&
            binary.op != BinaryOp::Add) {
          add_error("matrix arithmetic expects numeric element types");
          add_context_reason({.message = "invalid matrix element type in arithmetic", .normalizable = false});
        }
      } else if (!is_numeric_type(*other) && other->kind != Type::Kind::Unknown &&
                 !allow_hetero_matrix) {
        add_error("matrix arithmetic with scalar expects numeric scalar");
        add_context_reason({.message = "invalid matrix-scalar arithmetic operand", .normalizable = false});
      }
      return;
    }

    if (left->kind == Type::Kind::List || right->kind == Type::Kind::List) {
      const auto list = (left->kind == Type::Kind::List) ? left : right;
      const auto other = (left->kind == Type::Kind::List) ? right : left;
      const bool allow_hetero_list = (binary.op == BinaryOp::Add || binary.op == BinaryOp::Mul);
      const auto list_elem_numeric_or_unknown = [&](const TypePtr& t) {
        if (!t || !t->list_element) {
          return true;
        }
        return t->list_element->kind == Type::Kind::Unknown || is_numeric_type(*t->list_element);
      };

      if (other->kind == Type::Kind::List && binary.op == BinaryOp::Add) {
        return;
      }
      if (!list_elem_numeric_or_unknown(list) ||
          (other->kind == Type::Kind::List && !list_elem_numeric_or_unknown(other))) {
        if (!allow_hetero_list) {
          add_error("list arithmetic expects numeric element types");
          add_context_reason({.message = "invalid list element type in arithmetic", .normalizable = false});
          return;
        }
      }
      if (other->kind == Type::Kind::List || is_numeric_type(*other) || other->kind == Type::Kind::Unknown) {
        return;
      }
      if (allow_hetero_list) {
        return;
      }
      add_error("list arithmetic expects list/list or list/scalar numeric operands");
      add_context_reason({.message = "invalid list arithmetic operands", .normalizable = false});
      return;
    }

    if (binary.op == BinaryOp::Add) {
      if (left->kind == Type::Kind::String && right->kind == Type::Kind::String) {
        return;
      }
      if (!is_numeric_type(*left) || !is_numeric_type(*right)) {
        add_error("binary + expects numeric values, two lists, or two strings");
        add_context_reason({.message = "invalid binary + operands", .normalizable = false});
      }
      return;
    }
    if (!is_numeric_type(*left) || !is_numeric_type(*right)) {
      add_error("binary arithmetic expects numeric values");
      add_context_reason({.message = "invalid arithmetic operands", .normalizable = false});
    }
    if ((binary.op == BinaryOp::Mod || binary.op == BinaryOp::Pow) &&
        (!is_numeric_type(*left) || !is_numeric_type(*right))) {
      add_error("numeric operator expects numeric operands");
      add_context_reason({.message = "invalid numeric operator use", .normalizable = false});
    }
    return;
  }

  if (binary.op == BinaryOp::And || binary.op == BinaryOp::Or) {
    if (!is_bool_like(*left) || !is_bool_like(*right)) {
      add_error("boolean operators require bool-like operands");
      add_context_reason({.message = "invalid boolean operands", .normalizable = false});
    }
    return;
  }

  if (binary.op == BinaryOp::Eq || binary.op == BinaryOp::Ne) {
    return;
  }
  if (left->kind == Type::Kind::Unknown || right->kind == Type::Kind::Unknown) {
    return;
  }
  if (left->kind == Type::Kind::String || right->kind == Type::Kind::String) {
    if (left->kind == Type::Kind::String && right->kind == Type::Kind::String) {
      return;
    }
    add_error("comparison operators require both operands to be string or numeric");
    add_context_reason({.message = "invalid comparison operands", .normalizable = false});
    return;
  }

  if (!is_numeric_type(*left) || !is_numeric_type(*right)) {
    add_error("comparison operators require numeric values");
    add_context_reason({.message = "invalid comparison operands", .normalizable = false});
  }
}

void TypeChecker::check_stmt(const Stmt& stmt) {
  switch (stmt.kind) {
    case Stmt::Kind::Expression: {
      const auto& expression_stmt = static_cast<const ExpressionStmt&>(stmt);
      auto expr_type = infer_expr(*expression_stmt.expression);
      analyze_expr(*expression_stmt.expression, expr_type);
      break;
    }
    case Stmt::Kind::Assign: {
      const auto& assign = static_cast<const AssignStmt&>(stmt);
      auto rhs = infer_expr(*assign.value);
      analyze_expr(*assign.value, rhs);

      if (assign.target->kind == Expr::Kind::Variable) {
        const auto& variable = static_cast<const VariableExpr&>(*assign.target);
        const auto& target_name = variable.name;
        auto current = get_name(target_name);

        if (current->kind == Type::Kind::Error) {
          define_name(target_name, rhs);
          break;
        }

        if (!is_assignable(*rhs, *current) && current->kind != Type::Kind::Unknown) {
          add_error("cannot assign " + type_to_string(*rhs) + " to existing name '" + target_name +
                    "' of type " + type_to_string(*current));
          add_context_reason(
              {.message = "type incompatibility in assignment to " + target_name, .normalizable = false});
          break;
        }

        if (current->kind == Type::Kind::Unknown || current->kind == Type::Kind::Error) {
          set_name(target_name, rhs);
        } else if (current->kind == Type::Kind::List && rhs->kind == Type::Kind::List &&
                   !same_or_unknown(*current->list_element, *rhs->list_element)) {
          auto merged = normalize_list_elements(current->list_element, rhs->list_element);
          if (!same_or_unknown(*merged, *current->list_element)) {
            add_context_reason({.message = "list variable '" + target_name + "' widened by reassignment",
                                .normalizable = true});
          }
          set_name(target_name, list_type(merged));
        } else {
          set_name(target_name, rhs);
        }
      } else if (assign.target->kind == Expr::Kind::Index) {
        auto target_type = infer_lvalue_type(*assign.target);
        if (target_type->kind == Type::Kind::Error) {
          break;
        }
        const auto* base = root_variable_expr(*assign.target);
        if (!base) {
          add_error("invalid assignment target");
          break;
        }
        auto current = get_name(base->name);
        if (current->kind == Type::Kind::Error) {
          add_error("cannot assign to undefined container '" + base->name + "'");
          break;
        }
        if (!is_assignable(*rhs, *target_type) && target_type->kind != Type::Kind::Unknown) {
          add_error("cannot assign " + type_to_string(*rhs) + " to index target of type " +
                    type_to_string(*target_type));
          add_context_reason({.message = "index assignment type mismatch", .normalizable = false});
          break;
        }
        if (current->kind == Type::Kind::List) {
          auto element = current->list_element ? current->list_element : unknown_type();
          auto merged = normalize_list_elements(element, rhs);
          if (!same_or_unknown(*merged, *element)) {
            add_context_reason({.message = "container element widened by index assignment", .normalizable = true});
          }
          set_name(base->name, list_type(merged));
        } else if (current->kind == Type::Kind::Matrix) {
          auto element = current->list_element ? current->list_element : unknown_type();
          auto merged = normalize_list_elements(element, rhs);
          if (!same_or_unknown(*merged, *element)) {
            add_context_reason({.message = "matrix element widened by index assignment", .normalizable = true});
          }
          set_name(base->name, matrix_type(merged, current->matrix_rows, current->matrix_cols));
        }
      } else {
        add_error("invalid assignment target");
      }
      break;
    }
    case Stmt::Kind::Return: {
      const auto& ret = static_cast<const ReturnStmt&>(stmt);
      if (ret.value) {
        auto ret_type = infer_expr(*ret.value);
        analyze_expr(*ret.value, ret_type);
      }
      break;
    }
    case Stmt::Kind::If: {
      const auto& if_stmt = static_cast<const IfStmt&>(stmt);
      auto cond = infer_expr(*if_stmt.condition);
      analyze_expr(*if_stmt.condition, cond);
      if (!is_bool_like(*cond) && cond->kind != Type::Kind::Unknown) {
        add_error("if condition is not bool-like");
        add_context_reason({.message = "if condition not bool-like", .normalizable = false});
      }
      check_program_body(if_stmt.then_body);
      for (const auto& elif_pair : if_stmt.elif_branches) {
        auto elif_cond = infer_expr(*elif_pair.first);
        analyze_expr(*elif_pair.first, elif_cond);
        if (!is_bool_like(*elif_cond) && elif_cond->kind != Type::Kind::Unknown) {
          add_error("elif condition is not bool-like");
          add_context_reason({.message = "elif condition not bool-like", .normalizable = false});
        }
        check_program_body(elif_pair.second);
      }
      if (!if_stmt.else_body.empty()) {
        check_program_body(if_stmt.else_body);
      }
      break;
    }
    case Stmt::Kind::While: {
      const auto& loop = static_cast<const WhileStmt&>(stmt);
      auto cond = infer_expr(*loop.condition);
      analyze_expr(*loop.condition, cond);
      const std::string owner =
          function_stack_.empty() ? "__main__" : function_stack_.back().name;
      push_loop_context(owner, "while");
      if (!is_bool_like(*cond) && cond->kind != Type::Kind::Unknown) {
        add_context_reason({.message = "while condition is not bool-like", .normalizable = false});
      }
      check_program_body(loop.body);
      pop_loop_context();
      break;
    }
    case Stmt::Kind::For: {
      const auto& loop = static_cast<const ForStmt&>(stmt);
      auto iterable = infer_expr(*loop.iterable);
      analyze_expr(*loop.iterable, iterable);
      const std::string owner =
          function_stack_.empty() ? "__main__" : function_stack_.back().name;
      push_loop_context(owner, loop.is_async ? "async_for" : "for");
      if (iterable->kind != Type::Kind::List && iterable->kind != Type::Kind::Matrix &&
          (!loop.is_async || iterable->kind != Type::Kind::Channel) &&
          iterable->kind != Type::Kind::Unknown) {
        add_error(loop.is_async ? "async for iterable must be Channel/List/Matrix"
                                : "for loop iterable must be List");
        add_context_reason({.message = loop.is_async
                                           ? "async for iterable is not a Channel/List/Matrix"
                                           : "for iterable is not a List/Matrix",
                            .normalizable = false});
      }
      if (loop.is_async) {
        add_context_reason({.message = "async for lowered to stream/await loop form",
                            .normalizable = true});
      }
      if (iterable->kind == Type::Kind::List && iterable->list_element &&
          iterable->list_element->kind == Type::Kind::Any) {
        add_context_reason({.message = "for loop iterates over list[Any]", .normalizable = true});
      }

      push_scope("loop");
      if (iterable->kind == Type::Kind::List && iterable->list_element) {
        define_name(loop.name, iterable->list_element);
      } else if (iterable->kind == Type::Kind::Matrix && iterable->list_element) {
        // Matrix iteration follows Python-like row iteration semantics.
        define_name(loop.name, list_type(iterable->list_element));
      } else if (iterable->kind == Type::Kind::Channel && iterable->channel_element) {
        define_name(loop.name, iterable->channel_element);
      } else {
        define_name(loop.name, unknown_type());
      }
      check_program_body(loop.body);
      pop_scope();
      pop_loop_context();
      break;
    }
    case Stmt::Kind::FunctionDef: {
      const auto& fn = static_cast<const FunctionDefStmt&>(stmt);
      std::vector<TypePtr> params;
      for (std::size_t i = 0; i < fn.params.size(); ++i) {
        (void)i;
        params.push_back(unknown_type());
      }
      auto fn_type = function_type(params, fn.is_async ? task_type(unknown_type()) : unknown_type());
      if (fn.is_async) {
        add_context_reason(
            {.message = "async function lowers to state-machine task frame (phase9 runtime path)",
             .normalizable = true});
      }
      define_name(fn.name, fn_type);
      check_function_body(fn, fn_type);
      break;
    }
    case Stmt::Kind::WithTaskGroup: {
      const auto& with_stmt = static_cast<const WithTaskGroupStmt&>(stmt);
      // Keep `as <name>` binding in outer scope (Python-style), while still
      // analyzing body declarations in a dedicated nested scope.
      define_name(with_stmt.name, task_group_type());
      push_scope("task_group");
      if (with_stmt.timeout_ms) {
        auto timeout_type = infer_expr(*with_stmt.timeout_ms);
        analyze_expr(*with_stmt.timeout_ms, timeout_type);
        if (!is_numeric_type(*timeout_type) && timeout_type->kind != Type::Kind::Unknown) {
          add_error("task_group timeout must be numeric");
          add_context_reason({.message = "task_group timeout is non-numeric", .normalizable = false});
        }
      }
      add_context_reason(
          {.message = "structured concurrency scope enables cooperative cancellation + join",
           .normalizable = true});
      check_program_body(with_stmt.body);
      pop_scope();
      break;
    }
    case Stmt::Kind::ClassDef: {
      const auto& cls = static_cast<const ClassDefStmt&>(stmt);
      const auto shape_id = next_shape_id_++;
      const auto cls_type = class_type(cls.name, cls.open_shape, shape_id);
      define_name(cls.name, cls_type);
