#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "lexer.hpp"
#include "parser.hpp"
#include "evaluator.hpp"

// Optional debug token printer (declare if you link it)
// void print_tokens(const std::vector<Token>& tokens);

static bool is_likely_incomplete_input(const std::string &err) {
    // Treat these parser expectations as "incomplete" so REPL will continue reading.
    // You can extend this list if you see other parser messages for unterminated input.
    return err.find("Expected dedent") != std::string::npos ||
           err.find("Expected '}'") != std::string::npos ||
           err.find("Expected ')'") != std::string::npos ||
           err.find("Expected ']'") != std::string::npos ||
           err.find("Expected ':'") != std::string::npos;
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
        std::vector<Token> tokens = lexer.tokenize();
        Parser parser(tokens);
        std::unique_ptr<ProgramNode> ast = parser.parse();
        Evaluator evaluator;
        evaluator.evaluate(ast.get());
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "Unknown fatal error\n";
    }
}

static void run_repl_mode() {
    std::string buffer;
    Evaluator evaluator; // reuse evaluator across inputs if desired
    std::cout << "Swazi REPL — type 'exit' or Ctrl-D to quit\n";
    while (true) {
        // Choose prompt: top-level or continuation
        std::cout << (buffer.empty() ? "Swazi>> " : "...>> ");
        std::cout.flush();
        std::string line;
        if (!std::getline(std::cin, line)) { // EOF (Ctrl-D)
            std::cout << "\n";
            break;
        }
        if (line == "exit" || line == "quit") break;

        // Append input line
        buffer += line;
        buffer.push_back('\n');

        // Try to lex/parse/evaluate the current buffer.
        try {
            // Ensure buffer ends with newline (helps indentation logic)
            if (buffer.empty() || buffer.back() != '\n') buffer.push_back('\n');

            Lexer lexer(buffer, "<repl>");
            std::vector<Token> tokens = lexer.tokenize();

            // Optional: debug token printing
            // print_tokens(tokens);

            Parser parser(tokens);
            std::unique_ptr<ProgramNode> ast = parser.parse();

            // If parse succeeded, evaluate and clear buffer
            evaluator.evaluate(ast.get());
            buffer.clear();
        } catch (const std::exception &e) {
            std::string msg = e.what();
            if (is_likely_incomplete_input(msg)) {
                // Keep reading more lines (no error message) — user is likely in a block.
                continue;
            }
            // Real error: show message and reset buffer.
            std::cerr << "Error: " << msg << std::endl;
            buffer.clear();
        } catch (...) {
            std::cerr << "Unknown fatal error\n";
            buffer.clear();
        }
    }
}

int main(int argc, char* argv[]) {
    // If a filename is provided, run file mode. If "-i" or no args, start REPL.
    if (argc == 1) {
        run_repl_mode();
        return 0;
    }
    std::string arg = argv[1];
    if (arg == "-i") {
        run_repl_mode();
        return 0;
    }
    // Otherwise treat argument as filename
    run_file_mode(arg);
    return 0;
}