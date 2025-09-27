#include "evaluator.hpp"
#include <iostream>
#include <cmath>
#include <stdexcept>
#include <sstream>

// Evaluate a single expression using the evaluator's global_env.
// Delegates to the existing private evaluate_expression(expr, env).
Value Evaluator::evaluate_expression(ExpressionNode* expr) {
   return evaluate_expression(expr, global_env); // calls the private 2-arg 
}

std::string Evaluator::value_to_string(const Value& v) {
   return to_string_value(v); // reuse your existing private formatter
}

bool Evaluator::is_void(const Value& v) {
   return std::holds_alternative < std::monostate > (v);
}