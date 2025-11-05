#pragma once
#include <cmath>
#include <memory>
#include <sstream>
#include <string>
#include <variant>

#include "evaluator.hpp"

void init_globals(EnvPtr env);

inline bool value_to_bool(const Value& v) {
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

// Convert Value -> number
inline double value_to_number(const Value& v) {
    if (std::holds_alternative<double>(v)) return std::get<double>(v);
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? 1.0 : 0.0;
    if (std::holds_alternative<std::string>(v)) {
        try {
            return std::stod(std::get<std::string>(v));
        } catch (...) {
            return std::nan("");  // JS-like NaN
        }
    }
    return 0.0;  // default
}

inline std::string value_to_string(const Value& v) {
    if (std::holds_alternative<std::monostate>(v)) return "null";
    if (std::holds_alternative<double>(v)) {
        double num = std::get<double>(v);
        if (std::floor(num) == num) {
            // it’s an integer in disguise
            return std::to_string(static_cast<long long>(num));
        } else {
            // a real decimal
            std::ostringstream oss;
            oss << num;  // default precision, won’t add useless .000000
            return oss.str();
        }
    }
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "kweli" : "sikweli";
    if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
    if (std::holds_alternative<ArrayPtr>(v)) return "[orodha]";
    if (std::holds_alternative<ObjectPtr>(v)) return "{object}";
    if (std::holds_alternative<FunctionPtr>(v)) return "[kazi]";
    if (std::holds_alternative<ClassPtr>(v)) return "<muundo>";
    return "unknown";
}
