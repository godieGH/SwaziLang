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
  auto print_usage = []() {
    std::cout << "Usage: swazi [options] [file]\n"
    << "Options:\n"
    << "  -v, --version    Print version and exit\n"
    << "  -i               Start REPL (interactive)\n"
    << "  -h, --help       Show this help message\n"
    << "\n"
    << "If a filename starts with '-', either use `--` to end options\n"
    << "or prefix the filename with a path (for example `./-weird.sl`):\n"
    << "  swazi -- -weird.sl\n";
  };


  if (argc == 1) {
    run_repl_mode();
    return 0;
  }

  // Simple options parser: scan argv until we hit a non-option or `--`.
  std::string potential;
  bool seen_double_dash = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (seen_double_dash) {
      // After `--` everything is a filename argument; take first one
      potential = arg;
      break;
    }

    if (arg == "--") {
      seen_double_dash = true;
      continue;
    }

    // If it looks like an option (starts with '-') handle/validate it
    if (!arg.empty() && arg[0] == '-') {
      if (arg == "-v" || arg == "--version") {
        std::cout << "swazi v" << SWAZI_VERSION << std::endl;
        return 0;
      } else if (arg == "-i") {
        run_repl_mode();
        return 0;
      } else if (arg == "-h" || arg == "--help") {
        print_usage();
        return 0;
      } else {
        std::cerr << "swazi: unknown option '" << arg << "'\n";
        std::cerr << "Try 'swazi --help' for more information.\n";
        return 1;
      }
    }

    // First non-option argument is treated as filename
    potential = arg;
    break;
  }

  if (potential.empty()) {
    // No filename provided; behave like `swazi` (start repl)
    run_repl_mode();
    return 0;
  }

  // existing file-resolution logic (handles explicit extension and basename fallback)
  fs::path p(potential);
  fs::path file_to_run;

  // If the user provided a path that exists (with or without extension), use it.
  if (fs::exists(p)) {
    file_to_run = p;
  } else if (p.has_extension()) {
    // User specified an extension but the file doesn't exist -> error.
    std::cerr << "Error: File not found: " << p << std::endl;
    return 1;
  } else {
    // No extension provided and file-as-is doesn't exist: try extension fallbacks.
    auto found = find_file_with_extensions(p);
    if (found.has_value()) {
      file_to_run = found.value();
    } else {
      fs::path dir = p.parent_path();
      std::string filename = p.filename().string();
      std::vector < fs::path > tried;
      tried.push_back(dir.empty() ? fs::current_path() / (filename + ".sl"): dir / (filename + ".sl"));
      tried.push_back(dir.empty() ? fs::current_path() / (filename + ".swz"): dir / (filename + ".swz"));

      std::cerr << "Error: Could not find file for base name '" << potential << "'. Tried:\n";
      for (const auto &t: tried) std::cerr << "  " << t << "\n";
      return 1;
    }
  }


  run_file_mode(file_to_run.string());
  return 0;
}