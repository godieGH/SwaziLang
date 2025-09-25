#include "evaluator.hpp"
#include <iostream>
#include <cmath>
#include <stdexcept>
#include <sstream>
#include <algorithm>


// ----------------- Evaluator helpers -----------------

static std::string value_type_name(const Value& v) {
   if (std::holds_alternative < std::monostate > (v)) return "void";
   if (std::holds_alternative < double > (v)) return "number";
   if (std::holds_alternative < std::string > (v)) return "string";
   if (std::holds_alternative < bool > (v)) return "boolean";
   if (std::holds_alternative < FunctionPtr > (v)) return "function";
   if (std::holds_alternative < ArrayPtr > (v)) return "array";
   return "unknown";
}

double Evaluator::to_number(const Value& v) {
   if (std::holds_alternative < double > (v)) return std::get < double > (v);
   if (std::holds_alternative < bool > (v)) return std::get < bool > (v) ? 1.0: 0.0;
   if (std::holds_alternative < std::string > (v)) {
      const auto &s = std::get < std::string > (v);
      try {
         size_t idx = 0;
         double d = std::stod(s, &idx);
         if (idx == 0) throw std::invalid_argument("no conversion");
         if (idx != s.size()) throw std::invalid_argument("partial conversion");
         return d;
      } catch (...) {
         throw std::runtime_error("Cannot convert string '" + s + "' to number");
      }
   }
   // Arrays and functions cannot be converted to numbers in current semantics
   throw std::runtime_error("Cannot convert value of type " + value_type_name(v) + " to number");
}

std::string Evaluator::to_string_value(const Value& v) {
   if (std::holds_alternative < std::monostate > (v)) return "";
   if (std::holds_alternative < double > (v)) {
      std::ostringstream ss;
      double d = std::get < double > (v);
      if (std::fabs(d - std::round(d)) < 1e-12) ss << (long long)std::llround(d);
      else ss << d;
      return ss.str();
   }
   if (std::holds_alternative < bool > (v)) return std::get < bool > (v) ? "kweli": "sikweli";
   if (std::holds_alternative < std::string > (v)) return std::get < std::string > (v);
   if (std::holds_alternative < FunctionPtr > (v)) return "<function:" + (std::get < FunctionPtr > (v)->name.empty() ? "<anon>": std::get < FunctionPtr > (v)->name) + ">";
   if (std::holds_alternative < ArrayPtr > (v)) {
      auto arr = std::get < ArrayPtr > (v);
      if (!arr) return "[]";
      std::string out = "[";
      for (size_t i = 0; i < arr->elements.size(); ++i) {
         if (i) out += ", ";
         out += to_string_value(arr->elements[i]);
      }
      out += "]";
      return out;
   }
   return "";
}

bool Evaluator::to_bool(const Value& v) {
   if (std::holds_alternative < bool > (v)) return std::get < bool > (v);
   if (std::holds_alternative < double > (v)) return std::get < double > (v) != 0.0;
   if (std::holds_alternative < std::string > (v)) return !std::get < std::string > (v).empty();
   if (std::holds_alternative < std::monostate > (v)) return false;
   if (std::holds_alternative < FunctionPtr > (v)) return true;
   if (std::holds_alternative < ArrayPtr > (v)) {
      auto arr = std::get < ArrayPtr > (v);
      return arr && !arr->elements.empty();
   }
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
        } catch (...) { /* fallthrough */ }
    }
    if (std::holds_alternative<std::string>(a) && std::holds_alternative<double>(b)) {
        try {
            double aa = std::stod(std::get<std::string>(a));
            return aa == std::get<double>(b);
        } catch (...) { /* fallthrough */ }
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