#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "lexer.hpp"
#include "parser.hpp"
#include "evaluator.hpp"

#include "print_debug.hpp"

#include <filesystem>
namespace fs = std::filesystem;


static bool is_likely_incomplete_input(const std::string &err) {
    // Treat parser expectations that mean "unterminated input" as incomplete so REPL continues.
    // Removed "Expected ':'" because the REPL now proactively handles ':'-started blocks.
    return err.find("Expected dedent") != std::string::npos ||
           err.find("Expected '}'") != std::string::npos ||
           err.find("Expected ')'") != std::string::npos ||
           err.find("Expected ']'") != std::string::npos;
}

// --- Helpers for REPL indentation and block detection ---

static int count_leading_indent_spaces(const std::string &line) {
    // Tabs are treated as 4 spaces here. Adjust if you want different behavior.
    int count = 0;
    for (char c : line) {
        if (c == ' ') count += 1;
        else if (c == '\t') count += 4;
        else break;
    }
    return count;
}

static bool is_blank_or_spaces(const std::string &s) {
    for (unsigned char c : s) if (!std::isspace(c)) return false;
    return true;
}

static char last_non_ws_char(const std::string &line) {
    for (int i = (int)line.size() - 1; i >= 0; --i) {
        unsigned char ch = static_cast<unsigned char>(line[i]);
        if (!std::isspace(ch)) return static_cast<char>(ch);
    }
    return '\0';
}

static bool ends_with_colon(const std::string &line) {
    return last_non_ws_char(line) == ':';
}

static bool ends_with_open_brace(const std::string &line) {
    return last_non_ws_char(line) == '{';
}

// --- REPL: colon-block tracking + brace continuation ---
// Behavior:
//  - If a line ends with ':'    -> push indent level and continue reading (colon block).
//  - If a line ends with '{'    -> continue reading (brace block) but DO NOT push indent level.
//  - If inside colon blocks: typing a line with indent <= top-of-stack dedents and triggers parse+exec.
//  - Parser-driven incomplete input (unclosed paren, expected '}', etc.) still handled by exception check.

static void run_repl_mode() {
    std::string buffer;
    Evaluator evaluator; // reuse evaluator across inputs if desired
    std::vector<int> colon_indent_stack; // record indentation bases only for ':' blocks
    std::cout << "Swazi REPL — type 'exit' or Ctrl-D to quit\n";

    while (true) {
        // Prompt
        std::cout << (buffer.empty() ? "Swazi>> " : "...>> ");
        std::cout.flush();

        std::string line;
        if (!std::getline(std::cin, line)) { // EOF (Ctrl-D)
            std::cout << "\n";
            break;
        }
        if (line == "exit" || line == "quit") break;

        // Append line to buffer with newline
        buffer += line;
        buffer.push_back('\n');

        // ---- Proactive continuation decisions ----
        // 1) If line ends with ':' -> colon-block: push indent and continue reading.
        if (ends_with_colon(line)) {
            int base_indent = count_leading_indent_spaces(line);
            colon_indent_stack.push_back(base_indent);
            continue; // show continuation prompt; do not parse yet
        }

        // 2) If line ends with '{' -> brace-block continuation: continue reading but don't record indent
        if (ends_with_open_brace(line)) {
            continue; // rely on parser to decide when brace block is complete
        }

        // 3) If we are inside colon blocks, use indentation to decide whether to keep reading
        if (!colon_indent_stack.empty()) {
            // If user typed a blank line, we keep reading inside the block (no dedent).
            if (is_blank_or_spaces(line)) {
                continue;
            }

            int leading = count_leading_indent_spaces(line);
            if (leading > colon_indent_stack.back()) {
                // still inside the most recent colon block
                continue;
            } else {
                // dedent observed (typed a line at indent <= base of last colon block)
                // pop any colon blocks that are closed by this dedent
                while (!colon_indent_stack.empty() && leading <= colon_indent_stack.back()) {
                    colon_indent_stack.pop_back();
                }
                // fall through -> attempt parse+evaluate
            }
        }

        // ---- Try lex/parse/evaluate the accumulated buffer ----
        try {
            if (buffer.empty() || buffer.back() != '\n') buffer.push_back('\n');

            Lexer lexer(buffer, "<repl>");
            std::vector<Token> tokens = lexer.tokenize();

            Parser parser(tokens);
            std::unique_ptr<ProgramNode> ast = parser.parse();

            // Auto-print single-expression ASTs (like Python/Node REPLs)
            bool did_auto_print = false;
            if (ast->body.size() == 1) {
                if (auto exprStmt = dynamic_cast<ExpressionStatementNode*>(ast->body[0].get())) {
                    Value v = evaluator.evaluate_expression(exprStmt->expression.get());
                    if (!evaluator.is_void(v)) {
                        std::cout << evaluator.value_to_string(v) << "\n";
                    }
                    did_auto_print = true;
                }
            }

            if (!did_auto_print) {
                evaluator.evaluate(ast.get());
            }

            // Success: clear buffer and colon-indent stack (brace blocks are handled by parser)
            buffer.clear();
            colon_indent_stack.clear();
        } catch (const std::exception &e) {
            std::string msg = e.what();
            if (is_likely_incomplete_input(msg)) {
                // Parser says input is incomplete (e.g. unclosed paren/expected '}'), so keep reading.
                // Do NOT clear buffer or colon_indent_stack here.
                continue;
            }
            // Real error: show message and reset buffer and indent stack.
            std::cerr << "Error: " << msg << std::endl;
            buffer.clear();
            colon_indent_stack.clear();
        } catch (...) {
            std::cerr << "Unknown fatal error\n";
            buffer.clear();
            colon_indent_stack.clear();
        }
    }
}

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