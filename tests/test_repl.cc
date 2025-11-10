#include <gtest/gtest.h>

#include <sstream>

#include "evaluator.hpp"
#include "lexer.hpp"
#include "parser.hpp"

// Test basic expression evaluation
TEST(REPLTest, EvaluateSimpleExpression) {
    std::string code = "2 + 3\n";
    Lexer lexer(code, "<test>");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();

    Evaluator evaluator;
    evaluator.set_entry_point("");

    ASSERT_EQ(ast->body.size(), 1);
    auto* exprStmt = dynamic_cast<ExpressionStatementNode*>(ast->body[0].get());
    ASSERT_NE(exprStmt, nullptr);

    Value result = evaluator.evaluate_expression(exprStmt->expression.get());
    EXPECT_FALSE(evaluator.is_void(result));
}

// Test variable assignment
TEST(REPLTest, VariableAssignment) {
    std::string code = "data x = 10\n";
    Lexer lexer(code, "<test>");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();

    Evaluator evaluator;
    EXPECT_NO_THROW(evaluator.evaluate(ast.get()));
}

// Test incomplete input detection
TEST(REPLTest, DetectsIncompleteInput) {
    std::string incomplete = "kama x > 5:\n";
    Lexer lexer(incomplete, "<test>");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);

    EXPECT_THROW({ auto ast = parser.parse(); }, std::exception);
}