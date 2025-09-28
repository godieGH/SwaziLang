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







// ----- Object / Property AST nodes -----

enum class PropertyKind {
    KeyValue,
    Method,
    Shorthand,
    Spread 
};

struct PropertyNode : public ExpressionNode {
    PropertyKind kind = PropertyKind::KeyValue;

    std::unique_ptr<ExpressionNode> key;
    bool computed = false;

    std::unique_ptr<ExpressionNode> value;

    std::string key_name;

    bool is_static = false;
    bool is_readonly = false;
    bool is_private = false;  
    bool is_locked = false;  

    std::string to_string() const override {
        std::string s;
        if (kind == PropertyKind::Spread) {
            // show privacy marker if present
            if (is_private) s += "@";
            s += "..." + (value ? value->to_string() : "<null>");
            return s;
        }
        // represent key
        if (computed) s += "[" + (key ? key->to_string() : "") + "]";
        else if (!key_name.empty()) s += key_name;
        else if (key) s += key->to_string();
        else s += "<no-key>";

        // prefix private marker
        if (is_private) s = "@" + s;

        if (kind == PropertyKind::Shorthand) return s;
        if (kind == PropertyKind::Method) {
            s += ": " + (value ? value->to_string() : "<fn>");
            return s;
        }
        // KeyValue
        s += ": " + (value ? value->to_string() : "null");
        return s;
    }

    std::unique_ptr<ExpressionNode> clone() const override {
        auto n = std::make_unique<PropertyNode>();
        n->token = token;
        n->kind = kind;
        n->computed = computed;
        n->key_name = key_name;
        n->is_static = is_static;
        n->is_readonly = is_readonly;
        n->is_private = is_private;   // <-- copy privacy flag
        if (key) n->key = key->clone();
        if (value) n->value = value->clone();
        return n;
    }
};
struct ObjectExpressionNode : public ExpressionNode {
    std::vector<std::unique_ptr<PropertyNode>> properties;

    std::string to_string() const override {
        std::string s = "{ ";
        for (size_t i = 0; i < properties.size(); ++i) {
            if (i) s += ", ";
            s += properties[i]->to_string();
        }
        s += " }";
        return s;
    }

    std::unique_ptr<ExpressionNode> clone() const override {
    auto n = std::make_unique<ObjectExpressionNode>();
    n->token = token;
    n->properties.reserve(properties.size());
    for (const auto &p : properties) {
        if (!p) { 
            n->properties.push_back(nullptr);
            continue;
        }
        // p->clone() returns unique_ptr<ExpressionNode> that should
        // actually point to a PropertyNode-derived object. Sanity-check.
        auto cloned_expr = p->clone(); // unique_ptr<ExpressionNode>
        auto prop_ptr = dynamic_cast<PropertyNode*>(cloned_expr.get());
        if (!prop_ptr) {
            throw std::runtime_error("ObjectExpressionNode::clone(): expected PropertyNode from clone()");
        }
        // transfer ownership into vector
        n->properties.push_back(std::unique_ptr<PropertyNode>(static_cast<PropertyNode*>(cloned_expr.release())));
    }
    return n;
}
};

struct SpreadElementNode : public ExpressionNode {
    Token token;
    std::unique_ptr<ExpressionNode> argument;
    SpreadElementNode() = default;
    SpreadElementNode(const Token& t, std::unique_ptr<ExpressionNode> arg)
        : token(t), argument(std::move(arg)) {}
    std::unique_ptr<ExpressionNode> clone() const override {
        auto node = std::make_unique<SpreadElementNode>();
        node->token = token;
        if (argument) node->argument = argument->clone();
        return node;
    }
};

struct SelfExpressionNode : public ExpressionNode {
    std::string to_string() const override { return "$"; }
    std::unique_ptr<ExpressionNode> clone() const override {
        auto n = std::make_unique<SelfExpressionNode>();
        n->token = token;
        return n;
    }
};


// Statements
struct StatementNode : public Node {
    // Non-pure virtual clone: default returns nullptr (safe, non-breaking).
    // Concrete statements override to provide proper cloning.
    virtual std::unique_ptr<StatementNode> clone() const {
        return nullptr;
    }
};

struct VariableDeclarationNode : public StatementNode {
    std::string identifier;
    std::unique_ptr<ExpressionNode> value;
    bool is_constant = false;

    std::unique_ptr<StatementNode> clone() const override {
        auto n = std::make_unique<VariableDeclarationNode>();
        n->token = token;
        n->identifier = identifier;
        n->is_constant = is_constant;
        n->value = value ? value->clone() : nullptr;
        return n;
    }
};

// Assignment: target can be an identifier or an index/member expression
struct AssignmentNode : public StatementNode {
    std::unique_ptr<ExpressionNode> target; // IdentifierNode or IndexExpressionNode or MemberExpressionNode
    std::unique_ptr<ExpressionNode> value;

    std::unique_ptr<StatementNode> clone() const override {
        auto n = std::make_unique<AssignmentNode>();
        n->token = token;
        n->target = target ? target->clone() : nullptr;
        n->value = value ? value->clone() : nullptr;
        return n;
    }
};

struct PrintStatementNode : public StatementNode {
    // multiple args allowed for chapisha/andika
    std::vector<std::unique_ptr<ExpressionNode>> expressions;
    bool newline = true; // chapisha -> true, andika -> false

    std::unique_ptr<StatementNode> clone() const override {
        auto n = std::make_unique<PrintStatementNode>();
        n->token = token;
        n->newline = newline;
        n->expressions.reserve(expressions.size());
        for (const auto &e : expressions) n->expressions.push_back(e ? e->clone() : nullptr);
        return n;
    }
};

struct ExpressionStatementNode : public StatementNode {
    std::unique_ptr<ExpressionNode> expression;

    std::unique_ptr<StatementNode> clone() const override {
        auto n = std::make_unique<ExpressionStatementNode>();
        n->token = token;
        n->expression = expression ? expression->clone() : nullptr;
        return n;
    }
};

// If statement
struct IfStatementNode : public StatementNode {
    std::unique_ptr<ExpressionNode> condition;
    std::vector<std::unique_ptr<StatementNode>> then_body;
    std::vector<std::unique_ptr<StatementNode>> else_body;
    bool has_else = false;

    std::unique_ptr<StatementNode> clone() const override {
        auto n = std::make_unique<IfStatementNode>();
        n->token = token;
        n->condition = condition ? condition->clone() : nullptr;
        n->has_else = has_else;
        n->then_body.reserve(then_body.size());
        for (const auto &s : then_body) n->then_body.push_back(s ? s->clone() : nullptr);
        n->else_body.reserve(else_body.size());
        for (const auto &s : else_body) n->else_body.push_back(s ? s->clone() : nullptr);
        return n;
    }
};

// For loop
struct ForStatementNode : public StatementNode {
    std::unique_ptr<StatementNode> init;           // optional
    std::unique_ptr<ExpressionNode> condition;     // optional
    std::unique_ptr<ExpressionNode> post;          // optional
    std::vector<std::unique_ptr<StatementNode>> body;

    std::unique_ptr<StatementNode> clone() const override {
        auto n = std::make_unique<ForStatementNode>();
        n->token = token;
        n->init = init ? init->clone() : nullptr;
        n->condition = condition ? condition->clone() : nullptr;
        n->post = post ? post->clone() : nullptr;
        n->body.reserve(body.size());
        for (const auto &s : body) n->body.push_back(s ? s->clone() : nullptr);
        return n;
    }
};

// For-in / for-each loop: `kwa kila t, i katika arr: ...`
struct ForInStatementNode : public StatementNode {
    std::unique_ptr<IdentifierNode> valueVar;
    std::unique_ptr<IdentifierNode> indexVar;   // optional
    std::unique_ptr<ExpressionNode> iterable;
    std::vector<std::unique_ptr<StatementNode>> body;

    std::unique_ptr<StatementNode> clone() const override {
        auto n = std::make_unique<ForInStatementNode>();
        n->token = token;
        if (valueVar) {
            n->valueVar = std::unique_ptr<IdentifierNode>(
                static_cast<IdentifierNode*>(valueVar->clone().release())
            );
        }
        if (indexVar) {
            n->indexVar = std::unique_ptr<IdentifierNode>(
                static_cast<IdentifierNode*>(indexVar->clone().release())
            );
        }
        n->iterable = iterable ? iterable->clone() : nullptr;
        n->body.reserve(body.size());
        for (const auto &s : body) {
            n->body.push_back(s ? s->clone() : nullptr);
        }
        return n;
    }

    std::string to_string() const override {
        std::string s = "kwa kila " + (valueVar ? valueVar->to_string() : "<val>");
        if (indexVar) {
            s += ", " + indexVar->to_string();
        }
        s += " katika " + (iterable ? iterable->to_string() : "<iterable>") + " { ... }";
        return s;
    }
};

// While loop
struct WhileStatementNode : public StatementNode {
    std::unique_ptr<ExpressionNode> condition;
    std::vector<std::unique_ptr<StatementNode>> body;

    std::unique_ptr<StatementNode> clone() const override {
        auto n = std::make_unique<WhileStatementNode>();
        n->token = token;
        n->condition = condition ? condition->clone() : nullptr;
        n->body.reserve(body.size());
        for (const auto &s : body) n->body.push_back(s ? s->clone() : nullptr);
        return n;
    }
};

// Do-while loop
struct DoWhileStatementNode : public StatementNode {
    std::vector<std::unique_ptr<StatementNode>> body;
    std::unique_ptr<ExpressionNode> condition; // trailing condition

    std::unique_ptr<StatementNode> clone() const override {
        auto n = std::make_unique<DoWhileStatementNode>();
        n->token = token;
        n->body.reserve(body.size());
        for (const auto &s : body) n->body.push_back(s ? s->clone() : nullptr);
        n->condition = condition ? condition->clone() : nullptr;
        return n;
    }
};

// Function declaration
struct FunctionDeclarationNode : public StatementNode {
    std::string name; // function name
    std::vector<std::string> parameters; // parameter names
    std::vector<std::unique_ptr<StatementNode>> body; // function body statements

    std::unique_ptr<StatementNode> clone() const override {
        auto n = std::make_unique<FunctionDeclarationNode>();
        n->token = token;
        n->name = name;
        n->parameters = parameters;
        n->body.reserve(body.size());
        for (const auto &s : body) n->body.push_back(s ? s->clone() : nullptr);
        return n;
    }
};

struct ReturnStatementNode : public StatementNode {
    std::unique_ptr<ExpressionNode> value; // expression to return

    std::unique_ptr<StatementNode> clone() const override {
        auto n = std::make_unique<ReturnStatementNode>();
        n->token = token;
        n->value = value ? value->clone() : nullptr;
        return n;
    }
};

struct ThisExpressionNode : public ExpressionNode {
    Token token;
    std::unique_ptr<ExpressionNode> clone() const override {
        auto n = std::make_unique<ThisExpressionNode>();
        n->token = token;
        return n;
    }
};


struct FunctionExpressionNode : public ExpressionNode {
    std::string name;
    std::vector<std::string> parameters;
    std::vector<std::unique_ptr<StatementNode>> body;
    bool is_getter = false;
    // use Node::token

    std::string to_string() const override {
        std::string s;
        if (is_getter) s += "[getter] ";
        s += name + "(";
        for (size_t i = 0; i < parameters.size(); ++i) {
            if (i) s += ", ";
            s += parameters[i];
        }
        s += ") { ... }";
        return s;
    }

    std::unique_ptr<ExpressionNode> clone() const override {
        auto n = std::make_unique<FunctionExpressionNode>();
        n->token = token;                  // Node::token
        n->name = name;
        n->parameters = parameters;
        n->is_getter = is_getter;
        n->body.reserve(body.size());
        for (const auto &s : body) {
            n->body.push_back(s ? s->clone() : nullptr);
        }
        return n;
    }
};
// Program root
struct ProgramNode : public Node {
    std::vector<std::unique_ptr<StatementNode>> body;
};