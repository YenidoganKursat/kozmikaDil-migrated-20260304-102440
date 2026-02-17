
void print_usage() {
  std::cerr << "usage:\n"
            << "  sparkc run <file.k>\n"
            << "  sparkc run [--interpret] [--allow-t5] <file.k>\n"
            << "  sparkc build <file.k> [--allow-t5] [-o <out_binary>]\n"
            << "  sparkc compile <file.k>\n"
            << "  sparkc compile <file.k> --emit-c [--emit-c-out <path>]\n"
            << "  sparkc compile <file.k> --emit-asm [--out <path>]  (C->assembly)\n"
            << "  sparkc compile <file.k> --emit-llvm [--emit-mlir] (pseudo-IR)\n"
            << "  sparkc parse <file.k> [--dump-ast]\n"
            << "  sparkc check <file.k>\n"
            << "  sparkc analyze <file.k> [--dump-types] [--dump-shapes] [--dump-tiers|--dump-tier]\n"
            << "                            [--dump-pipeline-ir] [--dump-fusion-plan] [--why-not-fused]\n"
            << "  sparkc <file.k>               (legacy: run)\n";
}

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage();
    return 1;
  }

  const std::string command = argv[1];
  try {
    if (command == "parse") {
      if (argc < 3) {
        print_usage();
        return 1;
      }
      bool dump_ast = false;
      std::string file_path;
      for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dump-ast") {
          dump_ast = true;
          continue;
        }
        if (!file_path.empty()) {
          std::cerr << "unexpected extra argument: " << arg << "\n";
          print_usage();
          return 1;
        }
        file_path = arg;
      }
      if (file_path.empty()) {
        print_usage();
        return 1;
      }
      return parse_mode_main(file_path, dump_ast);
    }

    if (command == "check") {
      if (argc != 3) {
        print_usage();
        return 1;
      }
      return check_mode_main(argv[2]);
    }

    if (command == "analyze") {
      if (argc < 3) {
        print_usage();
        return 1;
      }
      bool dump_types = false;
      bool dump_shapes = false;
      bool dump_tiers = false;
      bool dump_pipeline_ir = false;
      bool dump_fusion_plan = false;
      bool dump_why_not_fused = false;
      std::string file_path;
      for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dump-types") {
          dump_types = true;
          continue;
        }
        if (arg == "--dump-shapes") {
          dump_shapes = true;
          continue;
        }
        if (arg == "--dump-tiers" || arg == "--dump-tier") {
          dump_tiers = true;
          continue;
        }
        if (arg == "--dump-pipeline-ir") {
          dump_pipeline_ir = true;
          continue;
        }
        if (arg == "--dump-fusion-plan") {
          dump_fusion_plan = true;
          continue;
        }
        if (arg == "--why-not-fused") {
          dump_why_not_fused = true;
          continue;
        }
        if (!file_path.empty()) {
          std::cerr << "unexpected extra argument: " << arg << "\n";
          print_usage();
          return 1;
        }
        file_path = arg;
      }
      if (file_path.empty()) {
        print_usage();
        return 1;
      }
      return analyze_mode_main(file_path, dump_types, dump_shapes, dump_tiers,
                               dump_pipeline_ir, dump_fusion_plan, dump_why_not_fused);
    }

    if (command == "compile") {
      if (argc < 3) {
        print_usage();
        return 1;
      }
      bool emit_c = false;
      bool emit_asm = false;
      bool emit_llvm = false;
      bool emit_mlir = false;
      std::string output_path;
      std::string file_path;

      for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--emit-c") {
          emit_c = true;
          continue;
        }
        if (arg == "--emit-asm") {
          emit_asm = true;
          continue;
        }
        if (arg == "--emit-llvm") {
          emit_llvm = true;
          continue;
        }
        if (arg == "--emit-mlir") {
          emit_mlir = true;
          continue;
        }
        if (arg == "--emit-c-out" || arg == "--out" || arg == "-o") {
          if (i + 1 >= argc) {
            std::cerr << arg << " requires a path\n";
            return 1;
          }
          output_path = argv[i + 1];
          ++i;
          continue;
        }
        if (!file_path.empty()) {
          std::cerr << "unexpected extra argument: " << arg << "\n";
          print_usage();
          return 1;
        }
        file_path = arg;
      }
      if (file_path.empty()) {
        print_usage();
        return 1;
      }
      if (emit_asm && !emit_c && !emit_llvm && !emit_mlir) {
        emit_asm = true;
      }

      if (!emit_c && !emit_asm) {
        emit_llvm = emit_llvm || emit_mlir;
      }
      return compile_mode_main(file_path, emit_c, emit_asm, emit_llvm, emit_mlir, output_path);
    }

    if (command == "build") {
      if (argc < 3) {
        print_usage();
        return 1;
      }
      std::string output_path = "a.out";
      bool allow_t5_codegen = false;
      std::string file_path;
      for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--out" || arg == "-o") {
          if (i + 1 >= argc) {
            std::cerr << arg << " requires a path\n";
            return 1;
          }
          output_path = argv[i + 1];
          ++i;
          continue;
        }
        if (arg == "--allow-t5") {
          allow_t5_codegen = true;
          continue;
        }
        if (!file_path.empty()) {
          std::cerr << "unexpected extra argument: " << arg << "\n";
          print_usage();
          return 1;
        }
        file_path = arg;
      }
      if (file_path.empty()) {
        print_usage();
        return 1;
      }
      return build_mode_main(file_path, output_path, allow_t5_codegen);
    }

    if (command == "run") {
      if (argc < 3) {
        print_usage();
        return 1;
      }
      bool force_interpreter = false;
      bool allow_t5_codegen = false;
      std::string file_path;
      for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--interpret") {
          force_interpreter = true;
          continue;
        }
        if (arg == "--allow-t5") {
          allow_t5_codegen = true;
          continue;
        }
        if (!file_path.empty()) {
          std::cerr << "unexpected extra argument: " << arg << "\n";
          print_usage();
          return 1;
        }
        file_path = arg;
      }
      if (file_path.empty()) {
        print_usage();
        return 1;
      }
      return run_mode_main(file_path, force_interpreter, allow_t5_codegen);
    }

    // Legacy: sparkc <file.k> as run.
    return run_mode_main(command, false, false);
  } catch (const std::exception& err) {
    std::cerr << "error: " << err.what() << "\n";
  }
  return 1;
}
