    if (current->kind == Type::Kind::Matrix && current->list_element && next->list_element) {
      return matrix_type(normalize_list_elements(current->list_element, next->list_element),
                         std::max(current->matrix_rows, next->matrix_rows),
                         (current->matrix_cols == next->matrix_cols) ? current->matrix_cols : 0);
    }
    if (current->kind == Type::Kind::Class && current->class_name == next->class_name) {
      return current;
    }
    return current;
  }
  return any_type();
}

void TypeChecker::push_function_context(const std::string& name) {
  function_stack_.push_back(FunctionContext{.name = name});
}

void TypeChecker::pop_function_context() {
  if (function_stack_.empty()) {
    return;
  }

  auto context = function_stack_.back();
  function_stack_.pop_back();

  TierRecord record;
  record.name = context.name;
  record.kind = "function";
  record.parent = function_stack_.empty() ? "" : function_stack_.back().name;
  record.tier = fold_tier(context.reasons);
  record.reasons.reserve(context.reasons.size());
  for (const auto& reason : context.reasons) {
    record.reasons.push_back(reason.message);
  }
  function_reports_.push_back(record);
}

void TypeChecker::push_loop_context(const std::string& owner, const std::string& kind) {
  const std::string label = owner.empty() ? kind : owner + "/" + kind + "#" + std::to_string(loop_reports_.size() + 1);
  loop_stack_.push_back(LoopContext{.id = label});
}

void TypeChecker::pop_loop_context() {
  if (loop_stack_.empty()) {
    return;
  }

  auto context = loop_stack_.back();
  loop_stack_.pop_back();

  TierRecord record;
  record.name = context.id;
  record.kind = "loop";
  if (function_stack_.empty()) {
    record.parent = "";
  } else {
    record.parent = function_stack_.back().name;
  }
  record.tier = fold_tier(context.reasons);
  record.reasons.reserve(context.reasons.size());
  for (const auto& reason : context.reasons) {
    record.reasons.push_back(reason.message);
  }
  loop_reports_.push_back(record);
}

void TypeChecker::add_context_reason(const TierReason& reason) {
  if (reason.message.empty()) {
    return;
  }
  if (!loop_stack_.empty()) {
    loop_stack_.back().reasons.push_back(reason);
  }
  if (!function_stack_.empty()) {
    function_stack_.back().reasons.push_back(reason);
  }
}

TierLevel TypeChecker::fold_tier(const std::vector<TierReason>& reasons) const {
  bool has_hard = false;
  bool has_soft = false;
  for (const auto& reason : reasons) {
    if (reason.normalizable) {
      has_soft = true;
      continue;
    }
    has_hard = true;
  }
  if (has_hard) {
    return TierLevel::T8;
  }
  if (has_soft) {
    return TierLevel::T5;
  }
  return TierLevel::T4;
}

void TypeChecker::check_program_body(const StmtList& body) {
  for (const auto& stmt : body) {
    check_stmt(*stmt);
  }
}

bool TypeChecker::check_class_layout(const ClassDefStmt& cls, TypePtr class_type) {
  ShapeRecord shape;
  shape.name = cls.name;
  shape.shape_id = "shape_" + std::to_string(class_type->class_shape_id);
  shape.open = cls.open_shape;
  if (shape.open) {
    shape.reasons.push_back("explicit open class declaration");
  } else {
    shape.reasons.push_back("slots-style class declaration");
  }
  bool has_mutation = false;

  for (const auto& stmt : cls.body) {
    if (stmt->kind == Stmt::Kind::Assign) {
      const auto& assign = static_cast<const AssignStmt&>(*stmt);
      const auto field_name = target_to_name_or_expr(*assign.target);
      if (field_name != "<expr>") {
        shape.fields.push_back(field_name);
      }
    }
    if (stmt->kind == Stmt::Kind::Expression) {
      const auto& expr = static_cast<const ExpressionStmt&>(*stmt);
      if (expr.expression && expr.expression->kind == Expr::Kind::Call) {
        has_mutation = true;
      }
    }
  }

  std::sort(shape.fields.begin(), shape.fields.end());
  shape.fields.erase(std::unique(shape.fields.begin(), shape.fields.end()), shape.fields.end());
  shapes_.push_back(shape);
  if (shape.open && has_mutation) {
    return false;
  }
  return true;
}

TypePtr TypeChecker::infer_index_access_type(const IndexExpr& index, bool in_assignment) {
  auto target = infer_expr(*index.target);
  if (target->kind == Type::Kind::Unknown || target->kind == Type::Kind::Error) {
    return target;
  }

  TypePtr current = target;
  for (std::size_t i = 0; i < index.indices.size(); ++i) {
    const auto& item = index.indices[i];
    const bool last = i + 1 == index.indices.size();

    if (item.is_slice) {
      if (in_assignment && !last) {
        add_error("cannot use slice in non-final indexed assignment target");
        add_context_reason({.message = "slice in assignment target", .normalizable = false});
        return error_type();
      }
      if (in_assignment) {
        add_error("slice assignment is not supported");
        add_context_reason({.message = "slice assignment unsupported", .normalizable = false});
        return error_type();
      }

      if (item.slice_start) {
        auto start_type = infer_expr(*item.slice_start);
        analyze_expr(*item.slice_start, start_type);
        if (!is_numeric_type(*start_type) && start_type->kind != Type::Kind::Unknown) {
          add_error("slice bounds must be integers");
          add_context_reason({.message = "invalid slice bound type", .normalizable = false});
        }
      }
      if (item.slice_stop) {
        auto stop_type = infer_expr(*item.slice_stop);
        analyze_expr(*item.slice_stop, stop_type);
        if (!is_numeric_type(*stop_type) && stop_type->kind != Type::Kind::Unknown) {
          add_error("slice bounds must be integers");
          add_context_reason({.message = "invalid slice bound type", .normalizable = false});
        }
      }
      if (item.slice_step) {
        auto step_type = infer_expr(*item.slice_step);
        analyze_expr(*item.slice_step, step_type);
        if (!is_numeric_type(*step_type) && step_type->kind != Type::Kind::Unknown) {
          add_error("slice step must be integer");
          add_context_reason({.message = "invalid slice step", .normalizable = false});
        }
      }

      if (current->kind == Type::Kind::List) {
        current = list_type(current->list_element ? current->list_element : any_type());
        continue;
      }
      if (current->kind == Type::Kind::Matrix) {
        const auto element = current->list_element ? current->list_element : any_type();
        current = list_type(list_type(element));
        continue;
      }
      if (current->kind == Type::Kind::String) {
        current = string_type();
        continue;
      }
      add_error("invalid sliced target type");
      add_context_reason({.message = "cannot slice non-container", .normalizable = false});
      return error_type();
    }

    if (!item.index) {
      add_error("malformed index expression");
      return error_type();
    }

    const auto index_type = infer_expr(*item.index);
    analyze_expr(*item.index, index_type);
    if (!is_numeric_type(*index_type) && index_type->kind != Type::Kind::Unknown) {
      add_error("index must be integer");
      add_context_reason({.message = "invalid index type", .normalizable = false});
      return error_type();
    }

    if (current->kind == Type::Kind::List) {
      current = current->list_element ? current->list_element : unknown_type();
      continue;
    }
    if (current->kind == Type::Kind::Matrix) {
      current = list_type(current->list_element ? current->list_element : any_type());
      continue;
    }
    if (current->kind == Type::Kind::String) {
      current = string_type();
      continue;
    }

    add_error("cannot index non-container type");
    add_context_reason({.message = "indexing on non-container", .normalizable = false});
    return error_type();
  }

  if (current->kind == Type::Kind::Unknown && index.indices.size() == 1 && in_assignment) {
    return unknown_type();
  }
  if (index.indices.empty()) {
    return error_type();
  }

  return current;
}

TypePtr TypeChecker::infer_lvalue_type(const Expr& target) {
  if (const auto* variable = as_variable_target(target)) {
    if (!has_name(variable->name)) {
      add_error("cannot assign to undefined name: " + variable->name);
      return error_type();
    }
    return get_name(variable->name);
  }

  if (target.kind == Expr::Kind::Index) {
    return infer_index_access_type(static_cast<const IndexExpr&>(target), true);
  }

  add_error("invalid assignment target");
  return error_type();
}

void TypeChecker::check_function_body(const FunctionDefStmt& fn, TypePtr function_type) {
  push_scope("function:" + fn.name);
  push_function_context(fn.name);

  for (std::size_t i = 0; i < fn.params.size(); ++i) {
    const auto& param_name = fn.params[i];
    if (i < function_type->function_params.size()) {
      const auto& param_type = function_type->function_params[i];
      define_name(param_name, param_type);
    } else {
      define_name(param_name, unknown_type());
    }
  }
  check_program_body(fn.body);

  if (fn.is_async) {
    const auto count_awaits_in_expr = [](const Expr& expr, const auto& self) -> std::size_t {
      switch (expr.kind) {
        case Expr::Kind::Unary: {
          const auto& unary = static_cast<const UnaryExpr&>(expr);
          std::size_t out = unary.op == UnaryOp::Await ? 1 : 0;
          if (unary.operand) {
            out += self(*unary.operand, self);
          }
          return out;
        }
        case Expr::Kind::Binary: {
          const auto& binary = static_cast<const BinaryExpr&>(expr);
          std::size_t out = 0;
          if (binary.left) {
            out += self(*binary.left, self);
          }
          if (binary.right) {
            out += self(*binary.right, self);
          }
          return out;
        }
        case Expr::Kind::Call: {
          const auto& call = static_cast<const CallExpr&>(expr);
          std::size_t out = 0;
          if (call.callee) {
            out += self(*call.callee, self);
          }
          for (const auto& arg : call.args) {
            out += self(*arg, self);
          }
          return out;
        }
        case Expr::Kind::Attribute: {
          const auto& attr = static_cast<const AttributeExpr&>(expr);
          return attr.target ? self(*attr.target, self) : 0;
        }
        case Expr::Kind::Index: {
          const auto& index = static_cast<const IndexExpr&>(expr);
          std::size_t out = 0;
          if (index.target) {
            out += self(*index.target, self);
          }
          for (const auto& item : index.indices) {
            if (item.index) out += self(*item.index, self);
            if (item.slice_start) out += self(*item.slice_start, self);
            if (item.slice_stop) out += self(*item.slice_stop, self);
            if (item.slice_step) out += self(*item.slice_step, self);
          }
          return out;
        }
        case Expr::Kind::List: {
          const auto& list = static_cast<const ListExpr&>(expr);
          std::size_t out = 0;
          for (const auto& item : list.elements) {
            out += self(*item, self);
          }
          return out;
        }
        default:
          return 0;
      }
    };

    const auto count_awaits_in_stmt = [&](const Stmt& stmt, const auto& self_stmt) -> std::size_t {
      switch (stmt.kind) {
        case Stmt::Kind::Expression: {
          const auto& s = static_cast<const ExpressionStmt&>(stmt);
          return s.expression ? count_awaits_in_expr(*s.expression, count_awaits_in_expr) : 0;
        }
        case Stmt::Kind::Assign: {
          const auto& s = static_cast<const AssignStmt&>(stmt);
          std::size_t out = 0;
          if (s.target) out += count_awaits_in_expr(*s.target, count_awaits_in_expr);
          if (s.value) out += count_awaits_in_expr(*s.value, count_awaits_in_expr);
          return out;
        }
        case Stmt::Kind::Return: {
          const auto& s = static_cast<const ReturnStmt&>(stmt);
          return s.value ? count_awaits_in_expr(*s.value, count_awaits_in_expr) : 0;
        }
        case Stmt::Kind::If: {
          const auto& s = static_cast<const IfStmt&>(stmt);
          std::size_t out = s.condition ? count_awaits_in_expr(*s.condition, count_awaits_in_expr) : 0;
          for (const auto& child : s.then_body) out += self_stmt(*child, self_stmt);
          for (const auto& branch : s.elif_branches) {
            out += count_awaits_in_expr(*branch.first, count_awaits_in_expr);
            for (const auto& child : branch.second) out += self_stmt(*child, self_stmt);
          }
          for (const auto& child : s.else_body) out += self_stmt(*child, self_stmt);
          return out;
        }
        case Stmt::Kind::While: {
          const auto& s = static_cast<const WhileStmt&>(stmt);
          std::size_t out = s.condition ? count_awaits_in_expr(*s.condition, count_awaits_in_expr) : 0;
          for (const auto& child : s.body) out += self_stmt(*child, self_stmt);
          return out;
        }
        case Stmt::Kind::For: {
          const auto& s = static_cast<const ForStmt&>(stmt);
          std::size_t out = s.iterable ? count_awaits_in_expr(*s.iterable, count_awaits_in_expr) : 0;
          for (const auto& child : s.body) out += self_stmt(*child, self_stmt);
          return out;
        }
        case Stmt::Kind::WithTaskGroup: {
          const auto& s = static_cast<const WithTaskGroupStmt&>(stmt);
          std::size_t out = s.timeout_ms ? count_awaits_in_expr(*s.timeout_ms, count_awaits_in_expr) : 0;
          for (const auto& child : s.body) out += self_stmt(*child, self_stmt);
          return out;
        }
        case Stmt::Kind::FunctionDef:
        case Stmt::Kind::ClassDef:
          return 0;
      }
      return 0;
    };

    std::size_t await_points = 0;
    for (const auto& stmt : fn.body) {
      await_points += count_awaits_in_stmt(*stmt, count_awaits_in_stmt);
    }
    AsyncLoweringRecord record;
    record.function_name = fn.name;
    record.await_points = await_points;
    record.states = await_points + 1;
    record.heap_frame = await_points > 0 || fn.body.size() > 8;
    async_lowerings_.push_back(std::move(record));
  }

  pop_function_context();
  pop_scope();
}

void TypeChecker::check_class_body(const ClassDefStmt& cls, TypePtr class_ty) {
  push_scope("class:" + cls.name);
  define_name("__shape_open__", bool_type());
  check_class_layout(cls, class_ty);
  check_program_body(cls.body);
  pop_scope();
}

void TypeChecker::analyze_expr(const Expr& expr, const TypePtr& type) {
  if (!type || type->kind == Type::Kind::Error) {
    return;
  }
  if (type->kind == Type::Kind::Any) {
    add_context_reason({.message = "type has Any and can require normalization", .normalizable = true});
    (void)expr;
    return;
  }
  if (type->kind == Type::Kind::List) {
    if (!type->list_element) {
      return;
    }
    if (type->list_element->kind == Type::Kind::Any) {
      add_context_reason(
          {.message = "list element type is Any; normalization may be required", .normalizable = true});
    }
  }
  if (type->kind == Type::Kind::Matrix && type->list_element &&
      type->list_element->kind == Type::Kind::Any) {
    add_context_reason(
        {.message = "matrix element type is Any; normalize needed before T4 lowering", .normalizable = true});
  }
  (void)expr;
}

TypePtr TypeChecker::check_call(const CallExpr& call) {
  auto callee_expr = infer_expr(*call.callee);
  const auto is_sendable_for_phase9 = [](const Type& type) {
    switch (type.kind) {
      case Type::Kind::Nil:
      case Type::Kind::Int:
      case Type::Kind::Float:
      case Type::Kind::String:
      case Type::Kind::Bool:
      case Type::Kind::Task:
      case Type::Kind::Channel:
        return true;
      case Type::Kind::Unknown:
      case Type::Kind::Any:
        return true;
      default:
        return false;
    }
  };
  const auto report_non_sendable_capture = [&](const std::string& site, std::size_t arg_index,
                                               const TypePtr& arg_type) {
    add_error(site + " capture arg " + std::to_string(arg_index + 1) +
              " is not Sendable/Shareable: " + type_to_string(*arg_type));
    add_context_reason({.message = "Cannot lower to phase9 fast path: non-sendable capture (" + site + ")",
                        .normalizable = false});
  };

  if (call.callee->kind == Expr::Kind::Attribute) {
    const auto& attribute = static_cast<const AttributeExpr&>(*call.callee);

    const auto target_is_variable = attribute.target && attribute.target->kind == Expr::Kind::Variable;
    const auto target_name = target_is_variable
                                 ? static_cast<const VariableExpr&>(*attribute.target).name
                                 : "";
    auto target_type = target_is_variable ? get_name(target_name) : infer_expr(*attribute.target);

    const auto normalize_pipeline_terminal = [](const std::string& terminal) {
      if (terminal == "sum") {
        return std::string("reduce_sum");
      }
      if (terminal == "collect") {
        return std::string("to_list");
      }
      return terminal;
    };

    const auto record_pipeline_terminal = [&](const std::string& terminal_name) {
      PipelineRecord record;
      record.id = "pipeline_" + std::to_string(pipelines_.size() + 1);
      record.terminal = normalize_pipeline_terminal(terminal_name);
      record.receiver_type = type_to_string(*target_type);

      const Expr* cursor = &call;
      while (cursor && cursor->kind == Expr::Kind::Call) {
        const auto& call_expr = static_cast<const CallExpr&>(*cursor);
        if (!call_expr.callee || call_expr.callee->kind != Expr::Kind::Attribute) {
          break;
        }
        const auto& node_attr = static_cast<const AttributeExpr&>(*call_expr.callee);
        record.nodes.push_back(node_attr.attribute);
        cursor = node_attr.target.get();
      }
      std::reverse(record.nodes.begin(), record.nodes.end());
      record.receiver = (cursor && cursor->kind == Expr::Kind::Variable)
                            ? static_cast<const VariableExpr&>(*cursor).name
                            : "<expr>";

      auto normalized_nodes = std::vector<std::string>{};
      normalized_nodes.reserve(record.nodes.size());
      for (const auto& node : record.nodes) {
        normalized_nodes.push_back(normalize_pipeline_terminal(node));
      }
      record.nodes = std::move(normalized_nodes);

      const std::unordered_set<std::string> allowed = {
          "map_add", "map_mul", "filter_gt", "filter_lt", "filter_nonzero",
          "zip_add", "reduce_sum", "scan_sum", "to_list"};
      const std::unordered_set<std::string> mutating = {"append", "pop", "insert", "remove"};

      bool has_unsupported = false;
      bool has_mutation = false;
      for (const auto& node : record.nodes) {
        if (mutating.count(node) > 0) {
          has_mutation = true;
        }
        if (allowed.count(node) == 0) {
          has_unsupported = true;
        }
      }

      record.fused = true;
      if (target_type->kind != Type::Kind::List && target_type->kind != Type::Kind::Matrix) {
        record.fused = false;
        record.reasons.push_back("receiver is not list/matrix");
      }
      if (has_mutation) {
        record.fused = false;
        record.reasons.push_back("pipeline contains mutating stage");
      }
      if (has_unsupported) {
        record.fused = false;
        record.reasons.push_back("pipeline contains unsupported stage");
      }
      if (target_type->kind == Type::Kind::List && target_type->list_element &&
          (target_type->list_element->kind == Type::Kind::Any ||
           target_type->list_element->kind == Type::Kind::Unknown)) {
        record.materialize_required = true;
        record.reasons.push_back("list element type is Any/Unknown; normalize required");
      }
      if (target_type->kind == Type::Kind::Matrix && target_type->list_element &&
          (target_type->list_element->kind == Type::Kind::Any ||
           target_type->list_element->kind == Type::Kind::Unknown)) {
        record.materialize_required = true;
        record.reasons.push_back("matrix element type is Any/Unknown; normalize required");
      }
      if (record.reasons.empty()) {
        record.reasons.push_back("eligible for fused lowering");
      }

      pipelines_.push_back(std::move(record));
    };

    if (attribute.attribute == "append") {
      if (!target_is_variable) {
        add_error("append() method target must be a variable");
        return unknown_type();
      }
      if (target_type->kind != Type::Kind::List) {
        add_error("append() expects a list receiver");
        return unknown_type();
      }
      if (call.args.empty()) {
        add_error("append() expects exactly one argument");
        return unknown_type();
      }
      if (call.args.size() > 1) {
        add_error("append() expects exactly one argument");
      }

      auto arg_type = infer_expr(*call.args[0]);
      analyze_expr(*call.args[0], arg_type);
      auto current = target_type->list_element ? target_type->list_element : any_type();
      auto next = normalize_list_elements(current, arg_type);
      const bool became_any = current->kind != Type::Kind::Any && next->kind == Type::Kind::Any;
      if (became_any || !is_assignable(*next, *current)) {
        const bool normalizable = !became_any && is_numeric_widen_candidate(*current, *next);
        add_context_reason({.message = "cannot append value into typed list with incompatible element",
                            .normalizable = normalizable});
      }
      if (current->kind == Type::Kind::Unknown || !same_or_unknown(*next, *current)) {
        auto updated = list_type(next);
        set_name(target_name, updated);
      } else {
        set_name(target_name, list_type(current));
      }
      add_context_reason({.message = "mutating container via append", .normalizable = true});
      return nil_type();
    }

    if (attribute.attribute == "pop" || attribute.attribute == "insert" || attribute.attribute == "remove") {
      if (!target_is_variable) {
        add_error(attribute.attribute + "() method target must be a variable");
        return unknown_type();
      }
      if (target_type->kind != Type::Kind::List) {
        add_error(attribute.attribute + "() expects a list receiver");
        return unknown_type();
      }

      if (attribute.attribute == "insert") {
        if (call.args.size() != 2) {
          add_error("insert() expects exactly two arguments");
          return nil_type();
        }
        auto index_type = infer_expr(*call.args[0]);
        analyze_expr(*call.args[0], index_type);
        if (index_type->kind != Type::Kind::Int && index_type->kind != Type::Kind::Unknown) {
          add_error("insert() first argument expects an integer index");
          add_context_reason({.message = "insert() index argument is not integer", .normalizable = false});
        }

        auto value_type = infer_expr(*call.args[1]);
        analyze_expr(*call.args[1], value_type);
        auto current = target_type->list_element ? target_type->list_element : unknown_type();
        auto next = normalize_list_elements(current, value_type);
        if (!same_or_unknown(*next, *current)) {
          const bool normalizable =
              is_numeric_type(*current) && is_numeric_type(*next) && is_numeric_type(*value_type);
          add_context_reason({.message = "cannot insert value into typed list with incompatible element",
                              .normalizable = normalizable});
        }
        if (current->kind == Type::Kind::Unknown || !same_or_unknown(*next, *current)) {
          set_name(target_name, list_type(next));
        }

      } else if (attribute.attribute == "remove") {
        if (call.args.size() != 1) {
          add_error("remove() expects exactly one argument");
          return nil_type();
        }
      } else if (attribute.attribute == "pop") {
        if (call.args.size() > 1) {
          add_error("pop() expects at most one argument");
        }
        if (call.args.size() == 1) {
          auto index_type = infer_expr(*call.args[0]);
          analyze_expr(*call.args[0], index_type);
          if (index_type->kind != Type::Kind::Int && index_type->kind != Type::Kind::Unknown) {
            add_error("pop() index must be integer");
            add_context_reason({.message = "pop() index argument is not integer", .normalizable = false});
          }
        }
        return target_type->list_element ? target_type->list_element : unknown_type();
      }
      add_context_reason({.message = std::string("mutating container via ") + attribute.attribute,
                          .normalizable = true});
      return nil_type();
    }

    if (attribute.attribute == "reduce_sum" || attribute.attribute == "sum") {
      record_pipeline_terminal(attribute.attribute);
      if (target_type->kind != Type::Kind::List && target_type->kind != Type::Kind::Matrix) {
        add_error("reduce_sum() expects a list or matrix receiver");
        return unknown_type();
      }
      if (!call.args.empty()) {
        add_error("reduce_sum() expects no arguments");
      }
      if (target_type->list_element &&
          (target_type->list_element->kind == Type::Kind::Any ||
           target_type->list_element->kind == Type::Kind::Unknown)) {
        add_context_reason({.message = "reduce_sum() on hetero/unknown container triggers normalize+cache",
                            .normalizable = true});
      } else {
        add_context_reason({.message = "reduce_sum() can run on packed kernel path", .normalizable = true});
      }
      if (target_type->list_element && target_type->list_element->kind == Type::Kind::Int) {
        return int_type();
      }
      return float_type(Type::FloatKind::F64);
    }

    if (attribute.attribute == "map_add") {
      if (target_type->kind != Type::Kind::List && target_type->kind != Type::Kind::Matrix) {
        add_error("map_add() expects a list/matrix receiver");
        return unknown_type();
      }
      if (call.args.size() != 1) {
        add_error("map_add() expects exactly one numeric argument");
        if (target_type->kind == Type::Kind::Matrix) {
          return matrix_type(float_type(Type::FloatKind::F64), target_type->matrix_rows, target_type->matrix_cols);
        }
        return list_type(float_type(Type::FloatKind::F64));
      }
      auto delta_type = infer_expr(*call.args[0]);
      analyze_expr(*call.args[0], delta_type);
      if (!is_numeric_type(*delta_type) && delta_type->kind != Type::Kind::Unknown) {
        add_error("map_add() argument must be numeric");
        add_context_reason({.message = "map_add() scalar argument is non-numeric", .normalizable = false});
      }
      if (target_type->list_element &&
          (target_type->list_element->kind == Type::Kind::Any ||
           target_type->list_element->kind == Type::Kind::Unknown)) {
        add_context_reason({.message = "map_add() on hetero/unknown container may normalize before fusion",
                            .normalizable = true});
      }
      if (target_type->kind == Type::Kind::Matrix) {
        return matrix_type(float_type(Type::FloatKind::F64), target_type->matrix_rows, target_type->matrix_cols);
      }
      return list_type(float_type(Type::FloatKind::F64));
    }

    if (attribute.attribute == "map_mul") {
      if (target_type->kind != Type::Kind::List && target_type->kind != Type::Kind::Matrix) {
        add_error("map_mul() expects a list/matrix receiver");
        return unknown_type();
      }
      if (call.args.size() != 1) {
        add_error("map_mul() expects exactly one numeric argument");
        if (target_type->kind == Type::Kind::Matrix) {
          return matrix_type(float_type(Type::FloatKind::F64), target_type->matrix_rows, target_type->matrix_cols);
        }
        return list_type(float_type(Type::FloatKind::F64));
      }
      auto scalar_type = infer_expr(*call.args[0]);
      analyze_expr(*call.args[0], scalar_type);
      if (!is_numeric_type(*scalar_type) && scalar_type->kind != Type::Kind::Unknown) {
        add_error("map_mul() argument must be numeric");
        add_context_reason({.message = "map_mul() scalar argument is non-numeric", .normalizable = false});
      }
      if (target_type->kind == Type::Kind::Matrix) {
        return matrix_type(float_type(Type::FloatKind::F64), target_type->matrix_rows, target_type->matrix_cols);
      }
      return list_type(float_type(Type::FloatKind::F64));
    }

    if (attribute.attribute == "filter_gt" || attribute.attribute == "filter_lt") {
      if (target_type->kind != Type::Kind::List) {
        add_error(attribute.attribute + "() expects a list receiver");
        return unknown_type();
      }
      if (call.args.size() != 1) {
        add_error(attribute.attribute + "() expects exactly one numeric argument");
        return list_type(float_type(Type::FloatKind::F64));
      }
      auto scalar_type = infer_expr(*call.args[0]);
      analyze_expr(*call.args[0], scalar_type);
      if (!is_numeric_type(*scalar_type) && scalar_type->kind != Type::Kind::Unknown) {
        add_error(attribute.attribute + "() argument must be numeric");
      }
      return list_type(float_type(Type::FloatKind::F64));
    }

    if (attribute.attribute == "filter_nonzero") {
      if (target_type->kind != Type::Kind::List) {
        add_error("filter_nonzero() expects a list receiver");
        return unknown_type();
      }
      if (!call.args.empty()) {
        add_error("filter_nonzero() expects no arguments");
      }
      return list_type(float_type(Type::FloatKind::F64));
    }

    if (attribute.attribute == "zip_add") {
      if (target_type->kind != Type::Kind::List) {
        add_error("zip_add() expects a list receiver");
        return unknown_type();
      }
      if (call.args.size() != 1) {
        add_error("zip_add() expects exactly one list argument");
        return list_type(float_type(Type::FloatKind::F64));
      }
      auto rhs_type = infer_expr(*call.args[0]);
      analyze_expr(*call.args[0], rhs_type);
      if (rhs_type->kind != Type::Kind::List && rhs_type->kind != Type::Kind::Unknown) {
        add_error("zip_add() argument must be list");
      }
      return list_type(float_type(Type::FloatKind::F64));
    }

    if (attribute.attribute == "scan_sum") {
      record_pipeline_terminal(attribute.attribute);
      if (target_type->kind != Type::Kind::List) {
        add_error("scan_sum() expects a list receiver");
        return unknown_type();
      }
      if (!call.args.empty()) {
        add_error("scan_sum() expects no arguments");
      }
      return list_type(float_type(Type::FloatKind::F64));
    }

    if (attribute.attribute == "to_list" || attribute.attribute == "collect") {
      record_pipeline_terminal(attribute.attribute);
      if (!call.args.empty()) {
        add_error(attribute.attribute + "() expects no arguments");
      }
      if (target_type->kind == Type::Kind::List) {
        return target_type;
      }
      if (target_type->kind == Type::Kind::Matrix) {
        return target_type;
      }
      return unknown_type();
    }

    if (attribute.attribute == "matmul" || attribute.attribute == "matmul_f32" ||
        attribute.attribute == "matmul_f64" || attribute.attribute == "matmul_add" ||
        attribute.attribute == "matmul_axpby") {
      if (target_type->kind != Type::Kind::Matrix) {
        add_error(attribute.attribute + "() expects matrix receiver");
        return unknown_type();
      }

      const bool is_axpby = attribute.attribute == "matmul_axpby";
      const bool is_add = attribute.attribute == "matmul_add";
      const std::size_t expected_args = is_axpby ? 4 : (is_add ? 2 : 1);
      if (call.args.size() != expected_args) {
        add_error(attribute.attribute + "() expects exactly " + std::to_string(expected_args) + " arguments");
      }

      TypePtr rhs_type = unknown_type();
      if (!call.args.empty()) {
        rhs_type = infer_expr(*call.args[0]);
        analyze_expr(*call.args[0], rhs_type);
        if (rhs_type->kind != Type::Kind::Matrix && rhs_type->kind != Type::Kind::Unknown) {
          add_error(attribute.attribute + "() first argument must be matrix");
        }
      }

      if (is_add && call.args.size() >= 2) {
        auto bias_type = infer_expr(*call.args[1]);
        analyze_expr(*call.args[1], bias_type);
        const bool bias_ok = bias_type->kind == Type::Kind::Matrix || bias_type->kind == Type::Kind::List ||
                             is_numeric_type(*bias_type) || bias_type->kind == Type::Kind::Unknown;
        if (!bias_ok) {
          add_error("matmul_add() bias must be scalar, list, or matrix");
        }
      }

      if (is_axpby) {
        if (call.args.size() >= 2) {
          auto alpha_type = infer_expr(*call.args[1]);
          analyze_expr(*call.args[1], alpha_type);
          if (!is_numeric_type(*alpha_type) && alpha_type->kind != Type::Kind::Unknown) {
            add_error("matmul_axpby() alpha must be numeric");
          }
        }
        if (call.args.size() >= 3) {
          auto beta_type = infer_expr(*call.args[2]);
          analyze_expr(*call.args[2], beta_type);
          if (!is_numeric_type(*beta_type) && beta_type->kind != Type::Kind::Unknown) {
            add_error("matmul_axpby() beta must be numeric");
          }
        }
        if (call.args.size() >= 4) {
          auto accum_type = infer_expr(*call.args[3]);
          analyze_expr(*call.args[3], accum_type);
          if (accum_type->kind != Type::Kind::Matrix && accum_type->kind != Type::Kind::Unknown) {
            add_error("matmul_axpby() accumulator must be matrix");
          }
        }
      }

      const auto lhs_rows = target_type->matrix_rows;
      const auto lhs_cols = target_type->matrix_cols;
      auto rhs_rows = rhs_type->matrix_rows;
      auto rhs_cols = rhs_type->matrix_cols;
      if (rhs_type->kind != Type::Kind::Matrix) {
        rhs_rows = 0;
        rhs_cols = 0;
      }
      if (lhs_cols != 0 && rhs_rows != 0 && lhs_cols != rhs_rows) {
        add_error(attribute.attribute + "() shape mismatch: lhs.cols != rhs.rows");
        add_context_reason({.message = "matmul shape mismatch blocks perf-kernel lowering",
                            .normalizable = false});
      }

      if (target_type->list_element &&
          (target_type->list_element->kind == Type::Kind::Any ||
           target_type->list_element->kind == Type::Kind::Unknown)) {
        add_context_reason({.message = "matmul receiver element type not fully stabilized; normalize required",
                            .normalizable = true});
      } else {
        add_context_reason({.message = "matmul eligible for phase8 kernel scheduling path",
                            .normalizable = true});
      }

      Type::FloatKind fk = Type::FloatKind::F64;
      if (attribute.attribute == "matmul_f32") {
        fk = Type::FloatKind::F32;
      }
      return matrix_type(float_type(fk), lhs_rows, rhs_cols);
    }

    if (attribute.attribute == "matmul_stats" || attribute.attribute == "matmul_schedule") {
      if (target_type->kind != Type::Kind::Matrix) {
        add_error(attribute.attribute + "() expects matrix receiver");
        return unknown_type();
      }
      if (!call.args.empty()) {
        add_error(attribute.attribute + "() expects no arguments");
      }
      return list_type(int_type());
    }

    if (attribute.attribute == "spawn" || attribute.attribute == "join_all" ||
        attribute.attribute == "cancel_all") {
      if (target_type->kind != Type::Kind::TaskGroup) {
        add_error(attribute.attribute + "() expects task_group receiver");
        return unknown_type();
      }
      if (attribute.attribute == "spawn") {
        if (call.args.empty()) {
          add_error("spawn() expects callable first argument");
          return unknown_type();
        }
        if (call.args[0]->kind != Expr::Kind::Variable) {
          add_error("spawn() callable must be named function for sendable analysis");
          add_context_reason({.message = "Cannot lower spawn to T4: non-sendable capture candidate",
                              .normalizable = false});
        } else {
          add_context_reason({.message = "task_group.spawn() eligible for phase9 scheduler enqueue path",
                              .normalizable = true});
        }
        for (std::size_t i = 1; i < call.args.size(); ++i) {
          const auto arg_type = infer_expr(*call.args[i]);
          analyze_expr(*call.args[i], arg_type);
          if (arg_type->kind != Type::Kind::Error && !is_sendable_for_phase9(*arg_type)) {
            report_non_sendable_capture("task_group.spawn()", i, arg_type);
          }
        }
        return task_type(any_type());
      }
      if (!call.args.empty()) {
        add_error(attribute.attribute + "() expects no arguments");
      }
      add_context_reason({.message = "structured task_group lifecycle operation (" + attribute.attribute + ")",
                          .normalizable = true});
      if (attribute.attribute == "join_all") {
        return list_type(any_type());
      }
      return nil_type();
    }

    if (attribute.attribute == "join" || attribute.attribute == "cancel") {
      if (target_type->kind != Type::Kind::Task) {
        add_error(attribute.attribute + "() expects task receiver");
        return unknown_type();
      }
      if (!call.args.empty()) {
        add_error(attribute.attribute + "() expects no arguments");
      }
      if (attribute.attribute == "join") {
        add_context_reason({.message = "task.join() resumes async result in phase9 runtime",
                            .normalizable = true});
        return target_type->task_result ? target_type->task_result : any_type();
      }
      add_context_reason({.message = "task.cancel() uses cooperative cancellation token",
                          .normalizable = true});
      return nil_type();
    }

    if (attribute.attribute == "send" || attribute.attribute == "recv" ||
        attribute.attribute == "close" || attribute.attribute == "stats" ||
        attribute.attribute == "anext" || attribute.attribute == "next" ||
        attribute.attribute == "has_next") {
      if (target_type->kind != Type::Kind::Channel) {
        add_error(attribute.attribute + "() expects channel receiver");
        return unknown_type();
      }
      if (attribute.attribute == "send") {
        if (call.args.size() != 1) {
          add_error("send() expects exactly one message argument");
        }
        add_context_reason({.message = "channel send path uses bounded/unbounded queue with backpressure",
                            .normalizable = true});
        return nil_type();
      }
      if (attribute.attribute == "recv") {
        if (call.args.size() > 1) {
          add_error("recv() expects zero or one timeout argument");
        }
        add_context_reason({.message = "channel recv path integrates with phase9 async wait semantics",
                            .normalizable = true});
        return target_type->channel_element ? target_type->channel_element : any_type();
      }
      if (attribute.attribute == "anext" || attribute.attribute == "next") {
        if (!call.args.empty()) {
          add_error(attribute.attribute + "() expects no arguments");
        }
        add_context_reason({.message = "stream-style channel consumption path",
                            .normalizable = true});
        return target_type->channel_element ? target_type->channel_element : any_type();
      }
      if (attribute.attribute == "close") {
        if (!call.args.empty()) {
          add_error("close() expects no arguments");
        }
        return nil_type();
      }
      if (attribute.attribute == "has_next") {
        if (!call.args.empty()) {
          add_error("has_next() expects no arguments");
        }
        return bool_type();
      }
      if (!call.args.empty()) {
        add_error("stats() expects no arguments");
      }
      return list_type(int_type());
    }

    if (attribute.attribute == "plan_id" || attribute.attribute == "cache_stats" ||
        attribute.attribute == "cache_bytes" || attribute.attribute == "pipeline_stats" ||
        attribute.attribute == "pipeline_plan_id") {
      if (!target_is_variable) {
        add_error(attribute.attribute + "() method target must be a variable");
        return unknown_type();
      }
      if (target_type->kind != Type::Kind::List && target_type->kind != Type::Kind::Matrix) {
        add_error(attribute.attribute + "() expects a list or matrix receiver");
        return unknown_type();
      }
      if (!call.args.empty()) {
        add_error(attribute.attribute + "() expects no arguments");
      }
      if (attribute.attribute == "plan_id" || attribute.attribute == "cache_bytes" ||
          attribute.attribute == "pipeline_plan_id") {
        return int_type();
      }
      return list_type(int_type());
    }
  }

  if (callee_expr->kind == Type::Kind::Function || callee_expr->kind == Type::Kind::Builtin) {
    const auto arity_min = callee_expr->arity_min;
    const auto arity_max = std::max(callee_expr->arity_max, callee_expr->function_params.size());
    if (call.args.size() > arity_max) {
      add_error("function called with too many arguments");
      add_context_reason({.message = "function call arity mismatch", .normalizable = false});
      return callee_expr->function_return ? callee_expr->function_return : unknown_type();
