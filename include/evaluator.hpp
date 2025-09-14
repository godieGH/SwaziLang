#pragma once

#include "ast.hpp"
#include <string>
#include <variant>
#include <unordered_map>
#include <vector>
#include <memory>

// Forward declaration
class Environment;

// Our language's value types
struct FunctionValue;
using FunctionPtr = std::shared_ptr<FunctionValue>;
using EnvPtr = std::shared_ptr<Environment>;

// Include a "no-value" variant so we can represent undefined/void returns cleanly.
using Value = std::variant<
    std::monostate,        // represents undefined / no value
    double,
    std::string,
    bool,
    FunctionPtr
>;

// Function value: closure with parameters, body, and defining environment
struct FunctionValue {
    // optional name (helps debugging, recursion)
    std::string name;

    // parameter names in order
    std::vector<std::string> parameters;

    // pointer to function AST node (AST owns the node; ensure AST outlives function values)
    FunctionDeclarationNode* body;

    // closure environment captured at definition time
    EnvPtr closure;

    // token for error locations / diagnostics
    Token token;

    // constructor: accept const refs to avoid overload/temporary binding problems
    FunctionValue(
        const std::string& nm,
        const std::vector<std::string>& params,
        FunctionDeclarationNode* b,
        const EnvPtr& env,
        const Token& tok
    )
        : name(nm)
        , parameters(params)
        , body(b)
        , closure(env)
        , token(tok)
    {}
};

// Environment with lexical parent pointer
class Environment : public std::enable_shared_from_this<Environment> {
public:
    Environment(EnvPtr parent = nullptr)
        : parent(parent) {}

    struct Variable {
        Value value;
        bool is_constant = false;
    };

    // map from name -> Variable
    std::unordered_map<std::string, Variable> values;
    EnvPtr parent;

    // check if name exists in this environment or any parent
    bool has(const std::string& name) const;

    // get reference to variable (searches up the chain). Throws if not found.
    Variable& get(const std::string& name);

    // set variable in the current environment (creates or replaces)
    void set(const std::string& name, const Variable& var);
};

// Evaluator
class Evaluator {
public:
   Evaluator();
    // Evaluate whole program (caller must ensure ProgramNode lifetime covers evaluation)
    void evaluate(ProgramNode* program);

private:
    EnvPtr global_env;

    // Expression & statement evaluators. Pass the environment explicitly for lexical scoping.
    Value evaluate_expression(ExpressionNode* expr, EnvPtr env);
    Value call_function(FunctionPtr fn, const std::vector<Value>& args, const Token& callToken);
    void evaluate_statement(StatementNode* stmt, EnvPtr env, Value* return_value = nullptr, bool* did_return = nullptr);

    // helpers: conversions and formatting
    double to_number(const Value& v);
    std::string to_string_value(const Value& v);
    bool to_bool(const Value& v);
};