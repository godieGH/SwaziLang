#include "evaluator.hpp"
#include <iostream>
#include <cmath>
#include <stdexcept>
#include <sstream>

// Evaluate a single expression using the evaluator's global_env.
// Delegates to the existing private evaluate_expression(expr, env).
Value Evaluator::evaluate_expression(ExpressionNode* expr) {
    // Choose environment for interactive evaluation:
    // - If main_module_env exists (we're running the main program), use that.
    // - Otherwise create (once) and use a persistent repl_env childed to global_env so REPL
    //   top-level bindings persist and do not leak into builtins/global_env.
    EnvPtr run_env = nullptr;
    if (main_module_env) {
        run_env = main_module_env;
    } else {
        if (!repl_env) {
            // create persistent REPL env parented to global_env
            repl_env = std::make_shared<Environment>(global_env);
            // populate REPL metadata (optional, helpful for error traces)
            populate_module_metadata(repl_env, std::string(), std::string("<repl>"), true);
        }
        run_env = repl_env;
    }

    return evaluate_expression(expr, run_env); // delegate to the two-arg evaluator
}
std::string Evaluator::value_to_string(const Value& v) {
   return to_string_value(v); // reuse your existing private formatter
}

bool Evaluator::is_void(const Value& v) {
   return std::holds_alternative < std::monostate > (v);
}