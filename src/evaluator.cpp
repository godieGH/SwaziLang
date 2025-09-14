#include "evaluator.hpp"
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <cmath>

Evaluator::Evaluator()
  : global_env(std::make_shared<Environment>(nullptr)) {
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
    if (std::holds_alternative<std::monostate>(v)) return "void";
    if (std::holds_alternative<double>(v)) return "number";
    if (std::holds_alternative<std::string>(v)) return "string";
    if (std::holds_alternative<bool>(v)) return "boolean";
    if (std::holds_alternative<FunctionPtr>(v)) return "function";
    return "unknown";
}

double Evaluator::to_number(const Value& v) {
    if (std::holds_alternative<double>(v)) return std::get<double>(v);
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? 1.0 : 0.0;
    if (std::holds_alternative<std::string>(v)) {
        const auto &s = std::get<std::string>(v);
        try {
            size_t idx = 0;
            double d = std::stod(s, &idx);
            if (idx == 0) throw std::invalid_argument("no conversion");
            return d;
        } catch (...) {
            throw std::runtime_error("Cannot convert string '" + s + "' to number");
        }
    }
    throw std::runtime_error("Cannot convert value of type " + value_type_name(v) + " to number");
}

std::string Evaluator::to_string_value(const Value& v) {
    if (std::holds_alternative<std::monostate>(v)) return "";
    if (std::holds_alternative<double>(v)) {
        std::ostringstream ss;
        double d = std::get<double>(v);
        // print integers without decimal when appropriate
        if (std::fabs(d - std::round(d)) < 1e-12) ss << (long long)std::llround(d);
        else ss << d;
        return ss.str();
    }
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "kweli" : "sikweli";
    if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
    if (std::holds_alternative<FunctionPtr>(v)) return "<function:" + (std::get<FunctionPtr>(v)->name.empty() ? "<anon>" : std::get<FunctionPtr>(v)->name) + ">";
    return "";
}

bool Evaluator::to_bool(const Value& v) {
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v);
    if (std::holds_alternative<double>(v)) return std::get<double>(v) != 0.0;
    if (std::holds_alternative<std::string>(v)) return !std::get<std::string>(v).empty();
    if (std::holds_alternative<std::monostate>(v)) return false;
    if (std::holds_alternative<FunctionPtr>(v)) return true;
    return false;
}

// ----------------- Function calling -----------------

Value Evaluator::call_function(FunctionPtr fn, const std::vector<Value>& args, const Token& callToken) {
    if (!fn) throw std::runtime_error("Attempt to call null function at " + callToken.loc.to_string());

    // arity check (simple: require at least as many args as parameters)
    if (args.size() < fn->parameters.size()) {
        std::ostringstream ss;
        ss << "Function '" << (fn->name.empty() ? "<anonymous>" : fn->name)
           << "' expects " << fn->parameters.size() << " arguments but got " << args.size()
           << " at " << callToken.loc.to_string();
        throw std::runtime_error(ss.str());
    }

    // Create a new environment with closure as parent (lexical scoping)
    auto local = std::make_shared<Environment>(fn->closure);

    // Bind parameters
    for (size_t i = 0; i < fn->parameters.size(); ++i) {
        Environment::Variable var;
        var.value = args[i];
        var.is_constant = false;
        local->set(fn->parameters[i], var);
    }

    // Execute the function body
    Value ret_val = std::monostate{};
    bool did_return = false;

    for (auto &stmt_uptr : fn->body->body) {
        evaluate_statement(stmt_uptr.get(), local, &ret_val, &did_return);
        if (did_return) break;
    }

    if (did_return) return ret_val;
    // If no return, return void (monostate)
    return std::monostate{};
}

// ----------------- Expression evaluation -----------------

Value Evaluator::evaluate_expression(ExpressionNode* expr, EnvPtr env) {
    if (!expr) return std::monostate{};

    // Numeric literal
    if (auto n = dynamic_cast<NumericLiteralNode*>(expr)) {
        return Value{ n->value };
    }

    // String literal
    if (auto s = dynamic_cast<StringLiteralNode*>(expr)) {
        return Value{ s->value };
    }

    // Boolean literal
    if (auto b = dynamic_cast<BooleanLiteralNode*>(expr)) {
        return Value{ b->value };
    }

    // Identifier
    if (auto id = dynamic_cast<IdentifierNode*>(expr)) {
        if (!env) throw std::runtime_error("No environment when resolving identifier '" + id->name + "'");
        if (!env->has(id->name)) {
            throw std::runtime_error("Undefined identifier '" + id->name + "' at " + id->token.loc.to_string());
        }
        return env->get(id->name).value;
    }

    // Unary
    if (auto u = dynamic_cast<UnaryExpressionNode*>(expr)) {
        Value operand = evaluate_expression(u->operand.get(), env);
        if (u->op == "!" || u->op == "si") {
            return Value{ !to_bool(operand) };
        } else if (u->op == "-") {
            return Value{ -to_number(operand) };
        } else {
            throw std::runtime_error("Unknown unary operator '" + u->op + "' at " + u->token.loc.to_string());
        }
    }

    // Binary
    if (auto b = dynamic_cast<BinaryExpressionNode*>(expr)) {
        Value left = evaluate_expression(b->left.get(), env);
        Value right = evaluate_expression(b->right.get(), env);

        const std::string &op = b->op;

        if (op == "+" ) {
            if (std::holds_alternative<std::string>(left) || std::holds_alternative<std::string>(right)) {
                return Value{ to_string_value(left) + to_string_value(right) };
            }
            return Value{ to_number(left) + to_number(right) };
        } else if (op == "-") {
            return Value{ to_number(left) - to_number(right) };
        } else if (op == "*") {
            return Value{ to_number(left) * to_number(right) };
        } else if (op == "/") {
            return Value{ to_number(left) / to_number(right) };
        } else if (op == "%") {
            double a = to_number(left);
            double b2 = to_number(right);
            return Value{ std::fmod(a, b2) };
        } else if (op == "**") {
            return Value{ std::pow(to_number(left), to_number(right)) };
        } else if (op == "==" || op == "sawa") {
            if (std::holds_alternative<double>(left) && std::holds_alternative<double>(right))
                return Value{ std::get<double>(left) == std::get<double>(right) };
            return Value{ to_string_value(left) == to_string_value(right) };
        } else if (op == "!=" || op == "sisawa") {
            if (std::holds_alternative<double>(left) && std::holds_alternative<double>(right))
                return Value{ std::get<double>(left) != std::get<double>(right) };
            return Value{ to_string_value(left) != to_string_value(right) };
        } else if (op == ">" ) {
            return Value{ to_number(left) > to_number(right) };
        } else if (op == "<" ) {
            return Value{ to_number(left) < to_number(right) };
        } else if (op == ">=" ) {
            return Value{ to_number(left) >= to_number(right) };
        } else if (op == "<=" ) {
            return Value{ to_number(left) <= to_number(right) };
        } else if (op == "&&" || op == "na") {
            return Value{ to_bool(left) && to_bool(right) };
        } else if (op == "||" || op == "au") {
            return Value{ to_bool(left) || to_bool(right) };
        } else {
            throw std::runtime_error("Unknown binary operator '" + op + "' at " + b->token.loc.to_string());
        }
    }

    // Call expression
    if (auto call = dynamic_cast<CallExpressionNode*>(expr)) {
        // Evaluate callee expression first
        Value calleeVal = evaluate_expression(call->callee.get(), env);

        // Evaluate arguments
        std::vector<Value> args;
        for (auto &arg : call->arguments) args.push_back(evaluate_expression(arg.get(), env));

        if (std::holds_alternative<FunctionPtr>(calleeVal)) {
            FunctionPtr fn = std::get<FunctionPtr>(calleeVal);
            return call_function(fn, args, call->token);
        }

        throw std::runtime_error("Attempted to call a non-function value at " + call->token.loc.to_string());
    }

    throw std::runtime_error("Unhandled expression node in evaluator");
}

// ----------------- Statement evaluation -----------------

void Evaluator::evaluate_statement(StatementNode* stmt, EnvPtr env, Value* return_value, bool* did_return) {
    if (!stmt) return;

    // VariableDeclarationNode
    if (auto vd = dynamic_cast<VariableDeclarationNode*>(stmt)) {
        Value val = std::monostate{};
        if (vd->value) val = evaluate_expression(vd->value.get(), env);
        Environment::Variable var;
        var.value = val;
        var.is_constant = vd->is_constant;
        env->set(vd->identifier, var);
        return;
    }

    // AssignmentNode
    if (auto an = dynamic_cast<AssignmentNode*>(stmt)) {
        Value val = evaluate_expression(an->value.get(), env);
        // If variable exists in any env up the chain, update that variable (common semantics)
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
        // Not found: create in current env
        Environment::Variable var;
        var.value = val;
        var.is_constant = false;
        env->set(an->identifier, var);
        return;
    }

    // PrintStatementNode
    if (auto ps = dynamic_cast<PrintStatementNode*>(stmt)) {
        std::string out;
        for (size_t i = 0; i < ps->expressions.size(); ++i) {
            Value v = evaluate_expression(ps->expressions[i].get(), env);
            out += to_string_value(v);
            if (i + 1 < ps->expressions.size()) out += " ";
        }
        if (ps->newline) std::cout << out << std::endl;
        else std::cout << out;
        return;
    }

    // ExpressionStatementNode
    if (auto es = dynamic_cast<ExpressionStatementNode*>(stmt)) {
        evaluate_expression(es->expression.get(), env);
        return;
    }

    // FunctionDeclarationNode
    if (auto fd = dynamic_cast<FunctionDeclarationNode*>(stmt)) {
        // Persist the function AST body into a shared_ptr owned by the evaluator/function value.
        auto persisted = std::make_shared<FunctionDeclarationNode>();
        persisted->name = fd->name;
        persisted->parameters = fd->parameters;
        persisted->token = fd->token;
        // move statements out of the parser-owned node into persisted storage
        persisted->body = std::move(fd->body);

        auto fn = std::make_shared<FunctionValue>(
            persisted->name,
            persisted->parameters,
            persisted,
            env,
            persisted->token
        );
        Environment::Variable var;
        var.value = fn;
        var.is_constant = true; // functions are constants by default
        env->set(persisted->name, var);
        return;
    }

    // ReturnStatementNode
    if (auto rs = dynamic_cast<ReturnStatementNode*>(stmt)) {
        if (did_return) *did_return = true;
        if (return_value) {
            if (rs->value) *return_value = evaluate_expression(rs->value.get(), env);
            else *return_value = std::monostate{};
        }
        return;
    }

    // IfStatementNode (kama / vinginevyo)
    if (auto ifn = dynamic_cast<IfStatementNode*>(stmt)) {
        Value condVal = evaluate_expression(ifn->condition.get(), env);
        bool cond = to_bool(condVal);

        // execute chosen block in a new child environment so local declarations are block-scoped
        if (cond) {
            auto blockEnv = std::make_shared<Environment>(env);
            for (auto &s : ifn->then_body) {
                evaluate_statement(s.get(), blockEnv, return_value, did_return);
                if (did_return && *did_return) return;
            }
        } else if (ifn->has_else) {
            auto blockEnv = std::make_shared<Environment>(env);
            for (auto &s : ifn->else_body) {
                evaluate_statement(s.get(), blockEnv, return_value, did_return);
                if (did_return && *did_return) return;
            }
        }
        return;
    }

    throw std::runtime_error("Unhandled statement node in evaluator at " + stmt->token.loc.to_string());
}

// ----------------- Program evaluation -----------------

void Evaluator::evaluate(ProgramNode* program) {
    if (!program) return;

    // Execute top-level statements
    Value dummy_ret;
    bool did_return = false;
    for (auto &stmt_uptr : program->body) {
        evaluate_statement(stmt_uptr.get(), global_env, &dummy_ret, &did_return);
        if (did_return) break;
    }
}