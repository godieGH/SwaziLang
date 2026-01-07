#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "AsyncBridge.hpp"
#include "ClassRuntime.hpp"
#include "EventEmitterClass.hpp"
#include "Scheduler.hpp"
#include "SwaziError.hpp"

// ============================================================================
// EVENT EMITTER STATE (stored in __emitter_state__ property)
// ============================================================================

struct EventEmitterState {
    std::unordered_map<std::string, std::vector<FunctionPtr>> listeners;
    std::mutex mutex;
    long long id;
};

using EventEmitterStatePtr = std::shared_ptr<EventEmitterState>;

static std::atomic<long long> g_next_emitter_id{1};

// ============================================================================
// HELPER: GET STATE FROM OBJECT
// ============================================================================

// Better approach: use a global registry
static std::unordered_map<long long, EventEmitterStatePtr> g_emitter_registry;
static std::mutex g_registry_mutex;
static EventEmitterStatePtr get_or_create_emitter_state(ObjectPtr obj, const Token& tok) {
    auto it = obj->properties.find("__emitter_id__");

    // If missing, initialize now (for inheritance cases where constructor wasn't called)
    if (it == obj->properties.end() || !std::holds_alternative<double>(it->second.value)) {
        auto state = std::make_shared<EventEmitterState>();
        state->id = g_next_emitter_id.fetch_add(1);

        {
            std::lock_guard<std::mutex> lock(g_registry_mutex);
            g_emitter_registry[state->id] = state;
        }

        PropertyDescriptor pd;
        pd.value = static_cast<double>(state->id);
        pd.is_private = true;
        pd.is_readonly = true;
        pd.is_locked = false;
        pd.token = tok;
        obj->properties["__emitter_id__"] = std::move(pd);

        return state;
    }

    long long id = static_cast<long long>(std::get<double>(it->second.value));

    std::lock_guard<std::mutex> lock(g_registry_mutex);
    auto reg_it = g_emitter_registry.find(id);
    if (reg_it == g_emitter_registry.end()) {
        throw std::runtime_error("EventEmitter: state not found in registry at " + tok.loc.to_string());
    }

    return reg_it->second;
}

// ============================================================================
// NATIVE METHODS
// ============================================================================

// Constructor: new EventEmitter()
static Value native_EventEmitter_ctor(const std::vector<Value>& args, EnvPtr /*env*/, const Token& tok) {
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) {
        throw std::runtime_error("EventEmitter constructor expects 'this' at " + tok.loc.to_string());
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);

    // Create new emitter state
    auto state = std::make_shared<EventEmitterState>();
    state->id = g_next_emitter_id.fetch_add(1);

    // Register in global registry
    {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        g_emitter_registry[state->id] = state;
    }

    // Store ID in object
    PropertyDescriptor pd;
    pd.value = static_cast<double>(state->id);
    pd.is_private = true;
    pd.is_readonly = true;
    pd.is_locked = false;
    pd.token = tok;
    obj->properties["__emitter_id__"] = std::move(pd);

    return std::monostate{};
}

// on(event, listener)
static Value native_EventEmitter_on(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 3) {
        throw SwaziError("TypeError", "on requires (this, event, listener)", tok.loc);
    }
    if (!std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "on: this must be object", tok.loc);
    }
    if (!std::holds_alternative<std::string>(args[1])) {
        throw SwaziError("TypeError", "event must be string", tok.loc);
    }
    if (!std::holds_alternative<FunctionPtr>(args[2])) {
        throw SwaziError("TypeError", "listener must be function", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    std::string event = std::get<std::string>(args[1]);
    FunctionPtr listener = std::get<FunctionPtr>(args[2]);

    auto state = get_or_create_emitter_state(obj, tok);

    std::lock_guard<std::mutex> lock(state->mutex);
    state->listeners[event].push_back(listener);

    return args[0];  // Return this for chaining
}

// off(event, listener)
static Value native_EventEmitter_off(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 3) {
        throw SwaziError("TypeError", "off requires (this, event, listener)", tok.loc);
    }
    if (!std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "off: this must be object", tok.loc);
    }
    if (!std::holds_alternative<std::string>(args[1])) {
        throw SwaziError("TypeError", "event must be string", tok.loc);
    }
    if (!std::holds_alternative<FunctionPtr>(args[2])) {
        throw SwaziError("TypeError", "listener must be function", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    std::string event = std::get<std::string>(args[1]);
    FunctionPtr listener = std::get<FunctionPtr>(args[2]);

    auto state = get_or_create_emitter_state(obj, tok);

    std::lock_guard<std::mutex> lock(state->mutex);
    auto& vec = state->listeners[event];

    vec.erase(
        std::remove_if(vec.begin(), vec.end(),
            [&listener](const FunctionPtr& l) {
                return l.get() == listener.get();
            }),
        vec.end());

    return args[0];  // Return this for chaining
}

// once(event, listener)
static Value native_EventEmitter_once(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    if (args.size() < 3) {
        throw SwaziError("TypeError", "once requires (this, event, listener)", tok.loc);
    }
    if (!std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "once: this must be object", tok.loc);
    }
    if (!std::holds_alternative<std::string>(args[1])) {
        throw SwaziError("TypeError", "event must be string", tok.loc);
    }
    if (!std::holds_alternative<FunctionPtr>(args[2])) {
        throw SwaziError("TypeError", "listener must be function", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    std::string event = std::get<std::string>(args[1]);
    FunctionPtr listener = std::get<FunctionPtr>(args[2]);

    auto state = get_or_create_emitter_state(obj, tok);

    // Create wrapper with a "fired" flag to ensure single execution
    auto wrapper = std::make_shared<FunctionValue>("once_wrapper", nullptr, env, tok);
    std::weak_ptr<FunctionValue> wrapper_weak(wrapper);

    // Shared flag to track if this wrapper has already fired
    auto fired = std::make_shared<std::atomic<bool>>(false);

    auto wrapper_impl = [state, event, listener, wrapper_weak, fired](
                            const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
        // Check if already fired (atomic check-and-set)
        bool expected = false;
        if (!fired->compare_exchange_strong(expected, true)) {
            // Already fired by another emit, skip
            return std::monostate{};
        }

        // Remove self from listeners list
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            auto it = state->listeners.find(event);
            if (it != state->listeners.end()) {
                auto& vec = it->second;
                if (auto wrapper_ptr = wrapper_weak.lock()) {
                    vec.erase(
                        std::remove_if(vec.begin(), vec.end(),
                            [&wrapper_ptr](const FunctionPtr& l) {
                                return l.get() == wrapper_ptr.get();
                            }),
                        vec.end());
                }
            }
        }

        // Call original listener (asynchronously)
        CallbackPayload* p = new CallbackPayload(listener, args);
        enqueue_callback_global(static_cast<void*>(p));

        return std::monostate{};
    };

    wrapper->is_native = true;
    wrapper->native_impl = wrapper_impl;

    std::lock_guard<std::mutex> lock(state->mutex);
    state->listeners[event].push_back(wrapper);

    return args[0];  // Return this for chaining
}
// emit(event, ...args)
static Value native_EventEmitter_emit(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 2) {
        throw SwaziError("TypeError", "emit requires (this, event)", tok.loc);
    }
    if (!std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "emit: this must be object", tok.loc);
    }
    if (!std::holds_alternative<std::string>(args[1])) {
        throw SwaziError("TypeError", "event must be string", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    std::string event = std::get<std::string>(args[1]);

    auto state = get_or_create_emitter_state(obj, tok);

    std::vector<Value> call_args(args.begin() + 2, args.end());

    std::vector<FunctionPtr> listeners_copy;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        auto it = state->listeners.find(event);
        if (it != state->listeners.end()) {
            listeners_copy = it->second;
        }
    }

    for (auto& listener : listeners_copy) {
        if (!listener) continue;
        CallbackPayload* p = new CallbackPayload(listener, call_args);
        enqueue_callback_global(static_cast<void*>(p));
    }

    return true;  // Return true if had listeners
}

// removeAllListeners([event])
static Value native_EventEmitter_removeAllListeners(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "removeAllListeners: this must be object", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    auto state = get_or_create_emitter_state(obj, tok);

    std::lock_guard<std::mutex> lock(state->mutex);

    if (args.size() < 2) {
        state->listeners.clear();
    } else if (std::holds_alternative<std::string>(args[1])) {
        std::string event = std::get<std::string>(args[1]);
        state->listeners.erase(event);
    }

    return args[0];  // Return this for chaining
}

// listenerCount(event)
static Value native_EventEmitter_listenerCount(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 2) {
        throw SwaziError("TypeError", "listenerCount requires (this, event)", tok.loc);
    }
    if (!std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "listenerCount: this must be object", tok.loc);
    }
    if (!std::holds_alternative<std::string>(args[1])) {
        throw SwaziError("TypeError", "event must be string", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    std::string event = std::get<std::string>(args[1]);

    auto state = get_or_create_emitter_state(obj, tok);

    std::lock_guard<std::mutex> lock(state->mutex);
    auto it = state->listeners.find(event);

    if (it != state->listeners.end()) {
        return static_cast<double>(it->second.size());
    }

    return 0.0;
}

// listeners(event)
static Value native_EventEmitter_listeners(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 2) {
        throw SwaziError("TypeError", "listeners requires (this, event)", tok.loc);
    }
    if (!std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "listeners: this must be object", tok.loc);
    }
    if (!std::holds_alternative<std::string>(args[1])) {
        throw SwaziError("TypeError", "event must be string", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    std::string event = std::get<std::string>(args[1]);

    auto state = get_or_create_emitter_state(obj, tok);

    auto arr = std::make_shared<ArrayValue>();

    std::lock_guard<std::mutex> lock(state->mutex);
    auto it = state->listeners.find(event);

    if (it != state->listeners.end()) {
        for (const auto& listener : it->second) {
            arr->elements.push_back(Value{listener});
        }
    }

    return Value{arr};
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void init_event_emitter_class(EnvPtr env) {
    if (!env) return;

    auto add_native = [&](const std::string& name, auto fn) {
        auto fv = std::make_shared<FunctionValue>(name, fn, env, Token{});
        Environment::Variable v{fv, true};
        env->set(name, v);
    };

    add_native("EventEmitter_native_ctor", native_EventEmitter_ctor);
    add_native("EventEmitter_native_on", native_EventEmitter_on);
    add_native("EventEmitter_native_off", native_EventEmitter_off);
    add_native("EventEmitter_native_once", native_EventEmitter_once);
    add_native("EventEmitter_native_emit", native_EventEmitter_emit);
    add_native("EventEmitter_native_removeAllListeners", native_EventEmitter_removeAllListeners);
    add_native("EventEmitter_native_listenerCount", native_EventEmitter_listenerCount);
    add_native("EventEmitter_native_listeners", native_EventEmitter_listeners);

    // Build ClassValue
    auto classDesc = std::make_shared<ClassValue>();
    classDesc->name = "EventEmitter";
    classDesc->token = Token{};
    classDesc->body = std::make_unique<ClassBodyNode>();
    classDesc->defining_env = env;

    // Property: __emitter_id__
    {
        auto p = std::make_unique<ClassPropertyNode>();
        p->name = "__emitter_id__";
        p->is_private = true;
        p->is_locked = false;
        classDesc->body->properties.push_back(std::move(p));
    }

    // Constructor
    {
        auto ctor = std::make_unique<ClassMethodNode>();
        ctor->name = "EventEmitter";
        ctor->is_constructor = true;
        ctor->is_locked = false;
        ctor->is_private = false;

        auto call = std::make_unique<CallExpressionNode>();
        call->callee = std::make_unique<IdentifierNode>();
        static_cast<IdentifierNode*>(call->callee.get())->name = "EventEmitter_native_ctor";
        call->arguments.push_back(std::make_unique<ThisExpressionNode>());

        auto stmt = std::make_unique<ExpressionStatementNode>();
        stmt->expression = std::move(call);
        ctor->body.push_back(std::move(stmt));

        classDesc->body->methods.push_back(std::move(ctor));
    }

    // Helper to add forwarder methods
    auto add_forwarder = [&](const std::string& name, const std::vector<std::string>& params = {}) {
        auto m = std::make_unique<ClassMethodNode>();
        m->name = name;
        m->is_locked = false;
        m->is_private = false;

        for (const auto& pn : params) {
            auto pnode = std::make_unique<ParameterNode>();
            pnode->token = Token{};
            pnode->name = pn;
            pnode->is_rest = false;
            pnode->rest_required_count = 0;
            pnode->defaultValue = nullptr;
            m->params.push_back(std::move(pnode));
        }

        // Special handling for emit to support ...args
        if (name == "emit") {
            auto pnode = std::make_unique<ParameterNode>();
            pnode->token = Token{};
            pnode->name = "args";
            pnode->is_rest = true;
            pnode->rest_required_count = 0;
            pnode->defaultValue = nullptr;
            m->params.push_back(std::move(pnode));
        }

        auto call = std::make_unique<CallExpressionNode>();
        call->callee = std::make_unique<IdentifierNode>();
        static_cast<IdentifierNode*>(call->callee.get())->name = "EventEmitter_native_" + name;

        call->arguments.push_back(std::make_unique<ThisExpressionNode>());
        for (const auto& p : params) {
            auto id = std::make_unique<IdentifierNode>();
            id->name = p;
            call->arguments.push_back(std::move(id));
        }

        if (name == "emit") {
            auto spread = std::make_unique<SpreadElementNode>();
            auto argsId = std::make_unique<IdentifierNode>();
            argsId->name = "args";
            spread->argument = std::move(argsId);
            call->arguments.push_back(std::move(spread));
        }

        auto ret = std::make_unique<ReturnStatementNode>();
        ret->value = std::move(call);
        m->body.push_back(std::move(ret));

        classDesc->body->methods.push_back(std::move(m));
    };

    add_forwarder("on", {"event", "listener"});
    add_forwarder("off", {"event", "listener"});
    add_forwarder("once", {"event", "listener"});
    add_forwarder("emit", {"event"});  // ...args added inside
    add_forwarder("removeAllListeners", {"event"});
    add_forwarder("listenerCount", {"event"});
    add_forwarder("listeners", {"event"});

    Environment::Variable var;
    var.value = classDesc;
    var.is_constant = true;
    env->set("EventEmitter", var);
}
