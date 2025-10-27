#include "set_class.hpp"
#include "ClassRuntime.hpp"

#include <algorithm>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_set>
#include <iomanip>

// Local helper: compare Values using a strict-like behavior that is safe for runtime helpers.
// This mirrors the strict-equality idea used elsewhere: same variant type => compare contents,
// arrays/objects compared by identity (pointer) to avoid heavy recursion here.
static bool values_equal(const Value& a, const Value& b) {
    if (a.index() != b.index()) return false;

    if (auto pa = std::get_if<double>(&a)) {
        auto pb = std::get_if<double>(&b);
        return pb && (*pa == *pb);
    }
    if (auto sa = std::get_if<std::string>(&a)) {
        auto sb = std::get_if<std::string>(&b);
        return sb && (*sa == *sb);
    }
    if (auto ba = std::get_if<bool>(&a)) {
        auto bb = std::get_if<bool>(&b);
        return bb && (*ba == *bb);
    }
    if (auto fa = std::get_if<FunctionPtr>(&a)) {
        auto fb = std::get_if<FunctionPtr>(&b);
        return fb && (fa->get() == fb->get());
    }
    if (auto aa = std::get_if<ArrayPtr>(&a)) {
        auto ab = std::get_if<ArrayPtr>(&b);
        // identity equality for arrays (same object) keeps this helper cheap
        return ab && (aa->get() == ab->get());
    }
    if (auto oa = std::get_if<ObjectPtr>(&a)) {
        auto ob = std::get_if<ObjectPtr>(&b);
        // identity equality for objects
        return ob && (oa->get() == ob->get());
    }
    if (auto ca = std::get_if<ClassPtr>(&a)) {
        auto cb = std::get_if<ClassPtr>(&b);
        return cb && (ca->get() == cb->get());
    }
    // monostate
    return std::holds_alternative<std::monostate>(a) && std::holds_alternative<std::monostate>(b);
}

// Array helpers (operate on ArrayPtr used as internal storage)
static bool array_contains(const ArrayPtr& a, const Value& v) {
    if (!a) return false;
    for (const auto &e : a->elements) {
        if (values_equal(e, v)) return true;
    }
    return false;
}

static bool array_remove(ArrayPtr& a, const Value& v) {
    if (!a) return false;
    for (size_t i = 0; i < a->elements.size(); ++i) {
        if (values_equal(a->elements[i], v)) {
            a->elements.erase(a->elements.begin() + static_cast<long long>(i));
            return true;
        }
    }
    return false;
}

// Build an ArrayPtr from constructor args:
// - Set() -> empty
// - Set([1,2,3]) -> copy array
// - Set(1,2,3) -> use args as elements
// - Set(obj) -> snapshot object values (in whatever order the source object exposes)
static ArrayPtr build_array_from_ctor_args(const std::vector<Value>& args) {
    auto arr = std::make_shared<ArrayValue>();
    if (args.empty()) return arr;

    if (args.size() == 1) {
        if (std::holds_alternative<ArrayPtr>(args[0])) {
            ArrayPtr src = std::get<ArrayPtr>(args[0]);
            if (src) arr->elements = src->elements;
            return arr;
        }
        if (std::holds_alternative<ObjectPtr>(args[0])) {
            ObjectPtr src = std::get<ObjectPtr>(args[0]);
            if (src) {
                for (const auto &kv : src->properties) {
                    arr->elements.push_back(kv.second.value);
                }
            }
            return arr;
        }
    }

    // default: use arguments
    arr->elements = args;
    return arr;
}

// ---------------- native helpers for Set -----------------

// native constructor: returns an ArrayPtr (unique items kept by the constructor)
static Value native_Set_ctor(const std::vector<Value>& args, EnvPtr /*env*/, const Token& /*tok*/) {
    ArrayPtr arr = build_array_from_ctor_args(args);

    // make unique preserving first occurrence using values_equal
    auto uniq = std::make_shared<ArrayValue>();
    for (const auto &v : arr->elements) {
        bool found = false;
        for (const auto &u : uniq->elements) {
            if (values_equal(u, v)) { found = true; break; }
        }
        if (!found) uniq->elements.push_back(v);
    }
    return uniq;
}

// add(this, value) -> bool true if added
static Value native_Set_add(const std::vector<Value>& args, EnvPtr /*env*/, const Token& tok) {
    if (args.size() < 2) throw std::runtime_error("Set.add expects (this, value) at " + tok.loc.to_string());
    if (!std::holds_alternative<ObjectPtr>(args[0])) throw std::runtime_error("Set.add: this must be object at " + tok.loc.to_string());
    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    Value val = args[1];
    auto it = obj->properties.find("__items__");
    ArrayPtr items;
    if (it == obj->properties.end() || !std::holds_alternative<ArrayPtr>(it->second.value)) {
        items = std::make_shared<ArrayValue>();
        PropertyDescriptor pd;
        pd.value = items;
        pd.is_private = true;
        pd.is_readonly = false;
        pd.is_locked = false;
        pd.token = Token{};
        obj->properties["__items__"] = std::move(pd);
    } else {
        items = std::get<ArrayPtr>(obj->properties["__items__"].value);
    }
    if (!array_contains(items, val)) {
        items->elements.push_back(val);
        return true;
    }
    return false;
}

static Value native_Set_has(const std::vector<Value>& args, EnvPtr /*env*/, const Token& tok) {
    if (args.size() < 2) throw std::runtime_error("Set.has expects (this, value) at " + tok.loc.to_string());
    if (!std::holds_alternative<ObjectPtr>(args[0])) throw std::runtime_error("Set.has: this must be object at " + tok.loc.to_string());
    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    auto it = obj->properties.find("__items__");
    if (it == obj->properties.end() || !std::holds_alternative<ArrayPtr>(it->second.value)) return false;
    ArrayPtr items = std::get<ArrayPtr>(it->second.value);
    return array_contains(items, args[1]);
}

static Value native_Set_delete(const std::vector<Value>& args, EnvPtr /*env*/, const Token& tok) {
    if (args.size() < 2) throw std::runtime_error("Set.delete expects (this, value) at " + tok.loc.to_string());
    if (!std::holds_alternative<ObjectPtr>(args[0])) throw std::runtime_error("Set.delete: this must be object at " + tok.loc.to_string());
    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    auto it = obj->properties.find("__items__");
    if (it == obj->properties.end() || !std::holds_alternative<ArrayPtr>(it->second.value)) return false;
    ArrayPtr items = std::get<ArrayPtr>(it->second.value);
    return array_remove(items, args[1]);
}

static Value native_Set_size(const std::vector<Value>& args, EnvPtr /*env*/, const Token& tok) {
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) throw std::runtime_error("Set.size expects (this) at " + tok.loc.to_string());
    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    auto it = obj->properties.find("__items__");
    if (it == obj->properties.end() || !std::holds_alternative<ArrayPtr>(it->second.value)) return static_cast<double>(0.0);
    ArrayPtr items = std::get<ArrayPtr>(it->second.value);
    return static_cast<double>(items->elements.size());
}

static Value native_Set_values(const std::vector<Value>& args, EnvPtr /*env*/, const Token& tok) {
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) throw std::runtime_error("Set.values expects (this) at " + tok.loc.to_string());
    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    auto it = obj->properties.find("__items__");
    if (it == obj->properties.end() || !std::holds_alternative<ArrayPtr>(it->second.value)) {
        auto emptyArr = std::make_shared<ArrayValue>();
        return emptyArr;
    }
    return std::get<ArrayPtr>(it->second.value);
}

static Value native_Set_clear(const std::vector<Value>& args, EnvPtr /*env*/, const Token& tok) {
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) throw std::runtime_error("Set.clear expects (this) at " + tok.loc.to_string());
    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    auto it = obj->properties.find("__items__");
    if (it == obj->properties.end() || !std::holds_alternative<ArrayPtr>(it->second.value)) return std::monostate{};
    ArrayPtr items = std::get<ArrayPtr>(it->second.value);
    items->elements.clear();
    return std::monostate{};
}

static Value native_Set_toPlain(const std::vector<Value>& args, EnvPtr /*env*/, const Token& tok) {
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) throw std::runtime_error("Set.toPlain expects (this) at " + tok.loc.to_string());
    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    auto out = std::make_shared<ObjectValue>();
    auto it = obj->properties.find("__items__");
    if (it != obj->properties.end() && std::holds_alternative<ArrayPtr>(it->second.value)) {
        ArrayPtr items = std::get<ArrayPtr>(it->second.value);
        for (size_t i = 0; i < items->elements.size(); ++i) {
            PropertyDescriptor pd;
            pd.value = items->elements[i];
            pd.is_private = false;
            pd.is_readonly = false;
            pd.is_locked = false;
            pd.token = Token{};
            out->properties[std::to_string((long long)i)] = std::move(pd);
        }
    }
    return out;
}

// ---------------- init_set_class -----------------
void init_set_class(EnvPtr env) {
    if (!env) return;

    auto add_native = [&](const std::string& name, std::function<Value(const std::vector<Value>&, EnvPtr, const Token&)> fn) {
        auto fv = std::make_shared<FunctionValue>(name, fn, env, Token{});
        Environment::Variable v{fv, true};
        env->set(name, v);
    };

    add_native("Set_native_ctor", native_Set_ctor);
    add_native("Set_native_add", native_Set_add);
    add_native("Set_native_has", native_Set_has);
    add_native("Set_native_delete", native_Set_delete);
    add_native("Set_native_size", native_Set_size);
    add_native("Set_native_values", native_Set_values);
    add_native("Set_native_clear", native_Set_clear);
    add_native("Set_native_toPlain", native_Set_toPlain);
    // Note: toJson intentionally omitted (removed as requested)

    // Build the ClassValue descriptor
    auto classDesc = std::make_shared<ClassValue>();
    classDesc->name = "Set";
    classDesc->token = Token{};
    classDesc->body = std::make_unique<ClassBodyNode>();

    // property __items__
    {
        auto p = std::make_unique<ClassPropertyNode>();
        p->name = "__items__";
        p->is_private = true;
        p->is_locked = false;
        classDesc->body->properties.push_back(std::move(p));
    }

    // constructor: this.__items__ = Set_native_ctor(...args)
    {
        auto ctor = std::make_unique<ClassMethodNode>();
        ctor->name = classDesc->name;
        ctor->is_constructor = true;
        ctor->is_locked = false;
        ctor->is_private = false;

        // rest parameter ...args
        auto p = std::make_unique<ParameterNode>();
        p->token = Token{};
        p->name = "args";
        p->is_rest = true;
        p->rest_required_count = 0;
        p->defaultValue = nullptr;
        ctor->params.push_back(std::move(p));

        // assign this.__items__ = Set_native_ctor(...args)
        auto assign = std::make_unique<AssignmentNode>();
        auto target = std::make_unique<MemberExpressionNode>();
        target->object = std::make_unique<ThisExpressionNode>();
        target->property = "__items__";
        assign->target = std::move(target);

        auto call = std::make_unique<CallExpressionNode>();
        call->callee = std::make_unique<IdentifierNode>();
        static_cast<IdentifierNode*>(call->callee.get())->name = "Set_native_ctor";

        auto spread = std::make_unique<SpreadElementNode>();
        auto argsIdent = std::make_unique<IdentifierNode>();
        argsIdent->name = "args";
        spread->argument = std::move(argsIdent);
        call->arguments.push_back(std::move(spread));

        assign->value = std::move(call);
        ctor->body.push_back(std::move(assign));
        classDesc->body->methods.push_back(std::move(ctor));
    }

    // forwarder helpers
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

        auto call = std::make_unique<CallExpressionNode>();
        call->callee = std::make_unique<IdentifierNode>();
        static_cast<IdentifierNode*>(call->callee.get())->name = std::string("Set_native_") + name;

        call->arguments.push_back(std::make_unique<ThisExpressionNode>());
        for (const auto& p : params) {
            auto id = std::make_unique<IdentifierNode>();
            id->name = p;
            call->arguments.push_back(std::move(id));
        }

        auto ret = std::make_unique<ReturnStatementNode>();
        ret->value = std::move(call);
        m->body.push_back(std::move(ret));
        classDesc->body->methods.push_back(std::move(m));
    };

    add_forwarder("add", {"value"});
    add_forwarder("has", {"value"});
    add_forwarder("delete", {"value"});
    add_forwarder("size", {});
    add_forwarder("values", {});
    add_forwarder("clear", {});
    add_forwarder("toPlain", {});
    // toJson omitted per request

    Environment::Variable var;
    var.value = classDesc;
    var.is_constant = true;
    env->set(classDesc->name, var);
    env->set("Seti", var);
}