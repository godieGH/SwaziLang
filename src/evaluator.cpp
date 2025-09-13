#include "evaluator.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <cmath>
#include <type_traits>

// Helper: convert Value -> double (throws if not numeric)
double Evaluator::to_number(const Value& v) {
   if (std::holds_alternative<double>(v)) return std::get<double>(v);
   if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? 1.0 : 0.0;
   // strings can't be implicitly converted in arithmetic here
   throw std::runtime_error("Type error: expected number");
}

// Helper: convert Value -> string
std::string Evaluator::to_string_value(const Value& v) {
   if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
   if (std::holds_alternative<double>(v)) {
      std::ostringstream ss;
      ss << std::get<double>(v);
      return ss.str();
   }
   if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "kweli" : "sikweli";
   return {};
}

// Helper: convert Value -> bool (throws if not bool)
bool Evaluator::to_bool(const Value& v) {
   if (std::holds_alternative<bool>(v)) return std::get<bool>(v);
   // permissive conversions for numbers: 0 -> false, non-zero -> true
   if (std::holds_alternative<double>(v)) return std::get<double>(v) != 0.0;
   // strings: non-empty -> true (optional; we keep strict and throw)
   throw std::runtime_error("Type error: expected boolean");
}

static std::string format_location(const Token &t) {
   if (t.filename.empty()) return std::string();
   return " in file '" + t.filename + "' at line " + std::to_string(t.line) + ", column " + std::to_string(t.col);
}

// Evaluate an expression node
Value Evaluator::evaluate_expression(ExpressionNode* expr) {
   if (!expr) throw std::runtime_error("Null expression");

   // small helpers that add location info when converters throw
   auto ensure_number = [&](const Value &v, const Token &tok) -> double {
      try {
         return to_number(v);
      } catch (const std::runtime_error &e) {
         throw std::runtime_error(std::string(e.what()) + format_location(tok));
      }
   };
   auto ensure_bool = [&](const Value &v, const Token &tok) -> bool {
      try {
         return to_bool(v);
      } catch (const std::runtime_error &e) {
         throw std::runtime_error(std::string(e.what()) + format_location(tok));
      }
   };

   // Literals
   if (auto n = dynamic_cast<NumericLiteralNode*>(expr)) {
      return n->value;
   }
   if (auto s = dynamic_cast<StringLiteralNode*>(expr)) {
      return s->value;
   }
   if (auto b = dynamic_cast<BooleanLiteralNode*>(expr)) {
      return b->value;
   }

   // Identifier (variable read)
   if (auto id = dynamic_cast<IdentifierNode*>(expr)) {
      auto it = environment.find(id->name);
      if (it != environment.end()) {
         return it->second.value; // now we read value from Variable
      }
      throw std::runtime_error(
         "Undefined variable '" + id->name + "'" + format_location(id->token)
      );
   }

   // Unary
   if (auto u = dynamic_cast<UnaryExpressionNode*>(expr)) {
      Value v = evaluate_expression(u->operand.get());
      if (u->op == "-" || u->op == "neg") {
         double num = ensure_number(v, u->token);
         return -num;
      }
      if (u->op == "!" || u->op == "not") {
         bool bv = ensure_bool(v, u->token);
         return !bv;
      }
      throw std::runtime_error(
         "Unknown unary operator '" + u->op + "'" + format_location(u->token)
      );
   }

   // Binary
   if (auto bin = dynamic_cast<BinaryExpressionNode*>(expr)) {
      Value left = evaluate_expression(bin->left.get());
      Value right = evaluate_expression(bin->right.get());
      const std::string& op = bin->op;
      const Token &loc = bin->token;

      // Arithmetic: + supports string concatenation if either operand is string
      if (op == "+") {
         if (std::holds_alternative<std::string>(left) || std::holds_alternative<std::string>(right)) {
            return to_string_value(left) + to_string_value(right);
         }
         double a = ensure_number(left, loc);
         double b = ensure_number(right, loc);
         return a + b;
      }
      if (op == "-") {
         double a = ensure_number(left, loc);
         double b = ensure_number(right, loc);
         return a - b;
      }
      if (op == "*") {
         double a = ensure_number(left, loc);
         double b = ensure_number(right, loc);
         return a * b;
      }
      if (op == "/") {
         double a = ensure_number(left, loc);
         double b = ensure_number(right, loc);
         if (b == 0.0) throw std::runtime_error(std::string("Division by zero") + format_location(loc));
         return a / b;
      }
      if (op == "%") {
         double a = ensure_number(left, loc);
         double b = ensure_number(right, loc);
         if (b == 0.0) throw std::runtime_error(std::string("Division by zero (mod)") + format_location(loc));
         return std::fmod(a, b);
      }
      if (op == "**") {
         double a = ensure_number(left, loc);
         double b = ensure_number(right, loc);
         return std::pow(a, b);
      }

      // Comparisons (return bool)
      if (op == ">") {
         if (std::holds_alternative<double>(left) && std::holds_alternative<double>(right)) {
            return std::get<double>(left) > std::get<double>(right);
         }
         if (std::holds_alternative<std::string>(left) && std::holds_alternative<std::string>(right)) {
            return std::get<std::string>(left) > std::get<std::string>(right);
         }
         throw std::runtime_error(std::string("Type error: unsupported '>' operand types") + format_location(loc));
      }
      if (op == ">=") {
         if (std::holds_alternative<double>(left) && std::holds_alternative<double>(right)) {
            return std::get<double>(left) >= std::get<double>(right);
         }
         if (std::holds_alternative<std::string>(left) && std::holds_alternative<std::string>(right)) {
            return std::get<std::string>(left) >= std::get<std::string>(right);
         }
         throw std::runtime_error(std::string("Type error: unsupported '>=' operand types") + format_location(loc));
      }
      if (op == "<") {
         if (std::holds_alternative<double>(left) && std::holds_alternative<double>(right)) {
            return std::get<double>(left) < std::get<double>(right);
         }
         if (std::holds_alternative<std::string>(left) && std::holds_alternative<std::string>(right)) {
            return std::get<std::string>(left) < std::get<std::string>(right);
         }
         throw std::runtime_error(std::string("Type error: unsupported '<' operand types") + format_location(loc));
      }
      if (op == "<=") {
         if (std::holds_alternative<double>(left) && std::holds_alternative<double>(right)) {
            return std::get<double>(left) <= std::get<double>(right);
         }
         if (std::holds_alternative<std::string>(left) && std::holds_alternative<std::string>(right)) {
            return std::get<std::string>(left) <= std::get<std::string>(right);
         }
         throw std::runtime_error(std::string("Type error: unsupported '<=' operand types") + format_location(loc));
      }

      // Equality & inequality (work across types reasonably)
      if (op == "==" || op == "sawa") {
         if (left.index() == right.index()) {
            return left == right;
         }
         return false;
      }
      if (op == "!=" || op == "sisawa") {
         if (left.index() == right.index()) {
            return !(left == right);
         }
         return true;
      }

      // Logical (AND / OR)
      if (op == "&&" || op == "na") {
         bool a = ensure_bool(left, loc);
         if (!a) return false; // short-circuit
         bool b = ensure_bool(right, loc);
         return a && b;
      }
      if (op == "||" || op == "au") {
         bool a = ensure_bool(left, loc);
         if (a) return true; // short-circuit
         bool b = ensure_bool(right, loc);
         return a || b;
      }

      throw std::runtime_error(std::string("Unknown binary operator: ") + op + format_location(loc));
   }

   // Call expression (for builtins)
   if (auto call = dynamic_cast<CallExpressionNode*>(expr)) {
      // expect callee to be identifier for builtins
      if (auto calleeId = dynamic_cast<IdentifierNode*>(call->callee.get())) {
         std::string fname = calleeId->name;
         // evaluate args
         std::vector<Value> args;
         args.reserve(call->arguments.size());
         for (auto &a: call->arguments) args.push_back(evaluate_expression(a.get()));

         if (fname == "chapisha" || fname == "andika") {
            // print args separated by space
            for (size_t i = 0; i < args.size(); ++i) {
               std::cout << to_string_value(args[i]);
               if (i + 1 < args.size()) std::cout << " ";
            }
            if (fname == "chapisha") std::cout << std::endl;
            // return an empty string as the "void" value (safe)
            return std::string("");
         }

         // Other builtins can be added here.
         throw std::runtime_error(std::string("Unknown function: ") + fname + format_location(calleeId->token));
      }
      throw std::runtime_error(std::string("Unsupported call expression (non-identifier callee)") + format_location(call->token));
   }

   // Fallback: include token from expr
   throw std::runtime_error(std::string("Unknown expression type in evaluator") + format_location(expr->token));
}

// Evaluate a statement node
void Evaluator::evaluate_statement(StatementNode* stmt) {
   if (!stmt) return;

   if (auto decl = dynamic_cast<VariableDeclarationNode*>(stmt)) {
      Value v;
      if (decl->value) {
         v = evaluate_expression(decl->value.get());
      } else {
         v = "undefined";
      }

      auto it = environment.find(decl->identifier);
      if (it != environment.end()) {
         if (it->second.is_constant) {
            throw std::runtime_error(std::string("Variable already declared as constant: ") + decl->identifier + format_location(decl->token));
         }
         it->second.value = v;
         it->second.is_constant = decl->is_constant;
         return;
      }

      environment[decl->identifier] = Variable {
         v,
         decl->is_constant
      };
      return;
   }
   
   if (auto assign = dynamic_cast<AssignmentNode*>(stmt)) {
      Value v = evaluate_expression(assign->value.get());
      auto it = environment.find(assign->identifier);
      if (it == environment.end()) {
         // implicit declaration on assignment -> create mutable variable
         environment[assign->identifier] = Variable {
            v,
            false
         };
         return;
      }
      // variable exists; check if constant
      if (it->second.is_constant) {
         throw std::runtime_error(std::string("Cannot assign to constant: ") + assign->identifier + format_location(assign->token));
      }
      // update value
      it->second.value = v; // <-- must assign to .value
      return;
   }

   if (auto print = dynamic_cast<PrintStatementNode*>(stmt)) {
      for (size_t i = 0; i < print->expressions.size(); ++i) {
         Value v = evaluate_expression(print->expressions[i].get());
         std::cout << to_string_value(v);
         if (i + 1 < print->expressions.size()) std::cout << " ";
      }
      if (print->newline) std::cout << std::endl;
      return;
   }

   if (auto exprStmt = dynamic_cast<ExpressionStatementNode*>(stmt)) {
      // evaluate expression for side-effects (calls)
      Value result = evaluate_expression(exprStmt->expression.get());
      // ignore result
      (void)result;
      return;
   }

   throw std::runtime_error(std::string("Unknown statement type in evaluator") + format_location(stmt->token));
}

// Evaluate program
void Evaluator::evaluate(ProgramNode* program) {
   if (!program) return;
   for (auto &s: program->body) {
      evaluate_statement(s.get());
   }
}