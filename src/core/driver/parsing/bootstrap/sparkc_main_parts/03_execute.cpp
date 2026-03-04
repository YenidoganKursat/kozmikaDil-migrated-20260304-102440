}

bool compile_pipeline(const std::string& file_path, PipelineProducts& products, bool print_diagnostics = true) {
  ProgramBundle bundle;
  if (!parse_and_typecheck(file_path, bundle)) {
    if (print_diagnostics) {
      for (const auto& message : bundle.checker.diagnostics()) {
        std::cerr << "compile: " << message << "\n";
      }
    }
    return false;
  }
  if (!lower_to_products(bundle, products)) {
    if (print_diagnostics) {
      std::cerr << "compile: codegen failed\n";
      for (const auto& message : products.diagnostics) {
        std::cerr << "  " << message << "\n";
      }
      if (!products.ir.empty()) {
        std::cerr << "  ir:\n";
        std::cerr << "  " << products.ir << "\n";
      }
    }
    return false;
  }
  return true;
}

bool write_high_precision_interpreter_launcher(const std::filesystem::path& source_file,
                                               const std::filesystem::path& output_file,
                                               std::string& error_message) {
  const auto source_abs = std::filesystem::absolute(source_file);
  const auto local_sparkc = std::filesystem::absolute(std::filesystem::current_path() / "build/compiler/sparkc");
  const auto local_k = std::filesystem::absolute(std::filesystem::current_path() / "k");
  const auto output_parent = output_file.parent_path();
  if (!output_parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(output_parent, ec);
    if (ec) {
      error_message = "failed to create output directory: " + output_parent.string();
      return false;
    }
  }

  std::ofstream out(output_file);
  if (!out) {
    error_message = "failed to open output path: " + output_file.string();
    return false;
  }

  out << "#!/usr/bin/env bash\n";
  out << "set -euo pipefail\n";
  out << "SOURCE_FILE=" << shell_escape(source_abs.string()) << "\n";
  out << "if [ -x " << shell_escape(local_sparkc.string()) << " ]; then\n";
  out << "  exec " << shell_escape(local_sparkc.string()) << " run --interpret \"$SOURCE_FILE\"\n";
  out << "fi\n";
  out << "if [ -x " << shell_escape(local_k.string()) << " ]; then\n";
  out << "  exec " << shell_escape(local_k.string()) << " run --interpret \"$SOURCE_FILE\"\n";
  out << "fi\n";
  out << "if command -v sparkc >/dev/null 2>&1; then\n";
  out << "  exec sparkc run --interpret \"$SOURCE_FILE\"\n";
  out << "fi\n";
  out << "echo \"error: no spark launcher found for high-precision runtime\" >&2\n";
  out << "exit 1\n";
  out.close();
  if (!out) {
    error_message = "failed to write launcher output: " + output_file.string();
    return false;
  }

  std::error_code perm_error;
  std::filesystem::permissions(
      output_file,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
          std::filesystem::perms::owner_exec | std::filesystem::perms::group_read |
          std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
          std::filesystem::perms::others_exec,
      std::filesystem::perm_options::replace, perm_error);
  if (perm_error) {
    error_message = "failed to set executable permissions: " + output_file.string();
    return false;
  }
  return true;
}

int compile_mode_main(const std::string& file_path, bool emit_c, bool emit_asm,
                     bool emit_llvm, bool emit_mlir, const std::string& output_path) {
  const auto source_text = read_file(file_path);
  if (source_uses_high_precision_float_primitives(source_text)) {
    std::cerr << "compile: f128/f256/f512 detected; strict precision requires interpreter backend.\n";
    std::cerr << "compile: use `sparkc run --interpret` for high-precision execution.\n";
    return 1;
  }

  PipelineProducts products;
  if (!compile_pipeline(file_path, products)) {
    return 1;
  }

  const auto emit_entry = true;
  if (emit_asm) {
    std::string asm_text;
    std::string asm_error;
    if (!compile_to_assembly(products.c_source, asm_text, asm_error)) {
      std::cerr << "compile: " << asm_error << "\n";
      return 1;
    }
    if (!output_path.empty()) {
      std::ofstream out(output_path);
      if (!out) {
        std::cerr << "compile: failed to open output path: " << output_path << "\n";
        return 1;
      }
      out << asm_text << "\n";
      return 0;
    }
    std::cout << asm_text << "\n";
    return 0;
  }

  if (emit_c) {
    if (!output_path.empty()) {
      std::ofstream out(output_path);
      if (!out) {
        std::cerr << "compile: failed to open output path: " << output_path << "\n";
        return 1;
      }
      out << products.c_source << "\n";
      return 0;
    }
    std::cout << products.c_source << "\n";
    return 0;
  }

  if (emit_llvm || emit_mlir) {
    if (!output_path.empty()) {
      std::ofstream out(output_path);
      if (!out) {
        std::cerr << "compile: failed to open output path: " << output_path << "\n";
        return 1;
      }
      out << products.ir << "\n";
      return 0;
    }
    std::cout << products.ir << "\n";
    return 0;
  }

  (void)emit_entry;
  (void)emit_mlir;
  (void)emit_llvm;
  std::cout << products.ir << "\n";
  return 0;
}

int build_mode_main(const std::string& file_path, const std::string& output_path,
                   bool allow_t5_codegen, const BuildTuningOptions& tuning) {
  apply_build_tuning_env(tuning);
  ProgramBundle bundle;
  if (!parse_and_typecheck(file_path, bundle)) {
    for (const auto& message : bundle.checker.diagnostics()) {
      std::cerr << "build: " << message << "\n";
    }
    return 1;
  }

  if (source_uses_high_precision_float_primitives(bundle.source)) {
    const std::filesystem::path out_path = output_path.empty() ? "a.out" : output_path;
    std::string launcher_error;
    if (!write_high_precision_interpreter_launcher(file_path, out_path, launcher_error)) {
      std::cerr << "build: f128/f256/f512 detected; strict launcher generation failed: " << launcher_error << "\n";
      return 1;
    }
    std::cout << "built: " << out_path
              << " (high-precision strict launcher -> interpret runtime)\n";
    return 0;
  }

  const auto summary = summarize_tier_report(bundle.checker);
  if (summary.blocking_records.size() > 0 && has_tier_blockers(bundle.checker, allow_t5_codegen)) {
    print_tier_blocking_report(bundle.checker, "build", allow_t5_codegen);
    std::cerr << "build: aborting because full module is not T4 eligible yet\n";
    return 1;
  }

  PipelineProducts products;
  if (!lower_to_products(bundle, products)) {
    std::cerr << "build: codegen failed\n";
    for (const auto& message : products.diagnostics) {
      std::cerr << "  " << message << "\n";
    }
    if (!products.ir.empty()) {
      std::cerr << "  ir:\n";
      std::cerr << "  " << products.ir << "\n";
    }
    return 1;
  }

  std::string compile_error;
  TempFileRegistry temp_files;
  const std::filesystem::path out_path = output_path.empty() ? "a.out" : output_path;
  if (!write_temp_source_and_compile(temp_files, products.c_source, out_path, compile_error)) {
    std::cerr << "build: " << compile_error << "\n";
    return 1;
  }
  std::cout << "built: " << out_path << "\n";
  return 0;
}

int run_interpreter_main(const std::string& file_path, bool explain_layout) {
  const std::string source = read_file(file_path);
  spark::Parser parser(source);
  auto program = parser.parse_program();
  spark::Interpreter interpreter;
  interpreter.run(*program);
  if (explain_layout) {
    print_layout_summary(interpreter);
  }
  return 0;
}

std::uint64_t fnv1a64_hash(std::string_view text, std::uint64_t seed = 1469598103934665603ULL) {
  std::uint64_t h = seed;
  for (const unsigned char ch : text) {
    h ^= static_cast<std::uint64_t>(ch);
    h *= 1099511628211ULL;
  }
  return h;
}

std::string hex_u64(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << value;
  return out.str();
}

std::filesystem::path adaptive_run_cache_dir() {
  if (const auto* custom = getenv_non_empty("SPARK_RUN_CACHE_DIR")) {
    return std::filesystem::path(custom);
  }
  return std::filesystem::current_path() / ".kozmika_cache" / "run_native";
}

std::string adaptive_run_cache_key(const std::string& file_path, const std::string& source,
                                   const std::string& c_source, bool allow_t5_codegen,
                                   const BuildTuningOptions& tuning) {
  std::string material;
  material.reserve(source.size() + c_source.size() + 256);
  material += "abi=v2\n";
  material += "file=" + std::filesystem::absolute(file_path).string() + "\n";
  material += "allow_t5=" + std::string(allow_t5_codegen ? "1" : "0") + "\n";
  material += "target=" + tuning.target_triple + "\n";
  material += "sysroot=" + tuning.sysroot + "\n";
  material += "lto=" + tuning.lto_mode + "\n";
  material += "pgo_mode=" + tuning.pgo_mode + "\n";
  material += "pgo_profile=" + tuning.pgo_profile + "\n";
  material += "opt_profile=" + tuning.optimization_profile + "\n";
  material += "env_cflags=" + std::string(getenv_non_empty("SPARK_CFLAGS") ? getenv_non_empty("SPARK_CFLAGS") : "") + "\n";
  material += "env_cxxflags=" + std::string(getenv_non_empty("SPARK_CXXFLAGS") ? getenv_non_empty("SPARK_CXXFLAGS") : "") + "\n";
  material += "env_opt_profile=" + std::string(getenv_non_empty("SPARK_OPT_PROFILE") ? getenv_non_empty("SPARK_OPT_PROFILE") : "") + "\n";
  material += "--source--\n";
  material += source;
  material += "\n--c-source--\n";
  material += c_source;
  const auto h1 = fnv1a64_hash(material);
  const auto h2 = fnv1a64_hash(c_source, h1 ^ 0x9e3779b97f4a7c15ULL);
  return hex_u64(h1) + hex_u64(h2);
}

int run_native_adaptive_cached(const std::string& file_path, const ProgramBundle& bundle,
                               const PipelineProducts& products, bool allow_t5_codegen,
                               const BuildTuningOptions& tuning, bool& compiled_now,
                               std::string& compile_error) {
  const bool cache_enabled = env_truthy("SPARK_RUN_ADAPTIVE_CACHE", true);
  TempFileRegistry temp_files;
  std::filesystem::path binary_path;

  if (!cache_enabled) {
    binary_path = temp_files.make_temp_file(".out");
  } else {
    binary_path = adaptive_run_cache_dir() /
                  (adaptive_run_cache_key(file_path, bundle.source, products.c_source,
                                          allow_t5_codegen, tuning) +
                   ".out");
    std::error_code mkdir_error;
    std::filesystem::create_directories(binary_path.parent_path(), mkdir_error);
    if (mkdir_error) {
      compile_error = "failed to create adaptive run cache directory: " +
                      binary_path.parent_path().string();
      return 1;
    }
  }

  std::error_code stat_error;
  const bool binary_exists = std::filesystem::exists(binary_path, stat_error);
  if (stat_error) {
    compile_error = "failed to inspect adaptive run cache binary: " + binary_path.string();
    return 1;
  }
  if (!binary_exists) {
    compiled_now = true;
    if (!write_temp_source_and_compile(temp_files, products.c_source, binary_path, compile_error)) {
      return 1;
    }
  }

  return run_system_command(shell_escape(binary_path.string()));
}

int run_mode_main(const std::string& file_path, bool force_interpreter,
                  bool allow_t5_codegen, bool explain_layout,
                  const BuildTuningOptions& tuning) {
  if (force_interpreter || explain_layout) {
    return run_interpreter_main(file_path, explain_layout);
  }
  if (!target_matches_host(tuning.target_triple)) {
    std::cerr << "run: --target=" << tuning.target_triple
              << " differs from host architecture; use `sparkc build` for cross-target output\n";
    return 1;
  }
  apply_build_tuning_env(tuning);

  ProgramBundle bundle;
  if (!parse_and_typecheck(file_path, bundle)) {
    for (const auto& message : bundle.checker.diagnostics()) {
      std::cerr << "run: " << message << "\n";
    }
    return 1;
  }

  if (source_uses_high_precision_float_primitives(bundle.source)) {
    std::cerr << "run: f128/f256/f512 detected; switching to interpreter high-precision backend.\n";
    return run_interpreter_main(file_path, false);
  }

  if (has_tier_blockers(bundle.checker, allow_t5_codegen)) {
    print_tier_blocking_report(bundle.checker, "run", allow_t5_codegen);
    std::cerr << "run: non-T4 code detected; falling back to interpreter\n";
    return run_interpreter_main(file_path, false);
  }

  PipelineProducts products;
  if (!lower_to_products(bundle, products)) {
    std::cerr << "run: lowering failed, falling back to interpreter\n";
    for (const auto& message : products.diagnostics) {
      std::cerr << "  " << message << "\n";
    }
    if (!products.ir.empty()) {
      std::cerr << "  ir:\n";
      std::cerr << "  " << products.ir << "\n";
    }
    return run_interpreter_main(file_path, false);
  }

  std::string compile_error;
  bool compiled_now = false;
  const auto status = run_native_adaptive_cached(
      file_path, bundle, products, allow_t5_codegen, tuning, compiled_now, compile_error);
  if (status == 1 && !compile_error.empty()) {
    std::cerr << "run: native adaptive compile failed: " << compile_error << "\n";
    std::cerr << "run: falling back to interpreter\n";
    return run_interpreter_main(file_path, false);
  }

  return status;
}
