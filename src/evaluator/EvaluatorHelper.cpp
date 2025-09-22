#include "evaluator.hpp"
#include <iostream>
#include <cmath>
#include <stdexcept>
#include <sstream>


// ----------------- Evaluator helpers -----------------

static std::string value_type_name(const Value& v) {
   if (std::holds_alternative < std::monostate > (v)) return "void";
   if (std::holds_alternative < double > (v)) return "number";
   if (std::holds_alternative < std::string > (v)) return "string";
   if (std::holds_alternative < bool > (v)) return "boolean";
   if (std::holds_alternative < FunctionPtr > (v)) return "function";
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
   return "";
}

bool Evaluator::to_bool(const Value& v) {
   if (std::holds_alternative < bool > (v)) return std::get < bool > (v);
   if (std::holds_alternative < double > (v)) return std::get < double > (v) != 0.0;
   if (std::holds_alternative < std::string > (v)) return !std::get < std::string > (v).empty();
   if (std::holds_alternative < std::monostate > (v)) return false;
   if (std::holds_alternative < FunctionPtr > (v)) return true;
   return false;
}
