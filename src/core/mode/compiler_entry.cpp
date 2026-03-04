#include "spark/core/mode/compiler_entry.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "spark/application/wiring/compiler_wiring.h"
#include "spark/cpu_features.h"
#include "spark/evaluator.h"

namespace spark::core::mode {

namespace {

enum class Command {
  Help,
  PrintCpuFeatures,
  Run,
  Check,
  Compile,
  Invalid,
};

struct CliOptions {
  Command command = Command::Invalid;
  std::string source_file;
  bool emit_c = false;
  std::string emit_c_out;
  std::string error;
};

void print_usage() {
  std::cout << "usage:\n"
            << "  sparkc run <file.k>\n"
            << "  sparkc check <file.k>\n"
            << "  sparkc compile <file.k> [--emit-c] [--emit-c-out <path>]\n"
            << "  sparkc --print-cpu-features\n"
            << "  sparkc <file.k>\n";
}

std::string default_c_out_path(const std::string& source_file) {
  std::filesystem::path out(source_file);
  if (out.has_extension()) {
    out.replace_extension(".c");
  } else {
    out += ".c";
  }
  return out.string();
}

bool write_text_file(const std::string& path, const std::string& content) {
  std::ofstream out(path);
  if (!out.is_open()) {
    return false;
  }
  out << content;
  return static_cast<bool>(out);
}

void publish_diagnostics(application::wiring::CompilerWiring& wiring,
                         const std::vector<dto::DiagnosticEntry>& diagnostics) {
  if (diagnostics.empty()) {
    return;
  }
  wiring.diagnostics().publish(diagnostics);
}

CliOptions parse_cli(int argc, char** argv) {
  CliOptions options;
  if (argc <= 1) {
    options.command = Command::Help;
    return options;
  }

  const std::string arg1 = argv[1];
  if (arg1 == "-h" || arg1 == "--help") {
    options.command = Command::Help;
    return options;
  }

  if (arg1 == "--print-cpu-features") {
    options.command = Command::PrintCpuFeatures;
    return options;
  }

  if (arg1 == "env") {
    if (argc >= 3 && std::string(argv[2]) == "--print-cpu-features") {
      options.command = Command::PrintCpuFeatures;
      return options;
    }
    options.error = "env supports only --print-cpu-features";
    return options;
  }

  const auto require_source = [&](int index) -> bool {
    if (argc <= index) {
      options.error = "missing source file";
      return false;
    }
    options.source_file = argv[index];
    return true;
  };

  if (arg1 == "run") {
    if (!require_source(2)) {
      return options;
    }
    options.command = Command::Run;
    return options;
  }

  if (arg1 == "check") {
    if (!require_source(2)) {
      return options;
    }
    options.command = Command::Check;
    return options;
  }

  if (arg1 == "compile") {
    if (!require_source(2)) {
      return options;
    }
    options.command = Command::Compile;
    for (int i = 3; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--emit-c") {
        options.emit_c = true;
        continue;
      }
      if (arg == "--emit-c-out") {
        if (i + 1 >= argc) {
          options.error = "--emit-c-out requires a path";
          return options;
        }
        options.emit_c = true;
        options.emit_c_out = argv[++i];
        continue;
      }
      options.error = "unknown option for compile: " + arg;
      return options;
    }
    return options;
  }

  options.command = Command::Run;
  options.source_file = arg1;
  return options;
}

bool parse_and_typecheck(application::wiring::CompilerWiring& wiring, const std::string& source_file,
                         dto::ProgramBundle& out_bundle,
                         std::vector<dto::DiagnosticEntry>& out_diagnostics) {
  dto::FilePath file{source_file};
  const bool ok = wiring.ui().parse_and_typecheck(file, out_bundle);
  out_diagnostics = wiring.ui().diagnostics();
  return ok;
}

int run_check_command(const std::string& source_file) {
  application::wiring::CompilerWiring wiring;
  dto::ProgramBundle bundle;
  std::vector<dto::DiagnosticEntry> diagnostics;
  const bool ok = parse_and_typecheck(wiring, source_file, bundle, diagnostics);
  publish_diagnostics(wiring, diagnostics);
  return ok ? 0 : 1;
}

int run_compile_command(const CliOptions& options) {
  application::wiring::CompilerWiring wiring;
  dto::ProgramBundle bundle;
  std::vector<dto::DiagnosticEntry> diagnostics;
  if (!parse_and_typecheck(wiring, options.source_file, bundle, diagnostics)) {
    publish_diagnostics(wiring, diagnostics);
    return 1;
  }

  dto::PipelineProducts products;
  const bool lowered = wiring.ui().lower_to_products(bundle, products);
  diagnostics = wiring.ui().diagnostics();
  publish_diagnostics(wiring, diagnostics);
  if (!lowered) {
    return 1;
  }

  if (options.emit_c) {
    const std::string out_path =
        options.emit_c_out.empty() ? default_c_out_path(options.source_file) : options.emit_c_out;
    if (!write_text_file(out_path, products.c_source)) {
      std::cerr << "emit-c failed: " << out_path << "\n";
      return 1;
    }
    std::cout << out_path << "\n";
  }
  return 0;
}

int run_execute_command(const std::string& source_file) {
  application::wiring::CompilerWiring wiring;
  dto::ProgramBundle bundle;
  std::vector<dto::DiagnosticEntry> diagnostics;
  if (!parse_and_typecheck(wiring, source_file, bundle, diagnostics)) {
    publish_diagnostics(wiring, diagnostics);
    return 1;
  }
  publish_diagnostics(wiring, diagnostics);

  Interpreter interpreter;
  try {
    (void)interpreter.run(*bundle.program);
    return 0;
  } catch (const EvalException& ex) {
    std::cerr << "runtime error: " << ex.what() << "\n";
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "runtime failure: " << ex.what() << "\n";
    return 1;
  }
}

int run_print_cpu_features() {
  const auto info = detect_cpu_features();
  std::cout << "arch=" << info.arch << "\n";
  std::cout << "features=";
  for (std::size_t i = 0; i < info.features.size(); ++i) {
    if (i > 0) {
      std::cout << ",";
    }
    std::cout << info.features[i];
  }
  std::cout << "\n";
  return 0;
}

}  // namespace

int run_compiler_entry(int argc, char** argv) {
  const CliOptions options = parse_cli(argc, argv);
  if (!options.error.empty()) {
    std::cerr << options.error << "\n";
    print_usage();
    return 2;
  }

  switch (options.command) {
    case Command::Help:
      print_usage();
      return 0;
    case Command::PrintCpuFeatures:
      return run_print_cpu_features();
    case Command::Run:
      return run_execute_command(options.source_file);
    case Command::Check:
      return run_check_command(options.source_file);
    case Command::Compile:
      return run_compile_command(options);
    case Command::Invalid:
      break;
  }

  print_usage();
  return 2;
}

}  // namespace spark::core::mode
