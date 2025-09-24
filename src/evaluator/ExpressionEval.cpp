#include "evaluator.hpp"
#include <iostream>
#include <cmath>
#include <stdexcept>
#include <sstream>

// ----------------- Expression evaluation -----------------
Value Evaluator::evaluate_expression(ExpressionNode* expr, EnvPtr env) {
   if (!expr) return std::monostate {};

   if (auto n = dynamic_cast<NumericLiteralNode*>(expr)) return Value {
      n->value
   };
   if (auto s = dynamic_cast<StringLiteralNode*>(expr)) return Value {
      s->value
   };

   // Template literal evaluation: concatenate quasis and evaluated expressions.
   if (auto tpl = dynamic_cast<TemplateLiteralNode*>(expr)) {
      // quasis.size() is expected to be expressions.size() + 1, but tolerate mismatches.
      std::string out;
      size_t exprCount = tpl->expressions.size();
      size_t quasiCount = tpl->quasis.size();

      // Iterate through quasis and interleave expression values.
      for (size_t i = 0; i < quasiCount; ++i) {
         out += tpl->quasis[i];
         if (i < exprCount) {
            Value ev = evaluate_expression(tpl->expressions[i].get(), env);
            out += to_string_value(ev);
         }
      }

      return Value { out };
   }

   if (auto b = dynamic_cast<BooleanLiteralNode*>(expr)) return Value {
      b->value
   };

   if (auto id = dynamic_cast<IdentifierNode*>(expr)) {
      if (!env) throw std::runtime_error("No environment when resolving identifier '" + id->name + "'");
      if (!env->has(id->name)) {
         throw std::runtime_error("Undefined identifier '" + id->name + "' at " + id->token.loc.to_string());
      }
      return env->get(id->name).value;
   }

   if (auto u = dynamic_cast<UnaryExpressionNode*>(expr)) {
      Value operand = evaluate_expression(u->operand.get(), env);
      if (u->op == "!" || u->op == "si") return Value {
         !to_bool(operand)
      };
      if (u->op == "-") return Value {
         -to_number(operand)
      };
      throw std::runtime_error("Unknown unary operator '" + u->op + "' at " + u->token.loc.to_string());
   }

   if (auto b = dynamic_cast<BinaryExpressionNode*>(expr)) {

      // --- handle ++ / -- and += / -= as side-effecting ops when left is identifier ---
      if (b->token.type == TokenType::INCREMENT ||
         b->token.type == TokenType::DECREMENT ||
         b->token.type == TokenType::PLUS_ASSIGN ||
         b->token.type == TokenType::MINUS_ASSIGN) {

         // left must be an identifier for these side-effect ops
         if (auto leftIdent = dynamic_cast<IdentifierNode*>(b->left.get())) {
            // Resolve current value (but do NOT shadow lookup semantics: update the variable in its defining env)
            EnvPtr walk = env;
            while (walk) {
               auto it = walk->values.find(leftIdent->name);
               if (it != walk->values.end()) {
                  if (it->second.is_constant) {
                     throw std::runtime_error("Cannot assign to constant '" + leftIdent->name + "' at " + b->token.loc.to_string());
                  }

                  double oldv = to_number(it->second.value);
                  double delta = 0.0;

                  if (b->token.type == TokenType::INCREMENT) {
                     delta = 1.0;
                  } else if (b->token.type == TokenType::DECREMENT) {
                     delta = -1.0;
                  } else {
                     // For += / -= evaluate right side
                     Value rightVal = evaluate_expression(b->right.get(), env);
                     double rv = to_number(rightVal);
                     delta = (b->token.type == TokenType::PLUS_ASSIGN) ? rv: -rv;
                  }

                  double newv = oldv + delta;
                  it->second.value = newv;
                  return Value {
                     newv
                  };
               }
               walk = walk->parent;
            }

            // If not found in any parent, create in current env (same behavior as assignment elsewhere)
            double start = 0.0;
            if (b->token.type == TokenType::INCREMENT) start = 1.0;
            else if (b->token.type == TokenType::DECREMENT) start = -1.0;
            else {
               Value rightVal = evaluate_expression(b->right.get(), env);
               double rv = to_number(rightVal);
               start = (b->token.type == TokenType::PLUS_ASSIGN) ? rv: -rv;
            }
            Environment::Variable var;
            var.value = start;
            var.is_constant = false;
            env->set(leftIdent->name, var);
            return Value {
               start
            };
         }
         // if left isn't identifier fallthrough to normal binary handling (or throw)
      }

      Value left = evaluate_expression(b->left.get(), env);
      Value right = evaluate_expression(b->right.get(), env);
      const std::string &op = b->op;

      if (op == "+") {
         if (std::holds_alternative < std::string > (left) || std::holds_alternative < std::string > (right)) {
            return Value {
               to_string_value(left) + to_string_value(right)
            };
         }
         return Value {
            to_number(left) + to_number(right)
         };
      }
      if (op == "-") return Value {
         to_number(left) - to_number(right)
      };
      if (op == "*") return Value {
         to_number(left) * to_number(right)
      };
      if (op == "/") {
         double r = to_number(right);
         if (r == 0.0) throw std::runtime_error("Division by zero at " + b->token.loc.to_string());
         return Value {
            to_number(left) / r
         };
      }
      if (op == "%") {
         double r = to_number(right);
         if (r == 0.0) throw std::runtime_error("Modulo by zero at " + b->token.loc.to_string());
         return Value {
            std::fmod(to_number(left), r)
         };
      }
      if (op == "**") return Value {
         std::pow(to_number(left), to_number(right))
      };
      if (op == "==" || op == "sawa") {
         if (std::holds_alternative < double > (left) && std::holds_alternative < double > (right))
         return Value {
            std::get < double > (left) == std::get < double > (right)
         };
         // mixed number and string: try numeric compare
         if (std::holds_alternative < double > (left) && std::holds_alternative < std::string > (right)) {
            try {
               double rr = std::stod(std::get < std::string > (right));
               return Value {
                  std::get < double > (left) == rr
               };
            } catch (...) {
               /* fallthrough to string compare */
            }
         }
         if (std::holds_alternative < std::string > (left) && std::holds_alternative < double > (right)) {
            try {
               double ll = std::stod(std::get < std::string > (left));
               return Value {
                  ll == std::get < double > (right)
               };
            } catch (...) {
               /* fallthrough */
            }
         }
         return Value {
            to_string_value(left) == to_string_value(right)
         };
      }
      if (op == "!=" || op == "sisawa") {
         if (std::holds_alternative < double > (left) && std::holds_alternative < double > (right))
         return Value {
            std::get < double > (left) != std::get < double > (right)
         };
         return Value {
            to_string_value(left) != to_string_value(right)
         };
      }
      if (op == ">") return Value {
         to_number(left) > to_number(right)
      };
      if (op == "<") return Value {
         to_number(left) < to_number(right)
      };
      if (op == ">=") return Value {
         to_number(left) >= to_number(right)
      };
      if (op == "<=") return Value {
         to_number(left) <= to_number(right)
      };
      if (op == "&&" || op == "na") return Value {
         to_bool(left) && to_bool(right)
      };
      if (op == "||" || op == "au") return Value {
         to_bool(left) || to_bool(right)
      };

      throw std::runtime_error("Unknown binary operator '" + op + "' at " + b->token.loc.to_string());
   }

   if (auto call = dynamic_cast<CallExpressionNode*>(expr)) {
      Value calleeVal = evaluate_expression(call->callee.get(), env);
      std::vector < Value > args;
      for (auto &arg: call->arguments) args.push_back(evaluate_expression(arg.get(), env));
      if (std::holds_alternative < FunctionPtr > (calleeVal)) {
         return call_function(std::get < FunctionPtr > (calleeVal), args, call->token);
      }
      throw std::runtime_error("Attempted to call a non-function value at " + call->token.loc.to_string());
   }


   if (auto t = dynamic_cast<TernaryExpressionNode*>(expr)) {
      // Evaluate condition first
      Value condVal = evaluate_expression(t->condition.get(), env);
      if (to_bool(condVal)) {
         // condition true → evaluate thenExpr
         return evaluate_expression(t->thenExpr.get(), env);
      } else {
         // condition false → evaluate elseExpr
         return evaluate_expression(t->elseExpr.get(), env);
      }
   }



   throw std::runtime_error("Unhandled expression node in evaluator");
}