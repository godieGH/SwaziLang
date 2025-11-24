#include <algorithm>
#include <cstring>
#include <deque>
#include <functional>
#include <iomanip>
#include <list>
#include <memory>
#include <queue>
#include <sstream>
#include <stack>
#include <unordered_map>
#include <vector>

#include "ClassRuntime.hpp"
#include "SwaziError.hpp"
#include "builtins.hpp"
#include "evaluator.hpp"

// Helper: convert Value to string for hashing/comparison
static std::string value_to_string_simple_collections(const Value& v) {
    if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
    if (std::holds_alternative<double>(v)) {
        std::ostringstream ss;
        ss << std::get<double>(v);
        return ss.str();
    }
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "kweli" : "sikweli";
    return std::string();
}

// ============= HASHMAP CLASS NATIVES =============

static Value native_HashMap_ctor(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    // Constructor initializes __store__ from optional source object
    auto store = std::make_shared<std::unordered_map<std::string, Value>>();

    // If first arg is object, initialize from it
    if (!args.empty() && std::holds_alternative<ObjectPtr>(args[0])) {
        ObjectPtr src = std::get<ObjectPtr>(args[0]);
        if (src) {
            for (const auto& kv : src->properties) {
                (*store)[kv.first] = kv.second.value;
            }
        }
    }

    return std::monostate{};
}

static Value native_HashMap_set(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 3) {
        throw SwaziError("TypeError", "HashMap.set requires (this, key, value)", tok.loc);
    }

    if (!std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "HashMap method called on non-object", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    std::string key = value_to_string_simple_collections(args[1]);
    Value val = args[2];

    // Store directly in object properties with prefix to avoid collision
    obj->properties["$map$" + key] = PropertyDescriptor{val, false, false, false, tok};

    return std::monostate{};
}

static Value native_HashMap_get(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 2) {
        throw SwaziError("TypeError", "HashMap.get requires (this, key)", tok.loc);
    }

    if (!std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "HashMap method called on non-object", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    std::string key = value_to_string_simple_collections(args[1]);

    auto it = obj->properties.find("$map$" + key);
    if (it != obj->properties.end()) {
        return it->second.value;
    }

    return std::monostate{};
}

static Value native_HashMap_has(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 2) {
        throw SwaziError("TypeError", "HashMap.has requires (this, key)", tok.loc);
    }

    if (!std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "HashMap method called on non-object", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    std::string key = value_to_string_simple_collections(args[1]);

    return obj->properties.find("$map$" + key) != obj->properties.end();
}

static Value native_HashMap_delete(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 2) {
        throw SwaziError("TypeError", "HashMap.delete requires (this, key)", tok.loc);
    }

    if (!std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "HashMap method called on non-object", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    std::string key = value_to_string_simple_collections(args[1]);

    auto it = obj->properties.find("$map$" + key);
    if (it != obj->properties.end()) {
        obj->properties.erase(it);
        return true;
    }

    return false;
}

static Value native_HashMap_keys(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "HashMap.keys requires this", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    auto arr = std::make_shared<ArrayValue>();

    for (const auto& kv : obj->properties) {
        if (kv.first.substr(0, 5) == "$map$") {
            arr->elements.push_back(Value{kv.first.substr(5)});
        }
    }

    return arr;
}

static Value native_HashMap_values(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "HashMap.values requires this", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    auto arr = std::make_shared<ArrayValue>();

    for (const auto& kv : obj->properties) {
        if (kv.first.substr(0, 5) == "$map$") {
            arr->elements.push_back(kv.second.value);
        }
    }

    return arr;
}

static Value native_HashMap_size(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "HashMap.size requires this", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    size_t count = 0;

    for (const auto& kv : obj->properties) {
        if (kv.first.substr(0, 5) == "$map$") {
            count++;
        }
    }

    return static_cast<double>(count);
}

static Value native_HashMap_clear(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "HashMap.clear requires this", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);

    // Remove all $map$ prefixed properties
    auto it = obj->properties.begin();
    while (it != obj->properties.end()) {
        if (it->first.substr(0, 5) == "$map$") {
            it = obj->properties.erase(it);
        } else {
            ++it;
        }
    }

    return std::monostate{};
}

// ============= STACK CLASS NATIVES =============

static Value native_Stack_push(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 2) {
        throw SwaziError("TypeError", "Stack.push requires (this, value)", tok.loc);
    }

    if (!std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "Stack method called on non-object", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    auto it = obj->properties.find("__items__");

    ArrayPtr items;
    if (it == obj->properties.end() || !std::holds_alternative<ArrayPtr>(it->second.value)) {
        items = std::make_shared<ArrayValue>();
        obj->properties["__items__"] = PropertyDescriptor{items, true, false, false, tok};
    } else {
        items = std::get<ArrayPtr>(it->second.value);
    }

    items->elements.push_back(args[1]);

    return std::monostate{};
}

static Value native_Stack_pop(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "Stack.pop requires this", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    auto it = obj->properties.find("__items__");

    if (it == obj->properties.end() || !std::holds_alternative<ArrayPtr>(it->second.value)) {
        return std::monostate{};
    }

    ArrayPtr items = std::get<ArrayPtr>(it->second.value);
    if (items->elements.empty()) {
        return std::monostate{};
    }

    Value val = items->elements.back();
    items->elements.pop_back();

    return val;
}

static Value native_Stack_peek(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "Stack.peek requires this", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    auto it = obj->properties.find("__items__");

    if (it == obj->properties.end() || !std::holds_alternative<ArrayPtr>(it->second.value)) {
        return std::monostate{};
    }

    ArrayPtr items = std::get<ArrayPtr>(it->second.value);
    if (items->elements.empty()) {
        return std::monostate{};
    }

    return items->elements.back();
}

static Value native_Stack_isEmpty(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "Stack.isEmpty requires this", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    auto it = obj->properties.find("__items__");

    if (it == obj->properties.end() || !std::holds_alternative<ArrayPtr>(it->second.value)) {
        return true;
    }

    ArrayPtr items = std::get<ArrayPtr>(it->second.value);
    return items->elements.empty();
}

static Value native_Stack_size(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "Stack.size requires this", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    auto it = obj->properties.find("__items__");

    if (it == obj->properties.end() || !std::holds_alternative<ArrayPtr>(it->second.value)) {
        return 0.0;
    }

    ArrayPtr items = std::get<ArrayPtr>(it->second.value);
    return static_cast<double>(items->elements.size());
}

static Value native_Stack_clear(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "Stack.clear requires this", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    auto items = std::make_shared<ArrayValue>();
    obj->properties["__items__"] = PropertyDescriptor{items, true, false, false, tok};

    return std::monostate{};
}

// ============= QUEUE CLASS NATIVES =============

static Value native_Queue_enqueue(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 2) {
        throw SwaziError("TypeError", "Queue.enqueue requires (this, value)", tok.loc);
    }

    if (!std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "Queue method called on non-object", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    auto it = obj->properties.find("__items__");

    ArrayPtr items;
    if (it == obj->properties.end() || !std::holds_alternative<ArrayPtr>(it->second.value)) {
        items = std::make_shared<ArrayValue>();
        obj->properties["__items__"] = PropertyDescriptor{items, true, false, false, tok};
    } else {
        items = std::get<ArrayPtr>(it->second.value);
    }

    items->elements.push_back(args[1]);

    return std::monostate{};
}

static Value native_Queue_dequeue(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "Queue.dequeue requires this", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    auto it = obj->properties.find("__items__");

    if (it == obj->properties.end() || !std::holds_alternative<ArrayPtr>(it->second.value)) {
        return std::monostate{};
    }

    ArrayPtr items = std::get<ArrayPtr>(it->second.value);
    if (items->elements.empty()) {
        return std::monostate{};
    }

    Value val = items->elements.front();
    items->elements.erase(items->elements.begin());

    return val;
}

static Value native_Queue_front(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "Queue.front requires this", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    auto it = obj->properties.find("__items__");

    if (it == obj->properties.end() || !std::holds_alternative<ArrayPtr>(it->second.value)) {
        return std::monostate{};
    }

    ArrayPtr items = std::get<ArrayPtr>(it->second.value);
    if (items->elements.empty()) {
        return std::monostate{};
    }

    return items->elements.front();
}

static Value native_Queue_isEmpty(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "Queue.isEmpty requires this", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    auto it = obj->properties.find("__items__");

    if (it == obj->properties.end() || !std::holds_alternative<ArrayPtr>(it->second.value)) {
        return true;
    }

    ArrayPtr items = std::get<ArrayPtr>(it->second.value);
    return items->elements.empty();
}

static Value native_Queue_size(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "Queue.size requires this", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    auto it = obj->properties.find("__items__");

    if (it == obj->properties.end() || !std::holds_alternative<ArrayPtr>(it->second.value)) {
        return 0.0;
    }

    ArrayPtr items = std::get<ArrayPtr>(it->second.value);
    return static_cast<double>(items->elements.size());
}

static Value native_Queue_clear(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "Queue.clear requires this", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    auto items = std::make_shared<ArrayValue>();
    obj->properties["__items__"] = PropertyDescriptor{items, true, false, false, tok};

    return std::monostate{};
}

// ============= MAIN EXPORTS FUNCTION =============

std::shared_ptr<ObjectValue> make_collections_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();

    Token tok;
    tok.type = TokenType::IDENTIFIER;
    tok.loc = TokenLocation("<collections>", 0, 0, 0);

    // Register native helpers
    auto add_native = [&](const std::string& name, std::function<Value(const std::vector<Value>&, EnvPtr, const Token&)> fn) {
        auto fv = std::make_shared<FunctionValue>(name, fn, env, Token{});
        Environment::Variable v{fv, true};
        env->set(name, v);
    };

    add_native("HashMap_native_ctor", native_HashMap_ctor);
    add_native("HashMap_native_set", native_HashMap_set);
    add_native("HashMap_native_get", native_HashMap_get);
    add_native("HashMap_native_has", native_HashMap_has);
    add_native("HashMap_native_delete", native_HashMap_delete);
    add_native("HashMap_native_keys", native_HashMap_keys);
    add_native("HashMap_native_values", native_HashMap_values);
    add_native("HashMap_native_size", native_HashMap_size);
    add_native("HashMap_native_clear", native_HashMap_clear);

    add_native("Stack_native_push", native_Stack_push);
    add_native("Stack_native_pop", native_Stack_pop);
    add_native("Stack_native_peek", native_Stack_peek);
    add_native("Stack_native_isEmpty", native_Stack_isEmpty);
    add_native("Stack_native_size", native_Stack_size);
    add_native("Stack_native_clear", native_Stack_clear);

    add_native("Queue_native_enqueue", native_Queue_enqueue);
    add_native("Queue_native_dequeue", native_Queue_dequeue);
    add_native("Queue_native_front", native_Queue_front);
    add_native("Queue_native_isEmpty", native_Queue_isEmpty);
    add_native("Queue_native_size", native_Queue_size);
    add_native("Queue_native_clear", native_Queue_clear);

    // ============= HASHMAP CLASS =============
    auto hashMapClass = std::make_shared<ClassValue>();
    hashMapClass->name = "HashMap";
    hashMapClass->token = tok;
    hashMapClass->body = std::make_unique<ClassBodyNode>();
    hashMapClass->defining_env = env;

    // Constructor that calls native helper
    {
        auto ctor = std::make_unique<ClassMethodNode>();
        ctor->name = "HashMap";
        ctor->is_constructor = true;
        ctor->is_locked = false;
        ctor->is_private = false;

        // Optional parameter: sourceObj
        auto p = std::make_unique<ParameterNode>();
        p->name = "sourceObj";
        p->defaultValue = std::make_unique<NullNode>(tok);
        ctor->params.push_back(std::move(p));

        // Call native constructor
        auto call = std::make_unique<CallExpressionNode>();
        call->callee = std::make_unique<IdentifierNode>();
        static_cast<IdentifierNode*>(call->callee.get())->name = "HashMap_native_ctor";
        auto srcId = std::make_unique<IdentifierNode>();
        srcId->name = "sourceObj";
        call->arguments.push_back(std::move(srcId));

        auto exprStmt = std::make_unique<ExpressionStatementNode>();
        exprStmt->expression = std::move(call);
        ctor->body.push_back(std::move(exprStmt));

        hashMapClass->body->methods.push_back(std::move(ctor));
    }

    // Add instance methods (set, get, has, delete, keys, values, size, clear)
    auto add_hashmap_method = [&](const std::string& name, const std::vector<std::string>& params) {
        auto m = std::make_unique<ClassMethodNode>();
        m->name = name;
        m->is_locked = false;
        m->is_private = false;

        for (const auto& pname : params) {
            auto p = std::make_unique<ParameterNode>();
            p->name = pname;
            m->params.push_back(std::move(p));
        }

        auto call = std::make_unique<CallExpressionNode>();
        call->callee = std::make_unique<IdentifierNode>();
        static_cast<IdentifierNode*>(call->callee.get())->name = "HashMap_native_" + name;

        call->arguments.push_back(std::make_unique<ThisExpressionNode>());
        for (const auto& pname : params) {
            auto id = std::make_unique<IdentifierNode>();
            id->name = pname;
            call->arguments.push_back(std::move(id));
        }

        auto ret = std::make_unique<ReturnStatementNode>();
        ret->value = std::move(call);
        m->body.push_back(std::move(ret));

        hashMapClass->body->methods.push_back(std::move(m));
    };

    add_hashmap_method("set", {"key", "value"});
    add_hashmap_method("get", {"key"});
    add_hashmap_method("has", {"key"});
    add_hashmap_method("delete", {"key"});
    add_hashmap_method("keys", {});
    add_hashmap_method("values", {});
    add_hashmap_method("size", {});
    add_hashmap_method("clear", {});

    obj->properties["HashMap"] = PropertyDescriptor{hashMapClass, false, false, true, tok};

    // ============= STACK CLASS =============
    auto stackClass = std::make_shared<ClassValue>();
    stackClass->name = "Stack";
    stackClass->token = tok;
    stackClass->body = std::make_unique<ClassBodyNode>();
    stackClass->defining_env = env;

    // __items__ property
    {
        auto p = std::make_unique<ClassPropertyNode>();
        p->name = "__items__";
        p->is_private = true;
        stackClass->body->properties.push_back(std::move(p));
    }

    // Constructor (empty, items initialized on first push)
    {
        auto ctor = std::make_unique<ClassMethodNode>();
        ctor->name = "Stack";
        ctor->is_constructor = true;
        stackClass->body->methods.push_back(std::move(ctor));
    }

    auto add_stack_method = [&](const std::string& name, const std::vector<std::string>& params) {
        auto m = std::make_unique<ClassMethodNode>();
        m->name = name;

        for (const auto& pname : params) {
            auto p = std::make_unique<ParameterNode>();
            p->name = pname;
            m->params.push_back(std::move(p));
        }

        auto call = std::make_unique<CallExpressionNode>();
        call->callee = std::make_unique<IdentifierNode>();
        static_cast<IdentifierNode*>(call->callee.get())->name = "Stack_native_" + name;

        call->arguments.push_back(std::make_unique<ThisExpressionNode>());
        for (const auto& pname : params) {
            auto id = std::make_unique<IdentifierNode>();
            id->name = pname;
            call->arguments.push_back(std::move(id));
        }

        auto ret = std::make_unique<ReturnStatementNode>();
        ret->value = std::move(call);
        m->body.push_back(std::move(ret));

        stackClass->body->methods.push_back(std::move(m));
    };

    add_stack_method("push", {"value"});
    add_stack_method("pop", {});
    add_stack_method("peek", {});
    add_stack_method("isEmpty", {});
    add_stack_method("size", {});
    add_stack_method("clear", {});

    obj->properties["Stack"] = PropertyDescriptor{stackClass, false, false, true, tok};

    // ============= QUEUE CLASS =============
    auto queueClass = std::make_shared<ClassValue>();
    queueClass->name = "Queue";
    queueClass->token = tok;
    queueClass->body = std::make_unique<ClassBodyNode>();
    queueClass->defining_env = env;

    // __items__ property
    {
        auto p = std::make_unique<ClassPropertyNode>();
        p->name = "__items__";
        p->is_private = true;
        queueClass->body->properties.push_back(std::move(p));
    }

    // Constructor
    {
        auto ctor = std::make_unique<ClassMethodNode>();
        ctor->name = "Queue";
        ctor->is_constructor = true;
        queueClass->body->methods.push_back(std::move(ctor));
    }

    auto add_queue_method = [&](const std::string& name, const std::vector<std::string>& params) {
        auto m = std::make_unique<ClassMethodNode>();
        m->name = name;

        for (const auto& pname : params) {
            auto p = std::make_unique<ParameterNode>();
            p->name = pname;
            m->params.push_back(std::move(p));
        }

        auto call = std::make_unique<CallExpressionNode>();
        call->callee = std::make_unique<IdentifierNode>();
        static_cast<IdentifierNode*>(call->callee.get())->name = "Queue_native_" + name;

        call->arguments.push_back(std::make_unique<ThisExpressionNode>());
        for (const auto& pname : params) {
            auto id = std::make_unique<IdentifierNode>();
            id->name = pname;
            call->arguments.push_back(std::move(id));
        }

        auto ret = std::make_unique<ReturnStatementNode>();
        ret->value = std::move(call);
        m->body.push_back(std::move(ret));

        queueClass->body->methods.push_back(std::move(m));
    };

    add_queue_method("enqueue", {"value"});
    add_queue_method("dequeue", {});
    add_queue_method("front", {});
    add_queue_method("isEmpty", {});
    add_queue_method("size", {});
    add_queue_method("clear", {});

    obj->properties["Queue"] = PropertyDescriptor{queueClass, false, false, true, tok};

    return obj;
}