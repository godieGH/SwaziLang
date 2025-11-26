#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <numbers>
#include <numeric>
#include <random>
#include <thread>

#include "ClassRuntime.hpp"
#include "Frame.hpp"
#include "Scheduler.hpp"
#include "SwaziError.hpp"
#include "evaluator.hpp"
#include "globals.hpp"
#include "muda_class.hpp"
#include "set_class.hpp"
#include "time.hpp"
#include "token.hpp"
#include "url_class.hpp"

// Helper: build a TokenLocation from a user-supplied object value (if possible).
// The user-provided object may contain fields like:
//  - filename (string)
//  - line (number)
//  - col (number)
//  - length (number)
//  - line_trace (string) OR trace_str (string)
//  - linestrv (plain object mapping line numbers -> string)
//
// This function is defensive: if fields are missing or the arg isn't an object, it will
// fall back to the provided defaultLoc.
TokenLocation build_location_from_value(const Value& v, const TokenLocation& defaultLoc) {
    if (!std::holds_alternative<ObjectPtr>(v)) return defaultLoc;
    ObjectPtr o = std::get<ObjectPtr>(v);
    if (!o) return defaultLoc;

    TokenLocation loc = defaultLoc;

    auto get_string = [&](const std::string& key) -> std::optional<std::string> {
        auto it = o->properties.find(key);
        if (it == o->properties.end()) return std::nullopt;
        const Value& vv = it->second.value;
        if (std::holds_alternative<std::string>(vv)) return std::get<std::string>(vv);
        return std::nullopt;
    };

    auto get_number = [&](const std::string& key) -> std::optional<int> {
        auto it = o->properties.find(key);
        if (it == o->properties.end()) return std::nullopt;
        const Value& vv = it->second.value;
        if (std::holds_alternative<double>(vv))
            return static_cast<int>(std::llround(std::get<double>(vv)));
        return std::nullopt;
    };

    if (auto s = get_string("filename"))
        loc.filename = *s;
    else if (auto s2 = get_string("file"))
        loc.filename = *s2;

    if (auto n = get_number("line")) loc.line = *n;
    if (auto n = get_number("col")) loc.col = *n;
    if (auto n = get_number("length")) loc.length = *n;

    if (auto t = get_string("line_trace"))
        loc.line_trace = *t;
    else if (auto t2 = get_string("trace_str"))
        loc.line_trace = *t2;
    else if (auto t3 = get_string("trace"))
        loc.line_trace = *t3;

    auto it_lines = o->properties.find("linestrv");
    if (it_lines != o->properties.end() &&
        std::holds_alternative<ObjectPtr>(it_lines->second.value)) {
        ObjectPtr mobj = std::get<ObjectPtr>(it_lines->second.value);
        std::map<int, std::string> mp;
        for (auto& kv : mobj->properties) {
            try {
                int ln = std::stoi(kv.first);
                const Value& valv = kv.second.value;
                if (std::holds_alternative<std::string>(valv)) {
                    mp[ln] = std::get<std::string>(valv);
                } else {
                    if (std::holds_alternative<double>(valv))
                        mp[ln] = std::to_string(std::get<double>(valv));
                    else if (std::holds_alternative<bool>(valv))
                        mp[ln] = std::get<bool>(valv) ? "true" : "false";
                }
            } catch (...) {
                // ignore non-integer keys
            }
        }
        if (!mp.empty()) loc.set_map_linestr(mp);
    }

    return loc;
}

static Value builtin_ainaya(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    if (args.empty()) {
        return std::string("unknown");
    }

    const Value& v = args[0];

    return _type_name(v);
}
static Value builtin_orodha(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    auto arr = std::make_shared<ArrayValue>();

    if (args.empty()) {
        return arr;
    }

    // backward-compatible single numeric behavior: Orodha(5) -> array of length 5 filled with null
    // New optional second argument: if boolean true -> fill with holes instead of nulls
    bool fillWithHoles = true;
    /*if (args.size() >= 2) {
        if (std::holds_alternative<bool>(args[1])) {
            fillWithHoles = std::get<bool>(args[1]);
        } else if (std::holds_alternative<std::string>(args[1])) {
            // allow string flags like "holes"
            fillWithHoles = (std::get<std::string>(args[1]) == "holes");
        }
    }*/

    if (args.size() == 1) {
        const Value& first = args[0];

        if (std::holds_alternative<double>(first)) {
            // case: Orodha(5) -> array of length 5, filled with empty values (previous behavior)
            int len = static_cast<int>(std::get<double>(first));
            if (len < 0) len = 0;
            arr->elements.resize(len);
            if (fillWithHoles) {
                for (int i = 0; i < len; ++i) arr->elements[i] = HoleValue{};
            } else {
                for (int i = 0; i < len; ++i) arr->elements[i] = std::monostate{};
            }
            return arr;
        }

        if (std::holds_alternative<ArrayPtr>(first)) {
            // case: Orodha([1,2,3]) -> copy
            ArrayPtr src = std::get<ArrayPtr>(first);
            arr->elements = src->elements;
            return arr;
        }
    }

    // default case: Orodha(6,8,5,8) or any other list of arguments -> treat args as elements
    // No holes in this construction path (unless caller explicitly passes HoleValue which is possible via native means)
    arr->elements = args;
    return arr;
}
static Value builtin_bool(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    return args.empty() ? false : value_to_bool(args[0]);
}

static Value builtin_soma(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    std::string prompt = args.empty() ? "" : value_to_string(args[0]);
    if (!prompt.empty()) std::cout << prompt;
    std::string input;
    std::getline(std::cin, input);
    return input;  // return as string
}

static Value builtin_namba(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    return args.empty() ? 0.0 : value_to_number(args[0]);
}

static Value builtin_parseInt(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    // 1. Validate Input Argument Count
    if (args.empty()) {
        throw SwaziError("TypeError", "You should pass atleast one argument to parseInt(valu).", tok.loc);
    }

    const Value& input_value = args[0];

    // 2. Determine the base for conversion (optional second argument)
    int base = 10; // Default base is 10 (decimal)

    if (args.size() >= 2) {
        const Value& base_value = args[1];

        // Ensure the base argument is a number (double variant)
        if (std::holds_alternative<double>(base_value)) {
            double base_double = std::get<double>(base_value);
            
            // Check if the base is an integer and convert it
            if (std::abs(base_double - std::round(base_double)) > 1e-9) {
                 throw SwaziError("TypeError", "The conversion base must be an integer.", tok.loc);
            }
            base = static_cast<int>(std::round(base_double));

            // Input validation: Base must be between 2 and 36 for std::stoll/stod
            if (base < 2 || base > 36) {
                throw SwaziError("RangeError", "Base for conversion must be between 2 and 36.", tok.loc);
            }
        } else {
            // Error if the second argument is provided but isn't a number
            throw SwaziError("TypeError", "The second argument (base) must be a number.", tok.loc);
        }
    }
    
    // 3. Perform the conversion from String to Number
    if (std::holds_alternative<std::string>(input_value)) {
        const std::string& str = std::get<std::string>(input_value);
        
        try {
            
            size_t end_idx = 0; 
            long long l_val = std::stoll(str, &end_idx, base);
            
            if(end_idx != str.length()) {
              throw SwaziError("TypeError", "The string has garbage characters after the valid number.", tok.loc);
            }
            
            return static_cast<double>(l_val);
            
        } catch (const std::invalid_argument& e) {
            // String couldn't be parsed (e.g., "G" in base 10)
            std::string msg = "Invalid string '" + str + "' for conversion in base " + std::to_string(base) + ".";
            throw SwaziError("ValueError", msg, tok.loc);
            
        } catch (const std::out_of_range& e) {
            // Number is too large for long long
            std::string msg = "Number '" + str + "' is out of range for conversion.";
            throw SwaziError("RangeError", msg, tok.loc);
        }

    } 
    
    // 4. Fallback for Non-String Types
    // If input_value is already a number (double), or another valid type, use original logic.
    // This allows `namba(5.5)` to return 5.5, or `namba(true)` to return 1.0 (if value_to_number handles it).
    else {
        // Assuming value_to_number handles conversion from other types in the Value variant (like bool) to double.
        return value_to_number(input_value);
    }
}


static Value builtin_neno(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    return args.empty() ? std::string("") : value_to_string(args[0]);
}

static Value builtin_object_keys(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    auto arr = std::make_shared<ArrayValue>();
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) return arr;

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    if (!obj) return arr;

    // If this object is an env-proxy, enumerate the backing Environment's names.
    if (obj->is_env_proxy && obj->proxy_env) {
        for (const auto& kv : obj->proxy_env->values) {
            arr->elements.push_back(kv.first);
        }
        return arr;
    }

    // fallback: enumerate object properties map
    for (auto& pair : obj->properties) {
        arr->elements.push_back(pair.first);  // push key as string
    }
    return arr;
}

static Value builtin_object_values(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    auto arr = std::make_shared<ArrayValue>();
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) return arr;

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    if (!obj) return arr;

    // env-proxy: return the underlying variable values
    if (obj->is_env_proxy && obj->proxy_env) {
        for (const auto& kv : obj->proxy_env->values) {
            arr->elements.push_back(kv.second.value);
        }
        return arr;
    }

    for (auto& pair : obj->properties) {
        arr->elements.push_back(pair.second.value);
    }
    return arr;
}

static Value builtin_object_entry(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    auto arr = std::make_shared<ArrayValue>();
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) return arr;

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    if (!obj) return arr;

    // env-proxy: produce [key, value] pairs from the Environment map (in insertion/unordered map order)
    if (obj->is_env_proxy && obj->proxy_env) {
        for (const auto& kv : obj->proxy_env->values) {
            auto entry = std::make_shared<ArrayValue>();
            entry->elements.push_back(kv.first);         // key
            entry->elements.push_back(kv.second.value);  // value
            arr->elements.push_back(entry);
        }
        return arr;
    }

    for (auto& pair : obj->properties) {
        auto entry = std::make_shared<ArrayValue>();
        entry->elements.push_back(pair.first);         // key
        entry->elements.push_back(pair.second.value);  // value
        arr->elements.push_back(entry);
    }
    return arr;
}

static Value built_object_freeze(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) {
        throw SwaziError("TypeError", "You should pass an object in Object.freeze(obj) as an argument", tok.loc);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    obj->is_frozen = true;
    return obj;
}
static Value built_object_create(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) {
        auto obj = std::make_shared<ObjectValue>();
        return obj;
    }
    bool is_frozen = false;
    if (args.size() >= 2) {
        is_frozen = value_to_bool(args[1]);
    }

    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    obj->is_frozen = is_frozen;
    return obj;
}

static Value builtin_round(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    double x = args.empty() ? 0.0 : value_to_number(args[0]);
    return std::round(x);
}
static Value builtin_ceil(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    double x = args.empty() ? 0.0 : value_to_number(args[0]);
    return std::ceil(x);
}
static Value builtin_floor(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    double x = args.empty() ? 0.0 : value_to_number(args[0]);
    return std::floor(x);
}

static Value builtin_max(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    if (args.empty()) return 0.0;
    double m = value_to_number(args[0]);
    for (size_t i = 1; i < args.size(); ++i) m = std::fmax(m, value_to_number(args[i]));
    return m;
}
static Value builtin_min(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
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
    return d * (std::numbers::pi / 180.0);
}
static Value builtin_rad2deg(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    double r = args.empty() ? 0.0 : value_to_number(args[0]);
    return r * (180.0 / std::numbers::pi);
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

// Modified builtin_throw to support three formats:
// 1) throw("message") -> throws std::runtime_error with message and call-site loc appended.
// 2) throw("Type", "message") -> throws std::runtime_error with "Type: message" and call-site loc appended.
// 3) throw("Type", "message", locObj) -> constructs a TokenLocation from locObj and throws SwaziError(Type, message, locObjConverted).
static Value builtin_throw(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    // default type and message
    std::string type = "Error";
    std::string msg = "Error";

    if (args.empty()) {
        // no args: use default message and include call token location in the runtime_error text
        std::string out = type + " at " + tok.loc.to_string() + "\n" + msg;
        throw std::runtime_error(out);
    }

    // One argument: treat as message
    if (args.size() == 1) {
        msg = value_to_string(args[0]);
        std::string out = type + " at " + tok.loc.to_string() + "\n" + msg;
        throw std::runtime_error(out);
    }

    // Two arguments: first is type, second is message
    if (args.size() == 2) {
        type = value_to_string(args[0]);
        msg = value_to_string(args[1]);
        std::string out = type + " at " + tok.loc.to_string() + "\n" + msg;
        throw std::runtime_error(out);
    }

    // Three or more: first type, second message, third is location object to build TokenLocation
    type = value_to_string(args[0]);
    msg = value_to_string(args[1]);
    const Value& locVal = args[2];

    TokenLocation userLoc = build_location_from_value(locVal, tok.loc);
    // Use SwaziError when explicit location is provided
    throw SwaziError(type, msg, userLoc);
}
static Value builtin_Error(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    // Create error object (frozen) that can be thrown or returned
    ObjectPtr errObj = std::make_shared<ObjectValue>();
    errObj->is_frozen = true;

    std::string type = "Error";
    std::string msg = "An error occurred";
    Value locVal = std::monostate{};

    // Parse arguments based on count
    if (args.empty()) {
        // Error() - defaults
        type = "Error";
        msg = "An error occurred";
    } else if (args.size() == 1) {
        // Error("message") - message only
        msg = value_to_string(args[0]);
    } else if (args.size() == 2) {
        // Error("Type", "message")
        type = value_to_string(args[0]);
        msg = value_to_string(args[1]);
    } else {
        // Error("Type", "message", locObj)
        type = value_to_string(args[0]);
        msg = value_to_string(args[1]);
        locVal = args[2];
    }

    // Build error object properties
    errObj->properties["errortype"] = {type, false, false, true, tok};
    errObj->properties["message"] = {msg, false, false, true, tok};

    // If location provided, store it
    if (!std::holds_alternative<std::monostate>(locVal)) {
        errObj->properties["loc"] = {locVal, false, false, true, tok};
    }

    return errObj;
}

static Value builtin_thibitisha(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    bool ok = args.empty() ? false : value_to_bool(args[0]);
    if (!ok) {
        std::string msg = args.size() > 1 ? value_to_string(args[1]) : std::string("Assertion failed");
        // Use call-site location for assertions
        throw SwaziError("AssertionError", msg, tok.loc);
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
    if (args.empty()) {
        throw SwaziError("RuntimeError", "cerr should have an error message as an argument, you passed no argument", tok.loc);
    }
    std::string msg = Evaluator::cerr_colored(value_to_string(args[0]));
    std::cerr << msg << "\n";
    return Value();
}
static Value builtin_print(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    for (int i = 0; i < args.size(); ++i) {
        std::string msg = value_to_string(args[i]);
        std::cout << msg << " ";
    }
    std::cout << "\n";
    return Value();
}
static Value builtin_sleep(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty()) return std::monostate{};
    long long ms = static_cast<long long>(value_to_number(args[0]));

    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return std::monostate{};
}
// --------------------
// Ordered map factory: Object.ordered([plainObject])
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
            if (!std::isfinite(d)) throw SwaziError("TypeError", "Invalid number for property key", TokenLocation{"<builtin>", 0, 0, 0});
            double floor_d = std::floor(d);
            if (d == floor_d) return std::to_string(static_cast<long long>(d));
            return std::to_string(d);
        }
        if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "kweli" : "sikweli";
        throw SwaziError("TypeError", "Cannot convert value to property key", TokenLocation{"<builtin>", 0, 0, 0});
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
            if (a.empty()) throw SwaziError("TypeError", "map.set needs (key, value)", callTok.loc);
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
            if (a.empty()) throw SwaziError("TypeError", "map.get needs (key)", callTok.loc);
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
            if (a.empty()) throw SwaziError("TypeError", "map.has needs (key)", callTok.loc);
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
            if (a.empty()) throw SwaziError("TypeError", "map.delete needs (key)", callTok.loc);
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

void init_globals(EnvPtr env, Evaluator* evaluator) {
    if (!env) return;

    auto add_fn = [&](const std::string& name,
                      std::function<Value(const std::vector<Value>&, EnvPtr, const Token&)> impl) {
        auto fn = std::make_shared<FunctionValue>(name, impl, env, Token{});
        Environment::Variable var{
            fn,
            true};
        env->set(name, var);
    };

    // --- globals() builtin: return a live proxy object for the module-level environment.
    add_fn("globals", [env](const std::vector<Value>& /*args*/, EnvPtr callEnv, const Token& /*tok*/) -> Value {
        // Determine module-level environment:
        // Walk up from callEnv until we find an environment whose parent == global_env (env param),
        // or stop at callEnv if none found (fallback).
        EnvPtr module_env = nullptr;
        if (callEnv) {
            EnvPtr walk = callEnv;
            // Walk until we either reach an env whose parent is the global env or we reach nullptr
            while (walk) {
                if (walk->parent == env) {
                    module_env = walk;
                    break;
                }
                // If parent is nullptr, stop (no module boundary)
                if (!walk->parent) break;
                walk = walk->parent;
            }
        }

        // Fallbacks:
        if (!module_env) {
            // If callEnv is null or we didn't find a module boundary, prefer:
            // - callEnv (so REPL/local contexts still work), else global env as last resort
            module_env = callEnv ? callEnv : env;
        }

        // Create the env proxy object (live view of the module env)
        auto proxy = std::make_shared<ObjectValue>();
        proxy->is_env_proxy = true;
        proxy->proxy_env = module_env;

        // Create a builtins proxy object that points to the global_env (env param).
        auto builtins_proxy = std::make_shared<ObjectValue>();
        builtins_proxy->is_env_proxy = true;
        builtins_proxy->proxy_env = env;  // global env

        // Expose __builtins__ on the returned proxy as a readonly/locked property (so globals().__builtins__ works).
        PropertyDescriptor pd;
        pd.value = builtins_proxy;
        pd.is_private = false;
        pd.is_readonly = true;
        pd.is_locked = true;
        pd.token = Token();
        proxy->properties["__builtins__"] = std::move(pd);

        return Value{proxy};
    });

    add_fn("ainaya", builtin_ainaya);
    add_fn("Orodha", builtin_orodha);
    add_fn("Bool", builtin_bool);
    add_fn("Namba", builtin_namba);
    add_fn("parseInt", builtin_parseInt);
    add_fn("Neno", builtin_neno);
    add_fn("soma", builtin_soma);
    add_fn("Makosa", builtin_throw);
    add_fn("Error", builtin_Error);
    add_fn("thibitisha", builtin_thibitisha);
    add_fn("assert", builtin_thibitisha);
    add_fn("sleep", builtin_sleep);

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

        add("round", builtin_round);
        add("ceil", builtin_ceil);
        add("floor", builtin_floor);
        add("max", builtin_max);
        add("min", builtin_min);
        add("log", builtin_log);
        add("ln", builtin_ln);
        add("sin", builtin_sin);
        add("cos", builtin_cos);
        add("tan", builtin_tan);
        add("hypot", builtin_hypot);
        add("rand", builtin_rand);
        add("isNaN", builtin_isnan);
        add("deg2rad", builtin_deg2rad);
        add("rad2deg", builtin_rad2deg);
        add("sign", builtin_sign);
        add("gcd", builtin_gcd);
        add("lcm", builtin_lcm);
        add("mean", builtin_mean);
        add("median", builtin_median);
        add("stddev", builtin_stddev);
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

        hesabuVal->properties["PI"] = {
            Value(std::numbers::pi), false, false, true, Token{}};
        hesabuVal->properties["E"] = {
            Value(std::numbers::e), false, false, true, Token{}};

        Environment::Variable hesabuVar;
        hesabuVar.value = hesabuVal;
        hesabuVar.is_constant = true;
        env->set("Math", hesabuVar);
    }

    init_time(env);
    init_muda_class(env);
    init_set_class(env);
    init_url_class(env);

    {
        auto programVal = std::make_shared<ObjectValue>();
        programVal->is_frozen = true;

        auto add_method = [&](std::shared_ptr<ObjectValue> obj,
                              const std::string& name,
                              std::function<Value(const std::vector<Value>&, EnvPtr, const Token&)> impl) {
            auto fn = std::make_shared<FunctionValue>(name, impl, env, Token{});
            obj->properties[name] = {fn, false, false, true, Token{}};
        };

        // --- stdout ---
        auto stdoutVal = std::make_shared<ObjectValue>();
        add_method(stdoutVal, "write", builtin_print);

        // --- stderr ---
        auto stderrVal = std::make_shared<ObjectValue>();
        add_method(stderrVal, "write", builtin_cerr);

        // --- stdin ---
        auto stdinVal = std::make_shared<ObjectValue>();
        add_method(stdinVal, "readLine", builtin_soma);

        // Attach standard IO objects
        programVal->properties["stdout"] = {stdoutVal, false, false, true, Token{}};
        programVal->properties["stderr"] = {stderrVal, false, false, true, Token{}};
        programVal->properties["stdin"] = {stdinVal, false, false, true, Token{}};

        // --- top-level builtins ---
        add_method(programVal, "exit", builtin_toka);
        add_method(programVal, "log", builtin_print);

        // --- backward compatibility aliases ---
        // swazi.cin -> swazi.stdin.readLine
        auto cinFn = std::make_shared<FunctionValue>("cin", builtin_soma, env, Token{});
        programVal->properties["cin"] = {cinFn, false, false, true, Token{}};

        // swazi.cerr -> swazi.stderr.write
        auto cerrFn = std::make_shared<FunctionValue>("cerr", builtin_cerr, env, Token{});
        programVal->properties["cerr"] = {cerrFn, false, false, true, Token{}};

        // Finalize swazi global
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

    {
        auto argv_arr = std::make_shared<ArrayValue>();
        Environment::Variable argvVar;
        argvVar.value = argv_arr;
        argvVar.is_constant = true;
        env->set("argv", argvVar);
    }

    // -----------------------
    // Promise runtime (ClassValue) & native helpers
    // -----------------------
    // Native: create a PromiseValue that wraps on-instance "__promise__" slot and
    // provide resolve/reject wrappers that the executor will receive.
    // We must capture the Evaluator* so we can synchronously call the executor function.
    auto make_native_fn = [&](const std::string& name,
                              std::function<Value(const std::vector<Value>&, EnvPtr, const Token&)> impl) {
        auto fn = std::make_shared<FunctionValue>(name, impl, env, Token{});
        Environment::Variable var{fn, true};
        env->set(name, var);
    };

    // Promise_native_ctor(this, executor)
    auto native_Promise_native_ctor = [evaluator](const std::vector<Value>& args, EnvPtr callEnv, const Token& tok) -> Value {
        // Expect: this bound as first arg? In constructor AST we will call Promise_native_ctor(this, executor)
        if (args.size() < 2) {
            throw SwaziError("TypeError", "Promise constructor requires an executor function", tok.loc);
        }
        // thisObj must be an ObjectPtr (the Promise instance)
        if (!std::holds_alternative<ObjectPtr>(args[0])) {
            throw SwaziError("TypeError", "Promise constructor internal error: missing receiver", tok.loc);
        }
        ObjectPtr thisObj = std::get<ObjectPtr>(args[0]);

        // Executor may be a FunctionPtr or a native wrapper closure; if not, error
        if (!std::holds_alternative<FunctionPtr>(args[1])) {
            throw SwaziError("TypeError", "Promise executor must be a function", tok.loc);
        }
        FunctionPtr executor = std::get<FunctionPtr>(args[1]);

        // Create the underlying PromiseValue and attach to instance as private slot "__promise__"
        auto p = std::make_shared<PromiseValue>();
        p->state = PromiseValue::State::PENDING;
        p->handled = false;
        p->unhandled_reported = false;

        // store into instance
        PropertyDescriptor pd;
        pd.value = p;
        pd.is_private = true;
        pd.is_readonly = false;
        pd.is_locked = false;
        pd.token = tok;
        thisObj->properties["__promise__"] = std::move(pd);

        // Build language-callable resolve/reject functions that close over `p`
        // These are FunctionValue objects (native) that language code can call.
        // They capture the evaluator pointer so they can schedule microtasks and use evaluator->invoke_function indirectly.
        auto make_resolve_fn = [evaluator, p](bool isResolve) -> FunctionPtr {
            auto impl = [evaluator, p, isResolve](const std::vector<Value>& implArgs, EnvPtr /*env*/, const Token& /*token*/) -> Value {
                // resolve(value) or reject(reason)
                Value val = implArgs.empty() ? std::monostate{} : implArgs[0];

                // Only allow state transition if pending
                if (p->state != PromiseValue::State::PENDING) return std::monostate{};

                // Use evaluator helpers so resolution/rejection always schedule microtasks and report unhandled rejections.
                if (isResolve) {
                    if (evaluator) {
                        evaluator->fulfill_promise(p, val);
                    } else {
                        // Fallback: behave like before if evaluator missing
                        p->state = PromiseValue::State::FULFILLED;
                        p->result = val;
                    }
                } else {
                    if (evaluator) {
                        evaluator->reject_promise(p, val);
                    } else {
                        p->state = PromiseValue::State::REJECTED;
                        p->result = val;
                    }
                }
                return std::monostate{};
            };
            std::string nm = isResolve ? "native:promise.resolve_callback" : "native:promise.reject_callback";
            return std::make_shared<FunctionValue>(nm, impl, nullptr, Token{});
        };

        FunctionPtr resolve_fn = make_resolve_fn(true);
        FunctionPtr reject_fn = make_resolve_fn(false);

        // Now synchronously invoke the executor with (resolve_fn, reject_fn).
        // We use evaluator->invoke_function to run the executor in the interpreter context:
        try {
            if (!evaluator) throw std::runtime_error("internal: evaluator missing for Promise constructor");
            // call executor synchronously with two args (resolve, reject). The executor is a FunctionPtr.
            evaluator->invoke_function(executor, {resolve_fn, reject_fn}, executor->closure, tok);
        } catch (const std::exception& ex) {
            // If executor throws synchronously, reject the promise using evaluator helper
            if (p->state == PromiseValue::State::PENDING) {
                if (evaluator) {
                    evaluator->reject_promise(p, Value{std::string(ex.what())});
                } else {
                    p->state = PromiseValue::State::REJECTED;
                    p->result = std::string(ex.what());
                }
            }
        } catch (...) {
            if (p->state == PromiseValue::State::PENDING) {
                if (evaluator) {
                    evaluator->reject_promise(p, Value{std::string("unknown exception in Promise executor")});
                } else {
                    p->state = PromiseValue::State::REJECTED;
                    p->result = std::string("unknown exception in Promise executor");
                }
            }
        }

        return std::monostate{};  // constructor assignments already done on instance
    };

    // Register native helper so class constructor AST can call it
    make_native_fn("Promise_native_ctor", native_Promise_native_ctor);

    // Promise.static.resolve
    auto native_Promise_static_resolve = [evaluator](const std::vector<Value>& args, EnvPtr /*env*/, const Token& /*tok*/) -> Value {
        // No arg -> fulfilled undefined
        if (args.empty()) {
            auto p = std::make_shared<PromiseValue>();
            if (evaluator) {
                evaluator->fulfill_promise(p, std::monostate{});
            } else {
                p->state = PromiseValue::State::FULFILLED;
                p->result = std::monostate{};
            }
            return Value{p};
        }

        // If already a proper PromisePtr, return it directly
        if (std::holds_alternative<PromisePtr>(args[0])) {
            return args[0];
        }

        // If an object that wraps a promise (has "__promise__"), unwrap and return that
        if (std::holds_alternative<ObjectPtr>(args[0])) {
            ObjectPtr o = std::get<ObjectPtr>(args[0]);
            if (o) {
                auto it = o->properties.find("__promise__");
                if (it != o->properties.end() && std::holds_alternative<PromisePtr>(it->second.value)) {
                    return it->second.value;
                }
            }
        }

        // Otherwise create a fulfilled promise with provided value
        auto p = std::make_shared<PromiseValue>();
        if (evaluator) {
            evaluator->fulfill_promise(p, args[0]);
        } else {
            p->state = PromiseValue::State::FULFILLED;
            p->result = args[0];
        }
        return Value{p};
    };

    // Promise.static.reject
    auto native_Promise_static_reject = [evaluator](const std::vector<Value>& args, EnvPtr /*env*/, const Token& /*tok*/) -> Value {
        auto p = std::make_shared<PromiseValue>();
        if (evaluator) {
            Value reason = args.empty() ? std::monostate{} : args[0];
            evaluator->reject_promise(p, reason);
        } else {
            p->state = PromiseValue::State::REJECTED;
            if (!args.empty())
                p->result = args[0];
            else
                p->result = std::monostate{};
        }
        return Value{p};
    };

    // Build the ClassValue descriptor for Promise
    auto classDesc = std::make_shared<ClassValue>();
    classDesc->name = "Promise";
    classDesc->token = Token{};
    classDesc->body = std::make_unique<ClassBodyNode>();

    // private property '__promise__' (internal slot)
    {
        auto pprop = std::make_unique<ClassPropertyNode>();
        pprop->name = "__promise__";
        pprop->is_private = true;
        pprop->is_locked = true;
        classDesc->body->properties.push_back(std::move(pprop));
    }

    // constructor method AST: this.__promise__ = Promise_native_ctor(this, executor)
    {
        auto ctor = std::make_unique<ClassMethodNode>();
        ctor->name = classDesc->name;
        ctor->is_constructor = true;
        ctor->is_locked = true;
        ctor->is_private = false;
        // parameter: executor
        auto pnode = std::make_unique<ParameterNode>();
        pnode->token = Token{};
        pnode->name = "executor";
        pnode->is_rest = false;
        pnode->rest_required_count = 0;
        pnode->defaultValue = nullptr;
        ctor->params.push_back(std::move(pnode));

        // Build call: Promise_native_ctor(this, executor)
        auto call = std::make_unique<CallExpressionNode>();
        call->callee = std::make_unique<IdentifierNode>();
        static_cast<IdentifierNode*>(call->callee.get())->name = "Promise_native_ctor";
        call->arguments.push_back(std::make_unique<ThisExpressionNode>());
        auto execId = std::make_unique<IdentifierNode>();
        execId->name = "executor";
        call->arguments.push_back(std::move(execId));

        // add call as an expression statement (we don't use returned value)
        auto exprStmt = std::make_unique<ExpressionStatementNode>();
        exprStmt->expression = std::move(call);
        ctor->body.push_back(std::move(exprStmt));

        classDesc->body->methods.push_back(std::move(ctor));
    }

    // static member: resolve
    {
        auto m = std::make_unique<ClassMethodNode>();
        m->name = "resolve";
        m->is_static = true;
        m->is_locked = true;
        m->is_private = false;

        // Build call: return Promise_static_resolve(arg0)
        auto call = std::make_unique<CallExpressionNode>();
        call->callee = std::make_unique<IdentifierNode>();
        static_cast<IdentifierNode*>(call->callee.get())->name = "Promise_static_resolve";
        auto id0 = std::make_unique<IdentifierNode>();
        id0->name = "v";
        call->arguments.push_back(std::move(id0));

        auto ret = std::make_unique<ReturnStatementNode>();
        ret->value = std::move(call);
        // parameter v
        auto p = std::make_unique<ParameterNode>();
        p->name = "v";
        p->token = Token{};
        m->params.push_back(std::move(p));
        m->body.push_back(std::move(ret));
        classDesc->body->methods.push_back(std::move(m));
    }

    // static member: reject
    {
        auto m = std::make_unique<ClassMethodNode>();
        m->name = "reject";
        m->is_static = true;
        m->is_locked = true;
        m->is_private = false;

        auto call = std::make_unique<CallExpressionNode>();
        call->callee = std::make_unique<IdentifierNode>();
        static_cast<IdentifierNode*>(call->callee.get())->name = "Promise_static_reject";
        auto id0 = std::make_unique<IdentifierNode>();
        id0->name = "r";
        call->arguments.push_back(std::move(id0));

        auto ret = std::make_unique<ReturnStatementNode>();
        ret->value = std::move(call);
        auto p = std::make_unique<ParameterNode>();
        p->name = "r";
        p->token = Token{};
        m->params.push_back(std::move(p));
        m->body.push_back(std::move(ret));
        classDesc->body->methods.push_back(std::move(m));
    }

    // native: Promise.all([...])
    {
        Token tAll;
        tAll.type = TokenType::IDENTIFIER;
        tAll.loc = TokenLocation("<Promise>", 0, 0, 0);

        auto native_promise_all = [evaluator](const std::vector<Value>& args, EnvPtr /*env*/, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "Promise.all requires an array argument", token.loc);
            }
            if (!std::holds_alternative<ArrayPtr>(args[0])) {
                throw SwaziError("TypeError", "Promise.all requires an array argument", token.loc);
            }

            ArrayPtr input = std::get<ArrayPtr>(args[0]);
            if (!input) {
                // treat null array as empty -> resolve to []
                auto empty = std::make_shared<ArrayValue>();
                return Value{empty};
            }

            size_t n = input->elements.size();
            auto outPromise = std::make_shared<PromiseValue>();
            outPromise->state = PromiseValue::State::PENDING;

            // Fast-path: empty iterable -> resolve immediately to empty array
            if (n == 0) {
                auto results_empty = std::make_shared<ArrayValue>();
                evaluator->fulfill_promise(outPromise, Value{results_empty});
                return outPromise;
            }

            // Shared results array and remaining count (shared so lambdas can capture safely)
            auto results = std::make_shared<ArrayValue>();
            results->elements.resize(n);
            auto remaining = std::make_shared<size_t>(n);
            // Flag to ensure only first rejection wins
            auto settled = std::make_shared<std::atomic<bool>>(false);

            for (size_t i = 0; i < n; ++i) {
                Value v = input->elements[i];

                // If it's a Promise, attach handlers
                if (std::holds_alternative<PromisePtr>(v)) {
                    PromisePtr ip = std::get<PromisePtr>(v);
                    if (!ip) {
                        // treat null promise as undefined fulfilled
                        results->elements[i] = std::monostate{};
                        if (--(*remaining) == 0) {
                            evaluator->fulfill_promise(outPromise, Value{results});
                        }
                        continue;
                    }

                    // If already fulfilled or rejected, handle immediately
                    if (ip->state == PromiseValue::State::FULFILLED) {
                        results->elements[i] = ip->result;
                        if (--(*remaining) == 0) {
                            evaluator->fulfill_promise(outPromise, Value{results});
                        }
                        continue;
                    }
                    if (ip->state == PromiseValue::State::REJECTED) {
                        // immediate reject the aggregate promise (first rejection wins)
                        if (!settled->exchange(true)) {
                            evaluator->reject_promise(outPromise, ip->result);
                        }
                        return outPromise;
                    }

                    // attach then & catch callbacks
                    // then: store value at index, when all done fulfill outPromise
                    ip->then_callbacks.push_back([evaluator, outPromise, results, remaining, i, settled](Value got) {
                        // if already settled by a rejection, ignore
                        if (settled->load()) return;
                        results->elements[i] = got;
                        if (--(*remaining) == 0) {
                            evaluator->fulfill_promise(outPromise, Value{results});
                        }
                    });

                    // catch: reject the aggregate promise immediately
                    ip->catch_callbacks.push_back([evaluator, outPromise, settled](Value reason) {
                        if (!settled->exchange(true)) {
                            evaluator->reject_promise(outPromise, reason);
                        }
                    });

                    // mark the observed promise (and ancestors) as having handlers so it's not considered "unhandled"
                    evaluator->mark_promise_and_ancestors_handled(ip);

                } else {
                    // non-promise value counts as already fulfilled
                    results->elements[i] = v;
                    if (--(*remaining) == 0) {
                        evaluator->fulfill_promise(outPromise, Value{results});
                    }
                }
            }

            return outPromise;
        };

        auto fn_all = std::make_shared<FunctionValue>(std::string("native:Promise.all"), native_promise_all, nullptr, tAll);

        // Register as a global helper named "Promise_all" (fallback) so it's always available.
        // Best effort: also attach as a static method on the Promise constructor if a Promise binding exists.
        {
            Environment::Variable var_all;
            var_all.value = fn_all;
            var_all.is_constant = true;
            env->set("Promise_all", var_all);
        }

        // Attempt to attach as Promise.all if a Promise value is present in the globals.
        auto it = env->values.find("Promise");
        if (it != env->values.end()) {
            // If Promise is an object (ObjectPtr) we can add a property "all"
            if (std::holds_alternative<ObjectPtr>(it->second.value)) {
                ObjectPtr pobj = std::get<ObjectPtr>(it->second.value);
                pobj->properties["all"] = PropertyDescriptor{Value{fn_all}, false, false, false, tAll};
            } else if (std::holds_alternative<ClassPtr>(it->second.value)) {
                // If Promise is represented as a ClassPtr with a static_table, attach to that table
                ClassPtr c = std::get<ClassPtr>(it->second.value);
                if (c && c->static_table) {
                    c->static_table->properties["all"] = PropertyDescriptor{Value{fn_all}, false, false, false, tAll};
                }
            } else if (std::holds_alternative<FunctionPtr>(it->second.value)) {
                // If Promise is a raw function constructor, we cannot attach properties directly;
                // some runtimes expose constructor as an object. As a conservative fallback we also
                // set Promise_all (above) so callers can use Promise_all([...]) until you attach
                // to the constructor in the appropriate place where constructor object is created.
            }
        }
    }

    // register Promise class in the environment
    {
        Environment::Variable var;
        var.value = classDesc;
        var.is_constant = true;
        env->set("Promise", var);
    }

    {
        // Ensure we have an ObjectValue to host the static methods:
        ObjectPtr promise_holder = nullptr;

        if (env->has("Promise")) {
            Value pv = env->get("Promise").value;
            if (std::holds_alternative<ObjectPtr>(pv)) {
                promise_holder = std::get<ObjectPtr>(pv);
            } else if (std::holds_alternative<ClassPtr>(pv)) {
                ClassPtr cp = std::get<ClassPtr>(pv);
                if (cp) promise_holder = cp->static_table;
            }
        }

        // If no existing Promise object/class, create a plain object and bind it as Promise
        if (!promise_holder) {
            promise_holder = std::make_shared<ObjectValue>();
            Environment::Variable v;
            v.value = promise_holder;
            v.is_constant = true;
            env->set("Promise", v);
        }

        // Attach the static resolve/reject helpers that use evaluator helpers
        Token tResolve;
        tResolve.type = TokenType::IDENTIFIER;
        tResolve.loc = TokenLocation("<builtin:Promise.resolve>", 0, 0, 0);

        auto fnResolve = std::make_shared<FunctionValue>(std::string("native:Promise.resolve"), native_Promise_static_resolve, nullptr, tResolve);
        promise_holder->properties["resolve"] = PropertyDescriptor{fnResolve, false, false, false, tResolve};

        Token tReject;
        tReject.type = TokenType::IDENTIFIER;
        tReject.loc = TokenLocation("<builtin:Promise.reject>", 0, 0, 0);

        auto fnReject = std::make_shared<FunctionValue>(std::string("native:Promise.reject"), native_Promise_static_reject, nullptr, tReject);
        promise_holder->properties["reject"] = PropertyDescriptor{fnReject, false, false, false, tReject};

        // Attach Promise.all to the same promise_holder as resolve/reject if the
        // fallback Promise_all was registered earlier. This ensures Promise.all()
        // is available as a static method like Promise.resolve / Promise.reject.
        Token tAllBuiltin;
        tAllBuiltin.type = TokenType::IDENTIFIER;
        tAllBuiltin.loc = TokenLocation("<builtin:Promise.all>", 0, 0, 0);

        if (env->has("Promise_all")) {
            Value pa = env->get("Promise_all").value;
            if (std::holds_alternative<FunctionPtr>(pa)) {
                FunctionPtr fnAll = std::get<FunctionPtr>(pa);
                promise_holder->properties["all"] = PropertyDescriptor{fnAll, false, false, false, tAllBuiltin};
            }
        }
    }

    // -----------------------
    // End Promise runtime
    // -----------------------
}