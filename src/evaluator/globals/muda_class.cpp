#include "muda_class.hpp"
#include "muda_time_utils.hpp"
#include "ClassRuntime.hpp"
#include <iomanip>
#include <ctime>
#include <memory>
#include <sstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>

// NOTE: This file implements the Muda runtime class and native helpers.
// The design goal:
// - 'Muda' available as a low-level helper function (Muda(...)) and as a class (unda Muda(...)).
// - Class constructor supports many signatures (no args, ms, ISO/string+format, components).
// - Instances are immutable: mutators return new Muda instances (setiMuda returns a new object).
// - Methods forward to native_* helpers for computation; forwarders are created as AST ClassMethodNodes
//   whose bodies call the corresponding Muda_native_* function (so parameter handling uses ParameterNode descriptors).

// Small helper: instantiate a runtime object for a ClassPtr with given ms
static ObjectPtr instantiate_muda_from_class(ClassPtr cls, double ms, EnvPtr env) {
    if (!cls) return nullptr;
    auto instance = std::make_shared<ObjectValue>();

    PropertyDescriptor classLink;
    classLink.value = cls;
    classLink.is_private = true;
    instance->properties["__class__"] = classLink;

    PropertyDescriptor msdesc;
    msdesc.value = ms;
    msdesc.is_locked = true;
    msdesc.is_private = false;
    instance->properties["__ms__"] = msdesc;

    // build instance methods (clone param descriptors)
    std::vector<ClassPtr> chain;
    for (ClassPtr walk = cls; walk; walk = walk->super) chain.push_back(walk);
    std::reverse(chain.begin(), chain.end());

    for (auto &c: chain) {
        if (!c || !c->body) continue;
        for (auto &m: c->body->methods) {
            if (!m) continue;
            if (m->is_static) continue;

            auto persisted = std::make_shared<FunctionDeclarationNode>();
            persisted->name = m->name;
            persisted->token = m->token;
            persisted->parameters.reserve(m->params.size());
            for (const auto &pp : m->params) {
                if (pp) persisted->parameters.push_back(pp->clone());
                else persisted->parameters.push_back(nullptr);
            }
            persisted->body.reserve(m->body.size());
            for (const auto &s: m->body) persisted->body.push_back(s ? s->clone() : nullptr);

            EnvPtr methodClosure = std::make_shared<Environment>(env);
            Environment::Variable thisVar; thisVar.value = instance; thisVar.is_constant = true;
            methodClosure->set("$", thisVar);

            auto fn = std::make_shared<FunctionValue>(persisted->name, persisted->parameters, persisted, methodClosure, persisted->token);

            PropertyDescriptor pd;
            pd.value = fn;
            pd.is_private = false;
            pd.is_locked = m->is_locked;
            pd.is_readonly = m->is_getter;
            pd.token = m->token;
            instance->properties[m->name] = std::move(pd);
        }
    }

    return instance;
}

// Helper to extract __ms__ from a Muda instance (arg position or this)
static double recv_ms_from_args(const std::vector<Value>& args, size_t idx, const Token& tok) {
    if (idx >= args.size()) throw std::runtime_error("Missing argument at " + tok.loc.to_string());
    if (!std::holds_alternative<ObjectPtr>(args[idx])) throw std::runtime_error("Expected Muda object at " + tok.loc.to_string());
    ObjectPtr o = std::get<ObjectPtr>(args[idx]);
    auto it = o->properties.find("__ms__");
    if (it == o->properties.end() || !std::holds_alternative<double>(it->second.value)) throw std::runtime_error("Muda object missing __ms__");
    return std::get<double>(it->second.value);
}

// Native low-level helpers and forwarders

static Value native_NOW_MS(const std::vector<Value>&, EnvPtr, const Token&) {
    return epoch_ms_now();
}

// getters / methods that operate on "this"
static Value native_muda_mwaka(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    double ms = recv_ms_from_args(args, 0, tok);
    std::tm t = tm_from_ms(ms);
    return static_cast<double>(1900 + t.tm_year);
}
static Value native_muda_mwezi(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    double ms = recv_ms_from_args(args, 0, tok);
    std::tm t = tm_from_ms(ms);
    return static_cast<double>(t.tm_mon + 1);
}
static Value native_muda_tarehe(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    double ms = recv_ms_from_args(args, 0, tok);
    std::tm t = tm_from_ms(ms);
    return static_cast<double>(t.tm_mday);
}
static Value native_muda_sikuYaJuma(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    double ms = recv_ms_from_args(args, 0, tok);
    std::string fmt;
    if (args.size() >= 2 && std::holds_alternative<std::string>(args[1])) fmt = std::get<std::string>(args[1]);
    std::tm t = tm_from_ms(ms);
    if (fmt == "ddd") { std::ostringstream s; s << std::put_time(&t, "%a"); return s.str(); }
    if (fmt == "dddd") { std::ostringstream s; s << std::put_time(&t, "%A"); return s.str(); }
    return static_cast<double>(t.tm_wday);
}
static Value native_muda_saa(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    double ms = recv_ms_from_args(args, 0, tok);
    std::string fmt;
    if (args.size() >= 2 && std::holds_alternative<std::string>(args[1])) fmt = std::get<std::string>(args[1]);
    std::tm t = tm_from_ms(ms);
    if (fmt == "h") { int h = t.tm_hour % 12; if (h == 0) h = 12; return std::to_string(h); }
    if (fmt == "hh") { int h = t.tm_hour % 12; if (h == 0) h = 12; std::ostringstream s; s << std::setw(2) << std::setfill('0') << h; return s.str(); }
    if (fmt == "H") return static_cast<double>(t.tm_hour);
    if (fmt == "HH") { std::ostringstream s; s << std::setw(2) << std::setfill('0') << t.tm_hour; return s.str(); }
    return static_cast<double>(t.tm_hour);
}
static Value native_muda_dakika(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    double ms = recv_ms_from_args(args, 0, tok);
    std::tm t = tm_from_ms(ms);
    return static_cast<double>(t.tm_min);
}
static Value native_muda_sekunde(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    double ms = recv_ms_from_args(args, 0, tok);
    std::tm t = tm_from_ms(ms);
    return static_cast<double>(t.tm_sec);
}
static Value native_muda_millis(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    double ms = recv_ms_from_args(args, 0, tok);
    long long msi = static_cast<long long>(std::llround(ms));
    return static_cast<double>(msi % 1000);
}
static Value native_muda_zone(const std::vector<Value>&, EnvPtr, const Token&) {
    return std::string("UTC");
}
static Value native_muda_ms(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    double ms = recv_ms_from_args(args, 0, tok);
    return ms;
}
static Value native_muda_fmt(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 2) throw std::runtime_error("fmt requires format string");
    double ms = recv_ms_from_args(args, 0, tok);
    if (!std::holds_alternative<std::string>(args[1])) throw std::runtime_error("fmt expects string format");
    std::string fmt = std::get<std::string>(args[1]);
    std::string zone = "UTC";
    if (args.size() >= 3 && std::holds_alternative<std::string>(args[2])) zone = std::get<std::string>(args[2]);
    return format_time_from_ms(ms, fmt, zone);
}
static Value native_muda_iso(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    double ms = recv_ms_from_args(args, 0, tok);
    return format_time_from_ms(ms, "YYYY-MM-DD H:mm:ss", "UTC") + "Z";
}
static Value native_muda_object(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    double ms = recv_ms_from_args(args, 0, tok);
    std::tm t = tm_from_ms(ms);
    auto obj = std::make_shared<ObjectValue>();
    auto add_number = [&](const std::string& k, double v) {
        PropertyDescriptor pd; pd.value = v; pd.is_private = false; pd.is_readonly = false; obj->properties[k] = pd;
    };
    add_number("mwaka", static_cast<double>(1900 + t.tm_year));
    add_number("mwezi", static_cast<double>(t.tm_mon + 1));
    add_number("tarehe", static_cast<double>(t.tm_mday));
    add_number("sikuYaJuma", static_cast<double>(t.tm_wday));
    add_number("saa", static_cast<double>(t.tm_hour));
    add_number("dakika", static_cast<double>(t.tm_min));
    add_number("sekunde", static_cast<double>(t.tm_sec));
    long long msll = static_cast<long long>(std::llround(ms)); add_number("millis", static_cast<double>(msll % 1000));
    return obj;
}
static Value native_muda_eq(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 2) throw std::runtime_error("eq requires one argument");
    double a = recv_ms_from_args(args, 0, tok);
    double b = recv_ms_from_args(args, 1, tok);
    return a == b;
}
static Value native_muda_gt(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 2) throw std::runtime_error("gt requires one argument");
    double a = recv_ms_from_args(args, 0, tok);
    double b = recv_ms_from_args(args, 1, tok);
    return a > b;
}
static Value native_muda_lt(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 2) throw std::runtime_error("lt requires one argument");
    double a = recv_ms_from_args(args, 0, tok);
    double b = recv_ms_from_args(args, 1, tok);
    return a < b;
}
static Value native_muda_diff(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 2) throw std::runtime_error("diff requires two arguments");
    double a = recv_ms_from_args(args, 0, tok);
    double b = recv_ms_from_args(args, 1, tok);
    if (args.size() >= 3 && std::holds_alternative<std::string>(args[2])) {
        std::string unit = std::get<std::string>(args[2]);
        double diff = a - b;
        if (unit == "days" || unit == "siku") return diff / (1000.0*60.0*60.0*24.0);
        if (unit == "hours" || unit == "masaa") return diff / (1000.0*60.0*60.0);
        if (unit == "minutes" || unit == "dakika") return diff / (1000.0*60.0);
    }
    return a - b;
}

// add/sub (produce new instance)
static Value native_muda_ongeza(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    if (args.size() < 3) throw std::runtime_error("ongeza expects (this, unit, amount)");
    double orig = recv_ms_from_args(args, 0, tok);
    if (!std::holds_alternative<std::string>(args[1])) throw std::runtime_error("unit must be string");
    std::string unit = std::get<std::string>(args[1]);
    double amt = 0.0;
    if (std::holds_alternative<double>(args[2])) amt = std::get<double>(args[2]);
    else if (std::holds_alternative<std::string>(args[2])) amt = std::stod(std::get<std::string>(args[2]));
    else throw std::runtime_error("amount must be numeric");

    double new_ms = orig;
    if (unit == "sekunde" || unit == "s") new_ms += amt * 1000.0;
    else if (unit == "dakika" || unit == "dk" || unit == "m") new_ms += amt * 60.0 * 1000.0;
    else if (unit == "saa" || unit == "masaa" || unit == "h") new_ms += amt * 60.0 * 60.0 * 1000.0;
    else if (unit == "siku" || unit == "d") new_ms += amt * 24.0 * 60.0 * 60.0 * 1000.0;
    else if (unit == "wiki") new_ms += amt * 7.0 * 24.0 * 60.0 * 60.0 * 1000.0;
    else if (unit == "mwezi" || unit == "miezi" || unit == "M") {
        std::tm t = tm_from_ms(orig);
        int mon = t.tm_mon + static_cast<int>(std::llround(amt));
        int year_add = mon / 12;
        mon = mon % 12;
        if (mon < 0) { mon += 12; year_add -= 1; }
        t.tm_mon = mon;
        t.tm_year += year_add;
#if defined(_WIN32)
        time_t tt = _mkgmtime(&t);
#else
        time_t tt = timegm(&t);
#endif
        new_ms = static_cast<double>(static_cast<long long>(tt) * 1000LL);
    }
    else if (unit == "mwaka" || unit == "miaka" || unit == "y") {
        std::tm t = tm_from_ms(orig);
        t.tm_year += static_cast<int>(std::llround(amt));
#if defined(_WIN32)
        time_t tt = _mkgmtime(&t);
#else
        time_t tt = timegm(&t);
#endif
        new_ms = static_cast<double>(static_cast<long long>(tt) * 1000LL);
    } else {
        throw std::runtime_error("Unknown unit for ongeza: " + unit);
    }

    EnvPtr walk = env;
    ClassPtr cls = nullptr;
    while (walk) {
        if (walk->has("Muda")) {
            auto var = walk->get("Muda").value;
            if (std::holds_alternative<ClassPtr>(var)) cls = std::get<ClassPtr>(var);
            break;
        }
        walk = walk->parent;
    }
    if (!cls) throw std::runtime_error("Muda class not found when creating new instance");

    return instantiate_muda_from_class(cls, new_ms, env);
}

static Value native_muda_punguza(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    if (args.size() < 3) throw std::runtime_error("punguza expects (this, unit, amount)");
    std::vector<Value> n = args;
    if (std::holds_alternative<double>(n[2])) n[2] = std::get<double>(n[2]) * -1.0;
    else if (std::holds_alternative<std::string>(n[2])) n[2] = std::string(std::to_string(-std::stod(std::get<std::string>(n[2]))));
    else throw std::runtime_error("amount must be numeric");
    return native_muda_ongeza(n, env, tok);
}

// Low-level function-style helper Muda(...)
static Value native_Muda_lowlevel(const std::vector<Value>& args, EnvPtr /*env*/, const Token& /*tok*/) {
    double now_ms = epoch_ms_now();
    if (args.empty()) return now_ms;

    if (args.size() == 1 && std::holds_alternative<std::string>(args[0])) {
        std::string s = std::get<std::string>(args[0]);
        if (s == "ms") return now_ms;
        return format_time_from_ms(now_ms, s, "UTC");
    }

    if (std::holds_alternative<double>(args[0])) {
        double ms = std::get<double>(args[0]);
        if (args.size() >= 2 && std::holds_alternative<std::string>(args[1])) {
            std::string fmt = std::get<std::string>(args[1]);
            return format_time_from_ms(ms, fmt, "UTC");
        }
        return ms;
    }

    if (std::holds_alternative<std::string>(args[0])) {
        std::string s = std::get<std::string>(args[0]);
        if (args.size() >= 2 && std::holds_alternative<std::string>(args[1])) {
            double parsed_ms = parse_date_string_with_format_local(s, std::get<std::string>(args[1]));
            if (args.size() >= 3 && std::holds_alternative<std::string>(args[2])) {
                return format_time_from_ms(parsed_ms, std::get<std::string>(args[2]), "UTC");
            }
            return parsed_ms;
        }
        return parse_iso_like_local(s);
    }
    throw std::runtime_error("Invalid arguments to Muda()");
}

// Constructor native: accepts many signatures
static Value native_Muda_ctor(const std::vector<Value>& args, EnvPtr /*env*/, const Token& tok) {
    if (args.empty()) return epoch_ms_now();

    // numeric components form (year,month,day,[H,m,s])
    bool components = std::holds_alternative<double>(args[0]) && args.size() > 1;
    if (components) {
        std::vector<int> comp;
        for (size_t i=0;i<args.size() && i<7;i++) {
            if (!std::holds_alternative<double>(args[i])) { components = false; break; }
            comp.push_back(static_cast<int>(std::llround(std::get<double>(args[i]))));
        }
        if (components && comp.size() >= 2) {
            int year = comp[0];
            int mon = (comp.size() >= 2 ? comp[1] : 1);
            int day = (comp.size() >= 3 ? comp[2] : 1);
            int hour = (comp.size() >= 4 ? comp[3] : 0);
            int minute = (comp.size() >= 5 ? comp[4] : 0);
            int second = (comp.size() >= 6 ? comp[5] : 0);
            std::tm tm = {};
            tm.tm_year = year - 1900;
            tm.tm_mon = mon - 1;
            tm.tm_mday = day;
            tm.tm_hour = hour;
            tm.tm_min = minute;
            tm.tm_sec = second;
#if defined(_WIN32)
            time_t tt = _mkgmtime(&tm);
#else
            time_t tt = timegm(&tm);
#endif
            return static_cast<double>(static_cast<long long>(tt) * 1000LL);
        }
    }

    // single numeric -> epoch ms
    if (std::holds_alternative<double>(args[0]) && args.size() == 1) {
        return std::get<double>(args[0]);
    }

    // string parse
    if (std::holds_alternative<std::string>(args[0])) {
        std::string s = std::get<std::string>(args[0]);
        if (args.size() >= 2 && std::holds_alternative<std::string>(args[1])) {
            return parse_date_string_with_format_local(s, std::get<std::string>(args[1]));
        }
        return parse_iso_like_local(s);
    }
    throw std::runtime_error("Invalid constructor arguments for Muda at " + tok.loc.to_string());
}

// setiMuda(this, field, value)
static Value native_muda_seti(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    if (args.size() < 3) throw std::runtime_error("setiMuda expects (this, field, value)");
    if (!std::holds_alternative<ObjectPtr>(args[0])) throw std::runtime_error("setiMuda: first arg must be Muda object");
    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    auto it = obj->properties.find("__ms__");
    if (it == obj->properties.end() || !std::holds_alternative<double>(it->second.value)) throw std::runtime_error("Muda object missing __ms__");
    double ms = std::get<double>(it->second.value);
    std::tm tm = tm_from_ms(ms);

    if (!std::holds_alternative<std::string>(args[1])) throw std::runtime_error("setiMuda field must be string");
    std::string field = std::get<std::string>(args[1]);

    if (field == "saa") {
        int v = static_cast<int>(std::llround(std::holds_alternative<double>(args[2]) ? std::get<double>(args[2]) : 0.0));
        tm.tm_hour = v;
    } else if (field == "dakika") {
        int v = static_cast<int>(std::llround(std::holds_alternative<double>(args[2]) ? std::get<double>(args[2]) : 0.0));
        tm.tm_min = v;
    } else if (field == "sekunde") {
        int v = static_cast<int>(std::llround(std::holds_alternative<double>(args[2]) ? std::get<double>(args[2]) : 0.0));
        tm.tm_sec = v;
    } else if (field == "tarehe" || field == "siku") {
        int v = static_cast<int>(std::llround(std::holds_alternative<double>(args[2]) ? std::get<double>(args[2]) : 1.0));
        tm.tm_mday = v;
    } else if (field == "mwezi") {
        int v = static_cast<int>(std::llround(std::holds_alternative<double>(args[2]) ? std::get<double>(args[2]) : 1.0));
        tm.tm_mon = v - 1;
    } else if (field == "mwaka") {
        int v = static_cast<int>(std::llround(std::holds_alternative<double>(args[2]) ? std::get<double>(args[2]) : 1900.0));
        tm.tm_year = v - 1900;
    } else if (field == "ms") {
        double v = value_to_ms_or_throw(args[2]);
        EnvPtr walk = env;
        ClassPtr cls = nullptr;
        while (walk) {
            if (walk->has("Muda")) {
                auto var = walk->get("Muda").value;
                if (std::holds_alternative<ClassPtr>(var)) cls = std::get<ClassPtr>(var);
                break;
            }
            walk = walk->parent;
        }
        if (!cls) throw std::runtime_error("Muda class not found when creating new instance");
        return instantiate_muda_from_class(cls, v, env);
    } else {
        throw std::runtime_error("Unsupported setiMuda field: " + field);
    }

#if defined(_WIN32)
    time_t tt = _mkgmtime(&tm);
#else
    time_t tt = timegm(&tm);
#endif
    double new_ms = static_cast<double>(static_cast<long long>(tt) * 1000LL);

    EnvPtr walk = env;
    ClassPtr cls = nullptr;
    while (walk) {
        if (walk->has("Muda")) {
            auto var = walk->get("Muda").value;
            if (std::holds_alternative<ClassPtr>(var)) cls = std::get<ClassPtr>(var);
            break;
        }
        walk = walk->parent;
    }
    if (!cls) throw std::runtime_error("Muda class not found when creating new instance");

    return instantiate_muda_from_class(cls, new_ms, env);
}

// ---------------------------
// init_muda_class
// ---------------------------
void init_muda_class(EnvPtr env) {
    if (!env) return;

    auto add_native = [&](const std::string &name, std::function<Value(const std::vector<Value>&, EnvPtr, const Token&)> fn) {
        auto fv = std::make_shared<FunctionValue>(name, fn, env, Token{});
        Environment::Variable v{fv, true};
        env->set(name, v);
    };

    // Low-level helper and native helpers
    add_native("Muda", native_Muda_lowlevel);
    add_native("NOW_MS", native_NOW_MS);

    add_native("Muda_native_ctor", native_Muda_ctor);
    add_native("Muda_native_mwaka", native_muda_mwaka);
    add_native("Muda_native_mwezi", native_muda_mwezi);
    add_native("Muda_native_tarehe", native_muda_tarehe);
    add_native("Muda_native_sikuYaJuma", native_muda_sikuYaJuma);
    add_native("Muda_native_saa", native_muda_saa);
    add_native("Muda_native_dakika", native_muda_dakika);
    add_native("Muda_native_sekunde", native_muda_sekunde);
    add_native("Muda_native_millis", native_muda_millis);
    add_native("Muda_native_zone", [](const std::vector<Value>&, EnvPtr, const Token&){ return std::string("UTC"); });
    add_native("Muda_native_ms", native_muda_ms);
    add_native("Muda_native_fmt", native_muda_fmt);
    add_native("Muda_native_iso", native_muda_iso);
    add_native("Muda_native_object", native_muda_object);
    add_native("Muda_native_eq", native_muda_eq);
    add_native("Muda_native_gt", native_muda_gt);
    add_native("Muda_native_lt", native_muda_lt);
    add_native("Muda_native_diff", native_muda_diff);
    add_native("Muda_native_ongeza", native_muda_ongeza);
    add_native("Muda_native_punguza", native_muda_punguza);
    add_native("Muda_native_seti", native_muda_seti);
    add_native("Muda_native_setiMuda", native_muda_seti);

    // Build runtime ClassValue for Muda
    auto classDesc = std::make_shared<ClassValue>();
    classDesc->name = "Muda";
    classDesc->token = Token{};
    classDesc->body = std::make_unique<ClassBodyNode>();

    // property __ms__
    {
        auto p = std::make_unique<ClassPropertyNode>();
        p->name = "__ms__";
        p->is_locked = true;
        p->is_private = false;
        classDesc->body->properties.push_back(std::move(p));
    }

    // constructor: this.__ms__ = Muda_native_ctor(...args)
    {
        auto ctor = std::make_unique<ClassMethodNode>();
        ctor->name = classDesc->name;
        ctor->is_constructor = true;
        ctor->is_locked = true;
        ctor->is_private = false;

        // rest parameter ...args
        auto p = std::make_unique<ParameterNode>();
        p->token = Token{};
        p->name = "args";
        p->is_rest = true;
        p->rest_required_count = 0;
        p->defaultValue = nullptr;
        ctor->params.push_back(std::move(p));

        auto assign = std::make_unique<AssignmentNode>();
        auto target = std::make_unique<MemberExpressionNode>();
        target->object = std::make_unique<ThisExpressionNode>();
        target->property = "__ms__";
        assign->target = std::move(target);

        auto call = std::make_unique<CallExpressionNode>();
        call->callee = std::make_unique<IdentifierNode>();
        static_cast<IdentifierNode*>(call->callee.get())->name = "Muda_native_ctor";

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
    auto add_forwarder = [&](const std::string &name, const std::vector<std::string> &params = {}) {
        auto m = std::make_unique<ClassMethodNode>();
        m->name = name;
        m->is_locked = true;
        m->is_private = false;

        for (const auto &pn : params) {
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
        static_cast<IdentifierNode*>(call->callee.get())->name = std::string("Muda_native_") + name;

        call->arguments.push_back(std::make_unique<ThisExpressionNode>());
        for (const auto &p : params) {
            auto id = std::make_unique<IdentifierNode>();
            id->name = p;
            call->arguments.push_back(std::move(id));
        }

        auto ret = std::make_unique<ReturnStatementNode>();
        ret->value = std::move(call);
        m->body.push_back(std::move(ret));
        classDesc->body->methods.push_back(std::move(m));
    };

    add_forwarder("mwaka");
    add_forwarder("mwezi");
    add_forwarder("tarehe");
    add_forwarder("sikuYaJuma", {"fmt"});
    add_forwarder("saa", {"fmt"});
    add_forwarder("dakika");
    add_forwarder("sekunde");
    add_forwarder("millis");
    add_forwarder("zone");
    add_forwarder("fmt", {"fmt", "zone"});
    add_forwarder("ms");
    add_forwarder("iso");
    add_forwarder("object");
    add_forwarder("eq", {"other"});
    add_forwarder("gt", {"other"});
    add_forwarder("lt", {"other"});
    add_forwarder("diff", {"other", "unit"});
    add_forwarder("ongeza", {"unit", "amount"});
    add_forwarder("punguza", {"unit", "amount"});
    add_forwarder("setiMuda", {"field", "value"});

    // __muda__ getter -> this.iso()
    {
        auto g = std::make_unique<ClassMethodNode>();
        g->name = "__muda__";
        g->is_getter = true;
        g->is_locked = true;
        g->is_private = false;

        auto call = std::make_unique<CallExpressionNode>();
        auto mem = std::make_unique<MemberExpressionNode>();
        mem->object = std::make_unique<ThisExpressionNode>();
        mem->property = "iso";
        call->callee = std::move(mem);

        auto ret = std::make_unique<ReturnStatementNode>();
        ret->value = std::move(call);
        g->body.push_back(std::move(ret));
        classDesc->body->methods.push_back(std::move(g));
    }

    Environment::Variable var;
    var.value = classDesc;
    var.is_constant = true;
    env->set(classDesc->name, var);
}