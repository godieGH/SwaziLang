// src/evaluator/statementEval.cpp
#include "evaluator.hpp"
#include <iostream>
#include <cmath>
#include <stdexcept>
#include <sstream>

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
      if (vd->is_constant && std::holds_alternative < std::monostate > (val)) {
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
