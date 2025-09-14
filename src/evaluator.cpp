#include "evaluator.hpp"
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <cmath>

Evaluator::Evaluator(): global_env(std::make_shared < Environment > (nullptr)) {
   // Register builtins here if you want (e.g., chapisha), or leave empty.
}

// ----------------- Environment methods -----------------

bool Environment::has(const std::string& name) const {
   auto it = values.find(name);
   if (it != values.end()) return true;
   if (parent) return parent->has(name);
   return false;
}

Environment::Variable& Environment::get(const std::string& name) {
   auto it = values.find(name);
   if (it != values.end()) return it->second;
   if (parent) return parent->get(name);
   throw std::runtime_error("Undefined variable '" + name + "'");
}

void Environment::set(const std::string& name, const Variable& var) {
   // If variable exists in this environment, replace it here.
   // Otherwise create in current environment (no automatic up-chain assignment).
   values[name] = var;
}

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

// ----------------- Expression evaluation -----------------

Value Evaluator::evaluate_expression(ExpressionNode* expr, EnvPtr env) {
   if (!expr) return std::monostate {};

   if (auto n = dynamic_cast<NumericLiteralNode*>(expr)) return Value {
      n->value
   };
   if (auto s = dynamic_cast<StringLiteralNode*>(expr)) return Value {
      s->value
   };
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
                    delta = (b->token.type == TokenType::PLUS_ASSIGN) ? rv : -rv;
                }

                double newv = oldv + delta;
                it->second.value = newv;
                return Value{ newv };
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
            start = (b->token.type == TokenType::PLUS_ASSIGN) ? rv : -rv;
        }
        Environment::Variable var;
        var.value = start;
        var.is_constant = false;
        env->set(leftIdent->name, var);
        return Value{ start };
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

   throw std::runtime_error("Unhandled expression node in evaluator");
}

// ----------------- Statement evaluation -----------------

void Evaluator::evaluate_statement(StatementNode* stmt, EnvPtr env, Value* return_value, bool* did_return) {
   if (!stmt) return;

   if (auto vd = dynamic_cast<VariableDeclarationNode*>(stmt)) {
      Value val = std::monostate {};
      
      if (vd->value) val = evaluate_expression(vd->value.get(), env);
      Environment::Variable var {
         val,
         vd->is_constant
      };
      if (vd->is_constant && std::holds_alternative<std::monostate>(val)) {
       throw std::runtime_error("Constant '" + vd->identifier + "' must be initialized at " + vd->token.loc.to_string());
      }
      env->set(vd->identifier, var);
      return;
   }

   if (auto an = dynamic_cast<AssignmentNode*>(stmt)) {
      Value val = evaluate_expression(an->value.get(), env);
      EnvPtr walk = env;
      while (walk) {
         auto it = walk->values.find(an->identifier);
         if (it != walk->values.end()) {
            if (it->second.is_constant) throw std::runtime_error("Cannot assign to constant '" + an->identifier + "' at " + an->token.loc.to_string());
            it->second.value = val;
            return;
         }
         walk = walk->parent;
      }
      Environment::Variable var {
         val,
         false
      };
      env->set(an->identifier, var);
      return;
   }

   if (auto ps = dynamic_cast<PrintStatementNode*>(stmt)) {
      std::string out;
      for (size_t i = 0; i < ps->expressions.size(); ++i) {
         out += to_string_value(evaluate_expression(ps->expressions[i].get(), env));
         if (i + 1 < ps->expressions.size()) out += " ";
      }
      if (ps->newline) std::cout << out << std::endl;
      else std::cout << out;
      return;
   }

   if (auto es = dynamic_cast<ExpressionStatementNode*>(stmt)) {
      evaluate_expression(es->expression.get(), env);
      return;
   }

   if (auto fd = dynamic_cast<FunctionDeclarationNode*>(stmt)) {
      auto persisted = std::make_shared < FunctionDeclarationNode > ();
      persisted->name = fd->name;
      persisted->parameters = fd->parameters;
      persisted->token = fd->token;
      persisted->body = std::move(fd->body);

      auto fn = std::make_shared < FunctionValue > (persisted->name, persisted->parameters, persisted, env, persisted->token);
      Environment::Variable var {
         fn,
         true
      };
      env->set(persisted->name, var);
      return;
   }

   if (auto rs = dynamic_cast<ReturnStatementNode*>(stmt)) {
      if (did_return) *did_return = true;
      if (return_value) {
         *return_value = rs->value ? evaluate_expression(rs->value.get(), env): std::monostate {};
      }
      return;
   }

   if (auto ifn = dynamic_cast<IfStatementNode*>(stmt)) {
      Value condVal = evaluate_expression(ifn->condition.get(), env);
      if (to_bool(condVal)) {
         auto blockEnv = std::make_shared < Environment > (env);
         for (auto &s: ifn->then_body) {
            evaluate_statement(s.get(), blockEnv, return_value, did_return);
            if (did_return && *did_return) return;
         }
      } else if (ifn->has_else) {
         auto blockEnv = std::make_shared < Environment > (env);
         for (auto &s: ifn->else_body) {
            evaluate_statement(s.get(), blockEnv, return_value, did_return);
            if (did_return && *did_return) return;
         }
      }
      return;
   }

   // --- NEW: ForStatementNode (kwa) ---
   if (auto fn = dynamic_cast<ForStatementNode*>(stmt)) {
      // The entire loop (init, cond, post, body) lives in a new scope.
      auto forEnv = std::make_shared < Environment > (env);

      // 1. Initializer runs once in the new scope.
      if (fn->init) {
         evaluate_statement(fn->init.get(), forEnv, nullptr, nullptr);
      }

      while (true) {
         // 2. Condition check. Default to true if no condition.
         bool condition_met = true;
         if (fn->condition) {
            Value condVal = evaluate_expression(fn->condition.get(), forEnv);
            condition_met = to_bool(condVal);
         }
         if (!condition_met) {
            break;
         }

         // 3. Body execution in its own sub-scope to isolate declarations per iteration.
         auto bodyEnv = std::make_shared < Environment > (forEnv);
         for (auto &s: fn->body) {
            evaluate_statement(s.get(), bodyEnv, return_value, did_return);
            if (did_return && *did_return) return; // Propagate return upwards
         }

         // 4. Post-expression evaluation.
         if (fn->post) {
            evaluate_expression(fn->post.get(), forEnv);
         }
      }
      return;
   }

   // --- NEW: WhileStatementNode (wakati) ---
   if (auto wn = dynamic_cast<WhileStatementNode*>(stmt)) {
      while (to_bool(evaluate_expression(wn->condition.get(), env))) {
         auto bodyEnv = std::make_shared < Environment > (env);
         for (auto &s: wn->body) {
            evaluate_statement(s.get(), bodyEnv, return_value, did_return);
            if (did_return && *did_return) return; // Propagate return
         }
      }
      return;
   }

   // --- NEW: DoWhileStatementNode (fanya-wakati) ---
   if (auto dwn = dynamic_cast<DoWhileStatementNode*>(stmt)) {
      Value condVal;
      do {
         auto bodyEnv = std::make_shared < Environment > (env);
         for (auto &s: dwn->body) {
            evaluate_statement(s.get(), bodyEnv, return_value, did_return);
            if (did_return && *did_return) return; // Propagate return
         }
         condVal = evaluate_expression(dwn->condition.get(), bodyEnv);
      } while (to_bool(condVal));
      return;
   }

   throw std::runtime_error("Unhandled statement node in evaluator at " + stmt->token.loc.to_string());
}

// ----------------- Program evaluation -----------------

void Evaluator::evaluate(ProgramNode* program) {
   if (!program) return;
   Value dummy_ret;
   bool did_return = false;
   for (auto &stmt_uptr: program->body) {
      evaluate_statement(stmt_uptr.get(), global_env, &dummy_ret, &did_return);
      if (did_return) break;
   }
}