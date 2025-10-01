// src/evaluator/FunctionCall.cpp
#include "evaluator.hpp"
#include "ClassRuntime.hpp"
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <cmath>


Value Evaluator::call_function(FunctionPtr fn, const std::vector<Value>& args, const Token& callToken) {
   if (!fn) throw std::runtime_error("Attempt to call null function at " + callToken.loc.to_string());

   // Native function: call the C++ implementation directly.
   if (fn->is_native) {
       if (!fn->native_impl) throw std::runtime_error("Native function has no implementation at " + callToken.loc.to_string());
       // Note: we pass fn->closure as the env argument to native_impl; native_impl may
       // choose to create/bind its own environment if desired.
       return fn->native_impl(args, fn->closure, callToken);
   }

   // Non-native (interpreted) function: enforce arity check like your original implementation.
   if (args.size() < fn->parameters.size()) {
      std::ostringstream ss;
      ss << "Function '" << (fn->name.empty() ? "<anonymous>": fn->name)
         << "' expects " << fn->parameters.size() << " arguments but got " << args.size()
         << " at " << callToken.loc.to_string();
      throw std::runtime_error(ss.str());
   }

   auto local = std::make_shared<Environment>(fn->closure);

   for (size_t i = 0; i < fn->parameters.size(); ++i) {
      Environment::Variable var;
      var.value = args[i];
      var.is_constant = false;
      local->set(fn->parameters[i], var);
   }

   Value ret_val = std::monostate {};
   bool did_return = false;

   for (auto &stmt_uptr: fn->body->body) {
      evaluate_statement(stmt_uptr.get(), local, &ret_val, &did_return);
      if (did_return) break;
   }

   return did_return ? ret_val: std::monostate {};
}

Value Evaluator::call_function_with_receiver(FunctionPtr fn, ObjectPtr receiver, const std::vector<Value>& args, const Token& callToken) {
    if (!fn) throw std::runtime_error("Attempt to call null function at " + callToken.loc.to_string());

    // Native function case: forward (native_impl receives closure; if native needs 'this', it can get it from args or closure)
    if (fn->is_native) {
        if (!fn->native_impl) throw std::runtime_error("Native function has no implementation at " + callToken.loc.to_string());
        // native_impl receives fn->closure. If native needs $ it can be passed via closure or special conventions.
        return fn->native_impl(args, fn->closure, callToken);
    }

    if (args.size() < fn->parameters.size()) {
       std::ostringstream ss;
       ss << "Function '" << (fn->name.empty() ? "<anonymous>": fn->name)
          << "' expects " << fn->parameters.size() << " arguments but got " << args.size()
          << " at " << callToken.loc.to_string();
       throw std::runtime_error(ss.str());
    }

    // create local environment whose parent is the function's closure
    auto local = std::make_shared<Environment>(fn->closure);

    // Bind '$' to receiver so methods / getters can access private fields using existing checks
    Environment::Variable thisVar;
    thisVar.value = receiver;
    thisVar.is_constant = false;
    local->set("$", thisVar);

    for (size_t i = 0; i < fn->parameters.size(); ++i) {
       Environment::Variable var;
       var.value = args[i];
       var.is_constant = false;
       local->set(fn->parameters[i], var);
    }

    Value ret_val = std::monostate {};
    bool did_return = false;

    for (auto &stmt_uptr: fn->body->body) {
       evaluate_statement(stmt_uptr.get(), local, &ret_val, &did_return);
       if (did_return) break;
    }

    return did_return ? ret_val: std::monostate {};
}