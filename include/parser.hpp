#pragma once
#include <vector>
#include <memory>
#include "token.hpp"
#include "ast.hpp"

class Parser {
public:
    Parser(const std::vector<Token>& tokens);
    std::unique_ptr<ProgramNode> parse();

private:
    std::vector<Token> tokens;
    size_t position = 0;

    Token peek() const;
    Token consume();
    bool match(TokenType t);
    void expect(TokenType t, const std::string& errMsg);

    // expression parsing (precedence chain)
    std::unique_ptr<ExpressionNode> parse_expression();
    std::unique_ptr<ExpressionNode> parse_logical_or();
    std::unique_ptr<ExpressionNode> parse_logical_and();
    std::unique_ptr<ExpressionNode> parse_equality();
    std::unique_ptr<ExpressionNode> parse_comparison();
    std::unique_ptr<ExpressionNode> parse_additive();
    std::unique_ptr<ExpressionNode> parse_multiplicative();
    std::unique_ptr<ExpressionNode> parse_exponent();
    std::unique_ptr<ExpressionNode> parse_unary();
    std::unique_ptr<ExpressionNode> parse_primary();
    std::unique_ptr<ExpressionNode> parse_call(std::unique_ptr<ExpressionNode> callee);

    // statements
    std::unique_ptr<StatementNode> parse_statement();
    std::unique_ptr<StatementNode> parse_variable_declaration();
    std::unique_ptr<StatementNode> parse_print_statement(bool newline);
    std::unique_ptr<StatementNode> parse_assignment_or_expression_statement();

    // function parsing
    std::unique_ptr<StatementNode> parse_function_declaration();
    std::unique_ptr<StatementNode> parse_return_statement();

    // control-flow parsing
    std::unique_ptr<StatementNode> parse_if_statement();

    // helper to parse a block of statements either from an INDENT/DEDENT
    // or from brace-style { ... }. accept_brace_style==true allows '{ ... }',
    // otherwise will expect INDENT-style (called after COLON/NEWLINE).
    std::vector<std::unique_ptr<StatementNode>> parse_block(bool accept_brace_style);
};