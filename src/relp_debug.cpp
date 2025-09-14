#include "ast.hpp"
#include <iostream>
#include <typeinfo>

// Simple AST debug printer for the REPL / tests.
// Call print_program_debug(program) from your REPL harness after parsing.
void print_program_debug(ProgramNode* ast) {
    if (!ast) {
        std::cout << "AST: <null>\n";
        return;
    }

    std::cout << "Program: " << ast->body.size() << " top-level statements\n";
    for (const auto& stmt : ast->body) {
        if (!stmt) {
            std::cout << "  <null statement>\n";
            continue;
        }
        // Use token's to_string for file:line:col
        std::string loc = stmt->token.loc.to_string();

        // Avoid evaluating *stmt inside typeid(...) which can trigger the compiler warning.
        // Store the raw pointer first and then use typeid on the referenced object.
        auto raw = stmt.get();
        std::cout << "  stmt at " << loc << " - type: " << typeid(*raw).name() << "\n";
    }
}