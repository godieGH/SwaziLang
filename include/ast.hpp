#pragma once
#include <string>
#include <vector>
#include <memory>

// Base class for all AST nodes
struct Node {
    virtual ~Node() = default;
};

// Node for literals (numbers, strings, booleans, identifiers)
struct ExpressionNode : public Node {};

struct NumericLiteralNode : public ExpressionNode {
    double value;
};

struct StringLiteralNode : public ExpressionNode {
    std::string value;
};

struct BooleanLiteralNode : public ExpressionNode {
    bool value;
};

struct IdentifierNode : public ExpressionNode {
    std::string name;
};

// Node for statements
struct StatementNode : public Node {};

struct VariableDeclarationNode : public StatementNode {
    std::string identifier;
    std::unique_ptr<ExpressionNode> value;
};

struct PrintStatementNode : public StatementNode {
    std::unique_ptr<ExpressionNode> expression;
};

// The root of the AST
struct ProgramNode : public Node {
    std::vector<std::unique_ptr<StatementNode>> body;
};
