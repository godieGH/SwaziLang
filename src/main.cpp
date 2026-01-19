#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "cli_commands.hpp"
#include "evaluator.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "print_debug.hpp"
#include "repl.hpp"
#include "sample.h"  // just for testing

namespace fs = std::filesystem;

// ----------------------
// SUGGESTIONs
// ----------------------

static size_t levenshtein_distance(const std::string& s1, const std::string& s2) {
    const size_t len1 = s1.length(), len2 = s2.length();
    std::vector<std::vector<size_t>> d(len1 + 1, std::vector<size_t>(len2 + 1));

    for (size_t i = 0; i <= len1; ++i) d[i][0] = i;
    for (size_t j = 0; j <= len2; ++j) d[0][j] = j;

    for (size_t i = 1; i <= len1; ++i) {
        for (size_t j = 1; j <= len2; ++j) {
            size_t cost = (s1[i - 1] == s2[j - 1]) ? 0 : 1;
            d[i][j] = std::min({d[i - 1][j] + 1,  // Deletion
                d[i][j - 1] + 1,                  // Insertion
                d[i - 1][j - 1] + cost});         // Substitution
        }
    }
    return d[len1][len2];
}

// Function to suggest the closest file name
static std::optional<std::string> suggest_closest_file(const fs::path& base_name, int max_distance = 3) {
    fs::path dir = base_name.parent_path();
    if (dir.empty()) {
        dir = fs::current_path();
    }

    // Only suggest files/modules from the directory specified (or current)
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        return std::nullopt;
    }

    const std::string search_target = base_name.filename().string();
    size_t best_distance = max_distance + 1;
    std::string best_match;

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            std::string current_name = entry.path().filename().string();
            size_t distance = levenshtein_distance(search_target, current_name);

            if (distance < best_distance) {
                best_distance = distance;
                best_match = current_name;
            }
        }
    }

    if (best_distance <= max_distance) {
        // Construct the suggestion path, keeping the directory if present
        return (dir / best_match).string();
    }

    return std::nullopt;
}

static void run_file_mode(const std::string& filename, const std::vector<std::string>& cli_args) {
    if (fs::is_directory(filename)) {
        std::cerr << "Error: Cannot execute `" << filename << "` is a directory not a file/module." << std::endl;
        return;
    }

    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file `" << filename << "`. " << std::endl;
        return;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source_code = buffer.str();
    if (source_code.empty() || source_code.back() != '\n') source_code.push_back('\n');

    try {
        SourceManager src_mgr(filename, source_code);

        Lexer lexer(source_code, filename, &src_mgr);
        std::vector<Token> tokens = lexer.tokenize();

        // print_tokens(tokens);

        Parser parser(tokens);
        std::unique_ptr<ProgramNode> ast = parser.parse();

        // print_program_debug(ast.get(), 2);

        Evaluator evaluator;
        // populate argv in runtime before evaluating so modules can read it
        evaluator.set_cli_args(cli_args);
        evaluator.set_entry_point(filename);
        evaluator.evaluate(ast.get());
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "Unknown fatal error\n";
    }
}

static std::optional<fs::path> find_file_with_extensions(const fs::path& base) {
    const std::vector<std::string> exts = {
        ".sl",
        ".swz"};                                      // order matters
    fs::path dir = base.parent_path();                // may be empty
    std::string filename = base.filename().string();  // bare filename (no dir)

    for (const auto& ext : exts) {
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
    // uv_init();
    auto print_usage = []() {
        std::cout << "Usage: swazi [options] [file]\n"
                  << "       swazi [command] [args...]\n"
                  << "Options:\n"
                  << "  -v, --version    Print version and exit\n"
                  << "  -i               Start REPL (interactive)\n"
                  << "  -h, --help       Show this help message\n"
                  << "\n"
                  << "Commands:\n"
                  << "  init             Initialize a new Swazi project\n"
                  << "  project          Project information commands\n"
                  << "  vendor           Vendor directory management\n"
                  << "  cache            Cache management\n"
                  << "  start            Run the project\n"
                  << "  publish          Publish to registry\n"
                  << "  install          Install dependencies\n"
                  << "  format           Format code\n"
                  << "\n"
                  << "If a filename starts with '-', either use `--` to end options\n"
                  << "or prefix the filename with a path (for example `./-file.sl`):\n"
                  << "  swazi -- -file.sl\n";
    };

    std::vector<std::string> cli_args;
    cli_args.reserve((size_t)argc);
    for (int i = 0; i < argc; ++i) cli_args.emplace_back(argv[i]);

    if (argc == 1) {
        run_repl_mode();
        return 0;
    }

    // Check if first argument is a CLI command
    std::string first_arg = argv[1];
    std::vector<std::string> known_commands = {
        "init", "project", "vendor", "cache", "start", "run", "publish", "install", "format"};

    bool is_command = std::find(known_commands.begin(), known_commands.end(), first_arg) != known_commands.end();

    if (is_command) {
        // Build args vector for CLI commands (skip argv[0])
        std::vector<std::string> command_args;
        for (int i = 1; i < argc; ++i) {
            command_args.emplace_back(argv[i]);
        }

        auto result = swazi::cli::execute_command(command_args);
        if (!result.message.empty()) {
            std::cerr << result.message << std::endl;
        }
        return result.exit_code;
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

        auto suggestion = suggest_closest_file(p, 2);
        if (suggestion.has_value()) {
            std::cerr << " --> Did you mean: `" << suggestion.value() << "`?" << std::endl;
        }
        return 1;
    } else {
        // No extension provided and file-as-is doesn't exist: try extension fallbacks.
        auto found = find_file_with_extensions(p);
        if (found.has_value()) {
            file_to_run = found.value();
        } else {
            fs::path dir = p.parent_path();
            std::string filename = p.filename().string();
            std::vector<fs::path> tried;
            tried.push_back(dir.empty() ? fs::current_path() / (filename + ".sl") : dir / (filename + ".sl"));
            tried.push_back(dir.empty() ? fs::current_path() / (filename + ".swz") : dir / (filename + ".swz"));

            std::cerr << "Error: Could not find file for base name '" << potential << "'. Tried:\n";
            for (const auto& t : tried) std::cerr << "  " << t << "\n";
            return 1;
        }
    }

    // Pass cli_args through so evaluator can populate global argv for scripts.
    run_file_mode(file_to_run.string(), cli_args);
    return 0;
}