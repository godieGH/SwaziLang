#pragma once

#include "ast.hpp"
#include <string>
#include <variant>
#include <unordered_map>

// Our language's value types
using Value = std::variant<double, std::string, bool>;

class Evaluator {
public:
    // Evaluate whole program (takes ownership pointer from parser)
    void evaluate(ProgramNode* program);

private:
    std::unordered_map<std::string, Value> environment;

    // expression / statement evaluators
    Value evaluate_expression(ExpressionNode* expr);
    void evaluate_statement(StatementNode* stmt);

    // helpers
    double to_number(const Value& v);
    std::string to_string_value(const Value& v);
    bool to_bool(const Value& v);
};