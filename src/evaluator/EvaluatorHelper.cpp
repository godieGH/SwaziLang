#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include "ClassRuntime.hpp"
#include "Frame.hpp"
#include "SwaziError.hpp"
#include "evaluator.hpp"

bool supports_color() {
    return isatty(STDOUT_FILENO);
}

namespace Color {
const std::string reset = "\033[0m";

// Standard
const std::string black = "\033[30m";
const std::string red = "\033[31m";
const std::string green = "\033[32m";
const std::string yellow = "\033[33m";
const std::string blue = "\033[34m";
const std::string magenta = "\033[35m";
const std::string cyan = "\033[36m";
const std::string white = "\033[37m";

// Bright versions
const std::string bright_black = "\033[90m";  // gray
const std::string bright_red = "\033[91m";
const std::string bright_green = "\033[92m";
const std::string bright_yellow = "\033[93m";
const std::string bright_blue = "\033[94m";
const std::string bright_magenta = "\033[95m";
const std::string bright_cyan = "\033[96m";
const std::string bright_white = "\033[97m";
}  // namespace Color

// ----------------- Evaluator helpers -----------------

static std::string value_type_name(const Value& v) {
    if (std::holds_alternative<std::monostate>(v)) return "null";
    if (std::holds_alternative<double>(v)) return "namba";
    if (std::holds_alternative<std::string>(v)) return "neno";
    if (std::holds_alternative<bool>(v)) return "bool";
    if (std::holds_alternative<FunctionPtr>(v)) return "kazi";
    if (std::holds_alternative<ArrayPtr>(v)) return "orodha";
    if (std::holds_alternative<ObjectPtr>(v)) return "object";
    if (std::holds_alternative<ClassPtr>(v)) return "muundo";
    if (std::holds_alternative<HoleValue>(v)) return "emptyhole";
    if (std::holds_alternative<PromisePtr>(v)) return "promise";
    return "unknown";
}

std::string Evaluator::type_name(const Value& v) {
    return value_type_name(v);
}

double Evaluator::to_number(const Value& v, Token token) {
    if (std::holds_alternative<double>(v)) {
        return std::get<double>(v);
    }

    if (std::holds_alternative<bool>(v)) {
        return std::get<bool>(v) ? 1.0 : 0.0;
    }

    if (std::holds_alternative<std::string>(v)) {
        const auto& s = std::get<std::string>(v);
        try {
            size_t idx = 0;
            double d = std::stod(s, &idx);

            // if no characters were parsed
            if (idx == 0) {
                throw std::runtime_error(
                    "ValueError at " + token.loc.to_string() +
                    "\nCannot convert string '" + s + "' to number: no digits found" +
                    "\n --> Traced at:\n" + token.loc.get_line_trace());
            }

            // if only part of the string was parsed
            if (idx != s.size()) {
                throw std::runtime_error(
                    "ValueError at " + token.loc.to_string() +
                    "\nCannot convert string '" + s + "' to number: contains invalid characters after position " + std::to_string(idx) +
                    "\n --> Traced at:\n" + token.loc.get_line_trace());
            }

            return d;
        } catch (const std::exception& e) {
            throw std::runtime_error(
                "ValueError at " + token.loc.to_string() +
                "\nCannot convert string '" + s + "' to number(" + e.what() + ")" +
                "\n --> Traced at:\n" + token.loc.get_line_trace());
        }
    }

    // Arrays, functions, or other unsupported types
    throw std::runtime_error(
        "TypeError at " + token.loc.to_string() +
        "\nCannot convert value of type `" + value_type_name(v) + "` to a number" +
        "\n --> Traced at:\n" + token.loc.get_line_trace());
}

std::string Evaluator::to_string_value(const Value& v, bool no_color) {
    static bool supports_colors = supports_color();
    bool use_color = supports_colors && !no_color;

    if (std::holds_alternative<std::monostate>(v)) return use_color ? Color::bright_black + "null" + Color::reset : "null";
    if (std::holds_alternative<double>(v)) {
        std::ostringstream ss;
        double d = std::get<double>(v);
        if (std::fabs(d - std::round(d)) < 1e-12)
            ss << (long long)std::llround(d);
        else
            ss << d;
        return use_color ? Color::yellow + ss.str() + Color::reset : ss.str();
    }
    if (std::holds_alternative<bool>(v)) {
        std::string s = std::get<bool>(v) ? "kweli" : "sikweli";
        return use_color ? Color::bright_magenta + s + Color::reset : s;
    }
    if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
    if (std::holds_alternative<HoleValue>(v)) {
        // use a compact display for a hole when converted to string
        return std::string(use_color ? (Color::bright_black + "<empty>" + Color::reset) : "<empty>");
    }
    if (std::holds_alternative<FunctionPtr>(v)) {
        FunctionPtr fn = std::get<FunctionPtr>(v);
        std::string name = fn->name.empty() ? "<lambda>" : fn->name;
        std::string s = use_color
            ? (std::string(Color::bright_cyan) + "[" + (fn->is_async ? "Async->" : "") + "kazi " + name + "]" + Color::reset)
            : ("[" + std::string(fn->is_async ? "Async->" : "") + "kazi " + name + "]");
        return s;
    }
    if (std::holds_alternative<ArrayPtr>(v)) {
        std::unordered_set<const ArrayValue*> arrvisited;
        std::unordered_set<const ObjectValue*> visited;
        auto arr = std::get<ArrayPtr>(v);
        return print_value(arr, 0, visited, arrvisited);
    }
    if (std::holds_alternative<ObjectPtr>(v)) {
        ObjectPtr op = std::get<ObjectPtr>(v);
        if (!op) return "{}";
        std::unordered_set<const ObjectValue*> visited;
        return print_object(op, 0, visited);  // <- you write this pretty-printer
    }
    if (std::holds_alternative<ClassPtr>(v)) {
        ClassPtr cp = std::get<ClassPtr>(v);
        if (!cp) {
            std::ostringstream ss;
            ss << "[muundo <null>]";
            return use_color ? (Color::bright_blue + ss.str() + Color::reset) : ss.str();
        }
        std::string label = "[muundo " + cp->name + "]";
        std::string out;
        if (use_color)
            out = Color::bright_blue + label + Color::reset;
        else
            out = label;
        if (cp->static_table) {
            std::unordered_set<const ObjectValue*> visited;
            std::string static_repr = print_object(cp->static_table, 0, visited);
            if (!static_repr.empty() && static_repr != "{}") {
                out += " " + static_repr;
            }
        }
        return out;
    }

    if (std::holds_alternative<PromisePtr>(v)) {
        PromisePtr p = std::get<PromisePtr>(v);
        if (!p) {
            std::ostringstream ss;
            ss << "[Promise <null>]";
            return use_color ? (Color::bright_blue + ss.str() + Color::reset) : ss.str();
        }
        // Pending
        if (p->state == PromiseValue::State::PENDING) {
            return use_color ? (Color::bright_blue + "Promise {<PENDING>}" + Color::reset) : "Promise {<PENDING>}";
        }

        // Fulfilled — show the inner value using the regular pretty-printer
        if (p->state == PromiseValue::State::FULFILLED) {
            std::string inner = print_value(p->result, 0);
            if (use_color) {
                // color the outer label but leave inner value coloring as returned by print_value
                return Color::bright_blue + std::string("Promise { ") + Color::reset + inner + Color::bright_blue + " }" + Color::reset;
            }
            return std::string("Promise { ") + inner + " }";
        }

        // Rejected — show reason
        if (p->state == PromiseValue::State::REJECTED) {
            if (use_color) {
                return (Color::bright_blue +
                    "Promise {" +
                    Color::reset +
                    Color::bright_black +
                    std::string("<REJECTED>") +
                    Color::reset +
                    Color::bright_blue + "}" + Color::reset);
            }
            return std::string("Promise {<REJECTED>}");
        }
    }

    if (std::holds_alternative<GeneratorPtr>(v)) {
        GeneratorPtr g = std::get<GeneratorPtr>(v);
        if (!g || !g->frame || !g->frame->function) {
            return use_color ? (Color::bright_blue + "[generator <dead>]" + Color::reset) : "[generator <dead>]";
        }
        std::string fname = g->frame->function->name.empty() ? "<lambda>" : g->frame->function->name;
        std::string state_str;
        switch (g->state) {
            case GeneratorValue::State::SuspendedStart:
                state_str = "suspended-start";
                break;
            case GeneratorValue::State::SuspendedYield:
                state_str = "suspended";
                break;
            case GeneratorValue::State::Executing:
                state_str = "executing";
                break;
            case GeneratorValue::State::Completed:
                state_str = "closed";
                break;
        }
        std::ostringstream ss;
        ss << "[generator " << fname << " <" << state_str << ">]";
        return use_color ? (Color::bright_blue + ss.str() + Color::reset) : ss.str();
    }

    return "";
}

bool Evaluator::to_bool(const Value& v) {
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v);
    if (std::holds_alternative<double>(v)) return !std::isnan(std::get<double>(v)) && std::get<double>(v) != 0.0;
    if (std::holds_alternative<std::string>(v)) return !std::get<std::string>(v).empty();
    if (std::holds_alternative<std::monostate>(v)) return false;
    if (std::holds_alternative<FunctionPtr>(v)) return true;
    if (std::holds_alternative<ArrayPtr>(v)) {
        auto arr = std::get<ArrayPtr>(v);
        return arr && !arr->elements.empty();
    }
    if (std::holds_alternative<ObjectPtr>(v)) {
        auto obj = std::get<ObjectPtr>(v);
        return obj && !obj->properties.empty();
    };
    if (std::holds_alternative<ClassPtr>(v)) return true;  // classes always return true as they appear
    return false;
}

// ----------------------
// Globals / proxy helpers
// ----------------------

// Keys we treat as protected when writing via globals() proxy.
// These are builtin/module metadata and core objects that should not be overwritten by user code through globals().
static const std::unordered_set<std::string> g_protected_global_keys = {
    "__name__", "__file__", "__dir__", "__main__",
    "__builtins__",
    "Object", "Hesabu", "swazi", "Orodha", "Bool", "Namba", "Neno"};

// Deep/equivalence comparator used by array helpers (indexOf/includes/remove-by-value)
bool Evaluator::is_equal(const Value& a, const Value& b) {
    // both undefined
    if (std::holds_alternative<std::monostate>(a) && std::holds_alternative<std::monostate>(b)) return true;

    // numbers
    if (std::holds_alternative<double>(a) && std::holds_alternative<double>(b))
        return std::get<double>(a) == std::get<double>(b);

    // booleans
    if (std::holds_alternative<bool>(a) && std::holds_alternative<bool>(b))
        return std::get<bool>(a) == std::get<bool>(b);

    // strings
    if (std::holds_alternative<std::string>(a) && std::holds_alternative<std::string>(b))
        return std::get<std::string>(a) == std::get<std::string>(b);

    // mixed number <-> string: try numeric compare
    if (std::holds_alternative<double>(a) && std::holds_alternative<std::string>(b)) {
        try {
            double bb = std::stod(std::get<std::string>(b));
            return std::get<double>(a) == bb;
        } catch (...) {
            /* fallthrough */
        }
    }
    if (std::holds_alternative<std::string>(a) && std::holds_alternative<double>(b)) {
        try {
            double aa = std::stod(std::get<std::string>(a));
            return aa == std::get<double>(b);
        } catch (...) {
            /* fallthrough */
        }
    }

    // functions: compare pointer equality
    if (std::holds_alternative<FunctionPtr>(a) && std::holds_alternative<FunctionPtr>(b))
        return std::get<FunctionPtr>(a) == std::get<FunctionPtr>(b);

    // arrays: deep compare
    if (std::holds_alternative<ArrayPtr>(a) && std::holds_alternative<ArrayPtr>(b)) {
        ArrayPtr A = std::get<ArrayPtr>(a);
        ArrayPtr B = std::get<ArrayPtr>(b);
        if (!A || !B) return A == B;
        if (A->elements.size() != B->elements.size()) return false;
        for (size_t i = 0; i < A->elements.size(); ++i) {
            if (!is_equal(A->elements[i], B->elements[i])) return false;
        }
        return true;
    }

    // fallback: compare stringified values
    return to_string_value(a) == to_string_value(b);
}
bool Evaluator::is_strict_equal(const Value& a, const Value& b) {
    // Fast path: identical variant index -> compare by contained type without coercion
    if (a.index() != b.index()) return false;

    // monostate == monostate
    if (std::holds_alternative<std::monostate>(a)) return true;

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
        // strict equality for function values: compare shared_ptr identity
        return fb && (fa->get() == fb->get());
    }
    if (auto aa = std::get_if<ArrayPtr>(&a)) {
        auto ab = std::get_if<ArrayPtr>(&b);
        return ab && (aa->get() == ab->get());  // identity (same array object)
    }
    if (auto oa = std::get_if<ObjectPtr>(&a)) {
        auto ob = std::get_if<ObjectPtr>(&b);
        return ob && (oa->get() == ob->get());  // object identity
    }
    if (auto ca = std::get_if<ClassPtr>(&a)) {
        auto cb = std::get_if<ClassPtr>(&b);
        return cb && (ca->get() == cb->get());
    }

    // fallback false for unknown combinations
    return false;
}

bool Evaluator::is_private_access_allowed(ObjectPtr obj, EnvPtr env) {
    if (!env || !obj) return false;
    EnvPtr walk = env;
    while (walk) {
        auto it = walk->values.find("$");
        if (it != walk->values.end()) {
            const Value& v = it->second.value;
            // direct object equality ($ points exactly to the object)
            if (std::holds_alternative<ObjectPtr>(v)) {
                ObjectPtr bound = std::get<ObjectPtr>(v);
                if (bound == obj) return true;  // same shared_ptr
                // If $ is an instance, and the object being accessed is that instance's class static_table,
                // allow access (this makes $.staticProp refer to class's static when appropriate).
                // Check for __class__ link on the bound instance:
                if (bound) {
                    auto cls_it = bound->properties.find("__class__");
                    if (cls_it != bound->properties.end() && std::holds_alternative<ClassPtr>(cls_it->second.value)) {
                        ClassPtr instClass = std::get<ClassPtr>(cls_it->second.value);
                        if (instClass && instClass->static_table == obj) {
                            return true;
                        }
                    }
                }
            }
        }
        walk = walk->parent;
    }
    return false;
}

Value Evaluator::get_object_property(ObjectPtr op, const std::string& prop, EnvPtr accessorEnv, const Token& token) {
    if (!op) return std::monostate{};

    // Special-case: environment proxy. Map property access to environment lookup.
    if (op->is_env_proxy && op->proxy_env) {
        EnvPtr walk = op->proxy_env;
        while (walk) {
            auto it = walk->values.find(prop);
            if (it != walk->values.end()) {
                return it->second.value;
            }
            walk = walk->parent;
        }
        // Not found -> undefined
        return std::monostate{};
    }

    // --- Existing object semantics ---
    auto it = op->properties.find(prop);
    if (it == op->properties.end()) return std::monostate{};

    const PropertyDescriptor& desc = it->second;

    // Enforce private access rules (same as before)
    if (desc.is_private && !is_private_access_allowed(op, accessorEnv)) {
        throw SwaziError(
            "PermissionError", "Cannot access private property '" + prop + "'.", token.loc);
    }

    // If property is readonly and stores a function, treat as getter and call it
    if (desc.is_readonly && std::holds_alternative<FunctionPtr>(desc.value)) {
        FunctionPtr getter = std::get<FunctionPtr>(desc.value);
        // Call with zero args, passing the accessor environment as caller env
        return call_function(getter, {}, accessorEnv, desc.token);
    }

    return desc.value;
}
void Evaluator::set_object_property(ObjectPtr op, const std::string& prop, const Value& val, EnvPtr accessorEnv, const Token& token) {
    if (!op) {
        throw SwaziError("TypeError", "Attempted to set property on null object.", token.loc);
    }

    // if object is frozen just return silently
    if (op->is_frozen && !is_private_access_allowed(op, accessorEnv)) {
        return;
    }

    // Special-case: environment proxy — write into the proxied Environment
    if (op->is_env_proxy && op->proxy_env) {
        // Disallow writing protected global/module keys via the env-proxy.
        if (g_protected_global_keys.find(prop) != g_protected_global_keys.end()) {
            throw SwaziError(
                "PermissionError",
                "Cannot assign to protected module/builtin name '" + prop + "' via globals().",
                token.loc);
        }

        // If variable already exists in proxied environment chain, update the nearest defining env.
        // Otherwise, create the variable in the proxied environment itself (module-level behavior).
        EnvPtr walk = op->proxy_env;
        EnvPtr defining_env = nullptr;
        while (walk) {
            auto it = walk->values.find(prop);
            if (it != walk->values.end()) {
                defining_env = walk;
                break;
            }
            walk = walk->parent;
        }

        // Create variable descriptor and set
        Environment::Variable var;
        var.value = val;
        var.is_constant = false;  // assignments via globals() produce mutable vars by default

        if (defining_env) {
            defining_env->set(prop, var);  // update existing variable in its defining environment
        } else {
            op->proxy_env->set(prop, var);  // create new variable in proxied environment
        }
        return;
    }

    // --- Normal object property semantics ---

    // If property exists, enforce permission/lock/private rules
    auto it = op->properties.find(prop);
    if (it != op->properties.end()) {
        PropertyDescriptor& desc = it->second;
        if (desc.is_private && !is_private_access_allowed(op, accessorEnv)) {
            throw SwaziError("PermissionError", "Cannot assign to private property '" + prop + "'.", token.loc);
        }
        if (desc.is_locked && !is_private_access_allowed(op, accessorEnv)) {
            throw SwaziError("PermissionError", "Cannot assign to locked property '" + prop + "'.", token.loc);
        }
        if (desc.is_readonly) {
            throw SwaziError("TypeError", "Cannot assign to read-only property '" + prop + "'.", token.loc);
        }
        // Update descriptor
        desc.value = val;
        desc.token = token;
        return;
    }

    // Property does not exist: create a new public property descriptor
    PropertyDescriptor newDesc;
    newDesc.value = val;
    newDesc.is_private = false;
    newDesc.is_readonly = false;
    newDesc.is_locked = false;
    newDesc.token = token;
    op->properties[prop] = std::move(newDesc);
}
void Evaluator::bind_pattern_to_value(ExpressionNode* pattern, const Value& value, EnvPtr env, bool is_constant, const Token& declToken) {
    if (!pattern) return;

    // Array pattern
    if (auto arrPat = dynamic_cast<ArrayPatternNode*>(pattern)) {
        // RHS must be an array
        if (!std::holds_alternative<ArrayPtr>(value)) {
            throw SwaziError(
                "TypeError",
                "Cannot destructure a non-array value.",
                declToken.loc);
        }
        ArrayPtr src = std::get<ArrayPtr>(value);
        size_t srcLen = src ? src->elements.size() : 0;

        for (size_t i = 0; i < arrPat->elements.size(); ++i) {
            auto& elem = arrPat->elements[i];
            // hole
            if (!elem) {
                // leave hole as undefined / skip
                continue;
            }
            // rest element (SpreadElementNode)
            if (auto spread = dynamic_cast<SpreadElementNode*>(elem.get())) {
                // argument should evaluate to an IdentifierNode (target)
                if (!spread->argument) {
                    throw SwaziError(
                        "SyntaxError",
                        "Invalid rest target in array pattern — missing argument.",
                        spread->token.loc);
                }
                auto idTarget = dynamic_cast<IdentifierNode*>(spread->argument.get());
                if (!idTarget) {
                    throw SwaziError(
                        "SyntaxError",
                        "Only an identifier is allowed as the rest target in an array pattern.",
                        spread->token.loc);
                }
                // collect remaining elements starting at current index
                auto restArr = std::make_shared<ArrayValue>();
                restArr->elements.reserve((i < srcLen) ? (srcLen - i) : 0);
                for (size_t k = i; k < srcLen; ++k) {
                    restArr->elements.push_back(src->elements[k]);
                }
                Environment::Variable var;
                var.value = restArr;
                var.is_constant = is_constant;
                env->set(idTarget->name, var);
                // rest must be last; ignore any further pattern slots
                break;
            }

            // normal identifier element
            if (auto id = dynamic_cast<IdentifierNode*>(elem.get())) {
                Value v = std::monostate{};
                if (i < srcLen && src) v = src->elements[i];
                Environment::Variable var;
                var.value = v;
                var.is_constant = is_constant;
                env->set(id->name, var);
                continue;
            }

            // spread could also be represented as an Identifier wrapped differently, but we only support the above
            throw SwaziError(
                "SyntaxError",
                "Unsupported element in array destructuring pattern.",
                arrPat->token.loc);
        }
        return;
    }

    // Object pattern
    if (auto objPat = dynamic_cast<ObjectPatternNode*>(pattern)) {
        if (!std::holds_alternative<ObjectPtr>(value)) {
            throw SwaziError(
                "TypeError",
                "Cannot destructure a non-object value.",
                declToken.loc);
        }
        ObjectPtr src = std::get<ObjectPtr>(value);

        for (const auto& propPtr : objPat->properties) {
            if (!propPtr) continue;
            const std::string& key = propPtr->key;
            // target should be an IdentifierNode (we support only simple renames/shorthand)
            auto targetId = dynamic_cast<IdentifierNode*>(propPtr->value.get());
            if (!targetId) {
                throw SwaziError(
                    "SyntaxError",
                    "Only identifier targets are supported in object patterns.",
                    propPtr->value->token.loc);
            }

            // Use existing getter to respect privacy/getters (get_object_property is a member)
            Value v = std::monostate{};
            if (src) {
                try {
                    v = get_object_property(src, key, env, declToken);
                } catch (...) {
                    // if getter throws, rethrow with context
                    throw;
                }
            }
            Environment::Variable var;
            var.value = v;
            var.is_constant = is_constant;
            env->set(targetId->name, var);
        }
        return;
    }

    throw SwaziError(
        "SyntaxError",
        "Unsupported pattern node in destructuring assignment.",
        pattern->token.loc);
}
// Tune these to control "small object" inline behavior:
static constexpr int INLINE_MAX_PROPS = 5;
static constexpr int INLINE_MAX_LEN = 150;

// Helper: are we a primitive-ish value that can be inlined simply?
static bool is_simple_value(const Value& v) {
    if (std::holds_alternative<std::monostate>(v)) return true;
    if (std::holds_alternative<double>(v)) return true;
    if (std::holds_alternative<std::string>(v)) return true;
    if (std::holds_alternative<bool>(v)) return true;
    if (std::holds_alternative<HoleValue>(v)) return true;  // hole is simple for printing decisions
    return false;
}

// Try to render an object inline. Returns std::nullopt if not suitable for inline.
static std::optional<std::string> try_render_inline_object(ObjectPtr o,
    std::unordered_set<const ObjectValue*>& visited) {
    if (!o) return std::string("{}");
    const ObjectValue* p = o.get();
    if (visited.count(p)) return std::string("{/*cycle*/}");

    // Count visible properties and ensure they're all simple values (no nested objects/arrays/functions).
    std::vector<std::pair<std::string,
        Value>>
        kvs;
    kvs.reserve(o->properties.size());
    for (const auto& kv : o->properties) {
        const std::string& k = kv.first;
        const PropertyDescriptor& desc = kv.second;
        if (desc.is_private) continue;  // skip private from inline view
        // if value is not simple, bail
        if (!is_simple_value(desc.value)) return std::nullopt;
        kvs.emplace_back(k, desc.value);
        if ((int)kvs.size() > INLINE_MAX_PROPS) return std::nullopt;
    }

    // Build inline string (temporarily without tracking visited since values are simple)
    std::ostringstream oss;
    oss << "{";
    for (size_t i = 0; i < kvs.size(); ++i) {
        if (i) oss << ", ";
        // use plain, non-colored formatting here — color will be added in print_value
        // but print_value returns colored string already so use it.
        // key formatting: we want key only (no quotes)
        oss << kvs[i].first << ": " << /* value will be colored by print_value caller */ "";
    }
    oss << "}";

    // Now we need values rendered to check length — render values inline using print_value
    std::string combined;
    {
        std::ostringstream tmp;
        tmp << "{";
        for (size_t i = 0; i < kvs.size(); ++i) {
            if (i) tmp << ", ";
            tmp << kvs[i].first << ": " << /* placeholder, will be replaced below */ "";
        }
        tmp << "}";
        combined = tmp.str();
    }

    // To produce the exact inline string with colors, build properly:
    std::ostringstream finaloss;
    finaloss << "{";
    for (size_t i = 0; i < kvs.size(); ++i) {
        if (i) finaloss << ", ";
        finaloss << kvs[i].first << ": " << /* we'll call Evaluator::print_value below */ "";
    }
    // But we cannot call Evaluator::print_value here (static function). We'll instead let the caller
    // render final inline (so return list of keys+values). To keep it simpler, we will instead
    // let the caller call print_value. So return nullopt to avoid complexity.
    // For simplicity, here we bail out and let the main printer decide inline rendering.
    return std::nullopt;
}

// Color-aware string escape helper for display (keeps simple escaping)
static std::string quote_and_color(const std::string& s, bool use_color) {
    if (!use_color) {
        std::ostringstream ss;
        ss << "'";
        for (char c : s) {
            if (c == '\\')
                ss << "\\\\";
            else if (c == '\'')
                ss << "\\'";
            else if (c == '\n')
                ss << "\\n";
            else
                ss << c;
        }
        ss << "'";  // ← Changed
        return ss.str();
    }
    // with color: color the inner string green, keep quotes white
    std::ostringstream ss;
    ss << Color::white << "'" << Color::reset;  // ← Changed
    ss << Color::green;
    for (char c : s) {
        if (c == '\\')
            ss << "\\\\";
        else if (c == '\'')
            ss << "\\'";  // ← Changed to escape single quotes
        else if (c == '\n')
            ss << "\\n";
        else
            ss << c;
    }
    ss << Color::reset;
    ss << Color::white << "'" << Color::reset;  // ← Changed
    return ss.str();
}
std::string Evaluator::cerr_colored(const std::string& s) {
    bool use_color = supports_color();
    std::string err_str = use_color ? (Color::bright_red + "Error: " + Color::reset) : "Error: ";
    std::string ss = use_color ? (Color::bright_black + s + Color::reset) : s;
    return err_str + ss;
}

std::string Evaluator::print_value(
    const Value& v,
    int depth,
    std::unordered_set<const ObjectValue*> visited,
    std::unordered_set<const ArrayValue*> arrvisited) {
    bool use_color = supports_color();

    if (std::holds_alternative<std::monostate>(v)) {
        return use_color ? (Color::bright_black + std::string("null") + Color::reset) : std::string("null");
    }

    if (std::holds_alternative<double>(v)) {
        std::ostringstream ss;
        double d = std::get<double>(v);
        if (std::fabs(d - std::round(d)) < 1e-12)
            ss << (long long)std::llround(d);
        else
            ss << d;
        return use_color ? (Color::yellow + ss.str() + Color::reset) : ss.str();
    }

    if (std::holds_alternative<bool>(v)) {
        std::string s = std::get<bool>(v) ? "kweli" : "sikweli";
        return use_color ? (Color::bright_magenta + s + Color::reset) : s;
    }

    if (std::holds_alternative<std::string>(v)) {
        const std::string& s = std::get<std::string>(v);
        return quote_and_color(s, use_color);
    }

    if (std::holds_alternative<FunctionPtr>(v)) {
        FunctionPtr fn = std::get<FunctionPtr>(v);
        std::string nm = fn->name.empty() ? "<lambda>" : fn->name;
        std::ostringstream ss;
        ss << "[" << (fn->is_async ? "Async->" : "") << "kazi " << nm << "]";
        return use_color ? (Color::bright_cyan + ss.str() + Color::reset) : ss.str();
    }

    if (std::holds_alternative<ArrayPtr>(v)) {
        ArrayPtr arr = std::get<ArrayPtr>(v);
        if (!arr) return "[]";

        const ArrayValue* p = arr.get();
        if (arrvisited.count(p)) return "[/*cycle*/]";
        arrvisited.insert(p);

        // Check if the array consists entirely of holes
        bool allHoles = true;
        for (const auto& el : arr->elements) {
            if (!std::holds_alternative<HoleValue>(el)) {
                allHoles = false;
                break;
            }
        }

        // if arr is empty then all holes is false
        if (arr->elements.empty()) {
            allHoles = false;
        }

        if (allHoles) {
            std::ostringstream ss;
            if (use_color) {
                ss << "[" << Color::bright_black << "<" << arr->elements.size() << " empty holes" << ">" << Color::reset << "]";
            } else {
                ss << "[" << "<" << arr->elements.size() << " empty holes" << ">" << "]";
            }
            arrvisited.erase(p);
            return ss.str();
        }

        // otherwise try compact inline if small and elements simple
        bool can_inline = (arr->elements.size() <= (size_t)(15));
        if (can_inline) {
            for (const auto& e : arr->elements) {
                if (!is_simple_value(e)) {
                    can_inline = false;
                    break;
                }
            }
        }
        std::ostringstream ss;
        if (can_inline) {
            ss << "[";
            for (size_t i = 0; i < arr->elements.size(); ++i) {
                if (i) ss << ", ";
                // If hole — print "<empty>"
                if (std::holds_alternative<HoleValue>(arr->elements[i])) {
                    ss << (use_color ? (Color::bright_black + "<empty>" + Color::reset) : "<empty>");
                } else {
                    ss << print_value(arr->elements[i], depth + 1, visited, arrvisited);
                }
            }
            ss << "]";
            arrvisited.erase(p);
            return ss.str();
        } else {
            // multi-line array
            ss << "[\n";
            std::string ind(depth + 2, ' ');
            for (size_t i = 0; i < arr->elements.size(); ++i) {
                ss << ind;
                if (std::holds_alternative<HoleValue>(arr->elements[i])) {
                    ss << (use_color ? (Color::bright_black + "<empty>" + Color::reset) : "<empty>");
                } else {
                    ss << print_value(arr->elements[i], depth + 2, visited, arrvisited);
                }
                if (i + 1 < arr->elements.size()) ss << ",\n";
            }
            ss << "\n"
               << std::string(depth, ' ') << "]";
            arrvisited.erase(p);
            return ss.str();
        }
    }

    if (std::holds_alternative<ObjectPtr>(v)) {
        ObjectPtr op = std::get<ObjectPtr>(v);
        if (!op) return "{}";
        return print_object(op, depth, visited);
    }

    if (std::holds_alternative<ClassPtr>(v)) {
        ClassPtr cp = std::get<ClassPtr>(v);
        if (!cp) {
            std::ostringstream ss;
            ss << "[muundo " << "<null>" << "]";
            return use_color ? (Color::bright_blue + ss.str() + Color::reset) : ss.str();
        }

        // label only
        std::string label = "[muundo " + cp->name + "]";
        std::string out;
        if (use_color)
            out = Color::bright_blue + label + Color::reset;
        else
            out = label;

        // Append static table representation (use the same 'visited' set so cycles are detected)
        if (cp->static_table) {
            std::string static_repr = print_object(cp->static_table, depth, visited);
            if (!static_repr.empty() && static_repr != "{}") {
                out += " " + static_repr;
            }
        }

        return out;
    }

    if (std::holds_alternative<PromisePtr>(v)) {
        PromisePtr p = std::get<PromisePtr>(v);
        if (!p) return use_color ? (Color::bright_blue + "Promise {<null>}" + Color::reset) : "Promise {<null>}";

        if (p->state == PromiseValue::State::PENDING) {
            return use_color ? (Color::bright_blue + "Promise {<PENDING>}" + Color::reset) : "Promise {<PENDING>}";
        }

        if (p->state == PromiseValue::State::FULFILLED) {
            std::ostringstream ss;
            ss << "Promise { ";
            ss << print_value(p->result, depth + 1, visited, arrvisited);
            ss << " }";
            std::string s = ss.str();
            return use_color ? (Color::bright_blue + s + Color::reset) : s;
        }

        // REJECTED
        std::ostringstream ss;
        std::string reject_str = use_color ? (Color::bright_black + "<REJECTED>" + Color::reset) : "<REJECTED>";
        ss << Color::bright_blue << "Promise {" << Color::reset << (reject_str) << Color::bright_blue << "}" << Color::reset;
        return ss.str();
    }

    if (std::holds_alternative<GeneratorPtr>(v)) {
        GeneratorPtr g = std::get<GeneratorPtr>(v);
        if (!g || !g->frame || !g->frame->function) {
            return use_color ? (Color::bright_blue + "[generator <dead>]" + Color::reset) : "[generator <dead>]";
        }
        std::string fname = g->frame->function->name.empty() ? "<lambda>" : g->frame->function->name;
        std::string state_str;
        switch (g->state) {
            case GeneratorValue::State::SuspendedStart:
                state_str = "suspended-start";
                break;
            case GeneratorValue::State::SuspendedYield:
                state_str = "suspended";
                break;
            case GeneratorValue::State::Executing:
                state_str = "executing";
                break;
            case GeneratorValue::State::Completed:
                state_str = "closed";
                break;
        }
        std::ostringstream ss;
        ss << "[generator " << fname << " <" << state_str << ">]";
        return use_color ? (Color::bright_blue + ss.str() + Color::reset) : ss.str();
    }

    return "<?>";  // fallback
}

std::string Evaluator::print_object(
    ObjectPtr obj,
    int indent,
    std::unordered_set<const ObjectValue*> visited) {
    if (!obj) return "{}";

    bool use_color = supports_color();

    // If this object is an environment proxy, render its backing Environment's variables.
    if (obj->is_env_proxy) {
        if (!obj->proxy_env) return "{}";

        // Cycle detection: if we've already visited this ObjectValue (proxy), avoid recursing.
        const ObjectValue* proxy_ptr = obj.get();
        if (visited.count(proxy_ptr)) {
            return "{/*cycle*/}";
        }
        // Mark as visited so nested printing of the same proxy will be cut off.
        visited.insert(proxy_ptr);

        // Collect visible entries from the Environment
        std::vector<std::pair<std::string, Value>> props;
        props.reserve(obj->proxy_env->values.size());
        for (const auto& kv : obj->proxy_env->values) {
            const std::string& name = kv.first;
            const Environment::Variable& v = kv.second;
            props.emplace_back(name, v.value);
        }

        if (props.empty()) {
            // remove visited mark for tidy correctness (not strictly required)
            visited.erase(proxy_ptr);
            return "{}";
        }

        // Try inline representation if all values are simple and few in number
        bool inline_ok = true;
        if ((int)props.size() > INLINE_MAX_PROPS) inline_ok = false;
        for (const auto& p : props)
            if (!is_simple_value(p.second)) {
                inline_ok = false;
                break;
            }

        if (inline_ok) {
            std::ostringstream oss;
            oss << "{";
            for (size_t i = 0; i < props.size(); ++i) {
                if (i) oss << ", ";
                if (use_color) oss << Color::white;
                oss << props[i].first;
                if (use_color) oss << Color::reset;
                oss << ": " << print_value(props[i].second, indent + 1, visited);
            }
            oss << "}";
            std::string s = oss.str();
            if ((int)s.size() <= INLINE_MAX_LEN) {
                visited.erase(proxy_ptr);
                return s;
            }
            // otherwise fallthrough to expanded form
        }

        // Expanded multi-line representation
        std::ostringstream oss;
        std::string ind(indent, ' ');
        oss << "{\n";
        for (size_t i = 0; i < props.size(); ++i) {
            const auto& key = props[i].first;
            const auto& val = props[i].second;
            oss << ind << "  ";
            if (use_color) oss << Color::white;
            oss << key;
            if (use_color) oss << Color::reset;
            oss << ": ";
            oss << print_value(val, indent + 2, visited);
            if (i + 1 < props.size())
                oss << ",\n";
            else
                oss << "\n";
        }
        oss << ind << "}";

        visited.erase(proxy_ptr);
        return oss.str();
    }
    // ---------- existing object printing below remains (unchanged) ----------
    // Decide inline vs expanded:
    auto should_inline = [&](ObjectPtr o) -> bool {
        if (!o) return true;
        int visible = 0;
        for (const auto& kv : o->properties) {
            if (kv.second.is_private) continue;
            visible++;
            if (visible > INLINE_MAX_PROPS) return false;
            // only simple values allowed inline
            if (!is_simple_value(kv.second.value)) return false;
        }
        return visible > 0;  // inline empty object as {}
    };

    std::function<std::string(ObjectPtr, int)> rec;
    rec = [&](ObjectPtr o, int depth) -> std::string {
        if (!o) return "{}";
        const ObjectValue* p = o.get();
        if (visited.count(p)) return "{/*cycle*/}";
        visited.insert(p);

        // Count visible props and collect them in stable order
        std::vector<std::pair<std::string,
            const PropertyDescriptor*>>
            props;
        props.reserve(o->properties.size());
        std::string class_name;
        auto class_it = o->properties.find("__class__");
        if (class_it != o->properties.end() && std::holds_alternative<ClassPtr>(class_it->second.value)) {
            ClassPtr cp = std::get<ClassPtr>(class_it->second.value);
            if (cp) class_name = cp->name;
        }
        for (const auto& kv : o->properties) {
            // skip internal class link
            if (kv.first == "__class__") continue;

            // keep private hidden
            if (kv.second.is_private) continue;

            // Hide constructor/destructor methods:
            if (!class_name.empty() && std::holds_alternative<FunctionPtr>(kv.second.value)) {
                FunctionPtr f = std::get<FunctionPtr>(kv.second.value);
                if (f && f->name == class_name) continue;
            }

            props.push_back({kv.first, &kv.second});
        }

        if (props.empty()) return "{}";

        bool inline_ok = should_inline(o);
        if (inline_ok) {
            std::ostringstream oss;
            oss << "{";
            for (size_t i = 0; i < props.size(); ++i) {
                if (i) oss << ", ";
                if (use_color) oss << Color::white;
                oss << props[i].first;
                if (use_color) oss << Color::reset;
                oss << ": ";
                oss << print_value(props[i].second->value, depth + 1);
            }
            oss << "}";
            std::string s = oss.str();
            if ((int)s.size() <= INLINE_MAX_LEN) return s;
        }

        std::ostringstream oss;
        std::string ind(depth, ' ');
        oss << "{\n";
        for (size_t i = 0; i < props.size(); ++i) {
            const auto& key = props[i].first;
            const auto& desc = *props[i].second;

            oss << ind << "  ";
            if (use_color) oss << Color::white;
            oss << key;
            if (use_color) oss << Color::reset;
            oss << ": ";

            if (std::holds_alternative<FunctionPtr>(desc.value)) {
                FunctionPtr f = std::get<FunctionPtr>(desc.value);
                std::string nm = f->name.empty() ? "<lambda>" : f->name;

                std::ostringstream label;

                if (desc.is_readonly) {
                    label << "[getter]";
                    if (use_color) oss << Color::bright_magenta;
                } else {
                    label << "[" << (f->is_async ? "Async->" : "") << "tabia " << nm << "]";
                    if (use_color) oss << Color::bright_cyan;
                }

                oss << label.str();
                if (use_color) oss << Color::reset;

            } else {
                oss << print_value(desc.value, depth + 2, visited);
            }

            if (i + 1 < props.size())
                oss << ",\n";
            else
                oss << "\n";
        }
        oss << ind << "}";
        return oss.str();
    };

    return rec(obj, indent);
}