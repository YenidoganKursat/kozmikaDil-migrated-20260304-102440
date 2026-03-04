}

std::string TypeChecker::type_to_string(const Type& type) {
  switch (type.kind) {
    case Type::Kind::Unknown:
      return "Unknown";
    case Type::Kind::Nil:
      return "Nil";
    case Type::Kind::Int:
      return "Int";
    case Type::Kind::Float: {
      return std::string("Float(") + float_kind_to_string(type.float_kind) + ")";
    }
    case Type::Kind::String:
      return "String";
    case Type::Kind::Bool:
      return "Bool";
    case Type::Kind::Any:
      return "Any";
    case Type::Kind::List: {
      if (!type.list_element) {
        return "List[Unknown]";
      }
      return "List[" + type_to_string(*type.list_element) + "]";
    }
    case Type::Kind::Matrix: {
      std::string out = "Matrix[";
      if (!type.list_element) {
        out += "Unknown";
      } else {
        out += type_to_string(*type.list_element);
      }
      out += "][" + std::to_string(type.matrix_rows) + "," + std::to_string(type.matrix_cols) + "]";
      return out;
    }
    case Type::Kind::Task: {
      if (!type.task_result) {
        return "Task[Unknown]";
      }
      return "Task[" + type_to_string(*type.task_result) + "]";
    }
    case Type::Kind::TaskGroup:
      return "TaskGroup";
    case Type::Kind::Channel: {
      if (!type.channel_element) {
        return "Channel[Unknown]";
      }
      return "Channel[" + type_to_string(*type.channel_element) + "]";
    }
    case Type::Kind::Function: {
      std::string out = "Function(";
      for (std::size_t i = 0; i < type.function_params.size(); ++i) {
        if (i > 0) {
          out += ", ";
        }
        out += type_to_string(*type.function_params[i]);
      }
      out += ") -> ";
      if (!type.function_return) {
        return out + "Unknown";
      }
      out += type_to_string(*type.function_return);
      return out;
    }
    case Type::Kind::Class:
      return std::string("Class(") + type.class_name + ", " +
             class_shape_status(type) + ", #" + std::to_string(type.class_shape_id) + ")";
    case Type::Kind::Builtin: {
      std::string out = "Builtin(";
      for (std::size_t i = 0; i < type.function_params.size(); ++i) {
        if (i > 0) {
          out += ", ";
        }
        out += type_to_string(*type.function_params[i]);
      }
      out += ") -> ";
      if (!type.function_return) {
        return out + "Unknown";
      }
      out += type_to_string(*type.function_return);
      return out;
    }
    case Type::Kind::Error:
      return "Error";
  }
  return "Unknown";
}

std::string TypeChecker::dump_types() const {
  std::ostringstream stream;
  for (const auto& symbol : symbols_) {
    stream << symbol.name << "|" << symbol.owner << "|" << symbol.scope << "|" << symbol.type << "\n";
  }
  return stream.str();
}

std::string TypeChecker::dump_shapes() const {
  std::ostringstream stream;
  for (const auto& shape : shapes_) {
    stream << shape.shape_id << "|" << shape.name << "|" << (shape.open ? "open" : "slots") << "|";
    for (std::size_t i = 0; i < shape.fields.size(); ++i) {
      if (i > 0) {
        stream << ",";
      }
      stream << shape.fields[i];
    }
    stream << "\n";
  }
  return stream.str();
}

std::string TypeChecker::dump_tier_report() const {
  std::size_t t4 = 0;
  std::size_t t5 = 0;
  std::size_t t8 = 0;
  for (const auto& fn : function_reports_) {
    if (fn.tier == TierLevel::T4) {
      ++t4;
    } else if (fn.tier == TierLevel::T5) {
      ++t5;
    } else {
      ++t8;
    }
  }
  for (const auto& loop : loop_reports_) {
    if (loop.tier == TierLevel::T4) {
      ++t4;
    } else if (loop.tier == TierLevel::T5) {
      ++t5;
    } else {
      ++t8;
    }
  }

  const auto total = t4 + t5 + t8;
  const double pct4 = total == 0 ? 0.0 : (static_cast<double>(t4) * 100.0) / static_cast<double>(total);
  const double pct5 = total == 0 ? 0.0 : (static_cast<double>(t5) * 100.0) / static_cast<double>(total);
  const double pct8 = total == 0 ? 0.0 : (static_cast<double>(t8) * 100.0) / static_cast<double>(total);

  std::ostringstream stream;
  stream << "total_entities=" << total << "\n";
  stream << "T4=" << t4 << " (" << pct4 << "%)\n";
  stream << "T5=" << t5 << " (" << pct5 << "%)\n";
  stream << "T8=" << t8 << " (" << pct8 << "%)\n";

  stream << "functions:\n";
  for (const auto& fn : function_reports_) {
    stream << "  function " << fn.name << " -> " << tier_to_string(fn.tier) << "\n";
    for (const auto& reason : fn.reasons) {
      stream << "    - " << reason << "\n";
    }
  }
  stream << "loops:\n";
  for (const auto& loop : loop_reports_) {
    stream << "  loop " << loop.name << " -> " << tier_to_string(loop.tier) << "\n";
    for (const auto& reason : loop.reasons) {
      stream << "    - " << reason << "\n";
    }
  }
  return stream.str();
}

std::string TypeChecker::dump_pipeline_ir() const {
  std::ostringstream stream;
  for (const auto& pipeline : pipelines_) {
    stream << pipeline.id << "|" << pipeline.receiver << "|" << pipeline.receiver_type
           << "|" << pipeline.terminal << "\n";
    for (const auto& node : pipeline.nodes) {
      stream << "  node|" << node << "\n";
    }
  }
  return stream.str();
}

std::string TypeChecker::dump_fusion_plan() const {
  std::ostringstream stream;
  for (const auto& pipeline : pipelines_) {
    stream << pipeline.id << "|" << pipeline.receiver
           << "|fused=" << (pipeline.fused ? "yes" : "no")
           << "|materialize=" << (pipeline.materialize_required ? "yes" : "no")
           << "|terminal=" << pipeline.terminal << "\n";
    for (const auto& reason : pipeline.reasons) {
      stream << "  reason|" << reason << "\n";
    }
  }
  return stream.str();
}

std::string TypeChecker::dump_why_not_fused() const {
  std::ostringstream stream;
  for (const auto& pipeline : pipelines_) {
    if (pipeline.fused) {
      continue;
    }
    stream << pipeline.id << "|" << pipeline.receiver << "\n";
    for (const auto& reason : pipeline.reasons) {
      stream << "  reason|" << reason << "\n";
    }
  }
  return stream.str();
}

std::string TypeChecker::dump_async_lowering() const {
  std::ostringstream stream;
  for (const auto& entry : async_lowerings_) {
    stream << entry.function_name
           << "|await_points=" << entry.await_points
           << "|states=" << entry.states
           << "|frame=" << (entry.heap_frame ? "heap" : "stack_or_inline")
           << "\n";
    for (std::size_t state = 0; state < entry.states; ++state) {
      stream << "  state|" << state;
      if (state < entry.await_points) {
        stream << "|suspend_on=await";
      } else {
        stream << "|terminal=return";
      }
      stream << "\n";
    }
  }
  return stream.str();
}

}  // namespace spark
