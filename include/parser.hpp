#pragma once
#include <vector>
#include "token.hpp"
#include "ast.hpp"

class Parser {
public:
    Parser(std::vector<Token> tokens);
    std::unique_ptr<ProgramNode> parse();

private:
    std::vector<Token> tokens;
    size_t position = 0;

    Token peek();
    Token consume();
    
    std::unique_ptr<ExpressionNode> parse_expression();
    std::unique_ptr<StatementNode> parse_statement();
};
