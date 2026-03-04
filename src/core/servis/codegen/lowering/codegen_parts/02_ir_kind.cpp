}

std::string container_initializer_fn(const ValueKind kind) {
  if (kind == ValueKind::ListFloat) {
    return "__spark_list_new_f64";
  }
  if (kind == ValueKind::MatrixFloat) {
    return "__spark_matrix_new_f64";
  }
  if (kind == ValueKind::MatrixInt || kind == ValueKind::MatrixAny) {
    return "__spark_matrix_new_i64";
  }
  return "__spark_list_new_i64";
}

std::string container_set_fn_for(const ValueKind kind) {
  if (kind == ValueKind::ListFloat) {
    return "__spark_list_set_f64";
  }
  if (kind == ValueKind::MatrixFloat) {
    return "__spark_matrix_set_f64";
  }
  return "__spark_list_set_i64";
}

std::string container_get_fn_for(const ValueKind kind) {
  if (kind == ValueKind::ListFloat) {
    return "__spark_list_get_f64";
  }
  if (kind == ValueKind::MatrixFloat) {
    return "__spark_matrix_get_f64";
  }
  return "__spark_list_get_i64";
}

std::string matrix_transpose_fn_for(const ValueKind kind) {
  return (kind == ValueKind::MatrixFloat) ? "__spark_matrix_transpose_f64" : "__spark_matrix_transpose_i64";
}

std::string matrix_slice_fn_for(const ValueKind kind) {
  return (kind == ValueKind::MatrixFloat) ? "__spark_matrix_slice_rows_f64" : "__spark_matrix_slice_rows_i64";
}

std::string matrix_slice_cols_fn_for(const ValueKind kind) {
  return (kind == ValueKind::MatrixFloat) ? "__spark_matrix_slice_cols_f64" : "__spark_matrix_slice_cols_i64";
}

std::string matrix_slice_block_fn_for(const ValueKind kind) {
  return (kind == ValueKind::MatrixFloat) ? "__spark_matrix_slice_block_f64" : "__spark_matrix_slice_block_i64";
}

std::string matrix_rows_col_fn_for(const ValueKind kind) {
  return (kind == ValueKind::MatrixFloat) ? "__spark_matrix_rows_col_f64" : "__spark_matrix_rows_col_i64";
}

std::string matrix_col_fn_for(const ValueKind kind) {
  return (kind == ValueKind::MatrixFloat) ? "__spark_matrix_col_f64" : "__spark_matrix_col_i64";
}

std::string matrix_set_row_fn_for(const ValueKind kind) {
  return (kind == ValueKind::MatrixFloat) ? "__spark_matrix_set_row_f64" : "__spark_matrix_set_row_i64";
}

std::string list_slice_fn_for(const ValueKind kind) {
  return (kind == ValueKind::ListFloat) ? "__spark_list_slice_f64" : "__spark_list_slice_i64";
}

ValueKind matrix_element_type(const ValueKind matrix_kind) {
  if (matrix_kind == ValueKind::MatrixFloat) {
    return ValueKind::Float;
  }
  return ValueKind::Int;
}

std::string container_new_temp_name(std::size_t& temp_id, const std::string& prefix) {
  return "_" + prefix + std::to_string(temp_id++);
}

ValueKind promote_scalar_for_container_set(ValueKind container_kind, ValueKind value_kind) {
  if (container_kind == ValueKind::ListInt || container_kind == ValueKind::MatrixInt) {
    if (value_kind == ValueKind::Float) {
      return ValueKind::Int;
    }
    return ValueKind::Int;
  }
  if (container_kind == ValueKind::ListFloat || container_kind == ValueKind::MatrixFloat) {
    if (value_kind == ValueKind::Int) {
      return ValueKind::Float;
    }
    return ValueKind::Float;
  }
  return ValueKind::Int;
}

std::vector<std::string> collect_temp_refs(const std::string& expr) {
  static const std::regex pattern("(%[A-Za-z0-9_]+)", std::regex::ECMAScript);
  std::vector<std::string> refs;
  auto begin = std::sregex_iterator(expr.begin(), expr.end(), pattern);
  auto end = std::sregex_iterator();
  for (auto it = begin; it != end; ++it) {
    refs.push_back((*it).str());
  }
  return refs;
}

std::unordered_set<std::string> collect_referenced_labels(const std::vector<std::string>& lines) {
  static const std::regex goto_pattern(R"(^\s*goto\s+([A-Za-z_][A-Za-z0-9_]*)\s*;?\s*$)",
                                      std::regex::ECMAScript);
  static const std::regex branch_pattern(
      R"(^\s*br_if\s+([^,]+),\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*$)",
      std::regex::ECMAScript);
  std::unordered_set<std::string> refs;

  for (const auto& line : lines) {
    std::smatch match;
    if (std::regex_match(line, match, goto_pattern)) {
      refs.insert(match[1].str());
      continue;
    }
    if (std::regex_match(line, match, branch_pattern)) {
      refs.insert(match[2].str());
      refs.insert(match[3].str());
    }
  }

  return refs;
}

bool is_redundant_goto_to_next_label(const std::string& this_line, const std::string& next_line) {
  static const std::regex goto_pattern(R"(^\s*goto\s+([A-Za-z_][A-Za-z0-9_]*)\s*;?\s*$)",
                                      std::regex::ECMAScript);
  static const std::regex label_pattern(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*:\s*$)",
                                       std::regex::ECMAScript);
  std::smatch goto_match;
  if (!std::regex_match(this_line, goto_match, goto_pattern)) {
    return false;
  }
  std::smatch label_match;
  if (!std::regex_match(next_line, label_match, label_pattern)) {
    return false;
  }
  return goto_match[1].str() == label_match[1].str();
}

bool is_label_definition(const std::string& line) {
  static const std::regex label_pattern(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*:\s*$)",
                                       std::regex::ECMAScript);
  return std::regex_match(line, label_pattern);
}

std::string pseudo_kind_to_cpp(const std::string& kind) {
  if (kind == "i64") {
    return "long long";
  }
  if (kind == "f64" || kind == "f32" || kind == "f16" || kind == "f8" || kind == "bf16") {
    return "double";
  }
  if (kind == "f128" || kind == "f256" || kind == "f512") {
    return "long double";
  }
  if (kind == "bool") {
    return "bool";
  }
  if (kind == "str") {
    return "__spark_string";
  }
  if (kind == "list_i64") {
    return "__spark_list_i64*";
  }
  if (kind == "list_f64") {
    return "__spark_list_f64*";
  }
  if (kind == "matrix_i64") {
    return "__spark_matrix_i64*";
  }
  if (kind == "matrix_f64") {
    return "__spark_matrix_f64*";
  }
  if (kind == "void") {
    return "void";
  }
  return "long long";
}

std::string sanitize_identifier(const std::string& token) {
  if (token.empty()) {
    return "tmp";
  }

  std::string out;
  out.reserve(token.size());
  for (const auto ch : token) {
    if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_') {
      out += ch;
    } else {
      out += '_';
    }
  }

  if (!std::isalpha(static_cast<unsigned char>(out[0])) && out[0] != '_') {
    out = "_" + out;
  }
  return out;
}

std::string parse_identifier_type(const std::string& token, const std::unordered_map<std::string, std::string>& var_types) {
  const auto it = var_types.find(token);
  if (it != var_types.end()) {
    return it->second;
  }
  if (token == "list_i64" || token == "list_f64" || token == "list_any" || token == "matrix_i64" || token == "matrix_f64" ||
      token == "matrix_any") {
    return token;
  }
  if (token == "str") {
    return "str";
  }
  if (token == "true" || token == "false") {
    return "bool";
  }
  bool has_digit = false;
  bool has_dot = false;
  bool has_exp = false;
  for (char ch : token) {
    if (std::isdigit(static_cast<unsigned char>(ch))) {
      has_digit = true;
      continue;
    }
    if (ch == '.') {
      has_dot = true;
      continue;
    }
    if (ch == 'e' || ch == 'E') {
      has_exp = true;
      continue;
    }
    if (ch == '-' || ch == '+') {
      continue;
    }
  }
  if (token == "true" || token == "false") {
    return "bool";
  }
  if (has_dot || has_exp) {
    return has_digit ? "f64" : "unknown";
  }
  if (token == "f8" || token == "f16" || token == "bf16" || token == "f32" || token == "f64" ||
      token == "f128" || token == "f256" || token == "f512") {
    return "f64";
  }
  return "i64";
}

bool is_container_kind(const ValueKind kind) {
  return kind == ValueKind::ListInt || kind == ValueKind::ListFloat || kind == ValueKind::ListAny ||
         kind == ValueKind::MatrixInt || kind == ValueKind::MatrixFloat || kind == ValueKind::MatrixAny;
}

ValueKind promoted_scalar_kind_for_container_cast(ValueKind container_kind, ValueKind value_kind) {
  if (container_kind == ValueKind::ListFloat || container_kind == ValueKind::MatrixFloat) {
    if (value_kind == ValueKind::Int || value_kind == ValueKind::Bool) {
      return ValueKind::Float;
    }
  }
  if (container_kind == ValueKind::ListInt || container_kind == ValueKind::MatrixInt ||
      container_kind == ValueKind::ListAny || container_kind == ValueKind::MatrixAny) {
    if (value_kind == ValueKind::Float) {
      return ValueKind::Int;
    }
  }
  return value_kind;
}

static bool is_slice_index_item(const IndexExpr::IndexItem& item) {
  return item.is_slice;
}

static bool is_integer_like_kind(ValueKind kind) {
  return kind == ValueKind::Int || kind == ValueKind::Bool || kind == ValueKind::Unknown;
}

static bool is_indexable_scalar(ValueKind kind) {
  return kind == ValueKind::Int || kind == ValueKind::Float || kind == ValueKind::Bool || kind == ValueKind::Unknown;
}

static bool is_container_index_assignable(ValueKind kind) {
  return is_list_kind(kind) || is_matrix_kind(kind);
}

bool is_float_kind(const ValueKind kind);

bool is_container_of_numeric(const ValueKind kind) {
  return kind == ValueKind::ListInt || kind == ValueKind::ListFloat || kind == ValueKind::ListAny ||
         kind == ValueKind::MatrixInt || kind == ValueKind::MatrixFloat || kind == ValueKind::MatrixAny;
}

bool is_scalar_value_kind(const ValueKind kind) {
  return kind == ValueKind::Int || kind == ValueKind::Float || kind == ValueKind::Bool || kind == ValueKind::String;
}

bool is_matrix_kind(const ValueKind kind) {
  return kind == ValueKind::MatrixInt || kind == ValueKind::MatrixFloat || kind == ValueKind::MatrixAny;
}

bool is_list_kind(const ValueKind kind) {
  return kind == ValueKind::ListInt || kind == ValueKind::ListFloat || kind == ValueKind::ListAny;
}

bool is_numeric_or_scalar(const ValueKind kind) {
  return kind == ValueKind::Int || kind == ValueKind::Float || kind == ValueKind::Bool;
}

std::string container_len_fn(const ValueKind kind) {
  return is_matrix_kind(kind)
             ? (is_float_kind(kind) ? "__spark_matrix_len_rows_f64" : "__spark_matrix_len_rows_i64")
             : "__spark_list_len_i64";
}

std::string container_len_rows_fn(const ValueKind kind) {
  return is_matrix_kind(kind)
             ? (is_float_kind(kind) ? "__spark_matrix_len_rows_f64" : "__spark_matrix_len_rows_i64")
             : "__spark_list_len_i64";
}

std::string container_prefix(const ValueKind kind) {
  if (kind == ValueKind::ListInt || kind == ValueKind::MatrixInt) {
    return "i64";
  }
  if (kind == ValueKind::ListFloat || kind == ValueKind::MatrixFloat) {
    return "f64";
  }
  return "i64";
}

std::string container_index_get_fn(const ValueKind kind) {
  return (kind == ValueKind::MatrixInt || kind == ValueKind::MatrixFloat || kind == ValueKind::MatrixAny)
             ? (is_float_kind(kind) ? "__spark_matrix_get_f64" : "__spark_matrix_get_i64")
             : (is_float_kind(kind) ? "__spark_list_get_f64" : "__spark_list_get_i64");
}

std::string container_index_set_fn(const ValueKind kind) {
  return (kind == ValueKind::MatrixInt || kind == ValueKind::MatrixFloat || kind == ValueKind::MatrixAny)
             ? (is_float_kind(kind) ? "__spark_matrix_set_f64" : "__spark_matrix_set_i64")
             : (is_float_kind(kind) ? "__spark_list_set_f64" : "__spark_list_set_i64");
}

std::string container_append_fn(const ValueKind kind) {
  return is_float_kind(kind) ? "__spark_list_append_f64" : "__spark_list_append_i64";
}

std::string container_append_unchecked_fn(const ValueKind kind) {
  return is_float_kind(kind) ? "__spark_list_append_unchecked_f64" : "__spark_list_append_unchecked_i64";
}

std::string container_reserve_fn(const ValueKind kind) {
  return is_float_kind(kind) ? "__spark_list_ensure_f64" : "__spark_list_ensure_i64";
}

std::string container_new_fn(const ValueKind kind) {
  if (kind == ValueKind::ListFloat) {
    return "__spark_list_new_f64";
  }
  return "__spark_list_new_i64";
}

std::string matrix_new_fn(const ValueKind kind) {
  return is_float_kind(kind) ? "__spark_matrix_new_f64" : "__spark_matrix_new_i64";
}

std::string matrix_len_rows_fn(const ValueKind kind) {
  return is_float_kind(kind) ? "__spark_matrix_len_rows_f64" : "__spark_matrix_len_rows_i64";
}

std::string matrix_len_cols_fn(const ValueKind kind) {
  return is_float_kind(kind) ? "__spark_matrix_len_cols_f64" : "__spark_matrix_len_cols_i64";
}

bool is_scalar_numeric_kind(const ValueKind kind) {
  return kind == ValueKind::Int || kind == ValueKind::Float || kind == ValueKind::Bool;
}

bool is_float_kind(const ValueKind kind) {
  return kind == ValueKind::Float || kind == ValueKind::ListFloat || kind == ValueKind::MatrixFloat;
}

ValueKind container_element_kind(const ValueKind container_kind) {
  if (container_kind == ValueKind::ListFloat || container_kind == ValueKind::MatrixFloat) {
    return ValueKind::Float;
  }
  if (container_kind == ValueKind::ListInt || container_kind == ValueKind::MatrixInt) {
    return ValueKind::Int;
  }
  return ValueKind::Unknown;
}

std::string value_kind_to_container_type(ValueKind kind, const std::string& scalar_prefix) {
  if (scalar_prefix == "i64") {
    if (kind == ValueKind::ListInt || kind == ValueKind::ListAny) {
      return "list_i64";
    }
    if (kind == ValueKind::MatrixInt || kind == ValueKind::MatrixAny) {
      return "matrix_i64";
    }
  } else {
    if (kind == ValueKind::ListFloat) {
      return "list_f64";
    }
    if (kind == ValueKind::MatrixFloat) {
      return "matrix_f64";
    }
  }
  return "";
}

std::string canonical_runtime_type(const std::string& kind) {
  if (kind == "i64" || kind == "bool" || kind == "f64" || kind == "str" || kind == "void" || kind == "unknown" ||
      kind == "invalid") {
    return kind;
  }
  if (kind == "list_i64" || kind == "list_f64" || kind == "list_any" || kind == "matrix_i64" || kind == "matrix_f64" ||
      kind == "matrix_any") {
    return kind;
  }
  if (kind.rfind("list_", 0) == 0 || kind.rfind("matrix_", 0) == 0) {
    return kind;
  }
  return "unknown";
}

std::string kind_to_runtime_setter(const ValueKind kind) {
  if (kind == ValueKind::ListFloat) {
    return "__spark_list_set_f64";
  }
  if (kind == ValueKind::MatrixFloat) {
    return "__spark_matrix_set_f64";
  }
  if (kind == ValueKind::MatrixInt || kind == ValueKind::MatrixAny) {
    return "__spark_matrix_set_i64";
  }
  return "__spark_list_set_i64";
}

std::string kind_to_runtime_getter(const ValueKind kind) {
  if (kind == ValueKind::ListFloat) {
    return "__spark_list_get_f64";
  }
  if (kind == ValueKind::MatrixFloat) {
    return "__spark_matrix_get_f64";
  }
  return "__spark_list_get_i64";
}

std::string kind_to_runtime_len(const ValueKind kind) {
  if (kind == ValueKind::MatrixAny || kind == ValueKind::MatrixInt || kind == ValueKind::MatrixFloat) {
    return "__spark_matrix_len_rows_i64";
  }
  return "__spark_list_len_i64";
}

ValueKind infer_container_from_runtime_call(const std::string& callee, bool assignment_context) {
  if (callee.empty()) {
    return ValueKind::Unknown;
  }

  const auto has = [&](const std::string& token) {
    return callee.find(token) != std::string::npos;
  };

  const auto returns_void_if = [&](const std::string& token) {
    if (has(token)) {
      return ValueKind::Invalid;
    }
    return ValueKind::Unknown;
  };

  if (has("list_set_i64") || has("list_append_i64") || has("list_append_unchecked_i64") || has("__spark_list_insert_i64") || has("__spark_list_remove_i64") ||
      has("__spark_list_insert_f64") || has("__spark_list_remove_f64")) {
    return ValueKind::Invalid;
  }
  if (has("list_set_f64") || has("list_append_f64") || has("list_append_unchecked_f64")) {
    return ValueKind::Invalid;
  }
  if (assignment_context && returns_void_if("list_set_i64") != ValueKind::Unknown) {
    return ValueKind::Invalid;
  }
  if (has("__spark_list_new_i64")) {
    return ValueKind::ListInt;
  }
  if (has("__spark_list_new_f64")) {
    return ValueKind::ListFloat;
  }
  if (has("__spark_list_get_i64")) {
    return ValueKind::Int;
  }
  if (has("__spark_list_get_f64")) {
    return ValueKind::Float;
  }
  if (has("__spark_list_len_i64") || has("__spark_list_len_f64") || has("__spark_list_len_rows") ||
      has("__spark_list_len_cols")) {
    return ValueKind::Int;
  }
  if (has("__spark_list_slice_i64")) {
    return ValueKind::ListInt;
  }
  if (has("__spark_list_slice_f64")) {
    return ValueKind::ListFloat;
  }
  if (has("__spark_list_pop_i64")) {
    return ValueKind::Int;
  }
  if (has("__spark_list_pop_f64")) {
    return ValueKind::Float;
  }
  if (has("__spark_list_len_f64")) {
    return ValueKind::Int;
  }
  if (has("__spark_string_from_utf8") || has("__spark_string_from_i64") || has("__spark_string_from_f64") ||
      has("__spark_string_from_bool") || has("__spark_string_from_ascii1_i64_prefix") ||
      has("__spark_string_from_ascii1_i64_suffix") || has("__spark_string_concat") || has("__spark_string_index") ||
      has("__spark_string_slice")) {
    return ValueKind::String;
  }
  if (has("__spark_string_eq")) {
    return ValueKind::Bool;
  }
  if (has("__spark_string_len") || has("__spark_string_utf8_len") || has("__spark_string_utf16_len") ||
      has("__spark_string_cmp")) {
    return ValueKind::Int;
  }

  if (has("matrix_set_i64") || has("matrix_append_i64") || has("matrix_set_f64") ||
      has("__spark_matrix_set_row_i64") || has("__spark_matrix_set_row_f64")) {
    return ValueKind::Invalid;
  }
  if (has("__spark_matrix_new_i64") || has("__spark_matrix_mul_i64") || has("__spark_matrix_add_i64") ||
      has("__spark_matrix_sub_i64") || has("__spark_matrix_div_i64") || has("__spark_matrix_scalar_mul_i64") ||
      has("__spark_matrix_scalar_add_i64") || has("__spark_matrix_scalar_sub_i64") ||
      has("__spark_matrix_scalar_div_i64") || has("__spark_matrix_transpose_i64")) {
    return ValueKind::MatrixInt;
  }
  if (has("__spark_matrix_new_f64") || has("__spark_matrix_mul_f64") || has("__spark_matrix_add_f64") ||
      has("__spark_matrix_sub_f64") || has("__spark_matrix_div_f64") || has("__spark_matrix_scalar_mul_f64") ||
      has("__spark_matrix_scalar_add_f64") || has("__spark_matrix_scalar_sub_f64") ||
      has("__spark_matrix_scalar_div_f64") || has("__spark_matrix_transpose_f64")) {
    return ValueKind::MatrixFloat;
  }
  if (has("__spark_matrix_get_i64")) {
    return ValueKind::Int;
  }
  if (has("__spark_matrix_get_f64")) {
    return ValueKind::Float;
  }
  if (has("__spark_matrix_len_rows_i64") || has("__spark_matrix_len_cols_i64") || has("__spark_matrix_len_rows_f64") ||
      has("__spark_matrix_len_cols_f64")) {
    return ValueKind::Int;
  }
  if (has("__spark_matrix_row_i64") || has("__spark_matrix_col_i64") || has("__spark_matrix_rows_col_i64")) {
    return ValueKind::ListInt;
  }
  if (has("__spark_matrix_row_f64") || has("__spark_matrix_col_f64") || has("__spark_matrix_rows_col_f64")) {
    return ValueKind::ListFloat;
  }
  if (has("__spark_matrix_slice_rows_i64") || has("__spark_matrix_slice_cols_i64") ||
      has("__spark_matrix_slice_block_i64")) {
    return ValueKind::MatrixInt;
  }
  if (has("__spark_matrix_slice_rows_f64") || has("__spark_matrix_slice_cols_f64") ||
      has("__spark_matrix_slice_block_f64")) {
    return ValueKind::MatrixFloat;
  }
  if (assignment_context && returns_void_if("matrix_set_i64") != ValueKind::Unknown) {
    return ValueKind::Invalid;
  }
  return ValueKind::Unknown;
}

std::string detect_literal(const std::string& token) {
  static const std::regex int_pattern(R"(^[+-]?\d+$)");
  static const std::regex float_pattern(R"(^[+-]?(?:\d+\.\d*|\d*\.\d+|\d+)(?:[eE][+-]?\d+)?$)");
  if (std::regex_match(token, int_pattern)) {
    return "i64";
  }
  if (std::regex_match(token, float_pattern)) {
    return "f64";
  }
  return "unknown";
}

bool try_plain_ascii_literal_bytes(const std::string& token, std::size_t& out_bytes) {
  out_bytes = 0;
  if (token.size() < 2) {
    return false;
  }
  const char quote = token.front();
  if ((quote != '"' && quote != '\'') || token.back() != quote) {
    return false;
  }
  for (std::size_t i = 1; i + 1 < token.size(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(token[i]);
    if (ch == static_cast<unsigned char>('\\') || ch >= 0x80u) {
      return false;
    }
    ++out_bytes;
  }
  return true;
}

std::string emit_string_literal_ctor(const std::string& token) {
  std::size_t ascii_bytes = 0;
  if (try_plain_ascii_literal_bytes(token, ascii_bytes)) {
    return "__spark_string_from_ascii_literal(" + token + ", " +
           std::to_string(ascii_bytes) + ")";
  }
  return "__spark_string_from_utf8(" + token + ")";
}

bool try_parse_pow2_u64_literal(const std::string& token, unsigned long long& value_out, unsigned& shift_out) {
  std::string s = trim_ws(token);
  while (s.size() >= 2 && s.front() == '(' && s.back() == ')') {
    s = trim_ws(s.substr(1, s.size() - 2));
  }
  if (s.empty()) {
    return false;
  }
  if (s.front() == '+') {
    s.erase(s.begin());
  }
  if (s.empty()) {
    return false;
  }
  if (s.front() == '-') {
    return false;
  }
  for (const auto ch : s) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) {
      return false;
    }
  }
  unsigned long long value = 0ULL;
  try {
    value = std::stoull(s);
  } catch (const std::exception&) {
    return false;
  }
  if (value == 0ULL || (value & (value - 1ULL)) != 0ULL) {
    return false;
  }
  unsigned shift = 0U;
  unsigned long long tmp = value;
  while (tmp > 1ULL) {
    tmp >>= 1ULL;
    ++shift;
  }
  value_out = value;
  shift_out = shift;
  return true;
}

std::string emit_cast_for_kind(const std::string& target_type, const std::string& source_expr) {
  if (target_type == "f64") {
    return "((double)(" + source_expr + "))";
  }
  if (target_type == "f128" || target_type == "f256" || target_type == "f512") {
    return "((long double)(" + source_expr + "))";
  }
  if (target_type == "i64") {
    return "((long long)(" + source_expr + "))";
  }
  return source_expr;
}

struct TranslatedExpr {
  std::string code;
  std::string kind;
};

TranslatedExpr parse_pseudo_expression(
    const std::string& token,
    const std::unordered_map<std::string, std::string>& var_types,
    const std::unordered_map<std::string, std::string>& function_return_types) {
  auto t = trim_ws(token);
  if (t.empty()) {
    return {"", "unknown"};
  }

  if (t[0] == '"' || t[0] == '\'') {
    return {emit_string_literal_ctor(t), "str"};
  }

  const auto literal_kind = detect_literal(t);
  if (literal_kind != "unknown") {
    return {t, literal_kind};
  }

  const std::vector<std::pair<std::string, std::string>> comparisons = {
      {"cmp.lt.", "<"},
      {"cmp.le.", "<="},
      {"cmp.gt.", ">"},
      {"cmp.ge.", ">="},
      {"cmp.eq.", "=="},
      {"cmp.ne.", "!="},
  };

  auto space = t.find(' ');
  if (space == std::string::npos) {
    const auto sanitized = sanitize_identifier(t);
    const auto it_sanitized = var_types.find(sanitized);
    if (it_sanitized != var_types.end()) {
      return {sanitized, it_sanitized->second};
    }

    auto it = var_types.find(t);
    if (it != var_types.end()) {
      return {sanitize_identifier(t), it->second};
    }
    if (t == "true" || t == "false") {
      return {sanitize_identifier(t), "bool"};
    }
    return {sanitize_identifier(t), parse_identifier_type(t, var_types)};
  }

  const auto op = t.substr(0, space);
  const auto rest = trim_ws(t.substr(space + 1));
  if (op == "call") {
    if (rest.size() >= 2 && rest[0] == '@') {
      const auto open = rest.find('(');
      const auto close = rest.rfind(')');
      const std::string callee = (open == std::string::npos) ? rest.substr(1) : rest.substr(1, open - 1);
      std::vector<std::string> args;
      if (open != std::string::npos && close != std::string::npos && close > open + 1) {
        args = split_csv_args(rest.substr(open + 1, close - open - 1));
      }
      std::string arg_expr;
      if (!args.empty()) {
        std::vector<std::string> rendered_args;
        rendered_args.reserve(args.size());
        for (auto& arg : args) {
          const auto arg_expr = parse_pseudo_expression(arg, var_types, function_return_types);
          rendered_args.push_back(arg_expr.code);
        }
        arg_expr = "(" + std::accumulate(rendered_args.begin(), rendered_args.end(), std::string{}, [](const std::string& acc, const std::string& a) {
          return acc.empty() ? a : acc + ", " + a;
        }) + ")";
      } else {
        arg_expr = "()";
      }

      auto infer_runtime_kind = infer_container_from_runtime_call(callee, false);
      if (infer_runtime_kind != ValueKind::Unknown) {
        if (infer_runtime_kind == ValueKind::Invalid) {
          return {callee + arg_expr, "void"};
        }
        if (infer_runtime_kind == ValueKind::ListInt) {
          return {callee + arg_expr, "list_i64"};
        }
        if (infer_runtime_kind == ValueKind::ListFloat) {
          return {callee + arg_expr, "list_f64"};
        }
        if (infer_runtime_kind == ValueKind::MatrixInt) {
          return {callee + arg_expr, "matrix_i64"};
        }
        if (infer_runtime_kind == ValueKind::MatrixFloat) {
          return {callee + arg_expr, "matrix_f64"};
        }
        if (infer_runtime_kind == ValueKind::String) {
          return {callee + arg_expr, "str"};
        }
      }

      if (callee == "print") {
        if (args.empty()) {
          return {"__spark_print_i64()", "void"};
        }
        const auto arg = parse_pseudo_expression(args[0], var_types, function_return_types);
        auto print_kind = arg.kind;
        if (print_kind == "void" || print_kind == "unknown") {
          print_kind = parse_identifier_type(args[0], var_types);
        }
        if (print_kind == "f128" || print_kind == "f256" || print_kind == "f512") {
          return {"__spark_print_f64((double)(" + arg.code + "))", "void"};
        }
        if (print_kind == "str") {
          return {"__spark_print_str(" + arg.code + ")", "void"};
        }
        if (print_kind != "f64" && print_kind != "i64" && print_kind != "bool") {
          print_kind = "i64";
        }
        return {"__spark_print_" + print_kind + "(" + arg.code + ")", "void"};
      }
      if (callee == "len") {
        if (args.empty()) {
          return {"0", "i64"};
        }
        const auto arg = parse_pseudo_expression(args[0], var_types, function_return_types);
        if (arg.kind == "str") {
          return {"__spark_string_len(" + arg.code + ")", "i64"};
        }
        return {"__spark_list_len_i64(" + arg.code + ")", "i64"};
      }
      return {callee + arg_expr, function_return_types.count(callee) ? function_return_types.at(callee) : "i64"};
    }
  }

  if (op == "not") {
    auto arg = parse_pseudo_expression(rest, var_types, function_return_types);
    return {"(!(" + arg.code + "))", "bool"};
  }

  if (op == "i64.const" || op == "f64.const" || op == "bool.const" || op == "str.const") {
    const auto kind = op.substr(0, op.find(".const"));
    const auto value = rest;
    if (kind == "i64") {
      return {value, "i64"};
    }
    if (kind == "f64") {
      return {value, "f64"};
    }
    if (kind == "bool") {
      return {value, "bool"};
    }
    if (kind == "str") {
      return {emit_string_literal_ctor(value), "str"};
    }
  }

  for (const auto& [prefix, op_symbol] : comparisons) {
    if (op.rfind(prefix, 0) == 0) {
      const auto ops = split_csv_args(rest);
      if (ops.size() >= 2) {
        auto left = parse_pseudo_expression(ops[0], var_types, function_return_types);
        auto right = parse_pseudo_expression(ops[1], var_types, function_return_types);
        return {"((" + left.code + ") " + op_symbol + " (" + right.code + "))", "bool"};
      }
    }
  }

  if (op == "and" || op == "or") {
    const auto ops = split_csv_args(rest);
    if (ops.size() >= 2) {
      auto left = parse_pseudo_expression(ops[0], var_types, function_return_types);
      auto right = parse_pseudo_expression(ops[1], var_types, function_return_types);
      const std::string op_text = (op == "and" ? "&&" : "||");
      return {"((" + left.code + ") " + op_text + " (" + right.code + "))", "bool"};
    }
  }

  const auto dot = op.find_last_of('.');
  if (dot != std::string::npos) {
    const std::string base = op.substr(0, dot);
    const std::string kind = op.substr(dot + 1);
    const auto is_custom_float_kind = [&](const std::string& k) {
      return k == "f8" || k == "f16" || k == "bf16" || k == "f32" || k == "f64" ||
             k == "f128" || k == "f256" || k == "f512";
    };
    if (base == "neg") {
      auto arg = parse_pseudo_expression(rest, var_types, function_return_types);
      return {"((-" + arg.code + "))", kind};
    }
    if (base == "add" || base == "sub" || base == "mul" || base == "div" || base == "mod" || base == "pow") {
      const auto ops = split_csv_args(rest);
      if (ops.size() >= 2) {
        auto left = parse_pseudo_expression(ops[0], var_types, function_return_types);
        auto right = parse_pseudo_expression(ops[1], var_types, function_return_types);
        if (is_custom_float_kind(kind)) {
          const auto out_kind =
              (kind == "f128" || kind == "f256" || kind == "f512") ? kind : std::string("f64");
          if (kind == "f8" || kind == "f16" || kind == "bf16" || kind == "f32") {
            return {"__spark_num_" + base + "_fast_" + kind + "((" + left.code + "), (" + right.code + "))",
                    out_kind};
          }
          return {"__spark_num_" + base + "_" + kind + "((" + left.code + "), (" + right.code + "))", out_kind};
        }
        std::string op_text = base;
        if (op_text == "mul") {
          op_text = "*";
        } else if (op_text == "add") {
          op_text = "+";
        } else if (op_text == "sub") {
          op_text = "-";
        } else if (op_text == "div") {
          if (kind == "i64") {
            unsigned long long pow2 = 0ULL;
            unsigned shift = 0U;
            if (try_parse_pow2_u64_literal(right.code, pow2, shift)) {
              return {"__spark_div_i64_pow2((" + left.code + "), " + std::to_string(shift) + "u)", kind};
            }
          }
          op_text = "/";
        } else if (op_text == "mod") {
          if (kind == "f64") {
            return {"fmod((" + left.code + "), (" + right.code + "))", kind};
          }
          if (kind == "i64") {
            unsigned long long pow2 = 0ULL;
            unsigned shift = 0U;
            if (try_parse_pow2_u64_literal(right.code, pow2, shift)) {
              return {"__spark_mod_i64_pow2((" + left.code + "), " + std::to_string(pow2 - 1ULL) + ")", kind};
            }
          }
          op_text = "%";
        } else if (op_text == "pow") {
          if (kind == "i8" || kind == "i16" || kind == "i32" || kind == "i64" ||
              kind == "i128" || kind == "i256" || kind == "i512") {
            return {"__spark_pow_i64_i64((" + left.code + "), (" + right.code + "))", kind};
          }
          return {"pow((" + left.code + "), (" + right.code + "))", kind};
        }
        return {"((" + left.code + ") " + op_text + " (" + right.code + "))", kind};
      }
    }
  }

  if (op.rfind("cast.", 0) == 0) {
    const auto args = split_csv_args(rest);
    if (args.size() == 1) {
      if (op == "cast.i64_to_f64") {
        auto arg = parse_pseudo_expression(args[0], var_types, function_return_types);
        return {emit_cast_for_kind("f64", arg.code), "f64"};
      }
      if (op == "cast.f64_to_i64") {
        auto arg = parse_pseudo_expression(args[0], var_types, function_return_types);
        return {emit_cast_for_kind("i64", arg.code), "i64"};
      }
      if (op == "cast.list_i64_to_i64") {
        auto arg = parse_pseudo_expression(args[0], var_types, function_return_types);
        return {"((i64)(uintptr_t)(" + arg.code + "))", "i64"};
      }
      if (op == "cast.list_f64_to_i64") {
        auto arg = parse_pseudo_expression(args[0], var_types, function_return_types);
        return {"((i64)(uintptr_t)(" + arg.code + "))", "i64"};
      }
      if (op == "cast.matrix_i64_to_i64") {
        auto arg = parse_pseudo_expression(args[0], var_types, function_return_types);
        return {"((i64)(uintptr_t)(" + arg.code + "))", "i64"};
      }
      if (op == "cast.matrix_f64_to_i64") {
        auto arg = parse_pseudo_expression(args[0], var_types, function_return_types);
        return {"((i64)(uintptr_t)(" + arg.code + "))", "i64"};
      }
      if (op == "cast.i64_to_list_i64") {
        auto arg = parse_pseudo_expression(args[0], var_types, function_return_types);
        return {"((__spark_list_i64*) (uintptr_t)(" + arg.code + "))", "list_i64"};
      }
      if (op == "cast.i64_to_list_f64") {
        auto arg = parse_pseudo_expression(args[0], var_types, function_return_types);
        return {"((__spark_list_f64*) (uintptr_t)(" + arg.code + "))", "list_f64"};
      }
      if (op == "cast.i64_to_matrix_i64") {
        auto arg = parse_pseudo_expression(args[0], var_types, function_return_types);
        return {"((__spark_matrix_i64*) (uintptr_t)(" + arg.code + "))", "matrix_i64"};
      }
      if (op == "cast.i64_to_matrix_f64") {
        auto arg = parse_pseudo_expression(args[0], var_types, function_return_types);
        return {"((__spark_matrix_f64*) (uintptr_t)(" + arg.code + "))", "matrix_f64"};
      }
    }
  }

  return {sanitize_identifier(t), parse_identifier_type(t, var_types)};
}

struct FunctionDecl {
  std::string name;
  std::string raw_return_type;
  std::vector<std::string> param_names;
  std::vector<std::string> param_types;
  bool has_return = false;
  bool has_return_value = false;
};

bool parse_function_header(const std::string& line, FunctionDecl& out) {
  static const std::regex pattern("^function @([A-Za-z_][A-Za-z0-9_]*)\\((.*)\\) -> ([^ ]+) \\{$",
                                  std::regex::ECMAScript);
  std::smatch match;
  if (!std::regex_match(line, match, pattern)) {
    return false;
  }
  out = {};
  out.name = match[1].str();
  out.raw_return_type = match[3].str();
  const auto params = trim_ws(match[2].str());
  if (!params.empty()) {
    for (const auto& item : split_csv_args(params)) {
      const auto pos = item.find(':');
      if (pos == std::string::npos) {
        out.param_names.push_back(item);
        out.param_types.push_back("i64");
        continue;
      }
      out.param_names.push_back(trim_ws(item.substr(0, pos)));
      out.param_types.push_back(trim_ws(item.substr(pos + 1)));
    }
  }
  return true;
}

bool match_scalar_assignment(const std::string& line, std::string& lhs, std::string& rhs) {
  static const std::regex assign_pattern("^([A-Za-z_%][A-Za-z0-9_]*|%[A-Za-z0-9_]+)\\s*=\\s*(.+)$", std::regex::ECMAScript);
  std::smatch match;
  if (!std::regex_match(line, match, assign_pattern)) {
    return false;
  }
  lhs = trim_ws(match[1].str());
  rhs = trim_ws(match[2].str());
  return true;
}

void emit_line_to(std::ostringstream& out, int indent, const std::string& text) {
  if (!text.empty()) {
    out << std::string(indent * 2, ' ') << text << "\n";
  } else {
    out << "\n";
  }
}
