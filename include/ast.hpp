#pragma once
#include <string>
#include <vector>
#include <memory>
#include <sstream>
#include "token.hpp"  

// Base class for all AST nodes
struct Node {
    virtual ~Node() = default;
    Token token; // filename, line, column for this node (set by the parser)

    // Add a virtual to_string so derived nodes can override safely.
    // Default implementation is minimal; derived nodes can provide richer text.
    virtual std::string to_string() const {
        return "<node>";
    }
};

// Expressions
struct ExpressionNode : public Node {
    // Optionally override default string representation for expressions:
    // virtual std::string to_string() const override { return "<expr>"; }

    // Add a clone interface so AST nodes can be duplicated safely when needed
    // (useful for cases like compound-assignment where we need the L-value both
    // as the assignment target and as the computed left operand).
    virtual std::unique_ptr<ExpressionNode> clone() const = 0;
};

struct NumericLiteralNode : public ExpressionNode {
    double value;
    std::string to_string() const override {
        return std::to_string(value);
    }
    std::unique_ptr<ExpressionNode> clone() const override {
        auto n = std::make_unique<NumericLiteralNode>();
        n->value = value;
        n->token = token;
        return n;
    }
};

struct StringLiteralNode : public ExpressionNode {
    std::string value;
    std::string to_string() const override {
        return "\"" + value + "\"";
    }
    std::unique_ptr<ExpressionNode> clone() const override {
        auto n = std::make_unique<StringLiteralNode>();
        n->value = value;
        n->token = token;
        return n;
    }
};

struct BooleanLiteralNode : public ExpressionNode {
    bool value;
    std::string to_string() const override {
        return value ? "kweli" : "sikweli";
    }
    std::unique_ptr<ExpressionNode> clone() const override {
        auto n = std::make_unique<BooleanLiteralNode>();
        n->value = value;
        n->token = token;
        return n;
    }
};

struct IdentifierNode : public ExpressionNode {
    std::string name;
    std::string to_string() const override {
        return name;
    }
    std::unique_ptr<ExpressionNode> clone() const override {
        auto n = std::make_unique<IdentifierNode>();
        n->name = name;
        n->token = token;
        return n;
    }
};

struct UnaryExpressionNode : public ExpressionNode {
    std::string op; // e.g. "!" or "-"
    std::unique_ptr<ExpressionNode> operand;
    std::string to_string() const override {
        return "(" + op + operand->to_string() + ")";
    }
    std::unique_ptr<ExpressionNode> clone() const override {
        auto n = std::make_unique<UnaryExpressionNode>();
        n->op = op;
        n->token = token;
        if (operand) n->operand = operand->clone();
        return n;
    }
};

struct BinaryExpressionNode : public ExpressionNode {
    std::string op; // e.g. "+", "*", "==", "&&"
    std::unique_ptr<ExpressionNode> left;
    std::unique_ptr<ExpressionNode> right;
    std::string to_string() const override {
        return "(" + left->to_string() + " " + op + " " + right->to_string() + ")";
    }
    std::unique_ptr<ExpressionNode> clone() const override {
        auto n = std::make_unique<BinaryExpressionNode>();
        n->op = op;
        n->token = token;
        if (left) n->left = left->clone();
        if (right) n->right = right->clone();
        return n;
    }
};

struct CallExpressionNode : public ExpressionNode {
    std::unique_ptr<ExpressionNode> callee;
    std::vector<std::unique_ptr<ExpressionNode>> arguments;
    std::string to_string() const override {
        std::string s = callee->to_string() + "(";
        for (size_t i = 0; i < arguments.size(); ++i) {
            if (i) s += ", ";
            s += arguments[i]->to_string();
        }
        s += ")";
        return s;
    }
    std::unique_ptr<ExpressionNode> clone() const override {
        auto n = std::make_unique<CallExpressionNode>();
        n->token = token;
        if (callee) n->callee = callee->clone();
        n->arguments.reserve(arguments.size());
        for (const auto &arg : arguments) {
            n->arguments.push_back(arg ? arg->clone() : nullptr);
        }
        return n;
    }
};

// Member expression: obj.prop (e.g., arr.idadi, str.herufi, arr.ongeza)
struct MemberExpressionNode : public ExpressionNode {
    std::unique_ptr<ExpressionNode> object;
    std::string property; // property name (identifier part)
    std::string to_string() const override {
        return object->to_string() + "." + property;
    }
    std::unique_ptr<ExpressionNode> clone() const override {
        auto n = std::make_unique<MemberExpressionNode>();
        n->token = token;
        n->property = property;
        if (object) n->object = object->clone();
        return n;
    }
};

// Index expression: obj[expr] (e.g., arr[0], arr[i+1])
struct IndexExpressionNode : public ExpressionNode {
    std::unique_ptr<ExpressionNode> object;
    std::unique_ptr<ExpressionNode> index;
    std::string to_string() const override {
        return object->to_string() + "[" + (index ? index->to_string() : "") + "]";
    }
    std::unique_ptr<ExpressionNode> clone() const override {
        auto n = std::make_unique<IndexExpressionNode>();
        n->token = token;
        if (object) n->object = object->clone();
        if (index) n->index = index->clone();
        return n;
    }
};

// TernaryExpressionNode
struct TernaryExpressionNode : public ExpressionNode {
    std::unique_ptr<ExpressionNode> condition;
    std::unique_ptr<ExpressionNode> thenExpr;
    std::unique_ptr<ExpressionNode> elseExpr;
    // removed duplicate Token token; use Node::token instead

    std::string to_string() const override {
        return "(" + condition->to_string() + " ? " +
               thenExpr->to_string() + " : " +
               elseExpr->to_string() + ")";
    }
    std::unique_ptr<ExpressionNode> clone() const override {
        auto n = std::make_unique<TernaryExpressionNode>();
        n->token = token;
        if (condition) n->condition = condition->clone();
        if (thenExpr) n->thenExpr = thenExpr->clone();
        if (elseExpr) n->elseExpr = elseExpr->clone();
        return n;
    }
};

// Template literal node (supports template strings with interpolated expressions).
// Representation follows the common "quasis + expressions" model:
// - quasis: vector of raw string chunks (size == expressions.size() + 1)
// - expressions: vector of ExpressionNode*, each inserted between quasis
//
// Example template: `Hello ${name}, you have ${n} messages`
// quasis = ["Hello ", ", you have ", " messages"]
// expressions = [ IdentifierNode("name"), IdentifierNode("n") ]
struct TemplateLiteralNode : public ExpressionNode {
    // raw (unescaped) string chunks between expressions; there are always
    // expressions.size() + 1 quasis (possibly empty strings at ends)
    std::vector<std::string> quasis;

    // embedded expressions that get evaluated and concatenated between quasis
    std::vector<std::unique_ptr<ExpressionNode>> expressions;

    std::string to_string() const override {
        std::ostringstream ss;
        ss << "`";
        size_t exprCount = expressions.size();
        for (size_t i = 0; i < quasis.size(); ++i) {
            ss << quasis[i];
            if (i < exprCount) {
                ss << "${" << expressions[i]->to_string() << "}";
            }
        }
        ss << "`";
        return ss.str();
    }
    std::unique_ptr<ExpressionNode> clone() const override {
        auto n = std::make_unique<TemplateLiteralNode>();
        n->token = token;
        n->quasis = quasis;
        n->expressions.reserve(expressions.size());
        for (const auto &e : expressions) {
            n->expressions.push_back(e ? e->clone() : nullptr);
        }
        return n;
    }
};



struct ArrayExpressionNode : public ExpressionNode {
    std::vector<std::unique_ptr<ExpressionNode>> elements;

    std::string to_string() const override {
        std::string s = "[";
        for (size_t i = 0; i < elements.size(); ++i) {
            if (i) s += ", ";
            s += elements[i]->to_string();
        }
        s += "]";
        return s;
    }
    std::unique_ptr<ExpressionNode> clone() const override {
        auto n = std::make_unique<ArrayExpressionNode>();
        n->token = token;
        n->elements.reserve(elements.size());
        for (const auto &e : elements) {
            n->elements.push_back(e ? e->clone() : nullptr);
        }
        return n;
    }
};



// Statements
struct StatementNode : public Node {};

struct VariableDeclarationNode : public StatementNode {
    std::string identifier;
    std::unique_ptr<ExpressionNode> value;
    bool is_constant = false;
};

// Assignment: target can be an identifier or an index/member expression (e.g., a = 1; a[0] = x; obj.prop = y)
struct AssignmentNode : public StatementNode {
    std::unique_ptr<ExpressionNode> target; // IdentifierNode or IndexExpressionNode or MemberExpressionNode
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

// Program root
struct ProgramNode : public Node {
    std::vector<std::unique_ptr<StatementNode>> body;
};