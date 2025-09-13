#include "parser.hpp"
#include <stdexcept>

Parser::Parser(std::vector<Token> tokens) : tokens(tokens) {}

Token Parser::peek() { return tokens[position]; }
Token Parser::consume() { return tokens[position++]; }

std::unique_ptr<ExpressionNode> Parser::parse_expression() {
    Token token = peek();
    if (token.type == TokenType::NAMBA) {
        auto node = std::make_unique<NumericLiteralNode>();
        node->value = std::stod(consume().value);
        return node;
    }
    if (token.type == TokenType::NENO) {
        auto node = std::make_unique<StringLiteralNode>();
        node->value = consume().value;
        return node;
    }
    if (token.type == TokenType::BOOLEAN) {
        auto node = std::make_unique<BooleanLiteralNode>();
        node->value = (consume().value == "kweli");
        return node;
    }
    if (token.type == TokenType::IDENTIFIER) {
        auto node = std::make_unique<IdentifierNode>();
        node->name = consume().value;
        return node;
    }
    throw std::runtime_error("Unexpected token in expression");
}

std::unique_ptr<StatementNode> Parser::parse_statement() {
    if (peek().type == TokenType::DATA) {
        consume();
        auto node = std::make_unique<VariableDeclarationNode>();
        node->identifier = consume().value;
        consume(); // Consume '='
        node->value = parse_expression();
        if (peek().type == TokenType::SEMICOLON) consume();
        return node;
    }
    if (peek().type == TokenType::CHAPISHA) {
        consume(); // Consume 'print'
        auto node = std::make_unique<PrintStatementNode>();
        node->expression = parse_expression();
        if (peek().type == TokenType::SEMICOLON) consume();
        return node;
    }
    throw std::runtime_error("Unknown statement type");
}

std::unique_ptr<ProgramNode> Parser::parse() {
    auto program = std::make_unique<ProgramNode>();
    while (peek().type != TokenType::EOF_TOKEN) {
        program->body.push_back(parse_statement());
    }
    return program;
}
