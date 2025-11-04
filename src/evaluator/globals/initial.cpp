#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <numeric>
#include <random>

#include "evaluator.hpp"
#include "globals.hpp"
#include "muda_class.hpp"
#include "set_class.hpp"
#include "time.hpp"

static Value builtin_ainaya(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    if (args.empty()) {
        return std::string("unknown");
    }

    const Value& v = args[0];

    if (std::holds_alternative<double>(v)) return std::string("namba");
    if (std::holds_alternative<std::monostate>(v)) return std::string("null");
    if (std::holds_alternative<bool>(v)) return std::string("bool");
    if (std::holds_alternative<std::string>(v)) return std::string("neno");
    if (std::holds_alternative<ObjectPtr>(v)) return std::string("object");
    if (std::holds_alternative<ClassPtr>(v)) return std::string("muundo");
    if (std::holds_alternative<ArrayPtr>(v)) return std::string("orodha");
    if (std::holds_alternative<FunctionPtr>(v)) return std::string("kazi");

    return std::string("unknown");
}
static Value builtin_orodha(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    auto arr = std::make_shared<ArrayValue>();

    if (args.empty()) {
        return arr;
    }

    if (args.size() == 1) {
        const Value& first = args[0];

        if (std::holds_alternative<double>(first)) {
            // case: Orodha(5) -> array of length 5, filled with empty values
            int len = static_cast<int>(std::get<double>(first));
            if (len < 0) len = 0;
            arr->elements.resize(len);  // default-constructed Values (monostate)
            return arr;
        }

        if (std::holds_alternative<ArrayPtr>(first)) {
            // case: Orodha([1,2,3]) -> copy
            ArrayPtr src = std::get<ArrayPtr>(first);
            arr->elements = src->elements;
            return arr;
        }
    }

    // default case: Orodha(6,8,5,8) or any other list of arguments
    arr->elements = args;
    return arr;
}
static Value builtin_bool(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    return args.empty() ? false : value_to_bool(args[0]);
}

static Value builtin_ingiza(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    std::string prompt = args.empty() ? "" : value_to_string(args[0]);
    if (!prompt.empty()) std::cout << prompt;
    std::string input;
    std::getline(std::cin, input);
    return input;  // return as string
}

static Value builtin_namba(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    return args.empty() ? 0.0 : value_to_number(args[0]);
}

static Value builtin_neno(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    return args.empty() ? std::string("") : value_to_string(args[0]);
}

static Value builtin_object_keys(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    auto arr = std::make_shared<ArrayValue>();
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) return arr;

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    for (auto& pair : obj->properties) {
        arr->elements.push_back(pair.first);  // push key as string
    }
    return arr;
}

static Value builtin_object_values(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    auto arr = std::make_shared<ArrayValue>();
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) return arr;

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    for (auto& pair : obj->properties) {
        arr->elements.push_back(pair.second.value);
    }
    return arr;
}

static Value builtin_object_entry(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    auto arr = std::make_shared<ArrayValue>();
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) return arr;

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    for (auto& pair : obj->properties) {
        auto entry = std::make_shared<ArrayValue>();
        entry->elements.push_back(pair.first);         // key
        entry->elements.push_back(pair.second.value);  // value
        arr->elements.push_back(entry);
    }
    return arr;
}

static Value built_object_freeze(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])){
      throw std::runtime_error("You should pass an object in Object.freeze(obj) as an argument at " + tok.loc.to_string());
    }
    
    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    obj->is_frozen = true;
    return obj;
}
static Value built_object_create(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])){
      auto obj = std::make_shared<ObjectValue>();
      return obj;
    }
    bool is_frozen = false;
    if(args.size() >= 2) {
      is_frozen = value_to_bool(args[1]);
    }
    
    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    obj->is_frozen = is_frozen;
    return obj;
}

static Value builtin_kadiria(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    double x = args.empty() ? 0.0 : value_to_number(args[0]);
    return std::round(x);
}
static Value builtin_kadiriajuu(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    double x = args.empty() ? 0.0 : value_to_number(args[0]);
    return std::ceil(x);
}
static Value builtin_kadiriachini(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    double x = args.empty() ? 0.0 : value_to_number(args[0]);
    return std::floor(x);
}

static Value builtin_kubwa(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    if (args.empty()) return 0.0;
    double m = value_to_number(args[0]);
    for (size_t i = 1; i < args.size(); ++i) m = std::fmax(m, value_to_number(args[i]));
    return m;
}
static Value builtin_ndogo(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    if (args.empty()) return 0.0;
    double m = value_to_number(args[0]);
    for (size_t i = 1; i < args.size(); ++i) m = std::fmin(m, value_to_number(args[i]));
    return m;
}

static Value builtin_log(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    if (args.empty()) return 0.0;
    double n = value_to_number(args[0]);
    if (args.size() == 1) return std::log10(n);
    double base = value_to_number(args[1]);
    // safe: if base <= 0 or base == 1 will produce nan/inf as per std::log behaviour
    return std::log(n) / std::log(base);
}
static Value builtin_ln(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    if (args.empty()) return 0.0;
    double n = value_to_number(args[0]);
    return std::log(n);
}

static Value builtin_sin(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    double x = args.empty() ? 0.0 : value_to_number(args[0]);
    return std::sin(x);
}
static Value builtin_cos(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    double x = args.empty() ? 0.0 : value_to_number(args[0]);
    return std::cos(x);
}
static Value builtin_tan(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    double x = args.empty() ? 0.0 : value_to_number(args[0]);
    return std::tan(x);
}
static Value builtin_hypot(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    if (args.empty()) return 0.0;
    // fold into hypot: hypot(x1, x2, x3...) -> use pairwise hypot
    double h = value_to_number(args[0]);
    for (size_t i = 1; i < args.size(); ++i) h = std::hypot(h, value_to_number(args[i]));
    return h;
}
static Value builtin_isnan(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    if (args.empty()) return false;
    double x = value_to_number(args[0]);
    return std::isnan(x);
}
static Value builtin_rand(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    // thread_local engine seeded once per thread
    static thread_local std::mt19937_64 rng(std::random_device{}());

    auto value_to_double = [](const Value& v) {
        return value_to_number(v);
    };

    if (args.empty()) {
        std::uniform_real_distribution<double> d(0.0, std::nextafter(1.0, 2.0));  // [0,1)
        return d(rng);
    }

    if (args.size() == 1) {
        double a = value_to_double(args[0]);
        std::uniform_real_distribution<double> d(0.0, std::nextafter(a, std::numeric_limits<double>::infinity()));
        return d(rng);  // uniform in [0, a) (works even if a<0)
    }

    double a = value_to_double(args[0]);
    double b = value_to_double(args[1]);
    if (a > b) std::swap(a, b);
    std::uniform_real_distribution<double> d(a, std::nextafter(b, std::numeric_limits<double>::infinity()));
    return d(rng);  // uniform in [a, b)
}
static Value builtin_sign(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    if (args.empty()) return 0.0;
    double x = value_to_number(args[0]);
    if (x > 0) return 1.0;
    if (x < 0) return -1.0;
    return 0.0;
}
static Value builtin_deg2rad(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    double d = args.empty() ? 0.0 : value_to_number(args[0]);
    return d * (M_PI / 180.0);
}
static Value builtin_rad2deg(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    double r = args.empty() ? 0.0 : value_to_number(args[0]);
    return r * (180.0 / M_PI);
}
static void collect_numbers_from_args_or_array(const std::vector<Value>& args, std::vector<double>& out) {
    if (args.size() == 1 && std::holds_alternative<ArrayPtr>(args[0])) {
        ArrayPtr arr = std::get<ArrayPtr>(args[0]);
        for (auto& v : arr->elements) out.push_back(value_to_number(v));
    } else {
        for (auto& v : args) out.push_back(value_to_number(v));
    }
}

static Value builtin_mean(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    std::vector<double> vals;
    collect_numbers_from_args_or_array(args, vals);
    if (vals.empty()) return 0.0;
    double sum = std::accumulate(vals.begin(), vals.end(), 0.0);
    return sum / static_cast<double>(vals.size());
}

static Value builtin_median(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    std::vector<double> vals;
    collect_numbers_from_args_or_array(args, vals);
    if (vals.empty()) return 0.0;
    std::sort(vals.begin(), vals.end());
    size_t n = vals.size();
    if (n % 2 == 1) return vals[n / 2];
    return (vals[n / 2 - 1] + vals[n / 2]) / 2.0;
}

static Value builtin_stddev(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    std::vector<double> vals;
    collect_numbers_from_args_or_array(args, vals);
    if (vals.empty()) return 0.0;
    double mean = std::accumulate(vals.begin(), vals.end(), 0.0) / vals.size();
    double acc = 0.0;
    for (double x : vals) acc += (x - mean) * (x - mean);
    // population stddev
    return std::sqrt(acc / vals.size());
}

static Value builtin_roundTo(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    if (args.empty()) return 0.0;
    double x = value_to_number(args[0]);
    int digits = args.size() > 1 ? static_cast<int>(value_to_number(args[1])) : 0;
    double scale = std::pow(10.0, digits);
    return std::round(x * scale) / scale;
}

static long long ll_gcd(long long a, long long b) {
    if (a == 0) return std::llabs(b);
    if (b == 0) return std::llabs(a);
    a = std::llabs(a);
    b = std::llabs(b);
    while (b != 0) {
        long long t = a % b;
        a = b;
        b = t;
    }
    return std::llabs(a);
}
static Value builtin_gcd(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    if (args.empty()) return 0.0;
    long long a = static_cast<long long>(std::llround(value_to_number(args[0])));
    if (args.size() == 1) return static_cast<double>(std::llabs(a));
    long long b = static_cast<long long>(std::llround(value_to_number(args[1])));
    long long g = ll_gcd(a, b);
    return static_cast<double>(g);
}
static Value builtin_lcm(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    if (args.size() < 2) return 0.0;
    long long a = static_cast<long long>(std::llround(value_to_number(args[0])));
    long long b = static_cast<long long>(std::llround(value_to_number(args[1])));
    if (a == 0 || b == 0) return 0.0;
    long long g = ll_gcd(a, b);
    // lcm = abs(a / g * b) — avoid overflow if possible (still risky for huge numbers)
    long long l = std::llabs((a / g) * b);
    return static_cast<double>(l);
}

static Value builtin_throw(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    std::string msg = args.empty() ? "Error" : value_to_string(args[0]);
    throw std::runtime_error(msg);
}
static Value builtin_thibitisha(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    bool ok = args.empty() ? false : value_to_bool(args[0]);
    if (!ok) {
        std::string msg = args.size() > 1 ? value_to_string(args[1]) : std::string("Assertion failed");
        throw std::runtime_error(msg);
    }
    return Value();
}

static Value builtin_toka(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    int code = 0;
    if (!args.empty()) code = static_cast<int>(std::llround(value_to_number(args[0])));
    std::exit(code);
    return std::string("");  // unreachable, keeps signature happy
}
static Value builtin_cerr(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    if(args.empty()) {
      throw std::runtime_error("cerr should have an error message as an argument, you passed no argument");
    }
    std::string msg = Evaluator::cerr_colored(value_to_string(args[0]));
    std::cerr << msg << "\n";
    return Value();
}

// --------------------
// Ordered map factory: Object.map([plainObject])
// --------------------
// Add this after builtin_object_entry in this file.
static Value built_object_ordered(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    // internal ordered store
    auto store = std::make_shared<std::vector<std::pair<std::string, Value>>>();

    // helper: canonicalize a Value -> property key string (reuse simple rules)
    auto key_to_string = [](const Value& v) -> std::string {
        if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
        if (std::holds_alternative<double>(v)) {
            double d = std::get<double>(v);
            if (!std::isfinite(d)) throw std::runtime_error("Invalid number for property key");
            double floor_d = std::floor(d);
            if (d == floor_d) return std::to_string(static_cast<long long>(d));
            return std::to_string(d);
        }
        if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "kweli" : "sikweli";
        throw std::runtime_error("Cannot convert value to property key");
    };

    // If caller provided a plain object as first argument, copy its properties into store
    if (!args.empty() && std::holds_alternative<ObjectPtr>(args[0])) {
        ObjectPtr src = std::get<ObjectPtr>(args[0]);
        if (src) {
            // copy properties in the map's iteration order (note: plain object iteration order is
            // whatever the runtime's map yields; this merely constructs an ordered snapshot)
            for (const auto& kv : src->properties) {
                store->emplace_back(kv.first, kv.second.value);
            }
        }
    }

    // Create the returned object that exposes methods which operate on 'store'
    auto ret = std::make_shared<ObjectValue>();

    // Helper to create native methods easily (each method uses env as closure)
    auto make_native_fn = [&](const std::string& name,
                              std::function<Value(const std::vector<Value>&, EnvPtr, const Token&)> impl) -> Value {
        auto fn = std::make_shared<FunctionValue>(name, impl, env, Token{});
        return Value{fn};
    };

    // set(key, value)
    {
        auto impl = [store, key_to_string](const std::vector<Value>& a, EnvPtr, const Token& callTok) -> Value {
            if (a.empty()) throw std::runtime_error("map.set needs (key, value) at " + callTok.loc.to_string());
            std::string k = key_to_string(a[0]);
            Value val = (a.size() >= 2) ? a[1] : std::monostate{};
            for (auto& p : *store) {
                if (p.first == k) {
                    p.second = val;
                    return val;
                }
            }
            store->emplace_back(k, val);
            return val;
        };
        ret->properties["set"] = {
            std::get<FunctionPtr>(make_native_fn("map.set", impl)),
            false,
            false,
            true,
            Token{}};
    }

    // get(key)
    {
        auto impl = [store, key_to_string](const std::vector<Value>& a, EnvPtr, const Token& callTok) -> Value {
            if (a.empty()) throw std::runtime_error("map.get needs (key) at " + callTok.loc.to_string());
            std::string k = key_to_string(a[0]);
            for (auto& p : *store)
                if (p.first == k) return p.second;
            return std::monostate{};
        };
        ret->properties["get"] = {
            std::get<FunctionPtr>(make_native_fn("map.get", impl)),
            false,
            false,
            true,
            Token{}};
    }

    // has(key)
    {
        auto impl = [store, key_to_string](const std::vector<Value>& a, EnvPtr, const Token& callTok) -> Value {
            if (a.empty()) throw std::runtime_error("map.has needs (key) at " + callTok.loc.to_string());
            std::string k = key_to_string(a[0]);
            for (auto& p : *store)
                if (p.first == k) return true;
            return false;
        };
        ret->properties["has"] = {
            std::get<FunctionPtr>(make_native_fn("map.has", impl)),
            false,
            false,
            true,
            Token{}};
    }

    // delete(key) -> bool
    {
        auto impl = [store, key_to_string](const std::vector<Value>& a, EnvPtr, const Token& callTok) -> Value {
            if (a.empty()) throw std::runtime_error("map.delete needs (key) at " + callTok.loc.to_string());
            std::string k = key_to_string(a[0]);
            for (size_t i = 0; i < store->size(); ++i) {
                if ((*store)[i].first == k) {
                    store->erase(store->begin() + static_cast<long long>(i));
                    return true;
                }
            }
            return false;
        };
        ret->properties["delete"] = {
            std::get<FunctionPtr>(make_native_fn("map.delete", impl)),
            false,
            false,
            true,
            Token{}};
    }

    // keys() -> Array of strings in insertion order
    {
        auto impl = [store](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            auto arr = std::make_shared<ArrayValue>();
            for (auto& p : *store) arr->elements.push_back(p.first);
            return arr;
        };
        ret->properties["keys"] = {
            std::get<FunctionPtr>(make_native_fn("map.keys", impl)),
            false,
            false,
            true,
            Token{}};
    }

    // values() -> Array of Values in insertion order
    {
        auto impl = [store](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            auto arr = std::make_shared<ArrayValue>();
            for (auto& p : *store) arr->elements.push_back(p.second);
            return arr;
        };
        ret->properties["values"] = {
            std::get<FunctionPtr>(make_native_fn("map.values", impl)),
            false,
            false,
            true,
            Token{}};
    }

    // entries() -> Array of [key, value] arrays
    {
        auto impl = [store](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            auto arr = std::make_shared<ArrayValue>();
            for (auto& p : *store) {
                auto pairArr = std::make_shared<ArrayValue>();
                pairArr->elements.push_back(p.first);
                pairArr->elements.push_back(p.second);
                arr->elements.push_back(pairArr);
            }
            return arr;
        };
        ret->properties["entries"] = {
            std::get<FunctionPtr>(make_native_fn("map.entries", impl)),
            false,
            false,
            true,
            Token{}};
    }

    // size() -> number
    {
        auto impl = [store](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            return static_cast<double>(store->size());
        };
        ret->properties["size"] = {
            std::get<FunctionPtr>(make_native_fn("map.size", impl)),
            false,
            false,
            true,
            Token{}};
    }

    // toPlain() -> convert to a normal ObjectValue snapshot (unordered map of the current entries)
    {
        auto impl = [store](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            auto out = std::make_shared<ObjectValue>();
            for (auto& p : *store) {
                PropertyDescriptor pd;
                pd.value = p.second;
                pd.is_private = false;
                pd.is_readonly = false;
                pd.is_locked = false;
                pd.token = Token{};
                out->properties[p.first] = std::move(pd);
            }
            return out;
        };
        ret->properties["toPlain"] = {
            std::get<FunctionPtr>(make_native_fn("map.toPlain", impl)),
            false,
            false,
            true,
            Token{}};
    }

    return ret;
}

void init_globals(EnvPtr env) {
    if (!env) return;

    auto add_fn = [&](const std::string& name,
                      std::function<Value(const std::vector<Value>&, EnvPtr, const Token&)> impl) {
        auto fn = std::make_shared<FunctionValue>(name, impl, env, Token{});
        Environment::Variable var{
            fn,
            true};
        env->set(name, var);
    };

    // Existing builtins
    add_fn("ainaya", builtin_ainaya);
    add_fn("Orodha", builtin_orodha);
    add_fn("Bool", builtin_bool);
    add_fn("Namba", builtin_namba);
    add_fn("Neno", builtin_neno);
    add_fn("soma", builtin_ingiza);
    add_fn("Makosa", builtin_throw);
    add_fn("thibitisha", builtin_thibitisha);
    add_fn("assert", builtin_thibitisha);

    auto objectVal = std::make_shared<ObjectValue>();

    {
        auto fn = std::make_shared<FunctionValue>("keys", builtin_object_keys, env, Token{});
        objectVal->properties["keys"] = {
            fn,
            false,
            false,
            true,
            Token{}};
    }
    {
        auto fn = std::make_shared<FunctionValue>("values", builtin_object_values, env, Token{});
        objectVal->properties["values"] = {
            fn,
            false,
            false,
            true,
            Token{}};
    }
    {
        auto fn = std::make_shared<FunctionValue>("entry", builtin_object_entry, env, Token{});
        objectVal->properties["entry"] = {
            fn,
            false,
            false,
            true,
            Token{}};
    }
    {
        auto fn = std::make_shared<FunctionValue>("ordered", built_object_ordered, env, Token{});
        objectVal->properties["ordered"] = {
            fn,
            false,
            false,
            true,
            Token{}};
    }
    {
        auto fn = std::make_shared<FunctionValue>("freeze", built_object_freeze, env, Token{});
        objectVal->properties["freeze"] = {
            fn,
            false,
            false,
            true,
            Token{}};
    }
    {
        auto fn = std::make_shared<FunctionValue>("create", built_object_create, env, Token{});
        objectVal->properties["create"] = {
            fn,
            false,
            false,
            true,
            Token{}};
    }

    Environment::Variable objectVar;
    objectVar.value = objectVal;
    objectVar.is_constant = true;
    env->set("Object", objectVar);

    {
        auto hesabuVal = std::make_shared<ObjectValue>();

        auto add = [&](const std::string& name,
                       std::function<Value(const std::vector<Value>&, EnvPtr, const Token&)> impl) {
            auto fn = std::make_shared<FunctionValue>(name, impl, env, Token{});
            hesabuVal->properties[name] = {
                fn,
                false,
                false,
                true,
                Token{}};
        };

        add("kadiria", builtin_kadiria);
        add("kadiriajuu", builtin_kadiriajuu);
        add("kadiriachini", builtin_kadiriachini);
        add("kubwa", builtin_kubwa);
        add("ndogo", builtin_ndogo);
        add("log", builtin_log);
        add("ln", builtin_ln);
        add("sin", builtin_sin);
        add("cos", builtin_cos);
        add("tan", builtin_tan);
        add("hypot", builtin_hypot);
        add("rand", builtin_rand);
        add("siNambaSahihi", builtin_isnan);
        add("isNaN", builtin_isnan);
        add("deg2rad", builtin_deg2rad);
        add("rad2deg", builtin_rad2deg);
        add("alama", builtin_sign);
        add("gcd", builtin_gcd);
        add("lcm", builtin_lcm);
        add("mean", builtin_mean);
        add("median", builtin_median);
        add("stddev", builtin_stddev);
        add("kadiriaKtkDes", builtin_roundTo);
        add("fixAt", builtin_roundTo);

        Value nanValue = Value(std::numeric_limits<double>::quiet_NaN());

        hesabuVal->properties["NaN"] = {
            nanValue,
            false,
            false,
            true,
            Token{}};

        Value infValue = Value(std::numeric_limits<double>::infinity());

        hesabuVal->properties["Inf"] = {
            infValue,
            false,
            false,
            true,
            Token{}};

        Environment::Variable hesabuVar;
        hesabuVar.value = hesabuVal;
        hesabuVar.is_constant = true;
        env->set("Hesabu", hesabuVar);
    }

    init_time(env);
    init_muda_class(env);
    init_set_class(env);

    {
        auto programVal = std::make_shared<ObjectValue>();
        programVal->is_frozen = true;
        auto add = [&](const std::string& name,
                       std::function<Value(const std::vector<Value>&, EnvPtr, const Token&)> impl) {
            auto fn = std::make_shared<FunctionValue>(name, impl, env, Token{});
            programVal->properties[name] = {
                fn,
                false,
                false,
                true,
                Token{}};
        };

        add("exit", builtin_toka);
        add("cerr", builtin_cerr);
        add("cin", builtin_ingiza);

        Environment::Variable program;
        program.value = programVal;
        program.is_constant = true;
        env->set("swazi", program);
    }

    {
        std::ostringstream ss;
        const char* user = std::getenv("USER");
        if (!user) user = std::getenv("USERNAME");
        ss << "swazi v" << SWAZI_VERSION << " built on " << __DATE__;
        if (user && user[0] != '\0') ss << " user=" << user;
#if defined(__linux__)
        ss << " os=linux";
#elif defined(_WIN32)
        ss << " os=windows";
#elif defined(__APPLE__)
        ss << " os=macos";
#endif

        Environment::Variable infoVar;
        infoVar.value = ss.str();
        infoVar.is_constant = true;
        env->set("__info__", infoVar);
    }
}