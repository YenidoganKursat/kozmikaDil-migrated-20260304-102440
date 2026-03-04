#pragma once

#include <memory>
#include <string>
#include <vector>

#include "spark/ast.h"
#include "spark/semantic.h"

namespace spark::core::dto {

// Atom DTO
struct SourceId {
  std::string value;
};

struct FilePath {
  std::string value;
};

struct SourceText {
  std::string value;
};

struct DiagnosticMessage {
  std::string value;
};

// Molecule DTO
struct SourceUnit {
  SourceId source_id;
  FilePath path;
  SourceText source;
};

struct DiagnosticEntry {
  SourceId source_id;
  DiagnosticMessage message;
};

// Compound DTO
struct ProgramBundle {
  SourceUnit source_unit;
  std::unique_ptr<Program> program;
  TypeChecker checker;
};

struct PipelineProducts {
  std::string ir;
  std::string c_source;
  std::vector<std::string> diagnostics;
};

struct BuildTuning {
  std::string target_triple;
  std::string sysroot;
  std::string lto_mode;
  std::string pgo_mode;
  std::string pgo_profile;
  std::string optimization_profile;
  int auto_pgo_runs = 0;
};

// Tissue DTO
struct CompileRequest {
  FilePath source_file;
  bool emit_c = false;
  bool emit_asm = false;
  bool emit_llvm = false;
  bool emit_mlir = false;
  std::string output_path;
};

struct RunRequest {
  FilePath source_file;
  bool force_interpreter = false;
  bool allow_t5 = false;
  bool explain_layout = false;
  BuildTuning tuning;
};

// Organ DTO
struct CompileResponse {
  bool success = false;
  PipelineProducts products;
  std::vector<std::string> diagnostics;
};

struct RunResponse {
  bool success = false;
  int exit_code = 1;
  std::vector<std::string> diagnostics;
};

// System DTO
struct CompilerSystemState {
  std::vector<DiagnosticEntry> diagnostics;
};

// Organism DTO
struct CompilerOrganismSnapshot {
  CompilerSystemState state;
  std::vector<std::string> notes;
};

}  // namespace spark::core::dto
