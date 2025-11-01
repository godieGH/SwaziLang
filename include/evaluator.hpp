#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "ast.hpp"
#include "token.hpp"

// Forward declaration
class Environment;

// Our language's value types
struct FunctionValue;
using FunctionPtr = std::shared_ptr<FunctionValue>;
using EnvPtr = std::shared_ptr<Environment>;

// Forward-declare ArrayValue so Value can hold a pointer to it (avoids recursive-instantiation issues)
struct ArrayValue;
using ArrayPtr = std::shared_ptr<ArrayValue>;

struct ClassValue;
using ClassPtr = std::shared_ptr<ClassValue>;

struct ObjectValue;
using ObjectPtr = std::shared_ptr<ObjectValue>;

using Value = std::variant<
    std::monostate,
    double,
    std::string,
    bool,
    FunctionPtr,
    ArrayPtr,
    ObjectPtr,
    ClassPtr>;

struct PropertyDescriptor {
    Value value;
    bool is_private = false;
    bool is_readonly = false;
    bool is_locked = false;
    Token token;
};

struct ObjectValue {
    std::unordered_map<std::string,
        PropertyDescriptor>
        properties;
};
// Now that Value is defined, define ArrayValue containing a vector of Values.
struct ArrayValue {
    std::vector<Value> elements;
};

// Function value: closure with parameters, body, and defining environment
struct FunctionValue {
    std::string name;
    std::vector<std::shared_ptr<ParameterNode>> parameters;
    std::shared_ptr<FunctionDeclarationNode> body;
    EnvPtr closure;
    Token token;
    bool is_native = false;
    std::function<Value(const std::vector<Value>&, EnvPtr, const Token&)> native_impl;

    FunctionValue(
        const std::string& nm,
        const std::vector<std::unique_ptr<ParameterNode>>& params,
        const std::shared_ptr<FunctionDeclarationNode>& b,
        const EnvPtr& env,
        const Token& tok) : name(nm),
                            body(b),
                            closure(env),
                            token(tok),
                            is_native(false) {
        parameters.reserve(params.size());
        for (const auto& p : params) {
            if (p) {
                auto cloned = p->clone();
                parameters.emplace_back(std::shared_ptr<ParameterNode>(cloned.release()));
            } else {
                parameters.emplace_back(nullptr);
            }
        }
    }

    FunctionValue(
        const std::string& nm,
        const std::vector<std::shared_ptr<ParameterNode>>& params,
        const std::shared_ptr<FunctionDeclarationNode>& b,
        const EnvPtr& env,
        const Token& tok) : name(nm),
                            parameters(params),
                            body(b),
                            closure(env),
                            token(tok),
                            is_native(false) {
    }

    FunctionValue(
        const std::string& nm,
        std::function<Value(const std::vector<Value>&, EnvPtr, const Token&)> impl,
        const EnvPtr& env,
        const Token& tok) : name(nm),
                            parameters(),
                            body(nullptr),
                            closure(env),
                            token(tok),
                            is_native(true),
                            native_impl(std::move(impl)) {
    }
};
// Environment with lexical parent pointer
class Environment : public std::enable_shared_from_this<Environment> {
   public:
    Environment(EnvPtr parent = nullptr) : parent(parent) {
    }

    struct Variable {
        Value value;
        bool is_constant = false;
    };

    // map from name -> Variable
    std::unordered_map<std::string,
        Variable>
        values;
    EnvPtr parent;

    // check if name exists in this environment or any parent
    bool has(const std::string& name) const;

    // get reference to variable (searches up the chain). Throws if not found.
    Variable& get(const std::string& name);

    // set variable in the current environment (creates or replaces)
    void set(const std::string& name, const Variable& var);
};

struct LoopControl {
    bool did_break = false;
    bool did_continue = false;
};

// Evaluator
class Evaluator {
   public:
    Evaluator();
    // Evaluate whole program (caller must ensure ProgramNode lifetime covers evaluation)
    void evaluate(ProgramNode* program);

    // For RELP print on the go.
    Value evaluate_expression(ExpressionNode* expr);
    std::string value_to_string(const Value& v);
    bool is_void(const Value& v);
    static std::string cerr_colored(const std::string& s);

    void set_entry_point(const std::string& filename);

   private:
    EnvPtr global_env;

    ClassPtr current_class_context = nullptr;

    void populate_module_metadata(EnvPtr env, const std::string& resolved_path, const std::string& module_name, bool is_main);

    // Module loader records for caching and circular dependency handling.
    struct ModuleRecord {
        enum class State {
            Loading,
            Loaded
        };
        State state = State::Loading;
        ObjectPtr exports = nullptr;  // object holding exported properties
        EnvPtr module_env = nullptr;  // environment used while evaluating module
        std::string path;             // canonical filesystem path used as cache key
    };

    // map canonical module path -> ModuleRecord (shared_ptr)
    std::unordered_map<std::string,
        std::shared_ptr<ModuleRecord>>
        module_cache;

    // Import API: load module by specifier (relative path like "./file" or "./dir/file.sl"),
    // returns the module's exports object (ObjectPtr). 'requesterTok' is used to resolve
    // relative paths with respect to the importing file; for REPL/unknown filename use cwd.
    ObjectPtr import_module(const std::string& module_spec, const Token& requesterTok, EnvPtr requesterEnv);

    // Helper: resolve a module specifier to an existing filesystem path (tries spec, then .sl/.swz)
    std::string resolve_module_path(const std::string& module_spec, const std::string& requester_filename, const Token& tok);

    // Expression & statement evaluators. Pass the environment explicitly for lexical scoping.
    Value evaluate_expression(ExpressionNode* expr, EnvPtr env);
    Value call_function(FunctionPtr fn, const std::vector<Value>& args, const Token& callToken);
    Value call_function_with_receiver(FunctionPtr fn, ObjectPtr receiver, const std::vector<Value>& args, const Token& callToken);
    void evaluate_statement(StatementNode* stmt, EnvPtr env, Value* return_value = nullptr, bool* did_return = nullptr, LoopControl* lc = nullptr);

    // helpers: conversions and formatting
    double to_number(const Value& v);
    std::string to_string_value(const Value& v, bool no_color = false);
    bool to_bool(const Value& v);
    void bind_pattern_to_value(ExpressionNode* pattern, const Value& value, EnvPtr env, bool is_constant, const Token& declToken);

    // value equality helper (deep for arrays, tolerant for mixed number/string)
    bool is_equal(const Value& a, const Value& b);
    // strict equality: no coercion — types must match and values compared directly
    bool is_strict_equal(const Value& a, const Value& b);

    inline bool is_nullish(const Value& v) const {
        return std::holds_alternative<std::monostate>(v);
    }

    Value get_object_property(ObjectPtr obj, const std::string& key, EnvPtr env);
    void set_object_property(ObjectPtr obj, const std::string& key, const Value& val, EnvPtr env, const Token& assignToken);

    std::string print_value(
        const Value& v,
        int depth = 0,
        std::unordered_set<const ObjectValue*> visited = {});

    std::string print_object(
        ObjectPtr obj,
        int indent = 0,
        std::unordered_set<const ObjectValue*> visited = {});

    bool is_private_access_allowed(ObjectPtr obj, EnvPtr env);

    void run_event_loop();
    void schedule_callback(FunctionPtr cb, const std::vector<Value>& args);
};