#include "evaluator.hpp"
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <cmath>


// ----------------- Function calling -----------------
Value Evaluator::call_function(FunctionPtr fn, const std::vector < Value>& args, const Token& callToken) {
   if (!fn) throw std::runtime_error("Attempt to call null function at " + callToken.loc.to_string());

   if (args.size() < fn->parameters.size()) {
      std::ostringstream ss;
      ss << "Function '" << (fn->name.empty() ? "<anonymous>": fn->name)
      << "' expects " << fn->parameters.size() << " arguments but got " << args.size()
      << " at " << callToken.loc.to_string();
      throw std::runtime_error(ss.str());
   }

   auto local = std::make_shared < Environment > (fn->closure);

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
