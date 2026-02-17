bool is_blocking_tier(spark::TierLevel tier, bool allow_t5) {
  if (tier == spark::TierLevel::T4) {
    return false;
  }
  if (allow_t5 && tier == spark::TierLevel::T5) {
    return false;
  }
  return true;
}

void print_tier_blocking_report(const spark::TypeChecker& checker, const std::string& command,
                                bool allow_t5) {
  const auto summary = summarize_tier_report(checker);
  if (summary.blocking_records.empty()) {
    return;
  }

  bool printed_any = false;

  const auto t4 = summary.t4_count;
  const auto t5 = summary.t5_count;
  const auto t8 = summary.t8_count;
  std::cerr << command << ": T4 gate blocked\n";
  std::cerr << "  tier stats: T4=" << t4 << ", T5=" << t5 << ", T8=" << t8 << "\n";
  for (const auto& record : summary.blocking_records) {
    if (!is_blocking_tier(record.tier, allow_t5)) {
      continue;
    }
    printed_any = true;
    std::cerr << "  " << record.kind << " " << record.name << " -> "
              << spark::TypeChecker::tier_to_string(record.tier) << "\n";
    for (const auto& reason : record.reasons) {
      std::cerr << "    - " << reason << "\n";
    }
  }

  if (!printed_any) {
    if (!allow_t5) {
      return;
    }
    std::cerr << "  no hard-blocking tier records\n";
  }
}

bool has_tier_blockers(const spark::TypeChecker& checker, bool allow_t5) {
  for (const auto& fn : checker.function_reports()) {
    if (is_blocking_tier(fn.tier, allow_t5)) {
      return true;
    }
  }
  for (const auto& loop : checker.loop_reports()) {
    if (is_blocking_tier(loop.tier, allow_t5)) {
      return true;
    }
  }
  return false;
}

std::string default_cxx() {
  if (const auto* env = std::getenv("SPARK_CC")) {
    return env;
  }
  if (const auto* env = std::getenv("SPARK_CXX")) {
    return env;
  }
  return "clang";
}

bool write_temp_source_and_compile(
    TempFileRegistry& temp_files,
    const std::string& c_source,
    const std::filesystem::path& output_binary,
    std::string& compile_error) {
  const auto c_path = temp_files.make_temp_file(".c");
  write_file(c_path, c_source);

  if (!std::filesystem::exists(output_binary.parent_path()) && !output_binary.parent_path().empty()) {
    std::error_code ec;
    std::filesystem::create_directories(output_binary.parent_path(), ec);
    if (ec) {
      compile_error = "failed to create output directory: " + output_binary.parent_path().string();
      return false;
    }
  }

  const auto compiler = default_cxx();
  const auto native_cxx_flags = resolve_native_cxx_flags();
  const auto native_ld_flags = split_env_flags(std::getenv("SPARK_LDFLAGS"));
  std::ostringstream command;
  command << shell_escape(compiler);
  append_flags(command, native_cxx_flags);
  command << " " << shell_escape(c_path.string())
          << " -o " << shell_escape(output_binary.string());
  append_flags(command, native_ld_flags);
  const auto status = run_system_command(command.str());
  if (status != 0) {
    compile_error = "native compile failed (exit " + std::to_string(status) + ")";
    return false;
  }
  return true;
}

bool compile_to_assembly(const std::string& c_source, std::string& asm_output, std::string& asm_error) {
  TempFileRegistry temp_files;
  const auto c_path = temp_files.make_temp_file(".c");
  const auto asm_path = temp_files.make_temp_file(".s");
  write_file(c_path, c_source);

  const auto compiler = default_cxx();
  const auto native_cxx_flags = resolve_native_cxx_flags();
  std::ostringstream command;
  command << shell_escape(compiler);
  append_flags(command, native_cxx_flags);
  command << " -S " << shell_escape(c_path.string())
          << " -o " << shell_escape(asm_path.string());

  const auto status = run_system_command(command.str());
  if (status != 0) {
    asm_error = "asm emit failed (exit " + std::to_string(status) + ")";
    return false;
  }

  std::ifstream asm_file(asm_path);
  if (!asm_file) {
    asm_error = "failed to read generated assembly: " + asm_path.string();
    return false;
  }
  asm_output = std::string((std::istreambuf_iterator<char>(asm_file)),
                          std::istreambuf_iterator<char>());
  return true;
}

int parse_mode_main(const std::string& file_path, bool dump_ast) {
  const auto source = read_file(file_path);
  spark::Parser parser(source);
  auto program = parser.parse_program();
  if (!dump_ast) {
    std::cout << "parsed: " << file_path << "\n";
  }

  std::cout << spark::to_source(*program) << "\n";
  return 0;
}

int check_mode_main(const std::string& file_path) {
  ProgramBundle bundle;
  if (!parse_and_typecheck(file_path, bundle)) {
    for (const auto& message : bundle.checker.diagnostics()) {
      std::cerr << "typecheck: " << message << "\n";
    }
    return 1;
  }
  std::cout << "typecheck OK\n";
  return 0;
}

int analyze_mode_main(const std::string& file_path, bool dump_types, bool dump_shapes,
                      bool dump_tiers, bool dump_pipeline_ir, bool dump_fusion_plan,
                      bool dump_why_not_fused) {
  ProgramBundle bundle;
  if (!parse_and_typecheck(file_path, bundle)) {
    for (const auto& message : bundle.checker.diagnostics()) {
      std::cerr << "analyze: " << message << "\n";
    }
    return 1;
  }

  if (dump_types) {
    std::cout << "Types:\n" << bundle.checker.dump_types() << "\n";
  }
  if (dump_shapes) {
    std::cout << "Shapes:\n" << bundle.checker.dump_shapes() << "\n";
  }
  if (dump_tiers || (!dump_types && !dump_shapes && !dump_pipeline_ir &&
                     !dump_fusion_plan && !dump_why_not_fused)) {
    std::cout << "Tiers:\n" << bundle.checker.dump_tier_report() << "\n";
  }
  if (dump_pipeline_ir) {
    std::cout << "PipelineIR:\n" << bundle.checker.dump_pipeline_ir() << "\n";
  }
  if (dump_fusion_plan) {
    std::cout << "FusionPlan:\n" << bundle.checker.dump_fusion_plan() << "\n";
  }
  if (dump_why_not_fused) {
    std::cout << "WhyNotFused:\n" << bundle.checker.dump_why_not_fused() << "\n";
  }
  return 0;
}
