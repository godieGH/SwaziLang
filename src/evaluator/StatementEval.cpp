#include "evaluator.hpp"
#include <iostream>
#include <cmath>
#include <stdexcept>
#include <sstream>
#include <string>
#include <stdexcept>

static std::string to_property_key(const Value &v) {
   // string first
   if (auto ps = std::get_if < std::string > (&v)) {
      return *ps;
   }

   // number -> canonical integer if whole, otherwise decimal string
   if (auto pd = std::get_if < double > (&v)) {
      double d = *pd;
      if (!std::isfinite(d)) {
         throw std::runtime_error("Invalid number for property key");
      }
      double floor_d = std::floor(d);
      if (d == floor_d) {
         // whole number — print as integer to match typical JS-like semantics
         return std::to_string(static_cast<long long > (d));
      }
      return std::to_string(d);
   }

   // boolean
   if (auto pb = std::get_if < bool > (&v)) {
      return *pb ? "true": "false";
   }

   // null/undefined handling: depending on your Value types implement accordingly.
   // If you have a NullTag/UndefinedTag type, handle here (example below is generic)
   // if (std::holds_alternative<NullPtr>(v)) return "null";

   throw std::runtime_error("Cannot convert value to property key");
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
      if (vd->is_constant && std::holds_alternative < std::monostate > (val)) {
         throw std::runtime_error("Constant '" + vd->identifier + "' must be initialized at " + vd->token.loc.to_string());
      }
      env->set(vd->identifier, var);
      return;
   }

   // Assignment: target is now an ExpressionNode (IdentifierNode / IndexExpressionNode / MemberExpressionNode)
   if (auto an = dynamic_cast<AssignmentNode*>(stmt)) {
      Value rhs = evaluate_expression(an->value.get(), env);

      // Identifier target: update variable in enclosing environment (search up chain) or create in current env
      if (auto id = dynamic_cast<IdentifierNode*>(an->target.get())) {
         EnvPtr walk = env;
         while (walk) {
            auto it = walk->values.find(id->name);
            if (it != walk->values.end()) {
               if (it->second.is_constant) throw std::runtime_error("Cannot assign to constant '" + id->name + "' at " + id->token.loc.to_string());
               it->second.value = rhs;
               return;
            }
            walk = walk->parent;
         }
         // not found -> create in current env
         Environment::Variable var {
            rhs,
            false
         };
         env->set(id->name, var);
         return;
      }

      if (auto idx = dynamic_cast<IndexExpressionNode*>(an->target.get())) {
         Value objVal = evaluate_expression(idx->object.get(), env);
         Value indexVal = evaluate_expression(idx->index.get(), env);

         // array path (unchanged)
         if (std::holds_alternative < ArrayPtr > (objVal)) {
            long long rawIndex = static_cast<long long > (to_number(indexVal));
            ArrayPtr arr = std::get < ArrayPtr > (objVal);
            if (!arr) {
               throw std::runtime_error("Cannot assign into null array at " + idx->token.loc.to_string());
            }
            if (rawIndex < 0) throw std::runtime_error("Negative array index not supported at " + idx->token.loc.to_string());
            size_t uidx = static_cast<size_t > (rawIndex);
            if (uidx >= arr->elements.size()) arr->elements.resize(uidx + 1);
            arr->elements[uidx] = rhs;
            return;
         }

         // object property path: o[key] = rhs
         if (std::holds_alternative < ObjectPtr > (objVal)) {
            ObjectPtr op = std::get < ObjectPtr > (objVal);
            std::string prop = to_property_key(indexVal); // your helper to convert index to string

            auto it = op->properties.find(prop);
            if (it != op->properties.end()) {
               if (it->second.is_private) {
                  // only allow if current '$' / '$this' equals the same object
                  bool allowed = false;
                  if (env) {
                     if (env->has("$")) {
                        Value thisVal = env->get("$").value;
                        if (std::holds_alternative < ObjectPtr > (thisVal) && std::get < ObjectPtr > (thisVal) == op) allowed = true;
                     }
                     if (!allowed && env->has("$this")) {
                        Value thisVal = env->get("$this").value;
                        if (std::holds_alternative < ObjectPtr > (thisVal) && std::get < ObjectPtr > (thisVal) == op) allowed = true;
                     }
                  }
                  if (!allowed) {
                     throw std::runtime_error("Cannot assign to private property '" + prop + "' from outside at " + idx->token.loc.to_string());
                  }
               }
               if (it->second.is_readonly) {
                  throw std::runtime_error("Cannot assign to read-only property '" + prop + "' at " + idx->token.loc.to_string());
               }

               // update existing descriptor
               it->second.value = rhs;
               it->second.token = idx->token;
            } else {
               // new public property
               PropertyDescriptor desc;
               desc.value = rhs;
               desc.is_private = false;
               desc.is_readonly = false;
               desc.token = idx->token;
               op->properties[prop] = std::move(desc);
            }
            return;
         }

         throw std::runtime_error("Attempted index assignment on non-array/non-object at " + idx->token.loc.to_string());
      }

      if (auto mem = dynamic_cast<MemberExpressionNode*>(an->target.get())) {
         Value objVal = evaluate_expression(mem->object.get(), env);
         if (!std::holds_alternative < ObjectPtr > (objVal)) {
            throw std::runtime_error("Member assignment on non-object at " + mem->token.loc.to_string());
         }
         ObjectPtr op = std::get < ObjectPtr > (objVal);

         // use the rhs already evaluated earlier: Value rhs = evaluate_expression(an->value.get(), env);
         auto it = op->properties.find(mem->property);
         if (it != op->properties.end()) {
            // existing property - enforce privacy / readonly
            if (it->second.is_private) {
               // allow only when current '$' / '$this' points to the same object
               bool allowed = false;
               if (env) {
                  if (env->has("$")) {
                     Value thisVal = env->get("$").value;
                     if (std::holds_alternative < ObjectPtr > (thisVal) && std::get < ObjectPtr > (thisVal) == op) allowed = true;
                  }
                  if (!allowed && env->has("$this")) {
                     Value thisVal = env->get("$this").value;
                     if (std::holds_alternative < ObjectPtr > (thisVal) && std::get < ObjectPtr > (thisVal) == op) allowed = true;
                  }
               }
               if (!allowed) {
                  throw std::runtime_error("Cannot assign to private property '" + mem->property + "' from outside at " + mem->token.loc.to_string());
               }
            }

            if (it->second.is_readonly) {
               throw std::runtime_error("Cannot assign to read-only property '" + mem->property + "' at " + mem->token.loc.to_string());
            }

            // allowed: update in-place (preserve flags)
            it->second.value = rhs;
            it->second.token = mem->token;
            return;
         } else {
            // not found -> create new public property (external code must not create private '@' properties)
            PropertyDescriptor desc;
            desc.value = rhs;
            desc.is_private = false;
            desc.is_readonly = false;
            desc.token = mem->token;
            op->properties[mem->property] = std::move(desc);
            return;
         }
      }

      throw std::runtime_error("Unsupported assignment target at " + an->token.loc.to_string());
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
      auto forEnv = std::make_shared < Environment > (env);

      if (fn->init) {
         evaluate_statement(fn->init.get(), forEnv, nullptr, nullptr);
      }

      while (true) {
         bool condition_met = true;
         if (fn->condition) {
            Value condVal = evaluate_expression(fn->condition.get(), forEnv);
            condition_met = to_bool(condVal);
         }
         if (!condition_met) break;

         auto bodyEnv = std::make_shared < Environment > (forEnv);
         for (auto &s: fn->body) {
            evaluate_statement(s.get(), bodyEnv, return_value, did_return);
            if (did_return && *did_return) return;
         }

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
            if (did_return && *did_return) return;
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
            if (did_return && *did_return) return;
         }
         condVal = evaluate_expression(dwn->condition.get(), bodyEnv);
      } while (to_bool(condVal));
      return;
   }

   throw std::runtime_error("Unhandled statement node in evaluator at " + stmt->token.loc.to_string());
}