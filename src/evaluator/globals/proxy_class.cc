#include "proxy_class.hpp"

#include "ClassRuntime.hpp"
#include "SwaziError.hpp"
#include "evaluator.hpp"
#include "globals.hpp"

// Forward declare ProxyValue
struct ProxyValue;
using ProxyPtr = std::shared_ptr<ProxyValue>;

// Helper to safely get handler method
FunctionPtr get_handler_method(ObjectPtr handler, const std::string& method_name, const Token& tok) {
    if (!handler) return nullptr;

    auto it = handler->properties.find(method_name);
    if (it == handler->properties.end()) return nullptr;

    const Value& v = it->second.value;
    if (!std::holds_alternative<FunctionPtr>(v)) return nullptr;

    return std::get<FunctionPtr>(v);
}

// Native constructor: Proxy(target, handler)
static Value native_Proxy_ctor(const std::vector<Value>& args, EnvPtr env, const Token& tok, Evaluator* evaluator) {
    if (args.size() < 3) {
        throw SwaziError("TypeError", "Proxy constructor requires (this, target, handler)", tok.loc);
    }

    // args[0] = this (proxy instance)
    // args[1] = target
    // args[2] = handler

    ObjectPtr instance = std::get<ObjectPtr>(args[0]);

    if (!std::holds_alternative<ObjectPtr>(args[1])) {
        throw SwaziError("TypeError", "Proxy target must be an object", tok.loc);
    }
    ObjectPtr target = std::get<ObjectPtr>(args[1]);

    if (!std::holds_alternative<ObjectPtr>(args[2])) {
        throw SwaziError("TypeError", "Proxy handler must be an object", tok.loc);
    }
    ObjectPtr handler = std::get<ObjectPtr>(args[2]);

    // Create ProxyValue and store in private __proxy__ slot
    auto proxy = std::make_shared<ProxyValue>(target, handler);

    PropertyDescriptor pd;
    pd.value = Value{proxy};  // We need to add ProxyPtr to Value variant
    pd.is_private = true;
    pd.is_readonly = true;
    pd.is_locked = true;
    pd.token = tok;
    instance->properties["__proxy__"] = std::move(pd);

    return std::monostate{};
}

void init_proxy_class(EnvPtr env, Evaluator* evaluator) {
    if (!env || !evaluator) return;

    // Register native constructor helper
    auto ctor_impl = [evaluator](const std::vector<Value>& args, EnvPtr e, const Token& t) {
        return native_Proxy_ctor(args, e, t, evaluator);
    };

    auto ctor_fn = std::make_shared<FunctionValue>("Proxy_native_ctor", ctor_impl, env, Token{});
    Environment::Variable ctor_var{ctor_fn, true};
    env->set("Proxy_native_ctor", ctor_var);

    // Build ClassValue descriptor
    auto classDesc = std::make_shared<ClassValue>();
    classDesc->name = "Proxy";
    classDesc->token = Token{};
    classDesc->body = std::make_unique<ClassBodyNode>();
    classDesc->defining_env = env;

    // Private property __proxy__
    {
        auto p = std::make_unique<ClassPropertyNode>();
        p->name = "__proxy__";
        p->is_private = true;
        p->is_locked = true;
        classDesc->body->properties.push_back(std::move(p));
    }

    // Constructor: Proxy(target, handler)
    {
        auto ctor = std::make_unique<ClassMethodNode>();
        ctor->name = "Proxy";
        ctor->is_constructor = true;

        // Parameters: target, handler
        auto p1 = std::make_unique<ParameterNode>();
        p1->name = "target";
        ctor->params.push_back(std::move(p1));

        auto p2 = std::make_unique<ParameterNode>();
        p2->name = "handler";
        ctor->params.push_back(std::move(p2));

        // Call native constructor
        auto call = std::make_unique<CallExpressionNode>();
        auto callee = std::make_unique<IdentifierNode>();
        callee->name = "Proxy_native_ctor";
        call->callee = std::move(callee);
        call->arguments.push_back(std::make_unique<ThisExpressionNode>());

        auto arg1 = std::make_unique<IdentifierNode>();
        arg1->name = "target";
        call->arguments.push_back(std::move(arg1));

        auto arg2 = std::make_unique<IdentifierNode>();
        arg2->name = "handler";
        call->arguments.push_back(std::move(arg2));

        auto stmt = std::make_unique<ExpressionStatementNode>();
        stmt->expression = std::move(call);
        ctor->body.push_back(std::move(stmt));

        classDesc->body->methods.push_back(std::move(ctor));
    }

    // Register class
    Environment::Variable var;
    var.value = classDesc;
    var.is_constant = true;
    env->set("Proxy", var);
}