#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <optional>
#include <cstdlib>

#include "lexer.hpp"
#include "parser.hpp"
#include "evaluator.hpp"
#include "repl.hpp"

#include "print_debug.hpp"

#include <filesystem>

namespace fs = std::filesystem;

static void run_file_mode(const std::string &filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    std::cerr << "Error: Could not open file " << filename << std::endl;
    return;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string source_code = buffer.str();
  if (source_code.empty() || source_code.back() != '\n') source_code.push_back('\n');

  try {
    Lexer lexer(source_code, filename);
    std::vector < Token > tokens = lexer.tokenize();

    //print_tokens(tokens);

    Parser parser(tokens);
    std::unique_ptr < ProgramNode > ast = parser.parse();

    //print_program_debug(ast.get(), 2);

    Evaluator evaluator;
    evaluator.evaluate(ast.get());
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
  } catch (...) {
    std::cerr << "Unknown fatal error\n";
  }
}

static std::optional < fs::path > find_file_with_extensions(const fs::path &base) {
  const std::vector < std::string > exts = {
    ".sl",
    ".swz"
  }; // order matters
  fs::path dir = base.parent_path(); // may be empty
  std::string filename = base.filename().string(); // bare filename (no dir)

  for (const auto &ext: exts) {
    fs::path candidate;
    if (dir.empty()) {
      candidate = fs::current_path() / (filename + ext);
    } else {
      candidate = dir / (filename + ext);
    }
    if (fs::exists(candidate)) return candidate;
  }
  return std::nullopt;
}

int main(int argc, char* argv[]) {
  if (argc == 1) {
    run_repl_mode();
    return 0;
  }

  std::string arg = argv[1];

  if (arg == "-v" || arg == "--version") {
    // SWAZI_VERSION comes from CMake via target_compile_definitions
    std::cout << "swazi v" << SWAZI_VERSION << std::endl;
    return 0;
  }

  if (arg == "-i") {
    run_repl_mode();
    return 0;
  }

  fs::path p(arg);
  fs::path file_to_run;


  if (p.has_extension()) {
    // User provided explicit filename (e.g., app.sl or dir/app.swz)
    if (fs::exists(p)) {
      file_to_run = p;
    } else {
      std::cerr << "Error: File not found: " << p << std::endl;
      return 1;
    }
  } else {
    // Try basename with .sl/.swz in the provided directory (or current dir).
    auto found = find_file_with_extensions(p);
    if (found.has_value()) {
      file_to_run = found.value();
    } else {
      // Build helpful error showing what was tried
      fs::path dir = p.parent_path();
      std::string filename = p.filename().string();
      std::vector < fs::path > tried;
      tried.push_back(dir.empty() ? fs::current_path() / (filename + ".sl"): dir / (filename + ".sl"));
      tried.push_back(dir.empty() ? fs::current_path() / (filename + ".swz"): dir / (filename + ".swz"));

      std::cerr << "Error: Could not find file for base name '" << arg << "'. Tried:\n";
      for (const auto &t: tried) std::cerr << "  " << t << "\n";
      return 1;
    }
  }

  run_file_mode(file_to_run.string());
  return 0;
}