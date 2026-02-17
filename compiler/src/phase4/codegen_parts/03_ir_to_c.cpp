IRToCResult IRToCGenerator::translate(const std::string& ir, const IRToCOptions& options) {
  IRToCResult result;
  std::ostringstream generated;
  std::istringstream source(ir);
  std::vector<std::string> lines;
  for (std::string line; std::getline(source, line);) {
    lines.push_back(line);
  }

  std::unordered_map<std::string, std::string> function_returns;
  std::unordered_map<std::string, FunctionDecl> declaration_by_name;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const auto line = trim_ws(lines[i]);
    if (line.empty()) {
      continue;
    }
    FunctionDecl decl;
    if (parse_function_header(line, decl)) {
      for (std::size_t body = i + 1; body < lines.size(); ++body) {
        const auto body_line = trim_ws(lines[body]);
        if (body_line == "}") {
          break;
        }
        if (body_line.rfind("return", 0) == 0) {
          decl.has_return = true;
          const auto suffix = trim_ws(body_line.substr(6));
          if (!suffix.empty()) {
            decl.has_return_value = true;
          }
        }
      }
      declaration_by_name[decl.name] = decl;
      function_returns[decl.name] = decl.raw_return_type;
    }
  }

  generated << "#include <stdbool.h>\n";
  generated << "#include <stdint.h>\n";
  generated << "#include <stdio.h>\n";
  generated << "#include <stdlib.h>\n";
  generated << "#include <string.h>\n";
  generated << "#if defined(__clang__) || defined(__GNUC__)\n";
  generated << "#define SPARK_FORCE_INLINE __attribute__((always_inline)) inline\n";
  generated << "#define SPARK_RESTRICT __restrict__\n";
  generated << "#define SPARK_LIKELY(x) __builtin_expect(!!(x), 1)\n";
  generated << "#define SPARK_UNLIKELY(x) __builtin_expect(!!(x), 0)\n";
  generated << "#define SPARK_ASSUME_ALIGNED64(ptr) (__typeof__(ptr))__builtin_assume_aligned((ptr), 64)\n";
  generated << "#else\n";
  generated << "#define SPARK_FORCE_INLINE inline\n";
  generated << "#define SPARK_RESTRICT\n";
  generated << "#define SPARK_LIKELY(x) (x)\n";
  generated << "#define SPARK_UNLIKELY(x) (x)\n";
  generated << "#define SPARK_ASSUME_ALIGNED64(ptr) (ptr)\n";
  generated << "#endif\n";
  generated << "typedef long long i64;\n";
  generated << "typedef double f64;\n\n";
  generated << "static void __spark_print_i64(i64 value) { printf(\"%lld\\n\", value); }\n";
  generated << "static void __spark_print_f64(f64 value) { printf(\"%.15g\\n\", value); }\n";
  generated << "static void __spark_print_bool(bool value) { printf(\"%s\\n\", value ? \"True\" : \"False\"); }\n\n";
  generated << "typedef struct { i64* data; i64 size; i64 capacity; } __spark_list_i64;\n";
  generated << "typedef struct { f64* data; i64 size; i64 capacity; } __spark_list_f64;\n";
  generated << "typedef struct { i64* data; i64 rows; i64 cols; } __spark_matrix_i64;\n";
  generated << "typedef struct { f64* data; i64 rows; i64 cols; } __spark_matrix_f64;\n\n";
  generated << "static i64 __spark_max_i64(i64 a, i64 b) { return (a > b) ? a : b; }\n";
  generated << "static void* __spark_alloc_aligned64(size_t bytes) {\n";
  generated << "  if (bytes == 0) return NULL;\n";
  generated << "#if defined(_POSIX_VERSION)\n";
  generated << "  void* out = NULL;\n";
  generated << "  if (posix_memalign(&out, 64u, bytes) == 0) return out;\n";
  generated << "#endif\n";
  generated << "  return malloc(bytes);\n";
  generated << "}\n";
  generated << "static void __spark_list_ensure_i64(__spark_list_i64* list, i64 required_capacity) {\n";
  generated << "  if (SPARK_LIKELY(required_capacity <= list->capacity)) return;\n";
  generated << "  i64 capacity = list->capacity > 0 ? list->capacity : 4;\n";
  generated << "  while (capacity < required_capacity) {\n";
  generated << "    capacity *= 2;\n";
  generated << "  }\n";
  generated << "  list->data = (i64*)realloc(list->data, (size_t)capacity * sizeof(i64));\n";
  generated << "  list->capacity = capacity;\n";
  generated << "}\n";
  generated << "static void __spark_list_ensure_f64(__spark_list_f64* list, i64 required_capacity) {\n";
  generated << "  if (SPARK_LIKELY(required_capacity <= list->capacity)) return;\n";
  generated << "  i64 capacity = list->capacity > 0 ? list->capacity : 4;\n";
  generated << "  while (capacity < required_capacity) {\n";
  generated << "    capacity *= 2;\n";
  generated << "  }\n";
  generated << "  list->data = (f64*)realloc(list->data, (size_t)capacity * sizeof(f64));\n";
  generated << "  list->capacity = capacity;\n";
  generated << "}\n";
  generated << "static __spark_list_i64* __spark_list_new_i64(i64 capacity) {\n";
  generated << "  __spark_list_i64* out = (__spark_list_i64*)malloc(sizeof(__spark_list_i64));\n";
  generated << "  if (!out) return NULL;\n";
  generated << "  out->size = 0;\n";
  generated << "  out->capacity = (capacity > 0) ? capacity : 0;\n";
  generated << "  out->data = out->capacity ? (i64*)malloc((size_t)out->capacity * sizeof(i64)) : NULL;\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_list_f64* __spark_list_new_f64(i64 capacity) {\n";
  generated << "  __spark_list_f64* out = (__spark_list_f64*)malloc(sizeof(__spark_list_f64));\n";
  generated << "  if (!out) return NULL;\n";
  generated << "  out->size = 0;\n";
  generated << "  out->capacity = (capacity > 0) ? capacity : 0;\n";
  generated << "  out->data = out->capacity ? (f64*)malloc((size_t)out->capacity * sizeof(f64)) : NULL;\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static SPARK_FORCE_INLINE i64 __spark_list_len_i64(const __spark_list_i64* list) { return list ? list->size : 0; }\n";
  generated << "static SPARK_FORCE_INLINE i64 __spark_list_get_i64(__spark_list_i64* list, i64 index) { return list->data[index]; }\n";
  generated << "static SPARK_FORCE_INLINE void __spark_list_set_i64(__spark_list_i64* list, i64 index, i64 value) { list->data[index] = value; }\n";
  generated << "static SPARK_FORCE_INLINE void __spark_list_append_unchecked_i64(__spark_list_i64* list, i64 value) {\n";
  generated << "  list->data[list->size] = value;\n";
  generated << "  list->size += 1;\n";
  generated << "}\n";
  generated << "static SPARK_FORCE_INLINE void __spark_list_append_i64(__spark_list_i64* list, i64 value) {\n";
  generated << "  if (!list) return;\n";
  generated << "  if (list->size >= list->capacity) {\n";
  generated << "    __spark_list_ensure_i64(list, list->size + 1);\n";
  generated << "  }\n";
  generated << "  __spark_list_append_unchecked_i64(list, value);\n";
  generated << "}\n";
  generated << "static i64 __spark_list_pop_i64(__spark_list_i64* list, i64 index) {\n";
  generated << "  if (!list || list->size <= 0) return 0;\n";
  generated << "  if (index < 0) index += list->size;\n";
  generated << "  if (index < 0) index = 0;\n";
  generated << "  if (index >= list->size) index = list->size - 1;\n";
  generated << "  const i64 out = list->data[index];\n";
  generated << "  if (index + 1 < list->size) {\n";
  generated << "    memmove(&list->data[index], &list->data[index + 1], (size_t)(list->size - index - 1) * sizeof(i64));\n";
  generated << "  }\n";
  generated << "  list->size -= 1;\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static void __spark_list_insert_i64(__spark_list_i64* list, i64 index, i64 value) {\n";
  generated << "  if (!list) return;\n";
  generated << "  if (index < 0) index += list->size;\n";
  generated << "  if (index < 0) index = 0;\n";
  generated << "  if (index > list->size) index = list->size;\n";
  generated << "  __spark_list_ensure_i64(list, list->size + 1);\n";
  generated << "  if (index < list->size) {\n";
  generated << "    memmove(&list->data[index + 1], &list->data[index], (size_t)(list->size - index) * sizeof(i64));\n";
  generated << "  }\n";
  generated << "  list->data[index] = value;\n";
  generated << "  list->size += 1;\n";
  generated << "}\n";
  generated << "static void __spark_list_remove_i64(__spark_list_i64* list, i64 value) {\n";
  generated << "  if (!list || list->size <= 0) return;\n";
  generated << "  for (i64 i = 0; i < list->size; ++i) {\n";
  generated << "    if (list->data[i] == value) {\n";
  generated << "      if (i + 1 < list->size) {\n";
  generated << "        memmove(&list->data[i], &list->data[i + 1], (size_t)(list->size - i - 1) * sizeof(i64));\n";
  generated << "      }\n";
  generated << "      list->size -= 1;\n";
  generated << "      return;\n";
  generated << "    }\n";
  generated << "  }\n";
  generated << "}\n";
  generated << "static __spark_list_i64* __spark_list_slice_i64(__spark_list_i64* list, i64 start, i64 stop, i64 step) {\n";
  generated << "  if (step == 0) {\n";
  generated << "    return __spark_list_new_i64(0);\n";
  generated << "  }\n";
  generated << "  if (!list) {\n";
  generated << "    return __spark_list_new_i64(0);\n";
  generated << "  }\n";
  generated << "  if (start < 0) {\n";
  generated << "    start = __spark_max_i64(0, list->size + start);\n";
  generated << "  }\n";
  generated << "  if (stop < 0) {\n";
  generated << "    stop = __spark_max_i64(0, list->size + stop);\n";
  generated << "  }\n";
  generated << "  if (start < 0) start = 0;\n";
  generated << "  if (stop > list->size) stop = list->size;\n";
  generated << "  if (start > stop) {\n";
  generated << "    i64 tmp = start; start = stop; stop = tmp;\n";
  generated << "  }\n";
  generated << "  i64 count = 0;\n";
  generated << "  for (i64 i = start; i < stop; i += step) {\n";
  generated << "    if (i >= 0 && i < list->size) {\n";
  generated << "      ++count;\n";
  generated << "    }\n";
  generated << "  }\n";
  generated << "  __spark_list_i64* out = __spark_list_new_i64(count);\n";
  generated << "  if (!out) return NULL;\n";
  generated << "  for (i64 i = start; i < stop; i += step) {\n";
  generated << "    if (i >= 0 && i < list->size) {\n";
  generated << "      __spark_list_ensure_i64(out, out->size + 1);\n";
  generated << "      __spark_list_set_i64(out, out->size, __spark_list_get_i64(list, i));\n";
  generated << "      out->size++;\n";
  generated << "    }\n";
  generated << "  }\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static SPARK_FORCE_INLINE f64 __spark_list_get_f64(__spark_list_f64* list, i64 index) { return list->data[index]; }\n";
  generated << "static SPARK_FORCE_INLINE void __spark_list_set_f64(__spark_list_f64* list, i64 index, f64 value) { list->data[index] = value; }\n";
  generated << "static SPARK_FORCE_INLINE void __spark_list_append_unchecked_f64(__spark_list_f64* list, f64 value) {\n";
  generated << "  list->data[list->size] = value;\n";
  generated << "  list->size += 1;\n";
  generated << "}\n";
  generated << "static SPARK_FORCE_INLINE void __spark_list_append_f64(__spark_list_f64* list, f64 value) {\n";
  generated << "  if (!list) return;\n";
  generated << "  if (list->size >= list->capacity) {\n";
  generated << "    __spark_list_ensure_f64(list, list->size + 1);\n";
  generated << "  }\n";
  generated << "  __spark_list_append_unchecked_f64(list, value);\n";
  generated << "}\n";
  generated << "static f64 __spark_list_pop_f64(__spark_list_f64* list, i64 index) {\n";
  generated << "  if (!list || list->size <= 0) return 0.0;\n";
  generated << "  if (index < 0) index += list->size;\n";
  generated << "  if (index < 0) index = 0;\n";
  generated << "  if (index >= list->size) index = list->size - 1;\n";
  generated << "  const f64 out = list->data[index];\n";
  generated << "  if (index + 1 < list->size) {\n";
  generated << "    memmove(&list->data[index], &list->data[index + 1], (size_t)(list->size - index - 1) * sizeof(f64));\n";
  generated << "  }\n";
  generated << "  list->size -= 1;\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static void __spark_list_insert_f64(__spark_list_f64* list, i64 index, f64 value) {\n";
  generated << "  if (!list) return;\n";
  generated << "  if (index < 0) index += list->size;\n";
  generated << "  if (index < 0) index = 0;\n";
  generated << "  if (index > list->size) index = list->size;\n";
  generated << "  __spark_list_ensure_f64(list, list->size + 1);\n";
  generated << "  if (index < list->size) {\n";
  generated << "    memmove(&list->data[index + 1], &list->data[index], (size_t)(list->size - index) * sizeof(f64));\n";
  generated << "  }\n";
  generated << "  list->data[index] = value;\n";
  generated << "  list->size += 1;\n";
  generated << "}\n";
  generated << "static void __spark_list_remove_f64(__spark_list_f64* list, f64 value) {\n";
  generated << "  if (!list || list->size <= 0) return;\n";
  generated << "  for (i64 i = 0; i < list->size; ++i) {\n";
  generated << "    if (list->data[i] == value) {\n";
  generated << "      if (i + 1 < list->size) {\n";
  generated << "        memmove(&list->data[i], &list->data[i + 1], (size_t)(list->size - i - 1) * sizeof(f64));\n";
  generated << "      }\n";
  generated << "      list->size -= 1;\n";
  generated << "      return;\n";
  generated << "    }\n";
  generated << "  }\n";
  generated << "}\n";
  generated << "static inline i64 __spark_list_len_f64(__spark_list_f64* list) { return list ? list->size : 0; }\n";
  generated << "static i64 __spark_list_get_len(const void* list, const char* kind) { return 0; }\n";
  generated << "static __spark_list_f64* __spark_list_new_f64_from_list(__spark_list_f64* list) {\n";
  generated << "  __spark_list_f64* out = __spark_list_new_f64(0);\n";
  generated << "  if (!out) return NULL;\n";
  generated << "  if (!list) return out;\n";
  generated << "  for (i64 i = 0; i < list->size; ++i) {\n";
  generated << "    __spark_list_append_f64(out, list->data[i]);\n";
  generated << "  }\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_list_f64* __spark_list_slice_f64(__spark_list_f64* list, i64 start, i64 stop, i64 step) {\n";
  generated << "  if (step == 0) {\n";
  generated << "    return __spark_list_new_f64(0);\n";
  generated << "  }\n";
  generated << "  if (!list) {\n";
  generated << "    return __spark_list_new_f64(0);\n";
  generated << "  }\n";
  generated << "  if (start < 0) start = __spark_max_i64(0, list->size + start);\n";
  generated << "  if (stop < 0) stop = __spark_max_i64(0, list->size + stop);\n";
  generated << "  if (start < 0) start = 0;\n";
  generated << "  if (stop > list->size) stop = list->size;\n";
  generated << "  if (start > stop) {\n";
  generated << "    i64 tmp = start; start = stop; stop = tmp;\n";
  generated << "  }\n";
  generated << "  i64 count = 0;\n";
  generated << "  for (i64 i = start; i < stop; i += step) {\n";
  generated << "    if (i >= 0 && i < list->size) {\n";
  generated << "      ++count;\n";
  generated << "    }\n";
  generated << "  }\n";
  generated << "  __spark_list_f64* out = __spark_list_new_f64(count);\n";
  generated << "  if (!out) return NULL;\n";
  generated << "  for (i64 i = start; i < stop; i += step) {\n";
  generated << "    if (i >= 0 && i < list->size) {\n";
  generated << "      __spark_list_ensure_f64(out, out->size + 1);\n";
  generated << "      __spark_list_set_f64(out, out->size, __spark_list_get_f64(list, i));\n";
  generated << "      out->size++;\n";
  generated << "    }\n";
  generated << "  }\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_matrix_i64* __spark_matrix_new_i64(i64 rows, i64 cols) {\n";
  generated << "  if (rows < 0 || cols < 0) return NULL;\n";
  generated << "  __spark_matrix_i64* out = (__spark_matrix_i64*)malloc(sizeof(__spark_matrix_i64));\n";
  generated << "  if (!out) return NULL;\n";
  generated << "  out->rows = rows;\n";
  generated << "  out->cols = cols;\n";
  generated << "  const i64 count = rows * cols;\n";
  generated << "  out->data = count > 0 ? (i64*)__spark_alloc_aligned64((size_t)count * sizeof(i64)) : NULL;\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_matrix_f64* __spark_matrix_new_f64(i64 rows, i64 cols) {\n";
  generated << "  if (rows < 0 || cols < 0) return NULL;\n";
  generated << "  __spark_matrix_f64* out = (__spark_matrix_f64*)malloc(sizeof(__spark_matrix_f64));\n";
  generated << "  if (!out) return NULL;\n";
  generated << "  out->rows = rows;\n";
  generated << "  out->cols = cols;\n";
  generated << "  const i64 count = rows * cols;\n";
  generated << "  out->data = count > 0 ? (f64*)__spark_alloc_aligned64((size_t)count * sizeof(f64)) : NULL;\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static SPARK_FORCE_INLINE i64 __spark_matrix_len_rows_i64(const __spark_matrix_i64* matrix) { return matrix ? matrix->rows : 0; }\n";
  generated << "static SPARK_FORCE_INLINE i64 __spark_matrix_get_i64(const __spark_matrix_i64* matrix, i64 row, i64 col) {\n";
  generated << "  const i64* SPARK_RESTRICT data = SPARK_ASSUME_ALIGNED64(matrix->data);\n";
  generated << "  return data[row * matrix->cols + col];\n";
  generated << "}\n";
  generated << "static SPARK_FORCE_INLINE void __spark_matrix_set_i64(__spark_matrix_i64* matrix, i64 row, i64 col, i64 value) {\n";
  generated << "  i64* SPARK_RESTRICT data = SPARK_ASSUME_ALIGNED64(matrix->data);\n";
  generated << "  data[row * matrix->cols + col] = value;\n";
  generated << "}\n";
  generated << "static SPARK_FORCE_INLINE f64 __spark_matrix_get_f64(const __spark_matrix_f64* matrix, i64 row, i64 col) {\n";
  generated << "  const f64* SPARK_RESTRICT data = SPARK_ASSUME_ALIGNED64(matrix->data);\n";
  generated << "  return data[row * matrix->cols + col];\n";
  generated << "}\n";
  generated << "static SPARK_FORCE_INLINE void __spark_matrix_set_f64(__spark_matrix_f64* matrix, i64 row, i64 col, f64 value) {\n";
  generated << "  f64* SPARK_RESTRICT data = SPARK_ASSUME_ALIGNED64(matrix->data);\n";
  generated << "  data[row * matrix->cols + col] = value;\n";
  generated << "}\n";
  generated << "static SPARK_FORCE_INLINE i64 __spark_matrix_len_cols_i64(const __spark_matrix_i64* matrix) { return matrix ? matrix->cols : 0; }\n";
  generated << "static __spark_list_i64* __spark_matrix_row_i64(const __spark_matrix_i64* matrix, i64 row) {\n";
  generated << "  if (!matrix || row < 0 || row >= matrix->rows) {\n";
  generated << "    return __spark_list_new_i64(0);\n";
  generated << "  }\n";
  generated << "  __spark_list_i64* out = __spark_list_new_i64(matrix->cols);\n";
  generated << "  if (!out) return NULL;\n";
  generated << "  out->size = matrix->cols;\n";
  generated << "  memcpy(out->data, matrix->data + row * matrix->cols, (size_t)matrix->cols * sizeof(i64));\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_list_f64* __spark_matrix_row_f64(const __spark_matrix_f64* matrix, i64 row) {\n";
  generated << "  if (!matrix || row < 0 || row >= matrix->rows) {\n";
  generated << "    return __spark_list_new_f64(0);\n";
  generated << "  }\n";
  generated << "  __spark_list_f64* out = __spark_list_new_f64(matrix->cols);\n";
  generated << "  if (!out) return NULL;\n";
  generated << "  out->size = matrix->cols;\n";
  generated << "  memcpy(out->data, matrix->data + row * matrix->cols, (size_t)matrix->cols * sizeof(f64));\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static SPARK_FORCE_INLINE i64 __spark_matrix_len_rows_f64(const __spark_matrix_f64* matrix) { return matrix ? matrix->rows : 0; }\n";
  generated << "static SPARK_FORCE_INLINE i64 __spark_matrix_len_cols_f64(const __spark_matrix_f64* matrix) { return matrix ? matrix->cols : 0; }\n";
  generated << "static i64 __spark_slice_start(i64 size, i64 start) {\n";
  generated << "  if (start < 0) start += size;\n";
  generated << "  if (start < 0) start = 0;\n";
  generated << "  if (start > size) start = size;\n";
  generated << "  return start;\n";
  generated << "}\n";
  generated << "static i64 __spark_slice_stop(i64 size, i64 stop) {\n";
  generated << "  if (stop < 0) stop += size;\n";
  generated << "  if (stop < 0) stop = 0;\n";
  generated << "  if (stop > size) stop = size;\n";
  generated << "  return stop;\n";
  generated << "}\n";
  generated << "static __spark_list_i64* __spark_matrix_col_i64(const __spark_matrix_i64* matrix, i64 col) {\n";
  generated << "  if (!matrix) return __spark_list_new_i64(0);\n";
  generated << "  if (col < 0) col += matrix->cols;\n";
  generated << "  if (col < 0 || col >= matrix->cols) return __spark_list_new_i64(0);\n";
  generated << "  __spark_list_i64* out = __spark_list_new_i64(matrix->rows);\n";
  generated << "  if (!out) return NULL;\n";
  generated << "  out->size = matrix->rows;\n";
  generated << "  for (i64 r = 0; r < matrix->rows; ++r) out->data[r] = matrix->data[r * matrix->cols + col];\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_list_f64* __spark_matrix_col_f64(const __spark_matrix_f64* matrix, i64 col) {\n";
  generated << "  if (!matrix) return __spark_list_new_f64(0);\n";
  generated << "  if (col < 0) col += matrix->cols;\n";
  generated << "  if (col < 0 || col >= matrix->cols) return __spark_list_new_f64(0);\n";
  generated << "  __spark_list_f64* out = __spark_list_new_f64(matrix->rows);\n";
  generated << "  if (!out) return NULL;\n";
  generated << "  out->size = matrix->rows;\n";
  generated << "  for (i64 r = 0; r < matrix->rows; ++r) out->data[r] = matrix->data[r * matrix->cols + col];\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_list_i64* __spark_matrix_rows_col_i64(const __spark_matrix_i64* matrix, i64 row_start, i64 row_stop, i64 row_step, i64 col) {\n";
  generated << "  if (!matrix || row_step == 0) return __spark_list_new_i64(0);\n";
  generated << "  if (col < 0) col += matrix->cols;\n";
  generated << "  if (col < 0 || col >= matrix->cols) return __spark_list_new_i64(0);\n";
  generated << "  row_start = __spark_slice_start(matrix->rows, row_start);\n";
  generated << "  row_stop = __spark_slice_stop(matrix->rows, row_stop);\n";
  generated << "  i64 count = 0;\n";
  generated << "  for (i64 r = row_start; (row_step > 0) ? (r < row_stop) : (r > row_stop); r += row_step) {\n";
  generated << "    ++count;\n";
  generated << "  }\n";
  generated << "  __spark_list_i64* out = __spark_list_new_i64(count);\n";
  generated << "  if (!out) return NULL;\n";
  generated << "  for (i64 r = row_start; (row_step > 0) ? (r < row_stop) : (r > row_stop); r += row_step) {\n";
  generated << "    out->data[out->size++] = matrix->data[r * matrix->cols + col];\n";
  generated << "  }\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_list_f64* __spark_matrix_rows_col_f64(const __spark_matrix_f64* matrix, i64 row_start, i64 row_stop, i64 row_step, i64 col) {\n";
  generated << "  if (!matrix || row_step == 0) return __spark_list_new_f64(0);\n";
  generated << "  if (col < 0) col += matrix->cols;\n";
  generated << "  if (col < 0 || col >= matrix->cols) return __spark_list_new_f64(0);\n";
  generated << "  row_start = __spark_slice_start(matrix->rows, row_start);\n";
  generated << "  row_stop = __spark_slice_stop(matrix->rows, row_stop);\n";
  generated << "  i64 count = 0;\n";
  generated << "  for (i64 r = row_start; (row_step > 0) ? (r < row_stop) : (r > row_stop); r += row_step) {\n";
  generated << "    ++count;\n";
  generated << "  }\n";
  generated << "  __spark_list_f64* out = __spark_list_new_f64(count);\n";
  generated << "  if (!out) return NULL;\n";
  generated << "  for (i64 r = row_start; (row_step > 0) ? (r < row_stop) : (r > row_stop); r += row_step) {\n";
  generated << "    out->data[out->size++] = matrix->data[r * matrix->cols + col];\n";
  generated << "  }\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_matrix_i64* __spark_matrix_slice_rows_i64(const __spark_matrix_i64* matrix, i64 row_start, i64 row_stop, i64 row_step) {\n";
  generated << "  if (!matrix || row_step == 0) return __spark_matrix_new_i64(0, 0);\n";
  generated << "  row_start = __spark_slice_start(matrix->rows, row_start);\n";
  generated << "  row_stop = __spark_slice_stop(matrix->rows, row_stop);\n";
  generated << "  i64 row_count = 0;\n";
  generated << "  for (i64 r = row_start; (row_step > 0) ? (r < row_stop) : (r > row_stop); r += row_step) ++row_count;\n";
  generated << "  __spark_matrix_i64* out = __spark_matrix_new_i64(row_count, matrix->cols);\n";
  generated << "  if (!out) return NULL;\n";
  generated << "  i64 out_row = 0;\n";
  generated << "  for (i64 r = row_start; (row_step > 0) ? (r < row_stop) : (r > row_stop); r += row_step) {\n";
  generated << "    memcpy(out->data + out_row * out->cols, matrix->data + r * matrix->cols, (size_t)matrix->cols * sizeof(i64));\n";
  generated << "    ++out_row;\n";
  generated << "  }\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_matrix_f64* __spark_matrix_slice_rows_f64(const __spark_matrix_f64* matrix, i64 row_start, i64 row_stop, i64 row_step) {\n";
  generated << "  if (!matrix || row_step == 0) return __spark_matrix_new_f64(0, 0);\n";
  generated << "  row_start = __spark_slice_start(matrix->rows, row_start);\n";
  generated << "  row_stop = __spark_slice_stop(matrix->rows, row_stop);\n";
  generated << "  i64 row_count = 0;\n";
  generated << "  for (i64 r = row_start; (row_step > 0) ? (r < row_stop) : (r > row_stop); r += row_step) ++row_count;\n";
  generated << "  __spark_matrix_f64* out = __spark_matrix_new_f64(row_count, matrix->cols);\n";
  generated << "  if (!out) return NULL;\n";
  generated << "  i64 out_row = 0;\n";
  generated << "  for (i64 r = row_start; (row_step > 0) ? (r < row_stop) : (r > row_stop); r += row_step) {\n";
  generated << "    memcpy(out->data + out_row * out->cols, matrix->data + r * matrix->cols, (size_t)matrix->cols * sizeof(f64));\n";
  generated << "    ++out_row;\n";
  generated << "  }\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_matrix_i64* __spark_matrix_slice_cols_i64(const __spark_matrix_i64* matrix, i64 col_start, i64 col_stop, i64 col_step) {\n";
  generated << "  if (!matrix || col_step == 0) return __spark_matrix_new_i64(0, 0);\n";
  generated << "  col_start = __spark_slice_start(matrix->cols, col_start);\n";
  generated << "  col_stop = __spark_slice_stop(matrix->cols, col_stop);\n";
  generated << "  i64 col_count = 0;\n";
  generated << "  for (i64 c = col_start; (col_step > 0) ? (c < col_stop) : (c > col_stop); c += col_step) ++col_count;\n";
  generated << "  __spark_matrix_i64* out = __spark_matrix_new_i64(matrix->rows, col_count);\n";
  generated << "  if (!out) return NULL;\n";
  generated << "  for (i64 r = 0; r < matrix->rows; ++r) {\n";
  generated << "    i64 out_col = 0;\n";
  generated << "    for (i64 c = col_start; (col_step > 0) ? (c < col_stop) : (c > col_stop); c += col_step) {\n";
  generated << "      out->data[r * out->cols + out_col] = matrix->data[r * matrix->cols + c];\n";
  generated << "      ++out_col;\n";
  generated << "    }\n";
  generated << "  }\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_matrix_f64* __spark_matrix_slice_cols_f64(const __spark_matrix_f64* matrix, i64 col_start, i64 col_stop, i64 col_step) {\n";
  generated << "  if (!matrix || col_step == 0) return __spark_matrix_new_f64(0, 0);\n";
  generated << "  col_start = __spark_slice_start(matrix->cols, col_start);\n";
  generated << "  col_stop = __spark_slice_stop(matrix->cols, col_stop);\n";
  generated << "  i64 col_count = 0;\n";
  generated << "  for (i64 c = col_start; (col_step > 0) ? (c < col_stop) : (c > col_stop); c += col_step) ++col_count;\n";
  generated << "  __spark_matrix_f64* out = __spark_matrix_new_f64(matrix->rows, col_count);\n";
  generated << "  if (!out) return NULL;\n";
  generated << "  for (i64 r = 0; r < matrix->rows; ++r) {\n";
  generated << "    i64 out_col = 0;\n";
  generated << "    for (i64 c = col_start; (col_step > 0) ? (c < col_stop) : (c > col_stop); c += col_step) {\n";
  generated << "      out->data[r * out->cols + out_col] = matrix->data[r * matrix->cols + c];\n";
  generated << "      ++out_col;\n";
  generated << "    }\n";
  generated << "  }\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_matrix_i64* __spark_matrix_slice_block_i64(const __spark_matrix_i64* matrix, i64 row_start, i64 row_stop, i64 row_step, i64 col_start, i64 col_stop, i64 col_step) {\n";
  generated << "  if (!matrix || row_step == 0 || col_step == 0) return __spark_matrix_new_i64(0, 0);\n";
  generated << "  row_start = __spark_slice_start(matrix->rows, row_start);\n";
  generated << "  row_stop = __spark_slice_stop(matrix->rows, row_stop);\n";
  generated << "  col_start = __spark_slice_start(matrix->cols, col_start);\n";
  generated << "  col_stop = __spark_slice_stop(matrix->cols, col_stop);\n";
  generated << "  i64 row_count = 0;\n";
  generated << "  i64 col_count = 0;\n";
  generated << "  for (i64 r = row_start; (row_step > 0) ? (r < row_stop) : (r > row_stop); r += row_step) ++row_count;\n";
  generated << "  for (i64 c = col_start; (col_step > 0) ? (c < col_stop) : (c > col_stop); c += col_step) ++col_count;\n";
  generated << "  __spark_matrix_i64* out = __spark_matrix_new_i64(row_count, col_count);\n";
  generated << "  if (!out) return NULL;\n";
  generated << "  i64 out_row = 0;\n";
  generated << "  for (i64 r = row_start; (row_step > 0) ? (r < row_stop) : (r > row_stop); r += row_step) {\n";
  generated << "    i64 out_col = 0;\n";
  generated << "    for (i64 c = col_start; (col_step > 0) ? (c < col_stop) : (c > col_stop); c += col_step) {\n";
  generated << "      out->data[out_row * out->cols + out_col] = matrix->data[r * matrix->cols + c];\n";
  generated << "      ++out_col;\n";
  generated << "    }\n";
  generated << "    ++out_row;\n";
  generated << "  }\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_matrix_f64* __spark_matrix_slice_block_f64(const __spark_matrix_f64* matrix, i64 row_start, i64 row_stop, i64 row_step, i64 col_start, i64 col_stop, i64 col_step) {\n";
  generated << "  if (!matrix || row_step == 0 || col_step == 0) return __spark_matrix_new_f64(0, 0);\n";
  generated << "  row_start = __spark_slice_start(matrix->rows, row_start);\n";
  generated << "  row_stop = __spark_slice_stop(matrix->rows, row_stop);\n";
  generated << "  col_start = __spark_slice_start(matrix->cols, col_start);\n";
  generated << "  col_stop = __spark_slice_stop(matrix->cols, col_stop);\n";
  generated << "  i64 row_count = 0;\n";
  generated << "  i64 col_count = 0;\n";
  generated << "  for (i64 r = row_start; (row_step > 0) ? (r < row_stop) : (r > row_stop); r += row_step) ++row_count;\n";
  generated << "  for (i64 c = col_start; (col_step > 0) ? (c < col_stop) : (c > col_stop); c += col_step) ++col_count;\n";
  generated << "  __spark_matrix_f64* out = __spark_matrix_new_f64(row_count, col_count);\n";
  generated << "  if (!out) return NULL;\n";
  generated << "  i64 out_row = 0;\n";
  generated << "  for (i64 r = row_start; (row_step > 0) ? (r < row_stop) : (r > row_stop); r += row_step) {\n";
  generated << "    i64 out_col = 0;\n";
  generated << "    for (i64 c = col_start; (col_step > 0) ? (c < col_stop) : (c > col_stop); c += col_step) {\n";
  generated << "      out->data[out_row * out->cols + out_col] = matrix->data[r * matrix->cols + c];\n";
  generated << "      ++out_col;\n";
  generated << "    }\n";
  generated << "    ++out_row;\n";
  generated << "  }\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static void __spark_matrix_set_row_i64(__spark_matrix_i64* matrix, i64 row, const __spark_list_i64* values) {\n";
  generated << "  if (!matrix || !values) return;\n";
  generated << "  if (row < 0) row += matrix->rows;\n";
  generated << "  if (row < 0 || row >= matrix->rows) return;\n";
  generated << "  const i64 limit = (values->size < matrix->cols) ? values->size : matrix->cols;\n";
  generated << "  for (i64 c = 0; c < limit; ++c) matrix->data[row * matrix->cols + c] = values->data[c];\n";
  generated << "}\n";
  generated << "static void __spark_matrix_set_row_f64(__spark_matrix_f64* matrix, i64 row, const __spark_list_f64* values) {\n";
  generated << "  if (!matrix || !values) return;\n";
  generated << "  if (row < 0) row += matrix->rows;\n";
  generated << "  if (row < 0 || row >= matrix->rows) return;\n";
  generated << "  const i64 limit = (values->size < matrix->cols) ? values->size : matrix->cols;\n";
  generated << "  for (i64 c = 0; c < limit; ++c) matrix->data[row * matrix->cols + c] = values->data[c];\n";
  generated << "}\n";
  generated << "static __spark_matrix_i64* __spark_matrix_add_i64(__spark_matrix_i64* lhs, __spark_matrix_i64* rhs) {\n";
  generated << "  if (!lhs || !rhs) return __spark_matrix_new_i64(0, 0);\n";
  generated << "  if (lhs->rows != rhs->rows || lhs->cols != rhs->cols) return __spark_matrix_new_i64(0, 0);\n";
  generated << "  __spark_matrix_i64* out = __spark_matrix_new_i64(lhs->rows, lhs->cols);\n";
  generated << "  if (!out) return NULL;\n";
  generated << "  for (i64 i = 0; i < lhs->rows * lhs->cols; ++i) out->data[i] = lhs->data[i] + rhs->data[i];\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_matrix_f64* __spark_matrix_add_f64(__spark_matrix_f64* lhs, __spark_matrix_f64* rhs) {\n";
  generated << "  if (!lhs || !rhs) return __spark_matrix_new_f64(0, 0);\n";
  generated << "  if (lhs->rows != rhs->rows || lhs->cols != rhs->cols) return __spark_matrix_new_f64(0, 0);\n";
  generated << "  __spark_matrix_f64* out = __spark_matrix_new_f64(lhs->rows, lhs->cols);\n";
  generated << "  if (!out) return NULL;\n";
  generated << "  for (i64 i = 0; i < lhs->rows * lhs->cols; ++i) out->data[i] = lhs->data[i] + rhs->data[i];\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_matrix_i64* __spark_matrix_sub_i64(__spark_matrix_i64* lhs, __spark_matrix_i64* rhs) {\n";
  generated << "  if (!lhs || !rhs) return __spark_matrix_new_i64(0, 0);\n";
  generated << "  if (lhs->rows != rhs->rows || lhs->cols != rhs->cols) return __spark_matrix_new_i64(0, 0);\n";
  generated << "  __spark_matrix_i64* out = __spark_matrix_new_i64(lhs->rows, lhs->cols);\n";
  generated << "  if (!out) return NULL;\n";
  generated << "  for (i64 i = 0; i < lhs->rows * lhs->cols; ++i) out->data[i] = lhs->data[i] - rhs->data[i];\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_matrix_f64* __spark_matrix_sub_f64(__spark_matrix_f64* lhs, __spark_matrix_f64* rhs) {\n";
  generated << "  if (!lhs || !rhs) return __spark_matrix_new_f64(0, 0);\n";
  generated << "  if (lhs->rows != rhs->rows || lhs->cols != rhs->cols) return __spark_matrix_new_f64(0, 0);\n";
  generated << "  __spark_matrix_f64* out = __spark_matrix_new_f64(lhs->rows, lhs->cols);\n";
  generated << "  if (!out) return NULL;\n";
  generated << "  for (i64 i = 0; i < lhs->rows * lhs->cols; ++i) out->data[i] = lhs->data[i] - rhs->data[i];\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_matrix_i64* __spark_matrix_mul_i64(__spark_matrix_i64* lhs, __spark_matrix_i64* rhs) {\n";
  generated << "  __spark_matrix_i64* out = __spark_matrix_new_i64(lhs ? lhs->rows : 0, rhs ? rhs->cols : 0);\n";
  generated << "  if (!out) return NULL;\n";
  generated << "  for (i64 i = 0; i < out->rows * out->cols; ++i) out->data[i] = 0;\n";
  generated << "  for (i64 r = 0; r < (lhs ? lhs->rows : 0); ++r) {\n";
  generated << "    for (i64 c = 0; c < (rhs ? rhs->cols : 0); ++c) {\n";
  generated << "      i64 total = 0;\n";
  generated << "      for (i64 k = 0; k < (lhs ? lhs->cols : 0); ++k) {\n";
  generated << "        if (r < lhs->rows && k < lhs->cols && k < rhs->rows && c < rhs->cols) {\n";
  generated << "          total += __spark_matrix_get_i64(lhs, r, k) * __spark_matrix_get_i64(rhs, k, c);\n";
  generated << "        }\n";
  generated << "      }\n";
  generated << "      if (r < out->rows && c < out->cols) __spark_matrix_set_i64(out, r, c, total);\n";
  generated << "    }\n";
  generated << "  }\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_matrix_f64* __spark_matrix_mul_f64(__spark_matrix_f64* lhs, __spark_matrix_f64* rhs) {\n";
  generated << "  __spark_matrix_f64* out = __spark_matrix_new_f64(lhs ? lhs->rows : 0, rhs ? rhs->cols : 0);\n";
  generated << "  if (!out) return NULL;\n";
  generated << "  for (i64 i = 0; i < out->rows * out->cols; ++i) out->data[i] = 0.0;\n";
  generated << "  for (i64 r = 0; r < (lhs ? lhs->rows : 0); ++r) {\n";
  generated << "    for (i64 c = 0; c < (rhs ? rhs->cols : 0); ++c) {\n";
  generated << "      f64 total = 0.0;\n";
  generated << "      for (i64 k = 0; k < (lhs ? lhs->cols : 0); ++k) {\n";
  generated << "        if (r < lhs->rows && k < lhs->cols && k < rhs->rows && c < rhs->cols) {\n";
  generated << "          total += __spark_matrix_get_f64(lhs, r, k) * __spark_matrix_get_f64(rhs, k, c);\n";
  generated << "        }\n";
  generated << "      }\n";
  generated << "      if (r < out->rows && c < out->cols) __spark_matrix_set_f64(out, r, c, total);\n";
  generated << "    }\n";
  generated << "  }\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_matrix_i64* __spark_matrix_div_i64(__spark_matrix_i64* lhs, __spark_matrix_i64* rhs) {\n";
  generated << "  if (!lhs || !rhs || lhs->rows != rhs->rows || lhs->cols != rhs->cols) {\n";
  generated << "    return __spark_matrix_new_i64(0, 0);\n";
  generated << "  }\n";
  generated << "  __spark_matrix_i64* out = __spark_matrix_new_i64(lhs->rows, lhs->cols);\n";
  generated << "  for (i64 i = 0; i < lhs->rows * lhs->cols; ++i) out->data[i] = rhs->data[i] == 0 ? 0 : lhs->data[i] / rhs->data[i];\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_matrix_f64* __spark_matrix_div_f64(__spark_matrix_f64* lhs, __spark_matrix_f64* rhs) {\n";
  generated << "  if (!lhs || !rhs || lhs->rows != rhs->rows || lhs->cols != rhs->cols) {\n";
  generated << "    return __spark_matrix_new_f64(0, 0);\n";
  generated << "  }\n";
  generated << "  __spark_matrix_f64* out = __spark_matrix_new_f64(lhs->rows, lhs->cols);\n";
  generated << "  for (i64 i = 0; i < lhs->rows * lhs->cols; ++i) out->data[i] = rhs->data[i] == 0.0 ? 0.0 : lhs->data[i] / rhs->data[i];\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_matrix_i64* __spark_matrix_scalar_mul_i64(__spark_matrix_i64* lhs, i64 rhs) {\n";
  generated << "  if (!lhs) return __spark_matrix_new_i64(0, 0);\n";
  generated << "  __spark_matrix_i64* out = __spark_matrix_new_i64(lhs->rows, lhs->cols);\n";
  generated << "  for (i64 i = 0; i < lhs->rows * lhs->cols; ++i) out->data[i] = lhs->data[i] * rhs;\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_matrix_f64* __spark_matrix_scalar_mul_f64(__spark_matrix_f64* lhs, f64 rhs) {\n";
  generated << "  if (!lhs) return __spark_matrix_new_f64(0, 0);\n";
  generated << "  __spark_matrix_f64* out = __spark_matrix_new_f64(lhs->rows, lhs->cols);\n";
  generated << "  for (i64 i = 0; i < lhs->rows * lhs->cols; ++i) out->data[i] = lhs->data[i] * rhs;\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_matrix_i64* __spark_matrix_scalar_add_i64(__spark_matrix_i64* lhs, i64 rhs) {\n";
  generated << "  if (!lhs) return __spark_matrix_new_i64(0, 0);\n";
  generated << "  __spark_matrix_i64* out = __spark_matrix_new_i64(lhs->rows, lhs->cols);\n";
  generated << "  for (i64 i = 0; i < lhs->rows * lhs->cols; ++i) out->data[i] = lhs->data[i] + rhs;\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_matrix_f64* __spark_matrix_scalar_add_f64(__spark_matrix_f64* lhs, f64 rhs) {\n";
  generated << "  if (!lhs) return __spark_matrix_new_f64(0, 0);\n";
  generated << "  __spark_matrix_f64* out = __spark_matrix_new_f64(lhs->rows, lhs->cols);\n";
  generated << "  for (i64 i = 0; i < lhs->rows * lhs->cols; ++i) out->data[i] = lhs->data[i] + rhs;\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_matrix_i64* __spark_matrix_scalar_sub_i64(__spark_matrix_i64* lhs, i64 rhs) {\n";
  generated << "  if (!lhs) return __spark_matrix_new_i64(0, 0);\n";
  generated << "  __spark_matrix_i64* out = __spark_matrix_new_i64(lhs->rows, lhs->cols);\n";
  generated << "  for (i64 i = 0; i < lhs->rows * lhs->cols; ++i) out->data[i] = lhs->data[i] - rhs;\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_matrix_f64* __spark_matrix_scalar_sub_f64(__spark_matrix_f64* lhs, f64 rhs) {\n";
  generated << "  if (!lhs) return __spark_matrix_new_f64(0, 0);\n";
  generated << "  __spark_matrix_f64* out = __spark_matrix_new_f64(lhs->rows, lhs->cols);\n";
  generated << "  for (i64 i = 0; i < lhs->rows * lhs->cols; ++i) out->data[i] = lhs->data[i] - rhs;\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_matrix_i64* __spark_matrix_scalar_div_i64(__spark_matrix_i64* lhs, i64 rhs) {\n";
  generated << "  if (!lhs) return __spark_matrix_new_i64(0, 0);\n";
  generated << "  __spark_matrix_i64* out = __spark_matrix_new_i64(lhs->rows, lhs->cols);\n";
  generated << "  for (i64 i = 0; i < lhs->rows * lhs->cols; ++i) out->data[i] = rhs == 0 ? 0 : lhs->data[i] / rhs;\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_matrix_f64* __spark_matrix_scalar_div_f64(__spark_matrix_f64* lhs, f64 rhs) {\n";
  generated << "  if (!lhs) return __spark_matrix_new_f64(0, 0);\n";
  generated << "  __spark_matrix_f64* out = __spark_matrix_new_f64(lhs->rows, lhs->cols);\n";
  generated << "  for (i64 i = 0; i < lhs->rows * lhs->cols; ++i) out->data[i] = rhs == 0.0 ? 0.0 : lhs->data[i] / rhs;\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_matrix_f64* __spark_matrix_transpose_f64(const __spark_matrix_f64* matrix) {\n";
  generated << "  if (!matrix) return __spark_matrix_new_f64(0, 0);\n";
  generated << "  __spark_matrix_f64* out = __spark_matrix_new_f64(matrix->cols, matrix->rows);\n";
  generated << "  for (i64 r = 0; r < matrix->rows; ++r) {\n";
  generated << "    for (i64 c = 0; c < matrix->cols; ++c) {\n";
  generated << "      __spark_matrix_set_f64(out, c, r, __spark_matrix_get_f64(matrix, r, c));\n";
  generated << "    }\n";
  generated << "  }\n";
  generated << "  return out;\n";
  generated << "}\n";
  generated << "static __spark_matrix_i64* __spark_matrix_transpose_i64(const __spark_matrix_i64* matrix) {\n";
  generated << "  if (!matrix) return __spark_matrix_new_i64(0, 0);\n";
  generated << "  __spark_matrix_i64* out = __spark_matrix_new_i64(matrix->cols, matrix->rows);\n";
  generated << "  for (i64 r = 0; r < matrix->rows; ++r) {\n";
  generated << "    for (i64 c = 0; c < matrix->cols; ++c) {\n";
  generated << "      __spark_matrix_set_i64(out, c, r, __spark_matrix_get_i64(matrix, r, c));\n";
  generated << "    }\n";
  generated << "  }\n";
  generated << "  return out;\n";
  generated << "}\n\n";

  bool in_function = false;
  bool first_function = true;
  std::unordered_map<std::string, std::string> var_types;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string raw = lines[i];
    const auto line = trim_ws(raw);
    if (line.empty()) {
      continue;
    }
    if (line == "module:") {
      continue;
    }

    FunctionDecl header;
    if (parse_function_header(line, header)) {
      in_function = true;
      const bool is_main = (header.name == "__main__");
      if (!first_function) {
        generated << "\n";
      }
      first_function = false;

      const auto iter = declaration_by_name.find(header.name);
      if (iter != declaration_by_name.end()) {
        header.has_return = iter->second.has_return;
        header.has_return_value = iter->second.has_return_value;
      }

      var_types.clear();
      std::vector<std::pair<std::string, std::string>> local_vars;
      std::unordered_set<std::string> declared;

      for (std::size_t p = 0; p < header.param_names.size(); ++p) {
        const auto param = sanitize_identifier(header.param_names[p]);
        var_types[param] = header.param_types[p];
        declared.insert(param);
      }

      std::string return_kind = header.raw_return_type;
      if (is_main) {
        return_kind = "i64";
      } else if (return_kind == "unknown") {
        if (!header.has_return_value) {
          return_kind = "void";
        } else {
          return_kind = "i64";
        }
      }
      function_returns[header.name] = return_kind;
      const auto emitted_name = is_main ? "main" : header.name;
      const auto emitted_type = is_main ? "int" : pseudo_kind_to_cpp(return_kind);
      if (!is_main) {
        generated << "static ";
      }
      generated << emitted_type << " " << emitted_name << "(";
      for (std::size_t p = 0; p < header.param_names.size(); ++p) {
        if (p) {
          generated << ", ";
        }
        const auto param = sanitize_identifier(header.param_names[p]);
        generated << pseudo_kind_to_cpp(header.param_types[p]) << " " << param;
      }
      generated << ") {\n";

      const auto register_local = [&](const std::string& raw_name, const std::string& inferred_kind) {
        const auto name = sanitize_identifier(raw_name);
        if (name.empty() || declared.count(name) > 0) {
          return;
        }
        declared.insert(name);
        const auto normalized_kind = inferred_kind.empty() ? "i64" : inferred_kind;
        const auto final_kind = normalized_kind == "unknown" ? "i64" : normalized_kind;
        local_vars.push_back({name, final_kind});
        var_types[name] = final_kind;
      };

      const std::size_t start = i + 1;
      for (std::size_t scan = start; scan < lines.size(); ++scan) {
        const auto body_line = trim_ws(lines[scan]);
        if (body_line.empty()) {
          continue;
        }
        if (body_line == "}") {
          break;
        }
        if (body_line.rfind("var ", 0) == 0) {
          const auto eq = body_line.find(':');
          if (eq == std::string::npos) {
            continue;
          }
          const auto raw_name = trim_ws(body_line.substr(4, eq - 4));
          auto kind = trim_ws(body_line.substr(eq + 1));
          if (!kind.empty() && kind.back() == ';') {
            kind.pop_back();
          }
          register_local(raw_name, kind);
          continue;
        }

        std::string lhs;
        std::string rhs;
        if (match_scalar_assignment(body_line, lhs, rhs)) {
          const auto raw_name = sanitize_identifier(lhs);
          if (raw_name.empty()) {
            result.diagnostics.push_back("empty lhs in line: " + body_line);
            return result;
          }
          for (const auto& temp_ref : collect_temp_refs(rhs)) {
            register_local(temp_ref, "i64");
          }
          const auto value = parse_pseudo_expression(rhs, var_types, function_returns);
          register_local(raw_name, value.kind);
        }
      }

      for (const auto& [name, kind] : local_vars) {
        emit_line_to(generated, 1, pseudo_kind_to_cpp(kind) + " " + name + ";");
        var_types[name] = kind;
      }
      continue;
    }

    if (!in_function) {
      continue;
    }

    if (line == "}") {
      generated << "}\n";
      in_function = false;
      continue;
    }

    if (line.rfind("L", 0) == 0 && line.back() == ':') {
      emit_line_to(generated, 1, sanitize_identifier(line.substr(0, line.size() - 1)) + ":");
      continue;
    }

    if (line.rfind("var ", 0) == 0) {
      continue;
    }

    if (line.rfind("br_if ", 0) == 0) {
      const auto content = trim_ws(line.substr(6));
      const auto parts = split_csv_args(content);
      if (parts.size() == 3) {
        auto cond = parse_pseudo_expression(parts[0], var_types, function_returns);
        const auto true_target = sanitize_identifier(parts[1]);
        const auto false_target = sanitize_identifier(parts[2]);
        emit_line_to(generated, 1, "if (" + cond.code + ") {");
        emit_line_to(generated, 2, "goto " + true_target + ";");
        emit_line_to(generated, 1, "} else {");
        emit_line_to(generated, 2, "goto " + false_target + ";");
        emit_line_to(generated, 1, "}");
      } else {
        result.diagnostics.push_back("malformed br_if line: " + line);
        return result;
      }
      continue;
    }

    if (line.rfind("goto ", 0) == 0) {
      emit_line_to(generated, 1, "goto " + sanitize_identifier(trim_ws(line.substr(5))) + ";");
      continue;
    }

    if (line.rfind("return", 0) == 0) {
      const auto suffix = trim_ws(line.substr(6));
      if (suffix.empty()) {
        emit_line_to(generated, 1, "return;");
      } else {
        auto expr = parse_pseudo_expression(suffix, var_types, function_returns);
        emit_line_to(generated, 1, "return " + expr.code + ";");
      }
      continue;
    }

    if (line.rfind("call ", 0) == 0) {
      auto expr = parse_pseudo_expression(line, var_types, function_returns);
      if (expr.code.empty()) {
        result.diagnostics.push_back("failed to lower call statement: " + line);
        return result;
      }
      emit_line_to(generated, 1, expr.code + ";");
      continue;
    }

    const auto assign = line.find('=');
    if (assign == std::string::npos) {
      result.diagnostics.push_back("unsupported line format: " + line);
      return result;
    }

    auto next_line_idx = next_non_empty_line(lines, i + 1);
    if (next_line_idx < lines.size()) {
      const auto candidate_br_if = trim_ws(lines[next_line_idx]);
      if (candidate_br_if.rfind("br_if ", 0) == 0) {
        const auto rhs_content = trim_ws(line.substr(assign + 1));
        const auto br_content = trim_ws(candidate_br_if.substr(6));
        const auto br_parts = split_csv_args(br_content);
        const auto lhs_raw = trim_ws(line.substr(0, assign));
        const auto lhs = sanitize_identifier(lhs_raw);
        if (br_parts.size() == 3 && !br_parts[0].empty() && sanitize_identifier(br_parts[0]) == lhs) {
          if (!token_used_after(lines, lhs_raw, next_line_idx + 1)) {
            const auto condition = parse_pseudo_expression(rhs_content, var_types, function_returns);
            if (condition.code.empty()) {
              result.diagnostics.push_back("failed to parse branch condition in line: " + line);
              return result;
            }
            const auto true_target = sanitize_identifier(br_parts[1]);
            const auto false_target = sanitize_identifier(br_parts[2]);
            emit_line_to(generated, 1, "if (" + condition.code + ") {");
            emit_line_to(generated, 2, "goto " + true_target + ";");
            emit_line_to(generated, 1, "} else {");
            emit_line_to(generated, 2, "goto " + false_target + ";");
            emit_line_to(generated, 1, "}");
            i = next_line_idx;
            continue;
          }
        }
      }
    }

    const std::string lhs = sanitize_identifier(trim_ws(line.substr(0, assign)));
    const std::string rhs = trim_ws(line.substr(assign + 1));
    if (lhs.empty()) {
      result.diagnostics.push_back("empty lhs in line: " + line);
      return result;
    }

    auto value = parse_pseudo_expression(rhs, var_types, function_returns);
    if (value.kind == "void") {
      if (!value.code.empty()) {
        emit_line_to(generated, 1, value.code + ";");
        continue;
      }
      result.diagnostics.push_back("cannot assign void expression in line: " + line);
      return result;
    }
    if (value.kind == "unknown" && value.code.empty()) {
      result.diagnostics.push_back("unsupported or empty expression in line: " + line);
      continue;
    }
    emit_line_to(generated, 1, lhs + " = " + value.code + ";");
  }

  auto translated_lines = split_lines(generated.str());
  auto optimized_lines = canonicalize_c_lines(std::move(translated_lines));
  result.success = true;
  result.output = join_lines(optimized_lines);
  return result;
}
