#pragma once
#include <string>
#include <vector>
#include <memory>
#include <sstream>
#include <stdexcept> // used by clone checks
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
        std::string opnd = operand ? operand->to_string() : "<null>";
        return "(" + op + opnd + ")";
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
        std::string l = left ? left->to_string() : "<null>";
        std::string r = right ? right->to_string() : "<null>";
        return "(" + l + " " + op + " " + r + ")";
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
        std::string c = callee ? callee->to_string() : "<null>";
        std::string s = c + "(";
        for (size_t i = 0; i < arguments.size(); ++i) {
            if (i) s += ", ";
            s += arguments[i] ? arguments[i]->to_string() : "<null>";
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
        std::string o = object ? object->to_string() : "<null>";
        return o + "." + property;
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
        std::string o = object ? object->to_string() : "<null>";
        std::string idx = index ? index->to_string() : "";
        return o + "[" + idx + "]";
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
        std::string cond = condition ? condition->to_string() : "<null>";
        std::string t = thenExpr ? thenExpr->to_string() : "<null>";
        std::string e = elseExpr ? elseExpr->to_string() : "<null>";
        return "(" + cond + " ? " + t + " : " + e + ")";
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
                ss << "${" << (expressions[i] ? expressions[i]->to_string() : "<null>") << "}";
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
            s += elements[i] ? elements[i]->to_string() : "<null>";
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

struct ArrayPatternNode : public ExpressionNode {
    // elements may be:
    //  - IdentifierNode for named targets
    //  - SpreadElementNode for rest (argument is IdentifierNode)
    //  - nullptr to indicate a hole (e.g., [a,,,b])
    std::vector<std::unique_ptr<ExpressionNode>> elements;

    std::string to_string() const override {
        std::string s = "[";
        for (size_t i = 0; i < elements.size(); ++i) {
            if (i) s += ", ";
            if (!elements[i]) s += "";          // hole
            else s += elements[i]->to_string();
        }
        s += "]";
        return s;
    }

    std::unique_ptr<ExpressionNode> clone() const override {
        auto n = std::make_unique<ArrayPatternNode>();
        n->token = token;
        n->elements.reserve(elements.size());
        for (const auto &e : elements) n->elements.push_back(e ? e->clone() : nullptr);
        return n;
    }
};

struct ObjectPatternProperty {
    std::string key; // literal key name in source (e.g. "name")
    // value is the target node (commonly an IdentifierNode); if shorthand, value is IdentifierNode with same name
    std::unique_ptr<ExpressionNode> value;
};

struct ObjectPatternNode : public ExpressionNode {
    std::vector<std::unique_ptr<ObjectPatternProperty>> properties;

    std::string to_string() const override {
        std::ostringstream ss;
        ss << "{ ";
        for (size_t i = 0; i < properties.size(); ++i) {
            if (i) ss << ", ";
            ss << properties[i]->key;
            if (properties[i]->value && properties[i]->value->to_string() != properties[i]->key)
                ss << " : " << properties[i]->value->to_string();
        }
        ss << " }";
        return ss.str();
    }

    std::unique_ptr<ExpressionNode> clone() const override {
        auto n = std::make_unique<ObjectPatternNode>();
        n->token = token;
        n->properties.reserve(properties.size());
        for (const auto &p : properties) {
            if (!p) { n->properties.push_back(nullptr); continue; }
            auto np = std::make_unique<ObjectPatternProperty>();
            np->key = p->key;
            np->value = p->value ? p->value->clone() : nullptr;
            n->properties.push_back(std::move(np));
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
            s += properties[i] ? properties[i]->to_string() : "<null>";
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
    std::unique_ptr<ExpressionNode> argument;
    SpreadElementNode() = default;
    SpreadElementNode(const Token& t, std::unique_ptr<ExpressionNode> arg)
        : argument(std::move(arg)) {
        // use Node::token
        this->token = t;
    }
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
    // For simple declaration: identifier != ""
    // For destructuring: pattern != nullptr and identifier == ""
    std::string identifier;
    std::unique_ptr<ExpressionNode> pattern; // ArrayPatternNode or ObjectPatternNode for destructuring targets
    std::unique_ptr<ExpressionNode> value;
    bool is_constant = false;

    std::unique_ptr<StatementNode> clone() const override {
        auto n = std::make_unique<VariableDeclarationNode>();
        n->token = token;
        n->identifier = identifier;
        n->is_constant = is_constant;
        n->value = value ? value->clone() : nullptr;
        n->pattern = pattern ? pattern->clone() : nullptr;
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

struct DoStatementNode : public StatementNode {
    std::vector<std::unique_ptr<StatementNode>> body;

    std::unique_ptr<StatementNode> clone() const override {
        auto n = std::make_unique<DoStatementNode>();
        n->token = token;
        n->body.reserve(body.size());
        for (const auto &s : body) {
            n->body.push_back(s ? s->clone() : nullptr);
        }
        return n;
    }
};
// Break statement
struct BreakStatementNode : public StatementNode {
    // No child expressions needed

    std::unique_ptr<StatementNode> clone() const override {
        auto n = std::make_unique<BreakStatementNode>();
        n->token = token;
        return n;
    }

    std::string to_string() const override {
        return "break";
    }
};

// Continue statement
struct ContinueStatementNode : public StatementNode {
    // No child expressions needed

    std::unique_ptr<StatementNode> clone() const override {
        auto n = std::make_unique<ContinueStatementNode>();
        n->token = token;
        return n;
    }

    std::string to_string() const override {
        return "continue";
    }
};

struct ParameterNode : public Node {
    // name: parameter identifier (empty for anonymous/rest-only?)
    std::string name;

    // optional default value (if present, parameter is optional)
    std::unique_ptr<ExpressionNode> defaultValue; // e.g. "b = 5"

    // rest (variadic) marker: true for `...args[n]` style param
    bool is_rest = false;

    // when is_rest == true, this is the "required count" encoded in brackets:
    // `...args[2]` sets rest_required_count = 2 meaning the first 2 elements of the
    // rest array are required; in effect the function requires (required_count +
    // number of prior non-default/non-rest required params) arguments to be provided.
    size_t rest_required_count = 0;

    ParameterNode() = default;

    std::unique_ptr<ParameterNode> clone() const {
        auto n = std::make_unique<ParameterNode>();
        n->token = token;
        n->name = name;
        n->is_rest = is_rest;
        n->rest_required_count = rest_required_count;
        n->defaultValue = defaultValue ? defaultValue->clone() : nullptr;
        return n;
    }

    std::string to_string() const {
        std::ostringstream ss;
        if (is_rest) {
            ss << "..." << name << "[" << rest_required_count << "]";
        } else {
            ss << name;
            if (defaultValue) ss << " = " << defaultValue->to_string();
        }
        return ss.str();
    }
};


// Function declaration
struct FunctionDeclarationNode : public StatementNode {
    std::string name; // function name

    // parameters are now ParameterNode objects (allow defaults and rest descriptors)
    std::vector<std::unique_ptr<ParameterNode>> parameters;

    std::vector<std::unique_ptr<StatementNode>> body; // function body statements

    std::unique_ptr<StatementNode> clone() const override {
        auto n = std::make_unique<FunctionDeclarationNode>();
        n->token = token;
        n->name = name;
        n->parameters.reserve(parameters.size());
        for (const auto &p : parameters) {
            n->parameters.push_back(p ? p->clone() : nullptr);
        }
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
    std::unique_ptr<ExpressionNode> clone() const override {
        auto n = std::make_unique<ThisExpressionNode>();
        n->token = token;
        return n;
    }
};
struct FunctionExpressionNode : public ExpressionNode {
    std::string name;
    std::vector<std::unique_ptr<ParameterNode>> parameters;
    std::vector<std::unique_ptr<StatementNode>> body;
    bool is_getter = false;
    // use Node::token

    std::string to_string() const override {
        std::string s;
        if (is_getter) s += "[getter] ";
        s += name + "(";
        for (size_t i = 0; i < parameters.size(); ++i) {
            if (i) s += ", ";
            s += parameters[i] ? parameters[i]->to_string() : "<param>";
        }
        s += ") { ... }";
        return s;
    }

    std::unique_ptr<ExpressionNode> clone() const override {
        auto n = std::make_unique<FunctionExpressionNode>();
        n->token = token;                  // Node::token
        n->name = name;
        n->is_getter = is_getter;
        n->parameters.reserve(parameters.size());
        for (const auto &p : parameters) n->parameters.push_back(p ? p->clone() : nullptr);
        n->body.reserve(body.size());
        for (const auto &s : body) {
            n->body.push_back(s ? s->clone() : nullptr);
        }
        return n;
    }
};

struct LambdaNode : public ExpressionNode {
    std::vector<std::unique_ptr<ParameterNode>> params;
    std::unique_ptr<ExpressionNode> exprBody;                     // for expr-lambdas
    std::vector<std::unique_ptr<StatementNode>> blockBody;        // for block-lambdas
    bool isBlock = false;

    // convenience constructor from names (keeps some backwards compatibility with existing parser)
    LambdaNode(const std::vector<std::string>& p, std::unique_ptr<ExpressionNode> expr)
        : exprBody(std::move(expr)), isBlock(false)
    {
        params.reserve(p.size());
        for (const auto &n : p) {
            auto pn = std::make_unique<ParameterNode>();
            pn->name = n;
            params.push_back(std::move(pn));
        }
    }

    LambdaNode(const std::vector<std::string>& p, std::vector<std::unique_ptr<StatementNode>> blk)
        : blockBody(std::move(blk)), isBlock(true)
    {
        params.reserve(p.size());
        for (const auto &n : p) {
            auto pn = std::make_unique<ParameterNode>();
            pn->name = n;
            params.push_back(std::move(pn));
        }
    }

    // preferred constructor that accepts ParameterNode descriptors
    LambdaNode(std::vector<std::unique_ptr<ParameterNode>> p, std::unique_ptr<ExpressionNode> expr)
        : params(std::move(p)), exprBody(std::move(expr)), isBlock(false) {}

    LambdaNode(std::vector<std::unique_ptr<ParameterNode>> p, std::vector<std::unique_ptr<StatementNode>> blk)
        : params(std::move(p)), blockBody(std::move(blk)), isBlock(true) {}

    std::unique_ptr<ExpressionNode> clone() const override {
        if (isBlock) {
            std::vector<std::unique_ptr<StatementNode>> clonedBlock;
            clonedBlock.reserve(blockBody.size());
            for (const auto &s : blockBody) {
                clonedBlock.push_back(s ? s->clone() : nullptr);
            }
            // clone params
            std::vector<std::unique_ptr<ParameterNode>> clonedParams;
            clonedParams.reserve(params.size());
            for (const auto &p : params) clonedParams.push_back(p ? p->clone() : nullptr);
            return std::make_unique<LambdaNode>(std::move(clonedParams), std::move(clonedBlock));
        } else {
            auto clonedExpr = exprBody ? exprBody->clone() : nullptr;
            std::vector<std::unique_ptr<ParameterNode>> clonedParams;
            clonedParams.reserve(params.size());
            for (const auto &p : params) clonedParams.push_back(p ? p->clone() : nullptr);
            return std::make_unique<LambdaNode>(std::move(clonedParams), std::move(clonedExpr));
        }
    }

    std::string to_string() const override {
        if (isBlock) return "lambda { ... }";
        else {
            std::string s = "lambda (";
            for (size_t i = 0; i < params.size(); ++i) {
                if (i) s += ", ";
                s += params[i] ? params[i]->to_string() : "<param>";
            }
            s += ") => ";
            s += (exprBody ? exprBody->to_string() : "<null>");
            return s;
        }
    }
};
struct NullNode : public ExpressionNode {
    explicit NullNode(const Token& t) { token = t; }

    std::string to_string() const override {
        return "null";
    }
    
    std::unique_ptr<ExpressionNode> clone() const override {
        return std::make_unique<NullNode>(*this);
    }
};


struct CaseNode : StatementNode {
    std::unique_ptr<ExpressionNode> test;  // null → kaida (default)
    std::vector<std::unique_ptr<StatementNode>> body;
};
struct SwitchNode : StatementNode {
    std::unique_ptr<ExpressionNode> discriminant;  
    std::vector<std::unique_ptr<CaseNode>> cases;  
};








// --- Class AST additions ---
struct ClassPropertyNode : public Node {
    std::string name; // simple name
    std::unique_ptr<ExpressionNode> value; // initializer (may be null)
    bool is_private = false; // @
    bool is_static  = false; // *
    bool is_locked  = false; // &

    ClassPropertyNode() = default;

    std::unique_ptr<ClassPropertyNode> clone() const {
        auto n = std::make_unique<ClassPropertyNode>();
        n->token = token;
        n->name = name;
        n->is_private = is_private;
        n->is_static = is_static;
        n->is_locked = is_locked;
        n->value = value ? value->clone() : nullptr;
        return n;
    }

    std::string to_string() const {
        std::ostringstream ss;
        if (is_static) ss << "*";
        if (is_private) ss << "@";
        if (is_locked) ss << "&";
        ss << name;
        if (value) ss << " = " << value->to_string();
        return ss.str();
    }
};

// Represents a method inside a class. This is NOT a wrapper around FunctionExpressionNode
// to avoid clone-signature mismatches — we store the method shape directly.
struct ClassMethodNode : public Node {
    std::string name; // method name (for constructor/destructor rules parser enforces equality)

    // parameters now allow defaults and rest descriptors
    std::vector<std::unique_ptr<ParameterNode>> params;

    std::vector<std::unique_ptr<StatementNode>> body;

    // modifiers
    bool is_private = false;      // @
    bool is_static  = false;      // *
    bool is_locked  = false;      // &
    bool is_getter  = false;      // 'thabiti' style: no params allowed; treated like readonly getter
    bool is_constructor = false;  // constructor (name must equal class name, parser enforces)
    bool is_destructor  = false;  // destructor (marked with ~ in source; parser enforces name)

    ClassMethodNode() = default;

    std::unique_ptr<ClassMethodNode> clone() const {
        auto n = std::make_unique<ClassMethodNode>();
        n->token = token;
        n->name = name;
        n->is_private = is_private;
        n->is_static  = is_static;
        n->is_locked  = is_locked;
        n->is_getter  = is_getter;
        n->is_constructor = is_constructor;
        n->is_destructor  = is_destructor;

        n->params.reserve(params.size());
        for (const auto &p : params) n->params.push_back(p ? p->clone() : nullptr);

        n->body.reserve(body.size());
        for (const auto &s : body) n->body.push_back(s ? s->clone() : nullptr);
        return n;
    }

    std::string to_string() const {
        std::ostringstream ss;
        if (is_static) ss << "*";
        if (is_private) ss << "@";
        if (is_locked) ss << "&";
        if (is_constructor) ss << name;             // constructors rendered as "Name(...) { ... }"
        else if (is_destructor) ss << "~" << name;  // destructor syntax
        else ss << "tabia " << name;

        if (!is_getter) {
            ss << "(";
            for (size_t i = 0; i < params.size(); ++i) {
                if (i) ss << ", ";
                ss << (params[i] ? params[i]->to_string() : "<param>");
            }
            ss << ")";
        } else {
            // getter has no args; indicate it
            ss << " (getter)";
        }

        ss << " " << "{ ... }";
        return ss.str();
    }
};
// Dedicated class body: ordered collection of properties and methods.
// Keeps class internals separate from normal blocks.
struct ClassBodyNode : public Node {
    std::vector<std::unique_ptr<ClassPropertyNode>> properties;
    std::vector<std::unique_ptr<ClassMethodNode>> methods;

    ClassBodyNode() = default;

    std::unique_ptr<ClassBodyNode> clone() const {
        auto n = std::make_unique<ClassBodyNode>();
        n->token = token;
        n->properties.reserve(properties.size());
        for (const auto &p : properties) n->properties.push_back(p ? p->clone() : nullptr);
        n->methods.reserve(methods.size());
        for (const auto &m : methods) n->methods.push_back(m ? m->clone() : nullptr);
        return n;
    }

    std::string to_string() const {
        std::ostringstream ss;
        ss << "{ ";
        for (size_t i = 0; i < properties.size(); ++i) {
            if (i) ss << "; ";
            ss << properties[i]->to_string();
        }
        if (!properties.empty() && !methods.empty()) ss << "; ";
        for (size_t i = 0; i < methods.size(); ++i) {
            if (i) ss << "; ";
            ss << methods[i]->to_string();
        }
        ss << " }";
        return ss.str();
    }
};

// Class declaration: uses IdentifierNode for class name and optional superClass (static only)
struct ClassDeclarationNode : public StatementNode {
    std::unique_ptr<IdentifierNode> name;
    std::unique_ptr<IdentifierNode> superClass; // optional static superclass identifier (rithi)
    std::unique_ptr<ClassBodyNode> body;

    ClassDeclarationNode(
        std::unique_ptr<IdentifierNode> n,
        std::unique_ptr<IdentifierNode> sc,
        std::unique_ptr<ClassBodyNode> b
    )
        : name(std::move(n)), superClass(std::move(sc)), body(std::move(b)) {}

    std::unique_ptr<StatementNode> clone() const override {
        return std::make_unique<ClassDeclarationNode>(
            name ? std::make_unique<IdentifierNode>(*name) : nullptr,
            superClass ? std::make_unique<IdentifierNode>(*superClass) : nullptr,
            body ? body->clone() : nullptr
        );
    }

    std::string to_string() const override {
        std::ostringstream ss;
        ss << "muundo " << (name ? name->to_string() : "<anon>");
        if (superClass) ss << " rithi " << superClass->to_string();
        ss << " " << (body ? body->to_string() : "{ }");
        return ss.str();
    }
};




struct SuperExpressionNode : public ExpressionNode {
    std::vector<std::unique_ptr<ExpressionNode>> arguments;

    std::unique_ptr<ExpressionNode> clone() const override {
        auto n = std::make_unique<SuperExpressionNode>();
        n->token = token;
        for (auto &arg : arguments) {
            n->arguments.push_back(arg ? arg->clone() : nullptr);
        }
        return n;
    }

    std::string to_string() const override {
        std::string s = "super(";
        for (size_t i = 0; i < arguments.size(); i++) {
            s += arguments[i] ? arguments[i]->to_string() : "<null>";
            if (i + 1 < arguments.size()) s += ", ";
        }
        return s + ")";
    }
};
struct NewExpressionNode : public ExpressionNode {
    std::unique_ptr<ExpressionNode> callee; // class identifier or expr
    std::vector<std::unique_ptr<ExpressionNode>> arguments;

    std::unique_ptr<ExpressionNode> clone() const override {
        auto n = std::make_unique<NewExpressionNode>();
        n->token = token;
        n->callee = callee ? callee->clone() : nullptr;
        for (auto &arg : arguments) {
            n->arguments.push_back(arg ? arg->clone() : nullptr);
        }
        return n;
    }

    std::string to_string() const override {
        std::string s = "new " + (callee ? callee->to_string() : "<null>") + "(";
        for (size_t i = 0; i < arguments.size(); i++) {
            s += arguments[i] ? arguments[i]->to_string() : "<null>";
            if (i + 1 < arguments.size()) s += ", ";
        }
        return s + ")";
    }
};
struct DeleteExpressionNode : public ExpressionNode {
    // the object to delete (Identifier, MemberExpression, NewExpression result, etc.)
    std::unique_ptr<ExpressionNode> target;

    DeleteExpressionNode() = default;
    explicit DeleteExpressionNode(std::unique_ptr<ExpressionNode> t) : target(std::move(t)) {}

    std::string to_string() const override {
        return "futa(" + (target ? target->to_string() : "<null>") + ")";
    }

    std::unique_ptr<ExpressionNode> clone() const override {
        auto n = std::make_unique<DeleteExpressionNode>();
        n->token = token;
        n->target = target ? target->clone() : nullptr;
        return n;
    }
};
struct DeleteStatementNode : public StatementNode {
    // store DeleteExpressionNode so tooling can inspect destructor return type if desired
    std::unique_ptr<DeleteExpressionNode> expr;

    DeleteStatementNode() = default;
    explicit DeleteStatementNode(std::unique_ptr<DeleteExpressionNode> e) : expr(std::move(e)) {}

    std::unique_ptr<StatementNode> clone() const override {
        auto n = std::make_unique<DeleteStatementNode>();
        n->token = token;
        if (expr) {
            // clone() on expr returns unique_ptr<ExpressionNode>, but it should point
            // to a DeleteExpressionNode-derived object. Perform a checked downcast.
            auto cloned = expr->clone(); // unique_ptr<ExpressionNode>
            auto del_ptr = dynamic_cast<DeleteExpressionNode*>(cloned.get());
            if (!del_ptr) {
                throw std::runtime_error("DeleteStatementNode::clone(): expected DeleteExpressionNode from clone()");
            }
            n->expr = std::unique_ptr<DeleteExpressionNode>(static_cast<DeleteExpressionNode*>(cloned.release()));
        } else {
            n->expr = nullptr;
        }
        return n;
    }

    std::string to_string() const override {
        return "futa " + (expr && expr->target ? expr->target->to_string() : "<null>");
    }
};

struct TryCatchNode : public StatementNode {
    std::vector<std::unique_ptr<StatementNode>> tryBlock;
    std::string errorVar;
    std::vector<std::unique_ptr<StatementNode>> catchBlock;
    std::vector<std::unique_ptr<StatementNode>> finallyBlock;

    std::unique_ptr<StatementNode> clone() const override {
        auto n = std::make_unique<TryCatchNode>();
        n->token = token;
        n->errorVar = errorVar;

        n->tryBlock.reserve(tryBlock.size());
        for (auto &s : tryBlock) n->tryBlock.push_back(s ? s->clone() : nullptr);

        n->catchBlock.reserve(catchBlock.size());
        for (auto &s : catchBlock) n->catchBlock.push_back(s ? s->clone() : nullptr);

        n->finallyBlock.reserve(finallyBlock.size());
        for (auto &s : finallyBlock) n->finallyBlock.push_back(s ? s->clone() : nullptr);

        return n;
    }

    std::string to_string() const override {
        std::ostringstream ss;
        ss << "try { ... } catch (" << errorVar << ") { ... }";
        if (!finallyBlock.empty()) {
            ss << " finally { ... }";
        }
        return ss.str();
    }
};

// Program root
struct ProgramNode : public Node {
    std::vector<std::unique_ptr<StatementNode>> body;
};