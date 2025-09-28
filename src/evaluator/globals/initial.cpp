#include "globals.hpp"
#include "evaluator.hpp"
#include <iostream>
#include <cmath>

static Value builtin_ainaya(const std::vector < Value>& args, EnvPtr env, const Token& tok) {
   if (args.empty()) {
      return std::string("unknown");
   }

   const Value& v = args[0];

   if (std::holds_alternative < double > (v)) return std::string("namba");
   if (std::holds_alternative < bool > (v)) return std::string("bool");
   if (std::holds_alternative < std::string > (v)) return std::string("neno");
   if (std::holds_alternative < ObjectPtr > (v)) return std::string("object");
   if (std::holds_alternative < ArrayPtr > (v)) return std::string("orodha");
   if (std::holds_alternative < FunctionPtr > (v)) return std::string("kazi");

   return std::string("unknown");
}
static Value builtin_orodha(const std::vector < Value>& args, EnvPtr env, const Token& tok) {
   auto arr = std::make_shared < ArrayValue > ();

   if (args.empty()) {
      return arr;
   }

   if (args.size() == 1) {
      const Value& first = args[0];

      if (std::holds_alternative < double > (first)) {
         // case: Orodha(5) -> array of length 5, filled with empty values
         int len = static_cast<int > (std::get < double > (first));
         if (len < 0) len = 0;
         arr->elements.resize(len); // default-constructed Values (monostate)
         return arr;
      }

      if (std::holds_alternative < ArrayPtr > (first)) {
         // case: Orodha([1,2,3]) -> copy
         ArrayPtr src = std::get < ArrayPtr > (first);
         arr->elements = src->elements;
         return arr;
      }
   }

   // default case: Orodha(6,8,5,8) or any other list of arguments
   arr->elements = args;
   return arr;
}
static Value builtin_bool(const std::vector < Value>& args, EnvPtr env, const Token& tok) {
   return args.empty() ? false: value_to_bool(args[0]);
}

static Value builtin_namba(const std::vector < Value>& args, EnvPtr env, const Token& tok) {
   return args.empty() ? 0.0: value_to_number(args[0]);
}

static Value builtin_neno(const std::vector < Value>& args, EnvPtr env, const Token& tok) {
   return args.empty() ? std::string(""): value_to_string(args[0]);
}

static Value builtin_object_keys(const std::vector < Value>& args, EnvPtr env, const Token& tok) {
   auto arr = std::make_shared < ArrayValue > ();
   if (args.empty() || !std::holds_alternative < ObjectPtr > (args[0])) return arr;

   ObjectPtr obj = std::get < ObjectPtr > (args[0]);
   for (auto &pair: obj->properties) {
      arr->elements.push_back(pair.first); // push key as string
   }
   return arr;
}

static Value builtin_object_values(const std::vector < Value>& args, EnvPtr env, const Token& tok) {
   auto arr = std::make_shared < ArrayValue > ();
   if (args.empty() || !std::holds_alternative < ObjectPtr > (args[0])) return arr;

   ObjectPtr obj = std::get < ObjectPtr > (args[0]);
   for (auto &pair: obj->properties) {
      arr->elements.push_back(pair.second.value);
   }
   return arr;
}

static Value builtin_object_entry(const std::vector < Value>& args, EnvPtr env, const Token& tok) {
   auto arr = std::make_shared < ArrayValue > ();
   if (args.empty() || !std::holds_alternative < ObjectPtr > (args[0])) return arr;

   ObjectPtr obj = std::get < ObjectPtr > (args[0]);
   for (auto &pair: obj->properties) {
      auto entry = std::make_shared < ArrayValue > ();
      entry->elements.push_back(pair.first); // key
      entry->elements.push_back(pair.second.value); // value
      arr->elements.push_back(entry);
   }
   return arr;
}




void init_globals(EnvPtr env) {
   if (!env) return;

   auto add_fn = [&](const std::string& name,
      std::function < Value(const std::vector < Value>&, EnvPtr, const Token&) > impl) {
      auto fn = std::make_shared < FunctionValue > (name, impl, env, Token {});
      Environment::Variable var {
         fn,
         true
      };
      env->set(name, var);
   };

   // Existing builtins
   add_fn("ainaya", builtin_ainaya);
   add_fn("Orodha", builtin_orodha);
   add_fn("Bool", builtin_bool);
   add_fn("Namba", builtin_namba);
   add_fn("Neno", builtin_neno);

   auto objectVal = std::make_shared < ObjectValue > ();

   {
      auto fn = std::make_shared < FunctionValue > ("keys", builtin_object_keys, env, Token {});
      objectVal->properties["keys"] = {
         fn,
         false,
         false,
         true,
         Token {}
      };
   }
   {
      auto fn = std::make_shared < FunctionValue > ("values", builtin_object_values, env, Token {});
      objectVal->properties["values"] = {
         fn,
         false,
         false,
         true,
         Token {}
      };
   }
   {
      auto fn = std::make_shared < FunctionValue > ("entry", builtin_object_entry, env, Token {});
      objectVal->properties["entry"] = {
         fn,
         false,
         false,
         true,
         Token {}
      };
   }


   Environment::Variable objectVar;
   objectVar.value = objectVal;
   objectVar.is_constant = true;
   env->set("Object", objectVar);
}