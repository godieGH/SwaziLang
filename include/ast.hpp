#pragma once
#include <string>
#include <vector>
#include <memory>

// Base class for all AST nodes
struct Node {
    virtual ~Node() = default;
};

// Expressions
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

struct UnaryExpressionNode : public ExpressionNode {
    std::string op; // e.g. "!" or "-"
    std::unique_ptr<ExpressionNode> operand;
};

struct BinaryExpressionNode : public ExpressionNode {
    std::string op; // e.g. "+", "*", "==", "&&"
    std::unique_ptr<ExpressionNode> left;
    std::unique_ptr<ExpressionNode> right;
};

struct CallExpressionNode : public ExpressionNode {
    std::unique_ptr<ExpressionNode> callee; // usually IdentifierNode
    std::vector<std::unique_ptr<ExpressionNode>> arguments;
};

// Statements
struct StatementNode : public Node {};

struct VariableDeclarationNode : public StatementNode {
    std::string identifier;
    std::unique_ptr<ExpressionNode> value;
    bool is_constant = false;
};

struct AssignmentNode : public StatementNode {
    std::string identifier;
    std::unique_ptr<ExpressionNode> value;
};

struct PrintStatementNode : public StatementNode {
    // multiple args allowed for chapisha/andika
    std::vector<std::unique_ptr<ExpressionNode>> expressions;
    bool newline = true; // chapisha -> true, andika -> false
};

struct ExpressionStatementNode : public StatementNode {
    std::unique_ptr<ExpressionNode> expression;
};

// Program root
struct ProgramNode : public Node {
    std::vector<std::unique_ptr<StatementNode>> body;
};