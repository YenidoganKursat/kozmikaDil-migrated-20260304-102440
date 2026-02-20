      check_class_body(cls, cls_type);
      break;
    }
  }
}

TypePtr TypeChecker::infer_expr(const Expr& expr) {
  switch (expr.kind) {
    case Expr::Kind::Number: {
      const auto& number = static_cast<const NumberExpr&>(expr);
      return number.is_int ? int_type() : float_type(Type::FloatKind::F64);
    }
    case Expr::Kind::String:
      return string_type();
    case Expr::Kind::Bool:
      return bool_type();
    case Expr::Kind::Variable: {
      const auto& variable = static_cast<const VariableExpr&>(expr);
      if (!has_name(variable.name)) {
        add_error("undefined variable: " + variable.name);
        return error_type();
      }
      return get_name(variable.name);
    }
    case Expr::Kind::Attribute: {
      const auto& attribute = static_cast<const AttributeExpr&>(expr);
      const auto target_type = infer_expr(*attribute.target);
      if (attribute.attribute == "append") {
        if (target_type->kind != Type::Kind::List) {
          add_error("'.append' only supported on list-like values");
          return error_type();
        }
        if (target_type->list_element) {
          return function_type({target_type->list_element}, nil_type());
        }
        return function_type({any_type()}, nil_type());
      }
      if (attribute.attribute == "pop") {
        if (target_type->kind != Type::Kind::List) {
          add_error("'.pop' only supported on list-like values");
          return unknown_type();
        }
        if (target_type->list_element) {
          return target_type->list_element;
        }
        return unknown_type();
      }
      if (attribute.attribute == "insert") {
        if (target_type->kind != Type::Kind::List) {
          add_error("'.insert' only supported on list-like values");
          return unknown_type();
        }
        return function_type({int_type(), any_type()}, nil_type());
      }
      if (attribute.attribute == "remove") {
        if (target_type->kind != Type::Kind::List) {
          add_error("'.remove' only supported on list-like values");
          return unknown_type();
        }
        return function_type({any_type()}, nil_type());
      }
      if (attribute.attribute == "reduce_sum" || attribute.attribute == "sum") {
        if (target_type->kind != Type::Kind::List && target_type->kind != Type::Kind::Matrix) {
          add_error("'.reduce_sum' only supported on list/matrix values");
          return unknown_type();
        }
        if (target_type->list_element && target_type->list_element->kind == Type::Kind::Int) {
          return function_type({}, int_type());
        }
        return function_type({}, float_type(Type::FloatKind::F64));
      }
      if (attribute.attribute == "map_add") {
        if (target_type->kind != Type::Kind::List && target_type->kind != Type::Kind::Matrix) {
          add_error("'.map_add' only supported on list/matrix values");
          return unknown_type();
        }
        if (target_type->kind == Type::Kind::Matrix) {
          return function_type({float_type(Type::FloatKind::F64)},
                               matrix_type(float_type(Type::FloatKind::F64),
                                           target_type->matrix_rows, target_type->matrix_cols));
        }
        return function_type({float_type(Type::FloatKind::F64)}, list_type(float_type(Type::FloatKind::F64)));
      }
      if (attribute.attribute == "map_mul") {
        if (target_type->kind != Type::Kind::List && target_type->kind != Type::Kind::Matrix) {
          add_error("'.map_mul' only supported on list/matrix values");
          return unknown_type();
        }
        if (target_type->kind == Type::Kind::Matrix) {
          return function_type({float_type(Type::FloatKind::F64)},
                               matrix_type(float_type(Type::FloatKind::F64),
                                           target_type->matrix_rows, target_type->matrix_cols));
        }
        return function_type({float_type(Type::FloatKind::F64)}, list_type(float_type(Type::FloatKind::F64)));
      }
      if (attribute.attribute == "filter_gt" || attribute.attribute == "filter_lt") {
        if (target_type->kind != Type::Kind::List) {
          add_error(std::string("'.") + attribute.attribute + "' only supported on list values");
          return unknown_type();
        }
        return function_type({float_type(Type::FloatKind::F64)}, list_type(float_type(Type::FloatKind::F64)));
      }
      if (attribute.attribute == "filter_nonzero") {
        if (target_type->kind != Type::Kind::List) {
          add_error("'.filter_nonzero' only supported on list values");
          return unknown_type();
        }
        return function_type({}, list_type(float_type(Type::FloatKind::F64)));
      }
      if (attribute.attribute == "zip_add") {
        if (target_type->kind != Type::Kind::List) {
          add_error("'.zip_add' only supported on list values");
          return unknown_type();
        }
        return function_type({list_type(any_type())}, list_type(float_type(Type::FloatKind::F64)));
      }
      if (attribute.attribute == "scan_sum") {
        if (target_type->kind != Type::Kind::List) {
          add_error("'.scan_sum' only supported on list values");
          return unknown_type();
        }
        return function_type({}, list_type(float_type(Type::FloatKind::F64)));
      }
      if (attribute.attribute == "to_list" || attribute.attribute == "collect") {
        if (target_type->kind != Type::Kind::List && target_type->kind != Type::Kind::Matrix) {
          add_error(std::string("'.") + attribute.attribute + "' only supported on list/matrix values");
          return unknown_type();
        }
        return function_type({}, target_type);
      }
      if (attribute.attribute == "matmul" || attribute.attribute == "matmul_f32" ||
          attribute.attribute == "matmul_f64") {
        if (target_type->kind != Type::Kind::Matrix) {
          add_error(std::string("'.") + attribute.attribute + "' only supported on matrix values");
          return unknown_type();
        }
        return function_type(
            {matrix_type(any_type(), 0, 0)},
            matrix_type(float_type(Type::FloatKind::F64), target_type->matrix_rows, 0));
      }
      if (attribute.attribute == "matmul_add") {
        if (target_type->kind != Type::Kind::Matrix) {
          add_error("'.matmul_add' only supported on matrix values");
          return unknown_type();
        }
        return function_type(
            {matrix_type(any_type(), 0, 0), any_type()},
            matrix_type(float_type(Type::FloatKind::F64), target_type->matrix_rows, 0));
      }
      if (attribute.attribute == "matmul_axpby") {
        if (target_type->kind != Type::Kind::Matrix) {
          add_error("'.matmul_axpby' only supported on matrix values");
          return unknown_type();
        }
        return function_type(
            {matrix_type(any_type(), 0, 0), float_type(Type::FloatKind::F64),
             float_type(Type::FloatKind::F64), matrix_type(any_type(), 0, 0)},
            matrix_type(float_type(Type::FloatKind::F64), target_type->matrix_rows, 0));
      }
      if (attribute.attribute == "matmul_stats" || attribute.attribute == "matmul_schedule") {
        if (target_type->kind != Type::Kind::Matrix) {
          add_error(std::string("'.") + attribute.attribute + "' only supported on matrix values");
          return unknown_type();
        }
        return function_type({}, list_type(int_type()));
      }
      if (attribute.attribute == "plan_id") {
        if (target_type->kind != Type::Kind::List && target_type->kind != Type::Kind::Matrix) {
          add_error("'.plan_id' only supported on list/matrix values");
          return unknown_type();
        }
        return function_type({}, int_type());
      }
      if (attribute.attribute == "cache_stats" || attribute.attribute == "pipeline_stats") {
        if (target_type->kind != Type::Kind::List && target_type->kind != Type::Kind::Matrix) {
          add_error(std::string("'.") + attribute.attribute + "' only supported on list/matrix values");
          return unknown_type();
        }
        return function_type({}, list_type(int_type()));
      }
      if (attribute.attribute == "cache_bytes" || attribute.attribute == "pipeline_plan_id") {
        if (target_type->kind != Type::Kind::List && target_type->kind != Type::Kind::Matrix) {
          add_error(std::string("'.") + attribute.attribute + "' only supported on list/matrix values");
          return unknown_type();
        }
        return function_type({}, int_type());
      }
      if (attribute.attribute == "spawn" || attribute.attribute == "join_all" ||
          attribute.attribute == "cancel_all") {
        if (target_type->kind != Type::Kind::TaskGroup) {
          add_error(std::string("'.") + attribute.attribute + "' only supported on task_group values");
          return unknown_type();
        }
        if (attribute.attribute == "spawn") {
          return function_type({any_type(), any_type()}, task_type(any_type()));
        }
        if (attribute.attribute == "join_all") {
          return function_type({}, list_type(any_type()));
        }
        return function_type({}, nil_type());
      }
      if (attribute.attribute == "join" || attribute.attribute == "cancel") {
        if (target_type->kind != Type::Kind::Task) {
          add_error(std::string("'.") + attribute.attribute + "' only supported on task values");
          return unknown_type();
        }
        if (attribute.attribute == "join") {
          return function_type({}, target_type->task_result ? target_type->task_result : any_type());
        }
        return function_type({}, nil_type());
      }
      if (attribute.attribute == "send" || attribute.attribute == "recv" ||
          attribute.attribute == "close" || attribute.attribute == "stats" ||
          attribute.attribute == "anext" || attribute.attribute == "next" ||
          attribute.attribute == "has_next") {
        if (target_type->kind != Type::Kind::Channel) {
          add_error(std::string("'.") + attribute.attribute + "' only supported on channel values");
          return unknown_type();
        }
        if (attribute.attribute == "send") {
          return function_type({target_type->channel_element ? target_type->channel_element : any_type()}, nil_type());
        }
        if (attribute.attribute == "recv" || attribute.attribute == "anext" ||
            attribute.attribute == "next") {
          return function_type({}, target_type->channel_element ? target_type->channel_element : any_type());
        }
        if (attribute.attribute == "stats") {
          return function_type({}, list_type(int_type()));
        }
        if (attribute.attribute == "has_next") {
          return function_type({}, bool_type());
        }
        return function_type({}, nil_type());
      }
      if (attribute.attribute == "T" || attribute.attribute == "transpose") {
        if (target_type->kind != Type::Kind::Matrix) {
          add_error("transpose attribute is only valid on matrix values");
          return unknown_type();
        }
        const auto element = target_type->list_element ? target_type->list_element : unknown_type();
        return matrix_type(element, target_type->matrix_cols, target_type->matrix_rows);
      }
      add_error("unsupported attribute: " + attribute.attribute);
      return error_type();
    }
    case Expr::Kind::List: {
      const auto& list = static_cast<const ListExpr&>(expr);
      if (list.elements.empty()) {
        return list_type(unknown_type());
      }

      bool all_rows = !list.elements.empty();
      std::vector<TypePtr> row_types;
      row_types.reserve(list.elements.size());
      for (const auto& element : list.elements) {
        auto current = infer_expr(*element);
        row_types.push_back(current);
        all_rows = all_rows && current->kind == Type::Kind::List;
      }

      if (all_rows) {
        TypePtr row_element = row_types.front()->list_element ? row_types.front()->list_element : any_type();
        std::size_t max_cols = 0;
        for (std::size_t i = 0; i < row_types.size(); ++i) {
          auto row = row_types[i];
          if (!row->list_element) {
            add_context_reason({.message = "matrix row type unknown", .normalizable = true});
            continue;
          }
          row_element = normalize_list_elements(row_element, row->list_element);
          if (const auto* row_expr = dynamic_cast<const ListExpr*>(list.elements[i].get())) {
            if (i == 0) {
              max_cols = row_expr->elements.size();
            } else if (max_cols != row_expr->elements.size()) {
              add_context_reason(
                  {.message = "matrix rows have different lengths", .normalizable = true});
              max_cols = 0;
            }
          } else {
            max_cols = 0;
          }
          if (row->kind == Type::Kind::Any) {
            add_context_reason(
                {.message = "matrix row has Any element", .normalizable = true});
          }
        }
        return matrix_type(row_element, row_types.size(), max_cols);
      }

      TypePtr element_type = unknown_type();
      for (auto& element_type_candidate : row_types) {
        if (element_type_candidate->kind == Type::Kind::Unknown) {
          continue;
        }
        if (element_type->kind == Type::Kind::Unknown) {
          element_type = element_type_candidate;
          continue;
        }
        auto next = normalize_list_elements(element_type, element_type_candidate);
        const bool numeric_widen =
            is_numeric_widen_candidate(*element_type, *element_type_candidate);
        if (!same_or_unknown(*next, *element_type) && !numeric_widen) {
          add_context_reason(
              {.message = "heterogeneous list literal was widened to list[Any]", .normalizable = true});
          next = any_type();
        }
        element_type = next;
      }
      return list_type(element_type);
    }
    case Expr::Kind::Unary: {
      const auto& unary = static_cast<const UnaryExpr&>(expr);
      check_unary(unary);
      auto operand = infer_expr(*unary.operand);
      if (unary.op == UnaryOp::Await) {
        if (operand->kind == Type::Kind::Task) {
          return operand->task_result ? operand->task_result : any_type();
        }
        return unknown_type();
      }
      if (unary.op == UnaryOp::Neg) {
        if (operand->kind == Type::Kind::Int) {
          return int_type();
        }
        if (operand->kind == Type::Kind::Float) {
          return float_type(operand->float_kind);
        }
        return unknown_type();
      }
      return bool_type();
    }
    case Expr::Kind::Binary: {
      const auto& binary = static_cast<const BinaryExpr&>(expr);
      check_binary(binary);
      if (binary.op == BinaryOp::Eq || binary.op == BinaryOp::Ne) {
        return bool_type();
      }
      if (binary.op == BinaryOp::And || binary.op == BinaryOp::Or) {
        return bool_type();
      }
      if (binary.op == BinaryOp::Lt || binary.op == BinaryOp::Lte ||
          binary.op == BinaryOp::Gt || binary.op == BinaryOp::Gte) {
        return bool_type();
      }
      if (binary.op == BinaryOp::Add || binary.op == BinaryOp::Sub ||
          binary.op == BinaryOp::Mul || binary.op == BinaryOp::Div ||
          binary.op == BinaryOp::Mod || binary.op == BinaryOp::Pow) {
        auto left = infer_expr(*binary.left);
        auto right = infer_expr(*binary.right);
        if (left->kind == Type::Kind::String || right->kind == Type::Kind::String) {
          if (left->kind != Type::Kind::String || right->kind != Type::Kind::String) {
            return unknown_type();
          }
          if (binary.op == BinaryOp::Add) {
            return string_type();
          }
          if (binary.op == BinaryOp::Lt || binary.op == BinaryOp::Lte ||
              binary.op == BinaryOp::Gt || binary.op == BinaryOp::Gte) {
            return bool_type();
          }
          return unknown_type();
        }
        if (left->kind == Type::Kind::Matrix || right->kind == Type::Kind::Matrix) {
          auto matrix = (left->kind == Type::Kind::Matrix) ? left : right;
          auto other = (left->kind == Type::Kind::Matrix) ? right : left;

          TypePtr element = matrix->list_element ? matrix->list_element : unknown_type();
          if (other->kind == Type::Kind::Matrix) {
            if (other->list_element) {
              element = normalize_list_elements(element, other->list_element);
            }
            if (binary.op == BinaryOp::Add && element &&
                element->kind != Type::Kind::Unknown && !is_numeric_type(*element)) {
              element = string_type();
            }
          } else if (is_numeric_type(*other) && other->kind != Type::Kind::Unknown) {
            element = normalize_list_elements(element, other);
          } else if ((binary.op == BinaryOp::Add || binary.op == BinaryOp::Mul) &&
                     other->kind != Type::Kind::Unknown) {
            element = string_type();
          }

          auto rows = matrix->matrix_rows;
          auto cols = matrix->matrix_cols;
          if (other->kind == Type::Kind::Matrix) {
            if (binary.op == BinaryOp::Mul && left->kind == Type::Kind::Matrix &&
                right->kind == Type::Kind::Matrix) {
              rows = left->matrix_rows;
              cols = right->matrix_cols;
            } else {
              if (rows == 0) {
                rows = other->matrix_rows;
              }
              if (cols == 0) {
                cols = other->matrix_cols;
              }
            }
          }
          return matrix_type(element, rows, cols);
        }

        if (left->kind == Type::Kind::List || right->kind == Type::Kind::List) {
          auto list = (left->kind == Type::Kind::List) ? left : right;
          auto other = (left->kind == Type::Kind::List) ? right : left;
          TypePtr element = list->list_element ? list->list_element : unknown_type();
          if (other->kind == Type::Kind::List) {
            if (binary.op == BinaryOp::Add) {
              return left;
            }
            if (other->list_element) {
              element = normalize_list_elements(element, other->list_element);
            }
          } else if (is_numeric_type(*other) && other->kind != Type::Kind::Unknown) {
            element = normalize_list_elements(element, other);
          } else if ((binary.op == BinaryOp::Add || binary.op == BinaryOp::Mul) &&
                     other->kind != Type::Kind::Unknown) {
            element = string_type();
          } else if (other->kind != Type::Kind::Unknown) {
            return unknown_type();
          }
          if (binary.op == BinaryOp::Div || binary.op == BinaryOp::Pow) {
            element = float_type();
          }
          return list_type(element);
        }
        if (!is_numeric_type(*left) || !is_numeric_type(*right)) {
          return unknown_type();
        }
        if (binary.op == BinaryOp::Div || binary.op == BinaryOp::Pow) {
          return float_type();
        }
        if (left->kind == Type::Kind::Float || right->kind == Type::Kind::Float) {
          return float_type();
        }
        return int_type();
      }
      return unknown_type();
    }
    case Expr::Kind::Call: {
      const auto& call = static_cast<const CallExpr&>(expr);
      return check_call(call);
    }
    case Expr::Kind::Index: {
      const auto& index = static_cast<const IndexExpr&>(expr);
      return infer_index_access_type(index, false);
    }
  }

  return unknown_type();
