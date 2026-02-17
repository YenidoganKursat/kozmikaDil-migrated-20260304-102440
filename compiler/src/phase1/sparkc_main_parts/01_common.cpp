#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <sys/wait.h>
#include <vector>

#include "spark/ast.h"
#include "spark/codegen.h"
#include "spark/evaluator.h"
#include "spark/parser.h"
#include "spark/semantic.h"

namespace {

struct ProgramBundle {
  std::string source;
  std::unique_ptr<spark::Program> program;
  spark::TypeChecker checker;
};

struct PipelineProducts {
  std::string ir;
  std::string c_source;
  std::vector<std::string> diagnostics;
};

struct TierSummary {
  std::size_t t4_count = 0;
  std::size_t t5_count = 0;
  std::size_t t8_count = 0;
  std::vector<spark::TierRecord> blocking_records;
};

struct TempFileRegistry {
  ~TempFileRegistry() {
    for (const auto& path : paths_) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  }

  std::filesystem::path make_temp_file(const std::string& suffix = "") {
    ++counter_;
    const auto now =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    std::ostringstream name;
    name << "sparkc_" << std::hash<std::string>{}(source_root_)
         << "_" << now << "_" << counter_ << suffix;
    const auto path = std::filesystem::temp_directory_path() / name.str();
    {
      std::ofstream out(path);
      (void)out;
    }
    paths_.push_back(path);
    return path;
  }

 private:
  unsigned long long counter_ = 0;
  std::string source_root_ = std::to_string(::getpid());
  std::vector<std::filesystem::path> paths_;
};

std::string read_file(const std::string& path) {
  std::ifstream source_file(path);
  if (!source_file) {
    throw std::runtime_error("failed to open source file: " + path);
  }
  return std::string((std::istreambuf_iterator<char>(source_file)),
                     std::istreambuf_iterator<char>());
}

std::string shell_escape(const std::string& value) {
  if (value.empty()) {
    return "''";
  }
  const auto safe = std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isalnum(ch) || ch == '_' || ch == '/' || ch == '.' || ch == '-' || ch == '+' || ch == ':' || ch == '=';
  });
  if (safe) {
    return value;
  }

  std::string out = "'";
  for (char ch : value) {
    if (ch == '\'') {
      out += "'\"'\"'";
      continue;
    }
    out += ch;
  }
  out += "'";
  return out;
}

std::vector<std::string> split_env_flags(const char* value) {
  std::vector<std::string> flags;
  if (value == nullptr || *value == '\0') {
    return flags;
  }
  std::istringstream ss(value);
  std::string token;
  while (ss >> token) {
    flags.push_back(token);
  }
  return flags;
}

bool vector_contains_flag(const std::vector<std::string>& flags, const std::string& value) {
  return std::find(flags.begin(), flags.end(), value) != flags.end();
}

void append_if_missing(std::vector<std::string>& flags, const std::string& value) {
  if (!vector_contains_flag(flags, value)) {
    flags.push_back(value);
  }
}

std::vector<std::string> resolve_native_cxx_flags() {
  const char* cflags = std::getenv("SPARK_CFLAGS");
  if (cflags == nullptr || *cflags == '\0') {
    cflags = std::getenv("SPARK_CXXFLAGS");
  }
  std::vector<std::string> flags = split_env_flags(cflags);
  if (flags.empty()) {
    flags = {
        "-std=c11",
        "-O3",
        "-DNDEBUG",
        "-fomit-frame-pointer",
        "-fno-stack-protector",
        "-fno-plt",
        "-fstrict-aliasing",
        "-fvectorize",
        "-fslp-vectorize",
        "-ftree-vectorize",
    };
  }

  if (!vector_contains_flag(flags, "-DNDEBUG")) {
    append_if_missing(flags, "-DNDEBUG");
  }

  if (const auto* lto = std::getenv("SPARK_LTO")) {
    const std::string p = lto;
    if (p == "thin" || p == "1" || p == "true" || p == "yes" || p == "on") {
      append_if_missing(flags, "-flto=thin");
    } else if (p == "full" || p == "all") {
      append_if_missing(flags, "-flto");
    }
  }

  if (const auto* pgo = std::getenv("SPARK_PGO")) {
    const std::string mode = pgo;
    const char* profile = std::getenv("SPARK_PGO_PROFILE");

    if (mode == "instrument") {
      append_if_missing(flags, "-fprofile-instr-generate");
      append_if_missing(flags, "-fcoverage-mapping");
    } else if (mode == "use" && profile != nullptr && *profile != '\0') {
      append_if_missing(flags, "-fprofile-instr-use=" + std::string(profile));
    }
  }

  return flags;
}

void append_flags(std::ostringstream& command, const std::vector<std::string>& flags) {
  for (const auto& flag : flags) {
    command << " " << shell_escape(flag);
  }
}

int command_exit_code(int status) {
  if (status == -1) {
    return 1;
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return 1;
}

int run_system_command(const std::string& command) {
  return command_exit_code(std::system(command.c_str()));
}

void write_file(const std::filesystem::path& path, const std::string& data) {
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("failed to open output file: " + path.string());
  }
  out << data;
}

bool parse_and_typecheck(const std::string& file_path, ProgramBundle& out) {
  out.source = read_file(file_path);
  spark::Parser parser(out.source);
  out.program = parser.parse_program();
  out.checker.check(*out.program);
  return !out.checker.has_errors();
}

bool lower_to_products(const ProgramBundle& bundle, PipelineProducts& products) {
  spark::CodeGenerator codegen;
  const auto result = codegen.generate(*bundle.program);
  if (!result.success) {
    for (const auto& message : result.diagnostics) {
      products.diagnostics.push_back("ir: " + message);
    }
    return false;
  }
  products.ir = result.output;

  spark::IRToCGenerator cgen;
  const auto c_result = cgen.translate(result.output);
  if (!c_result.success) {
    for (const auto& message : c_result.diagnostics) {
      products.diagnostics.push_back("cgen: " + message);
    }
    return false;
  }
  products.c_source = c_result.output;
  if (!c_result.diagnostics.empty()) {
    for (const auto& message : c_result.diagnostics) {
      products.diagnostics.push_back("cgen-warn: " + message);
    }
  }
  return true;
}

TierSummary summarize_tier_report(const spark::TypeChecker& checker) {
  TierSummary summary;

  for (const auto& fn : checker.function_reports()) {
    if (fn.tier == spark::TierLevel::T4) {
      ++summary.t4_count;
      continue;
    }
    if (fn.tier == spark::TierLevel::T5) {
      ++summary.t5_count;
    } else {
      ++summary.t8_count;
    }
    if (fn.tier != spark::TierLevel::T4) {
      summary.blocking_records.push_back(fn);
    }
  }

  for (const auto& loop : checker.loop_reports()) {
    if (loop.tier == spark::TierLevel::T4) {
      ++summary.t4_count;
      continue;
    }
    if (loop.tier == spark::TierLevel::T5) {
      ++summary.t5_count;
    } else {
      ++summary.t8_count;
    }
    if (loop.tier != spark::TierLevel::T4) {
      summary.blocking_records.push_back(loop);
    }
  }

  return summary;
}
