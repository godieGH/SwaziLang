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

// NOTE: Do NOT include Scheduler.hpp or Frame.hpp here to avoid include cycles.
// Forward-declare Scheduler so Evaluator can keep a pointer.
class Scheduler;

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

struct CallFrame;
using CallFramePtr = std::shared_ptr<CallFrame>;

struct PromiseValue;
using PromisePtr = std::shared_ptr<PromiseValue>;

// sentinel type to represent a JavaScript-like "hole" (an empty slot).
// It's an empty struct used only to distinguish holes from `null`/undefined`.
struct HoleValue {};

// Value union: add HoleValue so arrays can hold explicit holes distinct from null/undefined.
using Value = std::variant<
    std::monostate,
    double,
    std::string,
    bool,
    FunctionPtr,
    HoleValue,
    ArrayPtr,
    ObjectPtr,
    ClassPtr,
    PromisePtr
>;

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
    bool is_frozen = false;

    // When true this ObjectValue is a proxy for an Environment (live view).
    // Reads/writes/enumeration should forward to `proxy_env->values`.
    // This is used by the builtin globals() to expose a live global/module env.
    bool is_env_proxy = false;
    EnvPtr proxy_env = nullptr;
};

struct PromiseValue {
    enum class State { PENDING, FULFILLED, REJECTED };
    State state = State::PENDING;
    Value result;  // fulfilled value or rejection reason

    // Continuations to run when resolved
    std::vector<std::function<void(Value)>> then_callbacks;
    std::vector<std::function<void(Value)>> catch_callbacks;

    // NEW: whether this promise has an attached handler (then/catch). Used for unhandled rejection detection.
    bool handled = false;

    // (optional) you can add a small marker if you want to avoid printing multiple times
    bool unhandled_reported = false;

    // NEW: ensure we only schedule the "unhandled check" microtask once per rejection
    bool unhandled_check_scheduled = false;
};
// Now that Value is defined, define ArrayValue containing a vector of Values.
struct ArrayValue {
    // We keep simple contiguous vector storage, but elements can now be HoleValue
    // to indicate an empty slot (a "hole") which differs from std::monostate/null.
    std::vector<Value> elements;
};

// Function value: closure with parameters, body, and defining environment
struct FunctionValue {
    std::string name;
    std::vector<std::shared_ptr<ParameterNode>> parameters;
    std::shared_ptr<FunctionDeclarationNode> body;
    EnvPtr closure;
    Token token;
    // new: mark whether this function was declared async in the AST
    bool is_async = false;
    bool is_native = false;
    std::function<Value(const std::vector<Value>&, EnvPtr, const Token&)> native_impl;

    FunctionValue(
        const std::string& nm,
        const std::vector<std::unique_ptr<ParameterNode>>& params,
        const std::shared_ptr<FunctionDeclarationNode>& b,
        const EnvPtr& env,
        const Token& tok) : name(nm),
                            parameters(),     // default-initialize parameters, we'll fill below
                            body(b),
                            closure(env),
                            token(tok),
                            is_async(b ? b->is_async : false),
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
                            is_async(b ? b->is_async : false),
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
                            is_async(false),
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


struct SuspendExecution : public std::exception {
     // Exception used to short-circuit evaluation when an async await suspends the current frame.
     // It's not an error — the executor will catch it, keep the frame on the stack and return.
     const char* what() const noexcept override { return "Execution suspended for await"; }
 };

// Evaluator
class Evaluator {
   public:
    Evaluator();
    ~Evaluator();
    // Evaluate whole program (caller must ensure ProgramNode lifetime covers evaluation)
    void evaluate(ProgramNode* program);

    // For RELP print on the go.
    Value evaluate_expression(ExpressionNode* expr);
    std::string value_to_string(const Value& v);
    bool is_void(const Value& v);
    static std::string cerr_colored(const std::string& s);

    void set_entry_point(const std::string& filename);
    void set_cli_args(const std::vector<std::string>& args);
    
    // Accessor for the scheduler (non-owning for now).
    Scheduler* scheduler() { return scheduler_.get(); }
    
    // Public wrapper that lets native builtins synchronously invoke interpreter-callable functions.
    // This is a thin public forwarder to the private call_function implementation.
    Value invoke_function(FunctionPtr fn, const std::vector<Value>& args, EnvPtr caller_env, const Token& callToken);
    
    void fulfill_promise(PromisePtr p, const Value& value);
    void reject_promise(PromisePtr p, const Value& reason);
    void report_unhandled_rejection(PromisePtr p);
    

   private:
    EnvPtr global_env;
    EnvPtr main_module_env;
    EnvPtr repl_env;

    std::vector<std::string> cli_args;

    ClassPtr current_class_context = nullptr;
    
    // Scheduler instance used to host microtasks/macrotasks and future frame continuations.
    // Initialized in constructor (Phase 0). Using unique_ptr to avoid problems with header inclusion order.
    std::unique_ptr<Scheduler> scheduler_;
    std::vector<CallFramePtr> call_stack_;

    // call frame helpers
    void push_frame(CallFramePtr f);
    void pop_frame();
    CallFramePtr current_frame();
    void execute_frame_until_await_or_return(CallFramePtr frame, PromisePtr promise);
    void execute_frame_until_return(CallFramePtr frame);
    
    
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
    Value call_function(FunctionPtr fn, const std::vector<Value>& args, EnvPtr caller_env, const Token& callToken);
    Value call_function_with_receiver(FunctionPtr fn, ObjectPtr receiver, const std::vector<Value>& args, EnvPtr caller_env, const Token& callToken);
    void evaluate_statement(StatementNode* stmt, EnvPtr env, Value* return_value = nullptr, bool* did_return = nullptr, LoopControl* lc = nullptr);

    // helpers: conversions and formatting
    std::string type_name(const Value& v);
    double to_number(const Value& v, Token token = {});
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

    Value get_object_property(ObjectPtr obj, const std::string& key, EnvPtr env, const Token& accessToken);
    void set_object_property(ObjectPtr obj, const std::string& key, const Value& val, EnvPtr env, const Token& assignToken);

    std::string print_value(
        const Value& v,
        int depth = 0,
        std::unordered_set<const ObjectValue*> visited = {},
        std::unordered_set<const ArrayValue*> arrvisited = {});

    std::string print_object(
        ObjectPtr obj,
        int indent = 0,
        std::unordered_set<const ObjectValue*> visited = {});

    bool is_private_access_allowed(ObjectPtr obj, EnvPtr env);

    void run_event_loop();
    void schedule_callback(FunctionPtr cb, const std::vector<Value>& args);
};

TokenLocation build_location_from_value(const Value& v, const TokenLocation& defaultLoc);