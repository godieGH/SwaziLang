#include "evaluator.hpp"
#include <iostream>
#include <stdexcept>

Value Evaluator::evaluate_expression(ExpressionNode* expr) {
    if (auto node = dynamic_cast<NumericLiteralNode*>(expr)) {
        return node->value;
    }
    if (auto node = dynamic_cast<StringLiteralNode*>(expr)) {
        return node->value;
    }
    if (auto node = dynamic_cast<BooleanLiteralNode*>(expr)) {
        return node->value;
    }
    if (auto node = dynamic_cast<IdentifierNode*>(expr)) {
        if (environment.count(node->name)) {
            return environment[node->name];
        }
        throw std::runtime_error("Undefined variable: " + node->name);
    }
    throw std::runtime_error("Unknown expression type");
}

void Evaluator::evaluate_statement(StatementNode* stmt) {
    if (auto node = dynamic_cast<VariableDeclarationNode*>(stmt)) {
        Value value = evaluate_expression(node->value.get());
        environment[node->identifier] = value;
    } else if (auto node = dynamic_cast<PrintStatementNode*>(stmt)) {
        Value value = evaluate_expression(node->expression.get());
        // Use std::visit to print the correct type from the variant
        std::visit([](const auto& arg){ std::cout << arg << std::endl; }, value);
    }
}

void Evaluator::evaluate(ProgramNode* program) {
    for (const auto& stmt : program->body) {
        evaluate_statement(stmt.get());
    }
}
