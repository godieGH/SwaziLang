#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "lexer.hpp"
#include "parser.hpp"
#include "evaluator.hpp"

int main(int argc, char* argv[]) {
    // Ensure a filename is provided
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <filename.swz>" << " Or " << argv[0] << " <filename.sl>" << std::endl;
        return 1;
    }

    // Read the file
    std::ifstream file(argv[1]);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file " << argv[1] << std::endl;
        return 1;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source_code = buffer.str();

    try {
        // 1. Lexer
        Lexer lexer(source_code, argv[1]);
        std::vector<Token> tokens = lexer.tokenize();

        // 2. Parser
        Parser parser(tokens);
        std::unique_ptr<ProgramNode> ast = parser.parse();

        // 3. Evaluator
        Evaluator evaluator;
        evaluator.evaluate(ast.get());

    } catch (const std::runtime_error& e) {
        std::cerr << "Runtime Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
