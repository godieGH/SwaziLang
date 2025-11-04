#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include "SwaziError.hpp"
#include "ClassRuntime.hpp"
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
                    "\n --> Traced at:\n" + token.loc.get_line_trace()
                );
            }

            // if only part of the string was parsed
            if (idx != s.size()) {
                throw std::runtime_error(
                    "ValueError at " + token.loc.to_string() +
                    "\nCannot convert string '" + s + "' to number: contains invalid characters after position " + std::to_string(idx) +
                    "\n --> Traced at:\n" + token.loc.get_line_trace()
                );
            }

            return d;
        } catch (const std::exception& e) {
            throw std::runtime_error(
                "ValueError at " + token.loc.to_string() +
                "\nCannot convert string '" + s + "' to number(" + e.what() + ")" +
                "\n --> Traced at:\n" + token.loc.get_line_trace()
            );
        }
    }

    // Arrays, functions, or other unsupported types
    throw std::runtime_error(
        "TypeError at " + token.loc.to_string() +
        "\nCannot convert value of type `" + value_type_name(v) + "` to a number" +
        "\n --> Traced at:\n" + token.loc.get_line_trace()
    );
}

std::string Evaluator::to_string_value(const Value& v, bool no_color) {
    static bool use_color = supports_color();
    if(no_color) use_color = false;
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
    if (std::holds_alternative<FunctionPtr>(v)) {
        FunctionPtr fn = std::get<FunctionPtr>(v);
        std::string name = fn->name.empty() ? "<lambda>" : fn->name;
        std::string s = use_color ? (Color::bright_cyan + "[kazi " + name + "]" + Color::reset): "[kazi " + name + "]";
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

        // label only
        std::string label = "[muundo " + cp->name + "]";

        std::string out;
        if (use_color)
            out = Color::bright_blue + label + Color::reset;
        else
            out = label;

        // Render static table inline if it has visible properties (print_object already adds its own coloring)
        if (cp->static_table) {
            std::string static_repr = print_object(cp->static_table, 0, std::unordered_set<const ObjectValue*>{});
            if (!static_repr.empty() && static_repr != "{}") {
                out += " " + static_repr;
            }
        }

        return out;
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

Value Evaluator::get_object_property(ObjectPtr obj, const std::string& key, EnvPtr env) {
    if (!obj) return std::monostate{};

    auto it = obj->properties.find(key);
    if (it == obj->properties.end()) {
        // missing property -> undefined (or throw if you prefer)
        return std::monostate{};
    }

    const PropertyDescriptor& desc = it->second;

    // private property -> not allowed unless we are inside the same object's context ($ bound)
    if (desc.is_private && !is_private_access_allowed(obj, env)) {
        throw std::runtime_error(
            "PermissionError at " + desc.token.loc.to_string() +
            "\nCannot access private property '" + key + "' from outside the owning object." +
            "\n --> Traced at:\n" + desc.token.loc.get_line_trace()
        );
    }

    // If property is a readonly getter and value is a function -> call it and return its result
    if (desc.is_readonly && std::holds_alternative<FunctionPtr>(desc.value)) {
        FunctionPtr getter = std::get<FunctionPtr>(desc.value);
        // Call with zero args, preserving token for error messages
        return call_function(getter, {}, desc.token);
    }

    // normal return
    return desc.value;
}

void Evaluator::set_object_property(ObjectPtr obj, const std::string& key, const Value& val, EnvPtr env, const Token& assignToken) {
    if (!obj) throw std::runtime_error(
        "TypeError at " + assignToken.loc.to_string() +
        "\nAttempt to assign a property on a undefined object." +
        "\n --> Traced at:\n" + assignToken.loc.get_line_trace()
    );

    if (obj->is_frozen) return;

    auto it = obj->properties.find(key);
    if (it != obj->properties.end()) {
        PropertyDescriptor& desc = it->second;

        // cannot assign to private from outside
        if (desc.is_private && !is_private_access_allowed(obj, env)) {
            throw std::runtime_error(
                "PermissionError at " + assignToken.loc.to_string() +
                "\nCannot assign to private property '" + key + "' from outside the owning object." +
                "\n --> Traced at:\n" + assignToken.loc.get_line_trace()
            );
        }

        // cannot assign to readonly / getter-backed property
        if (desc.is_readonly) {
            throw std::runtime_error(
                "TypeError at " + assignToken.loc.to_string() +
                "\nCannot assign to readonly property '" + key + "'." +
                "\n --> Traced at:\n" + assignToken.loc.get_line_trace()
            );
        }

        // locked: only allow internal code (same-$ or class-internal) to overwrite
        if (desc.is_locked) {
            bool isInternal = is_private_access_allowed(obj, env);
            if (!isInternal) {
                throw std::runtime_error(
                    "PermissionError at " + assignToken.loc.to_string() +
                    "\nCannot overwrite locked property '" + key + "' from outside the owning object." +
                    "\n --> Traced at:\n" + assignToken.loc.get_line_trace()
                );
            }
        }

        desc.value = val;
        // keep private/readonly/locked flags & token as-is
        return;
    }

    // property didn't exist - create it
    PropertyDescriptor desc;
    desc.value = val;
    desc.is_private = false;
    desc.is_readonly = false;
    desc.token = assignToken;
    obj->properties[key] = std::move(desc);
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
                declToken.loc
            );
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
                        spread->token.loc
                    );
                }
                auto idTarget = dynamic_cast<IdentifierNode*>(spread->argument.get());
                if (!idTarget) {
                    throw SwaziError(
                        "SyntaxError",
                        "Only an identifier is allowed as the rest target in an array pattern.",
                        spread->token.loc
                    );
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
                arrPat->token.loc
            );
        }
        return;
    }

    // Object pattern
    if (auto objPat = dynamic_cast<ObjectPatternNode*>(pattern)) {
        if (!std::holds_alternative<ObjectPtr>(value)) {
            throw SwaziError(
                "TypeError",
                "Cannot destructure a non-object value.",
                declToken.loc
            );
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
                    propPtr->value->token.loc
                );
            }

            // Use existing getter to respect privacy/getters (get_object_property is a member)
            Value v = std::monostate{};
            if (src) {
                try {
                    v = get_object_property(src, key, env);
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
        pattern->token.loc
    );
}
// Tune these to control "small object" inline behavior:
static constexpr int INLINE_MAX_PROPS = 5;
static constexpr int INLINE_MAX_LEN = 150;

// Helper: are we a primitive-ish value that can be inlined simply?
static bool is_simple_value(const Value& v) {
    return std::holds_alternative<std::monostate>(v) ||
        std::holds_alternative<double>(v) ||
        std::holds_alternative<std::string>(v) ||
        std::holds_alternative<bool>(v);
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
        ss << "[kazi " << nm << "]";
        return use_color ? (Color::bright_cyan + ss.str() + Color::reset) : ss.str();
    }

    if (std::holds_alternative<ArrayPtr>(v)) {
        ArrayPtr arr = std::get<ArrayPtr>(v);
        if (!arr) return "[]";
        
        const ArrayValue* p = arr.get();
        if (arrvisited.count(p)) return "[/*cycle*/]";
        arrvisited.insert(p);
        
        // try compact inline if small and elements simple
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
                ss << print_value(arr->elements[i], depth + 1, visited, arrvisited);
            }
            ss << "]";
            return ss.str();
        } else {
            // multi-line array
            ss << "[\n";
            std::string ind(depth + 2, ' ');
            for (size_t i = 0; i < arr->elements.size(); ++i) {
                ss << ind << print_value(arr->elements[i], depth + 2, visited, arrvisited);
                if (i + 1 < arr->elements.size()) ss << ",\n";
            }
            ss << "\n"
               << std::string(depth, ' ') << "]";
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

        // append static table representation (print_object handles its own coloring and hides private props)
        if (cp->static_table) {
            std::string static_repr = print_object(cp->static_table, 0, std::unordered_set<const ObjectValue*>{});
            if (!static_repr.empty() && static_repr != "{}") {
                out += " " + static_repr;
            }
        }

        return out;
    }

    return "<?>";  // fallback
}

std::string Evaluator::print_object(
    ObjectPtr obj,
    int indent,
    std::unordered_set<const ObjectValue*> visited) {
    if (!obj) return "{}";

    bool use_color = supports_color();
    // std::unordered_set<const ObjectValue*> visited;

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
            // If this property is a function and its name matches the instance's class name,
            // skip it from the printed property list.
            if (!class_name.empty() && std::holds_alternative<FunctionPtr>(kv.second.value)) {
                FunctionPtr f = std::get<FunctionPtr>(kv.second.value);
                if (f && f->name == class_name) continue;
            }

            props.push_back({kv.first, &kv.second});
        }

        if (props.empty()) return "{}";

        bool inline_ok = should_inline(o);
        if (inline_ok) {
            // Build inline: {a: 1, b: "x"}
            std::ostringstream oss;
            oss << "{";
            for (size_t i = 0; i < props.size(); ++i) {
                if (i) oss << ", ";
                // key in white
                if (use_color) oss << Color::white;
                oss << props[i].first;
                if (use_color) oss << Color::reset;
                oss << ": ";
                oss << print_value(props[i].second->value, depth + 1);
            }
            oss << "}";
            std::string s = oss.str();
            if ((int)s.size() <= INLINE_MAX_LEN) return s;
            // otherwise fallthrough to expanded representation
        }

        // Expanded multi-line representation
        std::ostringstream oss;
        std::string ind(depth, ' ');
        oss << "{\n";
        for (size_t i = 0; i < props.size(); ++i) {
            const auto& key = props[i].first;
            const auto& desc = *props[i].second;

            oss << ind << "  ";
            // key color (white)
            if (use_color) oss << Color::white;
            oss << key;
            if (use_color) oss << Color::reset;
            oss << ": ";

            // Functions as short label
            if (std::holds_alternative<FunctionPtr>(desc.value)) {
                FunctionPtr f = std::get<FunctionPtr>(desc.value);
                std::string nm = f->name.empty() ? "<lambda>" : f->name;

                std::ostringstream label;

                if (desc.is_readonly) {
                    // getter: only show [getter]
                    label << "[getter]";
                    if (use_color) oss << Color::bright_magenta;
                } else {
                    // normal function: show [tabia name]
                    label << "[tabia " << nm << "]";
                    if (use_color) oss << Color::bright_cyan;
                }

                oss << label.str();
                if (use_color) oss << Color::reset;

            } else {
                // recurse for other values (arrays/objects/primitive)
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