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

   // --- compute minimum required arguments (including rest_required_count) ---
   size_t minRequired = 0;
   for (size_t i = 0; i < fn->parameters.size(); ++i) {
      auto &p = fn->parameters[i];
      if (!p) {
         // defensive: treat missing param descriptor as required single slot
         minRequired += 1;
         continue;
      }
      if (p->is_rest) {
         minRequired += p->rest_required_count;
      } else if (!p->defaultValue) {
         minRequired += 1;
      }
   }

   if (args.size() < minRequired) {
      std::ostringstream ss;
      ss << "Function '" << (fn->name.empty() ? "<anonymous>": fn->name)
         << "' expects at least " << minRequired << " argument(s) but got " << args.size()
         << " at " << callToken.loc.to_string();
      throw std::runtime_error(ss.str());
   }

   // create local environment whose parent is the function's closure
   auto local = std::make_shared<Environment>(fn->closure);

   // Bind parameters left-to-right. Rest parameter (if any) collects appropriate args.
   size_t argIndex = 0;
   for (size_t i = 0; i < fn->parameters.size(); ++i) {
      auto &p = fn->parameters[i];

      // defensive: if descriptor missing, bind positionally if available else undefined (skip binding name)
      if (!p) {
         if (argIndex < args.size()) argIndex++;
         continue;
      }

      if (p->is_rest) {
         // If rest_required_count > 0 => capture exactly that many and ignore extras.
         // If rest_required_count == 0 => capture all remaining args.
         auto arr = std::make_shared<ArrayValue>();

         if (p->rest_required_count > 0) {
            size_t remaining = (argIndex < args.size()) ? (args.size() - argIndex) : 0;
            if (remaining < p->rest_required_count) {
               std::ostringstream ss;
               ss << "Function '" << (fn->name.empty() ? "<anonymous>": fn->name)
                  << "' rest parameter '" << p->name << "' requires at least " << p->rest_required_count
                  << " elements but got " << remaining << " at " << callToken.loc.to_string();
               throw std::runtime_error(ss.str());
            }
            // capture exactly rest_required_count elements (ignore extras)
            for (size_t k = 0; k < p->rest_required_count; ++k) {
               arr->elements.push_back(args[argIndex++]);
            }
         } else {
            // capture all remaining
            while (argIndex < args.size()) {
               arr->elements.push_back(args[argIndex++]);
            }
         }

         Environment::Variable var;
         var.value = arr;
         var.is_constant = false;
         local->set(p->name, var);

         // rest must be last (parser should enforce) — but continue defensively
         continue;
      }

      // Non-rest parameter
      Environment::Variable var;
      if (argIndex < args.size()) {
         var.value = args[argIndex++];
      } else {
         // apply default if present, evaluating in 'local' so earlier params are visible
         if (p->defaultValue) {
            var.value = evaluate_expression(p->defaultValue.get(), local);
         } else {
            // missing required positional (shouldn't happen because minRequired was checked)
            std::ostringstream ss;
            ss << "Function '" << (fn->name.empty() ? "<anonymous>": fn->name)
               << "' missing required argument '" << p->name << "' at " << callToken.loc.to_string();
            throw std::runtime_error(ss.str());
         }
      }
      var.is_constant = false;
      local->set(p->name, var);
   }

   // Note: any args not captured by an explicit rest (because rest_required_count consumed only
   // a fixed number) are ignored per the requested semantics.

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

    // compute minimum required arguments (including rest_required_count)
    size_t minRequired = 0;
    for (size_t i = 0; i < fn->parameters.size(); ++i) {
       auto &p = fn->parameters[i];
       if (!p) { minRequired += 1; continue; }
       if (p->is_rest) minRequired += p->rest_required_count;
       else if (!p->defaultValue) minRequired += 1;
    }

    if (args.size() < minRequired) {
       std::ostringstream ss;
       ss << "Function '" << (fn->name.empty() ? "<anonymous>": fn->name)
          << "' expects at least " << minRequired << " arguments but got " << args.size()
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

    // Bind parameters left-to-right, building rest-array for rest param if present
    size_t argIndex = 0;
    for (size_t i = 0; i < fn->parameters.size(); ++i) {
       auto &p = fn->parameters[i];
       if (!p) {
          if (argIndex < args.size()) argIndex++;
          continue;
       }

       if (p->is_rest) {
          auto arr = std::make_shared<ArrayValue>();

          if (p->rest_required_count > 0) {
             size_t remaining = (argIndex < args.size()) ? (args.size() - argIndex) : 0;
             if (remaining < p->rest_required_count) {
                std::ostringstream ss;
                ss << "Function '" << (fn->name.empty() ? "<anonymous>": fn->name)
                   << "' rest parameter '" << p->name << "' requires at least " << p->rest_required_count
                   << " elements but got " << remaining << " at " << callToken.loc.to_string();
                throw std::runtime_error(ss.str());
             }
             // capture exactly rest_required_count arguments and ignore extras
             for (size_t k = 0; k < p->rest_required_count; ++k) {
                arr->elements.push_back(args[argIndex++]);
             }
          } else {
             // capture all remaining
             while (argIndex < args.size()) {
                arr->elements.push_back(args[argIndex++]);
             }
          }

          Environment::Variable var;
          var.value = arr;
          var.is_constant = false;
          local->set(p->name, var);
          continue;
       }

       Environment::Variable var;
       if (argIndex < args.size()) {
          var.value = args[argIndex++];
       } else {
          if (p->defaultValue) {
             var.value = evaluate_expression(p->defaultValue.get(), local);
          } else {
             std::ostringstream ss;
             ss << "Function '" << (fn->name.empty() ? "<anonymous>": fn->name)
                << "' missing required argument '" << p->name << "' at " << callToken.loc.to_string();
             throw std::runtime_error(ss.str());
          }
       }
       var.is_constant = false;
       local->set(p->name, var);
    }

    Value ret_val = std::monostate {};
    bool did_return = false;

    for (auto &stmt_uptr: fn->body->body) {
       evaluate_statement(stmt_uptr.get(), local, &ret_val, &did_return);
       if (did_return) break;
    }

    return did_return ? ret_val: std::monostate {};
}