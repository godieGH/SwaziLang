#include "evaluator.hpp"
#include <iostream>
#include <cmath>
#include <stdexcept>
#include <sstream>
#include <algorithm>
#include <unordered_set>


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
   if (std::holds_alternative < ObjectPtr > (v)) {
      ObjectPtr op = std::get < ObjectPtr > (v);
      if (!op) return "{}";
      return print_object(op); // <- you write this pretty-printer
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
   if (std::holds_alternative < std::monostate > (a) && std::holds_alternative < std::monostate > (b)) return true;

   // numbers
   if (std::holds_alternative < double > (a) && std::holds_alternative < double > (b))
   return std::get < double > (a) == std::get < double > (b);

   // booleans
   if (std::holds_alternative < bool > (a) && std::holds_alternative < bool > (b))
   return std::get < bool > (a) == std::get < bool > (b);

   // strings
   if (std::holds_alternative < std::string > (a) && std::holds_alternative < std::string > (b))
   return std::get < std::string > (a) == std::get < std::string > (b);

   // mixed number <-> string: try numeric compare
   if (std::holds_alternative < double > (a) && std::holds_alternative < std::string > (b)) {
      try {
         double bb = std::stod(std::get < std::string > (b));
         return std::get < double > (a) == bb;
      } catch (...) {
         /* fallthrough */
      }
   }
   if (std::holds_alternative < std::string > (a) && std::holds_alternative < double > (b)) {
      try {
         double aa = std::stod(std::get < std::string > (a));
         return aa == std::get < double > (b);
      } catch (...) {
         /* fallthrough */
      }
   }

   // functions: compare pointer equality
   if (std::holds_alternative < FunctionPtr > (a) && std::holds_alternative < FunctionPtr > (b))
   return std::get < FunctionPtr > (a) == std::get < FunctionPtr > (b);

   // arrays: deep compare
   if (std::holds_alternative < ArrayPtr > (a) && std::holds_alternative < ArrayPtr > (b)) {
      ArrayPtr A = std::get < ArrayPtr > (a);
      ArrayPtr B = std::get < ArrayPtr > (b);
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


bool Evaluator::is_private_access_allowed(ObjectPtr obj, EnvPtr env) {
   if (!env || !obj) return false;
   EnvPtr walk = env;
   while (walk) {
      auto it = walk->values.find("$");
      if (it != walk->values.end()) {
         const Value &v = it->second.value;
         if (std::holds_alternative < ObjectPtr > (v)) {
            ObjectPtr bound = std::get < ObjectPtr > (v);
            if (bound == obj) return true; // same shared_ptr
         }
      }
      walk = walk->parent;
   }
   return false;
}


Value Evaluator::get_object_property(ObjectPtr obj, const std::string &key, EnvPtr env) {
   if (!obj) return std::monostate {};

   auto it = obj->properties.find(key);
   if (it == obj->properties.end()) {
      // missing property -> undefined (or throw if you prefer)
      return std::monostate {};
   }

   const PropertyDescriptor &desc = it->second;

   // private property -> not allowed unless we are inside the same object's context ($ bound)
   if (desc.is_private && !is_private_access_allowed(obj, env)) {
      throw std::runtime_error("Cannot access private property '" + key + "' at " + desc.token.loc.to_string());
   }

   // If property is a readonly getter and value is a function -> call it and return its result
   if (desc.is_readonly && std::holds_alternative < FunctionPtr > (desc.value)) {
      FunctionPtr getter = std::get < FunctionPtr > (desc.value);
      // Call with zero args, preserving token for error messages
      return call_function(getter, {}, desc.token);
   }

   // normal return
   return desc.value;
}

void Evaluator::set_object_property(ObjectPtr obj, const std::string &key, const Value &val, EnvPtr env, const Token &assignToken) {
   if (!obj) throw std::runtime_error("Attempt to assign property on null object at " + assignToken.loc.to_string());

   auto it = obj->properties.find(key);
   if (it != obj->properties.end()) {
      PropertyDescriptor &desc = it->second;
      // cannot assign to private from outside
      if (desc.is_private && !is_private_access_allowed(obj, env)) {
         throw std::runtime_error("Cannot assign to private property '" + key + "' at " + assignToken.loc.to_string());
      }
      // cannot assign to readonly / getter-backed property
      if (desc.is_readonly) {
         throw std::runtime_error("Cannot assign to readonly property '" + key + "' at " + assignToken.loc.to_string());
      }
      desc.value = val;
      // keep private/readonly flags & token as-is
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


std::string Evaluator::print_object(ObjectPtr obj, int indent) {
   if (!obj) return "null";

   // detect cycles by addresses
   std::unordered_set < const ObjectValue*> visited;
   std::function < std::string(ObjectPtr, int) > rec;
   rec = [&](ObjectPtr o, int depth) -> std::string {
      if (!o) return "null";
      const ObjectValue* p = o.get();
      if (visited.count(p)) return "{ /*cycle*/ }";
      visited.insert(p);

      std::ostringstream oss;
      std::string ind(depth, ' ');
      oss << "{\n";
      bool first = true;
      for (const auto &kv: o->properties) {
         const std::string &k = kv.first;
         const PropertyDescriptor &desc = kv.second;

         // skip private properties
         if (desc.is_private) continue;
         // skip methods
         if (std::holds_alternative < FunctionPtr > (desc.value)) continue;

         if (!first) oss << ",\n";
         first = false;

         oss << ind << "  " << k << ": ";
         // nested object?
         if (std::holds_alternative < ObjectPtr > (desc.value)) {
            ObjectPtr child = std::get < ObjectPtr > (desc.value);
            oss << rec(child, depth + 2);
         } else {
            // use your existing to_string_value for primitives/arrays
            oss << to_string_value(desc.value);
         }
      }
      oss << "\n" << ind << "}";
      return oss.str();
   };

   return rec(obj, indent);
}