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

int compile_mode_main(const std::string& file_path, bool emit_c, bool emit_asm,
                     bool emit_llvm, bool emit_mlir, const std::string& output_path) {
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

  TempFileRegistry temp_files;
  const auto binary_path = temp_files.make_temp_file(".out");
  std::string compile_error;
  if (!write_temp_source_and_compile(temp_files, products.c_source, binary_path, compile_error)) {
    std::cerr << "run: native compile failed: " << compile_error << "\n";
    std::cerr << "run: falling back to interpreter\n";
    return run_interpreter_main(file_path, false);
  }

  const auto status = run_system_command(shell_escape(binary_path.string()));
  return status;
}
