#pragma once
#include "ast.hpp"
#include <string>
#include <variant>
#include <unordered_map>

// Our language's value types
using Value = std::variant<double, std::string, bool>;

class Evaluator {
public:
    void evaluate(ProgramNode* program);

private:
    std::unordered_map<std::string, Value> environment;

    Value evaluate_expression(ExpressionNode* expr);
    void evaluate_statement(StatementNode* stmt);
};
