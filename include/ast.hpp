#pragma once
#include <string>
#include <vector>
#include <memory>
#include "token.hpp"   // <-- needed for Token

// Base class for all AST nodes
struct Node {
    virtual ~Node() = default;
    Token token; // filename, line, column for this node (set by the parser)
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
    std::unique_ptr<ExpressionNode> callee;
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

// If statement: supports both colon+indent style and brace style
// 'kama' <condition> : <INDENT> ... <DEDENT> [vinginevyo ...]
// or 'kama' <condition> { ... } [vinginevyo { ... }]
struct IfStatementNode : public StatementNode {
    // token should point at the 'kama' keyword for diagnostics
    // (parser must set node->token when creating this node)
    std::unique_ptr<ExpressionNode> condition;
    std::vector<std::unique_ptr<StatementNode>> then_body;
    std::vector<std::unique_ptr<StatementNode>> else_body;
    bool has_else = false;
};

// For loop: kwa(<init>; <cond>; <post>) { ... }  OR kwa(<init>; <cond>; <post>): <INDENT> ... <DEDENT>
// - init may be a variable declaration or an assignment/expression (we represent it as a StatementNode)
// - cond and post are expressions and are optional (allow C-like empty slots)
struct ForStatementNode : public StatementNode {
    std::unique_ptr<StatementNode> init;           // optional (e.g., data i = 0) or assignment/expression
    std::unique_ptr<ExpressionNode> condition;     // optional (e.g., i < 10)
    std::unique_ptr<ExpressionNode> post;          // optional (e.g., i++, i += 2)
    std::vector<std::unique_ptr<StatementNode>> body;
};

// While loop: "wakati <condition> { ... }" or "wakati <condition>: <INDENT> ... <DEDENT>"
struct WhileStatementNode : public StatementNode {
    std::unique_ptr<ExpressionNode> condition;
    std::vector<std::unique_ptr<StatementNode>> body;
};

// Do-while loop: "fanya: <INDENT> ... <DEDENT> wakati <condition>" or "fanya { ... } wakati <condition>"
struct DoWhileStatementNode : public StatementNode {
    std::vector<std::unique_ptr<StatementNode>> body;
    std::unique_ptr<ExpressionNode> condition; // the trailing 'wakati' condition
};

// Program root
struct ProgramNode : public Node {
    std::vector<std::unique_ptr<StatementNode>> body;
};

// Function declaration
struct FunctionDeclarationNode : public StatementNode {
    std::string name; // function name
    std::vector<std::string> parameters; // parameter names
    std::vector<std::unique_ptr<StatementNode>> body; // function body statements
};

// Return statement
struct ReturnStatementNode : public StatementNode {
    std::unique_ptr<ExpressionNode> value; // expression to return
};