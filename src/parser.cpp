#include "parser.hpp"
#include <stdexcept>
#include <cctype>

Parser::Parser(const std::vector<Token>& tokens) : tokens(tokens) {}

// Return current token or EOF token
Token Parser::peek() const {
    if (position < tokens.size()) return tokens[position];
    return Token{ TokenType::EOF_TOKEN, "", "", 0, 0 };
}

// Consume and return token or EOF token
Token Parser::consume() {
    if (position < tokens.size()) return tokens[position++];
    return Token{ TokenType::EOF_TOKEN, "", "", 0, 0 };
}

bool Parser::match(TokenType t) {
    if (peek().type == t) {
        consume();
        return true;
    }
    return false;
}

void Parser::expect(TokenType t, const std::string& errMsg) {
    if (peek().type != t) {
        Token tok = peek();
        throw std::runtime_error(
            "Parse error in file '" + tok.filename + "' at line " +
            std::to_string(tok.line) + ", column " +
            std::to_string(tok.col) + ": " + errMsg
        );
    }
    consume();
}

// ---------- parse entry ----------
std::unique_ptr<ProgramNode> Parser::parse() {
    auto program = std::make_unique<ProgramNode>();
    while (peek().type != TokenType::EOF_TOKEN) {
        program->body.push_back(parse_statement());
    }
    return program;
}

// ---------- statements ----------
std::unique_ptr<StatementNode> Parser::parse_statement() {
    Token p = peek();
    if (p.type == TokenType::DATA) {
        consume();
        return parse_variable_declaration();
    }
    if (p.type == TokenType::CHAPISHA) {
        consume();
        return parse_print_statement(true);
    }
    if (p.type == TokenType::ANDIKA) {
        consume();
        return parse_print_statement(false);
    }
    return parse_assignment_or_expression_statement();
}

std::unique_ptr<StatementNode> Parser::parse_variable_declaration() {
    bool is_constant = false;

    if (peek().type == TokenType::CONSTANT) {
        consume();
        is_constant = true;
    }

    // Expect identifier next and capture its token immediately
    expect(TokenType::IDENTIFIER, "Expected identifier after 'data'");
    Token idTok = tokens[position - 1]; // identifier token
    std::string name = idTok.value;

    std::unique_ptr<ExpressionNode> value = nullptr;

    // Optional assignment
    if (peek().type == TokenType::ASSIGN) {
        consume(); // consume '='
        value = parse_expression();
    }

    if (peek().type == TokenType::SEMICOLON) consume();

    auto node = std::make_unique<VariableDeclarationNode>();
    node->identifier = name;
    node->value = std::move(value);
    node->is_constant = is_constant;
    node->token = idTok;
    return node;
}

std::unique_ptr<StatementNode> Parser::parse_print_statement(bool newline) {
    // capture the keyword token (CHAPISHA / ANDIKA) which was consumed by caller
    Token kwTok = tokens[position - 1];

    std::vector<std::unique_ptr<ExpressionNode>> args;
    if (peek().type == TokenType::OPENPARENTHESIS) {
        consume();
        if (peek().type != TokenType::CLOSEPARENTHESIS) {
            do {
                args.push_back(parse_expression());
            } while (match(TokenType::COMMA));
        }
        expect(TokenType::CLOSEPARENTHESIS, "Expected ')' after print arguments");
    } else {
        // single expression without parentheses
        args.push_back(parse_expression());
    }
    if (peek().type == TokenType::SEMICOLON) consume();
    auto node = std::make_unique<PrintStatementNode>();
    node->expressions = std::move(args);
    node->newline = newline;
    node->token = kwTok;
    return node;
}

std::unique_ptr<StatementNode> Parser::parse_assignment_or_expression_statement() {
    if (peek().type == TokenType::IDENTIFIER) {
        Token idTok = consume();
        if (peek().type == TokenType::ASSIGN) {
            // assignment
            consume(); // '='
            auto value = parse_expression();
            if (peek().type == TokenType::SEMICOLON) consume();
            auto node = std::make_unique<AssignmentNode>();
            node->identifier = idTok.value;
            node->value = std::move(value);
            node->token = idTok;
            return node;
        } else {
            // Could be call or identifier-expression statement
            auto ident = std::make_unique<IdentifierNode>();
            ident->name = idTok.value;
            ident->token = idTok;
            if (peek().type == TokenType::OPENPARENTHESIS) {
                auto call = parse_call(std::move(ident));
                auto stmt = std::make_unique<ExpressionStatementNode>();
                stmt->expression = std::move(call);
                if (peek().type == TokenType::SEMICOLON) consume();
                return stmt;
            } else {
                auto stmt = std::make_unique<ExpressionStatementNode>();
                stmt->expression = std::move(ident);
                if (peek().type == TokenType::SEMICOLON) consume();
                return stmt;
            }
        }
    }

    // fallback: expression statement
    auto expr = parse_expression();
    if (peek().type == TokenType::SEMICOLON) consume();
    auto stmt = std::make_unique<ExpressionStatementNode>();
    stmt->expression = std::move(expr);
    return stmt;
}

// ---------- expressions (precedence) ----------
std::unique_ptr<ExpressionNode> Parser::parse_expression() {
    return parse_logical_or();
}

std::unique_ptr<ExpressionNode> Parser::parse_logical_or() {
    auto left = parse_logical_and();
    while (peek().type == TokenType::OR) {
        Token op = consume();
        auto right = parse_logical_and();
        auto node = std::make_unique<BinaryExpressionNode>();
        node->op = !op.value.empty() ? op.value : "||";
        node->left = std::move(left);
        node->right = std::move(right);
        node->token = op;
        left = std::move(node);
    }
    return left;
}

std::unique_ptr<ExpressionNode> Parser::parse_logical_and() {
    auto left = parse_equality();
    while (peek().type == TokenType::AND) {
        Token op = consume();
        auto right = parse_equality();
        auto node = std::make_unique<BinaryExpressionNode>();
        node->op = !op.value.empty() ? op.value : "&&";
        node->left = std::move(left);
        node->right = std::move(right);
        node->token = op;
        left = std::move(node);
    }
    return left;
}

std::unique_ptr<ExpressionNode> Parser::parse_equality() {
    auto left = parse_comparison();
    while (peek().type == TokenType::EQUALITY || peek().type == TokenType::NOTEQUAL) {
        Token op = consume();
        auto right = parse_comparison();
        auto node = std::make_unique<BinaryExpressionNode>();
        if (!op.value.empty()) node->op = op.value;
        else node->op = (op.type == TokenType::EQUALITY) ? "==" : "!=";
        node->left = std::move(left);
        node->right = std::move(right);
        node->token = op;
        left = std::move(node);
    }
    return left;
}

std::unique_ptr<ExpressionNode> Parser::parse_comparison() {
    auto left = parse_additive();
    while (peek().type == TokenType::GREATERTHAN ||
           peek().type == TokenType::GREATEROREQUALTHAN ||
           peek().type == TokenType::LESSTHAN ||
           peek().type == TokenType::LESSOREQUALTHAN) {
        Token op = consume();
        auto right = parse_additive();
        auto node = std::make_unique<BinaryExpressionNode>();
        node->op = !op.value.empty() ? op.value : std::string();
        node->left = std::move(left);
        node->right = std::move(right);
        node->token = op;
        left = std::move(node);
    }
    return left;
}

std::unique_ptr<ExpressionNode> Parser::parse_additive() {
    auto left = parse_multiplicative();
    while (peek().type == TokenType::PLUS || peek().type == TokenType::MINUS) {
        Token op = consume();
        auto right = parse_multiplicative();
        auto node = std::make_unique<BinaryExpressionNode>();
        node->op = !op.value.empty() ? op.value : (op.type == TokenType::PLUS ? "+" : "-");
        node->left = std::move(left);
        node->right = std::move(right);
        node->token = op;
        left = std::move(node);
    }
    return left;
}

std::unique_ptr<ExpressionNode> Parser::parse_multiplicative() {
    auto left = parse_exponent();
    while (peek().type == TokenType::STAR || peek().type == TokenType::SLASH || peek().type == TokenType::PERCENT) {
        Token op = consume();
        auto right = parse_exponent();
        auto node = std::make_unique<BinaryExpressionNode>();
        if (!op.value.empty()) node->op = op.value;
        else {
            if (op.type == TokenType::STAR) node->op = "*";
            else if (op.type == TokenType::SLASH) node->op = "/";
            else node->op = "%";
        }
        node->left = std::move(left);
        node->right = std::move(right);
        node->token = op;
        left = std::move(node);
    }
    return left;
}

std::unique_ptr<ExpressionNode> Parser::parse_exponent() {
    // right-associative exponent
    auto left = parse_unary();
    if (peek().type == TokenType::POWER) {
        Token op = consume();
        auto right = parse_exponent(); // right-associative
        auto node = std::make_unique<BinaryExpressionNode>();
        node->op = !op.value.empty() ? op.value : "**";
        node->left = std::move(left);
        node->right = std::move(right);
        node->token = op;
        return node;
    }
    return left;
}

std::unique_ptr<ExpressionNode> Parser::parse_unary() {
    if (peek().type == TokenType::NOT || peek().type == TokenType::MINUS) {
        Token op = consume();
        auto operand = parse_unary();
        auto node = std::make_unique<UnaryExpressionNode>();
        node->op = !op.value.empty() ? op.value : (op.type == TokenType::NOT ? "!" : "-");
        node->operand = std::move(operand);
        node->token = op;
        return node;
    }
    return parse_primary();
}

std::unique_ptr<ExpressionNode> Parser::parse_primary() {
    Token t = peek();
    if (t.type == TokenType::NUMBER) {
        Token numTok = consume();
        auto n = std::make_unique<NumericLiteralNode>();
        n->value = std::stod(numTok.value);
        n->token = numTok;
        return n;
    }
    if (t.type == TokenType::STRING) {
        Token s = consume();
        auto node = std::make_unique<StringLiteralNode>();
        node->value = s.value;
        node->token = s;
        return node;
    }
    if (t.type == TokenType::BOOLEAN) {
        Token b = consume();
        auto node = std::make_unique<BooleanLiteralNode>();
        node->value = (b.value == "kweli" || b.value == "true");
        node->token = b;
        return node;
    }
    if (t.type == TokenType::IDENTIFIER) {
        Token id = consume();
        auto ident = std::make_unique<IdentifierNode>();
        ident->name = id.value;
        ident->token = id;
        if (peek().type == TokenType::OPENPARENTHESIS) {
            return parse_call(std::move(ident));
        }
        return ident;
    }
    if (t.type == TokenType::OPENPARENTHESIS) {
        consume();
        auto inner = parse_expression();
        expect(TokenType::CLOSEPARENTHESIS, "Expected ')' after expression");
        return inner;
    }

    Token tok = peek();
    throw std::runtime_error(
        "Unexpected token '" + tok.value + "' in file '" + tok.filename +
        "' at line " + std::to_string(tok.line) +
        ", column " + std::to_string(tok.col)
    );
}

std::unique_ptr<ExpressionNode> Parser::parse_call(std::unique_ptr<ExpressionNode> callee) {
    expect(TokenType::OPENPARENTHESIS, "Expected '(' in call");
    Token openTok = tokens[position - 1]; // the '(' token
    auto call = std::make_unique<CallExpressionNode>();
    call->callee = std::move(callee);
    call->token = openTok;
    if (peek().type != TokenType::CLOSEPARENTHESIS) {
        do {
            call->arguments.push_back(parse_expression());
        } while (match(TokenType::COMMA));
    }
    expect(TokenType::CLOSEPARENTHESIS, "Expected ')' after call arguments");
    return call;
}