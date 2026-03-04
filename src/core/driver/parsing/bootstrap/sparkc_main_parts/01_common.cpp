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

struct BuildTuningOptions {
  std::string target_triple;
  std::string sysroot;
  std::string lto_mode;
  std::string pgo_mode;
  std::string pgo_profile;
  std::string optimization_profile;
  int auto_pgo_runs = 0;
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

std::string normalize_optimization_profile(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  if (value == "layered" || value == "layered_max" || value == "layeredmax") {
    return "layered-max";
  }
  if (value == "max_perf" || value == "max-perf") {
    return "max";
  }
  if (value == "default" || value == "balanced" || value == "none") {
    return "balanced";
  }
  return value;
}

std::string lowercase_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool is_forbidden_relaxed_fp_flag(const std::string& flag) {
  const auto lower = lowercase_ascii(flag);
  return lower == "-ffast-math" || lower == "-ofast" ||
         lower == "-funsafe-math-optimizations" ||
         lower == "-fassociative-math" ||
         lower == "-freciprocal-math" ||
         lower == "-fno-signed-zeros" ||
         lower == "-fno-trapping-math";
}

void enforce_strict_precision_compiler_flags(std::vector<std::string>& flags) {
  std::vector<std::string> filtered;
  filtered.reserve(flags.size() + 4);
  for (const auto& flag : flags) {
    if (is_forbidden_relaxed_fp_flag(flag)) {
      if (lowercase_ascii(flag) == "-ofast") {
        filtered.push_back("-O3");
      }
      continue;
    }
    filtered.push_back(flag);
  }
  flags = std::move(filtered);
  append_if_missing(flags, "-fno-fast-math");
}

const char* getenv_non_empty(const char* name) {
  const char* raw = std::getenv(name);
  if (!raw || *raw == '\0') {
    return nullptr;
  }
  return raw;
}

bool env_truthy(const char* name, bool fallback = false) {
  const auto* raw = getenv_non_empty(name);
  if (!raw) {
    return fallback;
  }
  std::string text = raw;
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  if (text == "0" || text == "false" || text == "off" || text == "no") {
    return false;
  }
  return true;
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
        "-funroll-loops",
        "-fno-fast-math",
        "-fno-math-errno",
    };
  }

  if (!vector_contains_flag(flags, "-DNDEBUG")) {
    append_if_missing(flags, "-DNDEBUG");
  }

  const auto has_prefix = [&](const std::string& prefix) {
    return std::find_if(flags.begin(), flags.end(), [&](const std::string& flag) {
             return flag.rfind(prefix, 0) == 0;
           }) != flags.end();
  };
  const char* target = std::getenv("SPARK_TARGET_TRIPLE");
  const bool host_native_target = (target == nullptr || *target == '\0');
  if (host_native_target && !has_prefix("-march=")) {
    append_if_missing(flags, "-march=native");
  }
  if (host_native_target && !has_prefix("-mtune=")) {
    append_if_missing(flags, "-mtune=native");
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

  bool disable_target_flag = false;
  if (const auto* raw = std::getenv("SPARK_DISABLE_TARGET_FLAG")) {
    std::string flag = raw;
    std::transform(flag.begin(), flag.end(), flag.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    disable_target_flag = !(flag.empty() || flag == "0" || flag == "false" || flag == "off" || flag == "no");
  }

  if (const auto* target = std::getenv("SPARK_TARGET_TRIPLE")) {
    if (!disable_target_flag && *target != '\0') {
      append_if_missing(flags, "--target=" + std::string(target));
    }
  }
  if (const auto* sysroot = std::getenv("SPARK_SYSROOT")) {
    if (*sysroot != '\0') {
      append_if_missing(flags, "--sysroot=" + std::string(sysroot));
    }
  }
  {
    const auto target_extra_flags = split_env_flags(std::getenv("SPARK_TARGET_CFLAGS"));
    for (const auto& flag : target_extra_flags) {
      append_if_missing(flags, flag);
    }
  }

  const auto* profile_raw = getenv_non_empty("SPARK_OPT_PROFILE");
  const auto profile = profile_raw ? normalize_optimization_profile(profile_raw) : std::string("max");
  if (profile == "max" || profile == "layered-max") {
    append_if_missing(flags, "-fno-semantic-interposition");
    append_if_missing(flags, "-ffunction-sections");
    append_if_missing(flags, "-fdata-sections");
    // Host-tuned native code generation for local max runtime profile.
    // Keep cross-target workflows intact by skipping when explicit target/sysroot is set.
    const bool explicit_target = getenv_non_empty("SPARK_TARGET_TRIPLE") != nullptr ||
                                 getenv_non_empty("SPARK_SYSROOT") != nullptr;
    if (!explicit_target) {
#if defined(__aarch64__) || defined(__arm64__)
      append_if_missing(flags, "-mcpu=native");
#else
      append_if_missing(flags, "-march=native");
      append_if_missing(flags, "-mtune=native");
#endif
    }
  }
  if (profile == "layered-max") {
    append_if_missing(flags, "-falign-functions=32");
    append_if_missing(flags, "-falign-loops=32");
  }

  // Global numerical policy: strict precision only.
  // Strip any relaxed-FP flag and force conservative IEEE-like math options.
  enforce_strict_precision_compiler_flags(flags);

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

std::string layout_tag_to_string(spark::Value::LayoutTag tag) {
  switch (tag) {
    case spark::Value::LayoutTag::Unknown:
      return "Unknown";
    case spark::Value::LayoutTag::PackedInt:
      return "PackedInt";
    case spark::Value::LayoutTag::PackedDouble:
      return "PackedDouble";
    case spark::Value::LayoutTag::PromotedPackedDouble:
      return "PromotedPackedDouble";
    case spark::Value::LayoutTag::ChunkedUnion:
      return "ChunkedUnion";
    case spark::Value::LayoutTag::GatherScatter:
      return "GatherScatter";
    case spark::Value::LayoutTag::BoxedAny:
      return "BoxedAny";
  }
  return "Unknown";
}

void apply_build_tuning_env(const BuildTuningOptions& options) {
  if (!options.target_triple.empty()) {
    setenv("SPARK_TARGET_TRIPLE", options.target_triple.c_str(), 1);
  }
  if (!options.sysroot.empty()) {
    setenv("SPARK_SYSROOT", options.sysroot.c_str(), 1);
  }
  if (!options.lto_mode.empty()) {
    setenv("SPARK_LTO", options.lto_mode.c_str(), 1);
  }
  if (!options.pgo_mode.empty()) {
    setenv("SPARK_PGO", options.pgo_mode.c_str(), 1);
  }
  if (!options.pgo_profile.empty()) {
    setenv("SPARK_PGO_PROFILE", options.pgo_profile.c_str(), 1);
  }
  if (!options.optimization_profile.empty()) {
    const auto normalized = normalize_optimization_profile(options.optimization_profile);
    setenv("SPARK_OPT_PROFILE", normalized.c_str(), 1);
  }
  if (options.auto_pgo_runs > 0) {
    setenv("SPARK_AUTO_PGO", "1", 1);
    setenv("SPARK_AUTO_PGO_RUNS", std::to_string(options.auto_pgo_runs).c_str(), 1);
  }

  const auto* profile_raw = getenv_non_empty("SPARK_OPT_PROFILE");
  const auto profile = profile_raw ? normalize_optimization_profile(profile_raw) : std::string("max");
  if (profile == "max" || profile == "layered-max") {
    if (!getenv_non_empty("SPARK_LTO")) {
      setenv("SPARK_LTO", "full", 1);
    }
    // Semantic-preserving runtime fast path toggles.
    if (!getenv_non_empty("SPARK_ASSIGN_INPLACE_NUMERIC")) {
      setenv("SPARK_ASSIGN_INPLACE_NUMERIC", "1", 1);
    }
    if (!getenv_non_empty("SPARK_BINARY_EXPR_FUSION")) {
      setenv("SPARK_BINARY_EXPR_FUSION", "1", 1);
    }
  }
  if (profile == "layered-max") {
    // Build-time is intentionally expensive in this profile.
    if (!getenv_non_empty("SPARK_AUTO_PGO") && !getenv_non_empty("SPARK_PGO")) {
      setenv("SPARK_AUTO_PGO", "1", 1);
    }
    if (!getenv_non_empty("SPARK_AUTO_PGO_RUNS")) {
      setenv("SPARK_AUTO_PGO_RUNS", "2", 1);
    }
  }
}

bool target_matches_host(const std::string& target_triple) {
  if (target_triple.empty()) {
    return true;
  }
  const auto host = spark::detect_cpu_features();
  const std::string arch = host.arch.empty() ? "unknown" : host.arch;
  if (arch == "unknown") {
    return true;
  }
  return target_triple.find(arch) != std::string::npos;
}

void print_layout_summary(const spark::Interpreter& interpreter) {
  auto globals = interpreter.snapshot_globals();
  std::vector<std::string> names;
  names.reserve(globals.size());
  for (const auto& it : globals) {
    names.push_back(it.first);
  }
  std::sort(names.begin(), names.end());
  std::cout << "LayoutSummary:\n";
  for (const auto& name : names) {
    const auto iter = globals.find(name);
    if (iter == globals.end()) {
      continue;
    }
    const auto& value = iter->second;
    if (value.kind == spark::Value::Kind::List) {
      std::cout << "  " << name
                << " kind=list"
                << " plan=" << layout_tag_to_string(value.list_cache.plan)
                << " live=" << (value.list_cache.live_plan ? "1" : "0")
                << " analyze=" << value.list_cache.analyze_count
                << " materialize=" << value.list_cache.materialize_count
                << " cache_hits=" << value.list_cache.cache_hit_count
                << " invalidations=" << value.list_cache.invalidation_count
                << "\n";
    } else if (value.kind == spark::Value::Kind::Matrix && value.matrix_value) {
      std::cout << "  " << name
                << " kind=matrix"
                << " shape=" << value.matrix_value->rows << "x" << value.matrix_value->cols
                << " plan=" << layout_tag_to_string(value.matrix_cache.plan)
                << " live=" << (value.matrix_cache.live_plan ? "1" : "0")
                << " analyze=" << value.matrix_cache.analyze_count
                << " materialize=" << value.matrix_cache.materialize_count
                << " cache_hits=" << value.matrix_cache.cache_hit_count
                << " invalidations=" << value.matrix_cache.invalidation_count
                << "\n";
    }
  }
}

bool parse_and_typecheck(const std::string& file_path, ProgramBundle& out) {
  out.source = read_file(file_path);
  spark::Parser parser(out.source);
  out.program = parser.parse_program();
  out.checker.check(*out.program);
  return !out.checker.has_errors();
}

bool is_identifier_char_for_scan(char c) {
  const auto uc = static_cast<unsigned char>(c);
  return std::isalnum(uc) || c == '_';
}

bool source_contains_identifier_token(const std::string& source, const std::string& token) {
  if (token.empty()) {
    return false;
  }
  std::size_t pos = 0;
  while (true) {
    pos = source.find(token, pos);
    if (pos == std::string::npos) {
      return false;
    }
    const std::size_t end = pos + token.size();
    const bool left_ok = (pos == 0) || !is_identifier_char_for_scan(source[pos - 1]);
    const bool right_ok = (end >= source.size()) || !is_identifier_char_for_scan(source[end]);
    if (left_ok && right_ok) {
      return true;
    }
    ++pos;
  }
}

bool source_uses_high_precision_float_primitives(const std::string& source) {
  return source_contains_identifier_token(source, "f128") ||
         source_contains_identifier_token(source, "f256") ||
         source_contains_identifier_token(source, "f512");
}

bool env_flag_enabled(const char* name) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') {
    return false;
  }
  std::string v(raw);
  std::transform(v.begin(), v.end(), v.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return v == "1" || v == "true" || v == "on" || v == "yes";
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
