#include <gtest/gtest.h>

#include <memory>
#include <sstream>

#include "evaluator.hpp"
#include "lexer.hpp"
#include "parser.hpp"

// Helper function to evaluate a source string and return the result
class EvaluatorTestHelper {
   public:
    static Value eval(const std::string& source) {
        Lexer lexer(source + "\n", "test.sl");
        std::vector<Token> tokens = lexer.tokenize();

        Parser parser(tokens);
        auto ast = parser.parse();

        Evaluator evaluator;
        evaluator.evaluate(ast.get());

        // For expression evaluation, we need to capture the last value
        // Since evaluate() doesn't return anything, we'll use evaluate_expression
        // Parse as expression instead
        Parser expr_parser(tokens);
        auto expr_ast = expr_parser.parse();
        if (!expr_ast->body.empty()) {
            if (auto expr_stmt = dynamic_cast<ExpressionStatementNode*>(expr_ast->body[0].get())) {
                return evaluator.evaluate_expression(expr_stmt->expression.get());
            }
        }
        return std::monostate{};
    }

    static std::string evalToString(const std::string& source) {
        Value v = eval(source);
        Evaluator evaluator;
        return evaluator.value_to_string(v);
    }
};

// ============================================================================
// BASIC ARITHMETIC TESTS
// ============================================================================

TEST(EvaluatorTest, EvaluatesSimpleAddition) {
    Value result = EvaluatorTestHelper::eval("5 + 3");

    ASSERT_TRUE(std::holds_alternative<double>(result));
    EXPECT_DOUBLE_EQ(std::get<double>(result), 8.0);
}

TEST(EvaluatorTest, EvaluatesMultiplication) {
    Value result = EvaluatorTestHelper::eval("4 * 7");

    ASSERT_TRUE(std::holds_alternative<double>(result));
    EXPECT_DOUBLE_EQ(std::get<double>(result), 28.0);
}

TEST(EvaluatorTest, EvaluatesComplexExpression) {
    Value result = EvaluatorTestHelper::eval("2 + 3 * 4");

    ASSERT_TRUE(std::holds_alternative<double>(result));
    EXPECT_DOUBLE_EQ(std::get<double>(result), 14.0);
}

TEST(EvaluatorTest, EvaluatesSubtraction) {
    Value result = EvaluatorTestHelper::eval("10 - 3");

    ASSERT_TRUE(std::holds_alternative<double>(result));
    EXPECT_DOUBLE_EQ(std::get<double>(result), 7.0);
}

TEST(EvaluatorTest, EvaluatesDivision) {
    Value result = EvaluatorTestHelper::eval("20 / 4");

    ASSERT_TRUE(std::holds_alternative<double>(result));
    EXPECT_DOUBLE_EQ(std::get<double>(result), 5.0);
}

// ============================================================================
// STRING TESTS
// ============================================================================

TEST(EvaluatorTest, EvaluatesStringLiteral) {
    Value result = EvaluatorTestHelper::eval("'hello'");

    ASSERT_TRUE(std::holds_alternative<std::string>(result));
    EXPECT_EQ(std::get<std::string>(result), "hello");
}

TEST(EvaluatorTest, EvaluatesStringConcatenation) {
    Value result = EvaluatorTestHelper::eval("'Hello' + ' ' + 'World'");

    ASSERT_TRUE(std::holds_alternative<std::string>(result));
    EXPECT_EQ(std::get<std::string>(result), "Hello World");
}

TEST(EvaluatorTest, EvaluatesNumberToStringConcatenation) {
    Value result = EvaluatorTestHelper::eval("'Value: ' + 42");

    ASSERT_TRUE(std::holds_alternative<std::string>(result));
    EXPECT_EQ(std::get<std::string>(result), "Value: 42");
}

// ============================================================================
// BOOLEAN TESTS
// ============================================================================

TEST(EvaluatorTest, EvaluatesBooleanTrue) {
    Value result = EvaluatorTestHelper::eval("kweli");

    ASSERT_TRUE(std::holds_alternative<bool>(result));
    EXPECT_TRUE(std::get<bool>(result));
}

TEST(EvaluatorTest, EvaluatesBooleanFalse) {
    Value result = EvaluatorTestHelper::eval("sikweli");

    ASSERT_TRUE(std::holds_alternative<bool>(result));
    EXPECT_FALSE(std::get<bool>(result));
}

TEST(EvaluatorTest, EvaluatesLogicalAnd) {
    Value result = EvaluatorTestHelper::eval("kweli na kweli");
    ASSERT_TRUE(std::holds_alternative<bool>(result));
    EXPECT_TRUE(std::get<bool>(result));

    result = EvaluatorTestHelper::eval("kweli na sikweli");
    ASSERT_TRUE(std::holds_alternative<bool>(result));
    EXPECT_FALSE(std::get<bool>(result));
}

TEST(EvaluatorTest, EvaluatesLogicalOr) {
    Value result = EvaluatorTestHelper::eval("sikweli au kweli");
    ASSERT_TRUE(std::holds_alternative<bool>(result));
    EXPECT_TRUE(std::get<bool>(result));

    result = EvaluatorTestHelper::eval("sikweli au sikweli");
    ASSERT_TRUE(std::holds_alternative<bool>(result));
    EXPECT_FALSE(std::get<bool>(result));
}

TEST(EvaluatorTest, EvaluatesNegation) {
    Value result = EvaluatorTestHelper::eval("!kweli");
    ASSERT_TRUE(std::holds_alternative<bool>(result));
    EXPECT_FALSE(std::get<bool>(result));
}

// ============================================================================
// COMPARISON TESTS
// ============================================================================

TEST(EvaluatorTest, EvaluatesEquality) {
    Value result = EvaluatorTestHelper::eval("5 == 5");
    ASSERT_TRUE(std::holds_alternative<bool>(result));
    EXPECT_TRUE(std::get<bool>(result));

    result = EvaluatorTestHelper::eval("5 == 3");
    EXPECT_FALSE(std::get<bool>(result));
}

TEST(EvaluatorTest, EvaluatesInequality) {
    Value result = EvaluatorTestHelper::eval("5 != 3");
    ASSERT_TRUE(std::holds_alternative<bool>(result));
    EXPECT_TRUE(std::get<bool>(result));
}

TEST(EvaluatorTest, EvaluatesGreaterThan) {
    Value result = EvaluatorTestHelper::eval("10 > 5");
    ASSERT_TRUE(std::holds_alternative<bool>(result));
    EXPECT_TRUE(std::get<bool>(result));
}

TEST(EvaluatorTest, EvaluatesLessThan) {
    Value result = EvaluatorTestHelper::eval("3 < 7");
    ASSERT_TRUE(std::holds_alternative<bool>(result));
    EXPECT_TRUE(std::get<bool>(result));
}

// ============================================================================
// ARRAY TESTS
// ============================================================================

TEST(EvaluatorTest, EvaluatesEmptyArray) {
    Value result = EvaluatorTestHelper::eval("[]");

    ASSERT_TRUE(std::holds_alternative<ArrayPtr>(result));
    ArrayPtr arr = std::get<ArrayPtr>(result);
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->elements.size(), 0);
}

TEST(EvaluatorTest, EvaluatesArrayWithElements) {
    Value result = EvaluatorTestHelper::eval("[1, 2, 3]");

    ASSERT_TRUE(std::holds_alternative<ArrayPtr>(result));
    ArrayPtr arr = std::get<ArrayPtr>(result);
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->elements.size(), 3);

    EXPECT_DOUBLE_EQ(std::get<double>(arr->elements[0]), 1.0);
    EXPECT_DOUBLE_EQ(std::get<double>(arr->elements[1]), 2.0);
    EXPECT_DOUBLE_EQ(std::get<double>(arr->elements[2]), 3.0);
}

// ============================================================================
// OBJECT TESTS
// ============================================================================

TEST(EvaluatorTest, EvaluatesEmptyObject) {
    Value result = EvaluatorTestHelper::eval("{}");

    ASSERT_TRUE(std::holds_alternative<ObjectPtr>(result));
    ObjectPtr obj = std::get<ObjectPtr>(result);
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->properties.size(), 0);
}

TEST(EvaluatorTest, EvaluatesObjectWithProperties) {
    Value result = EvaluatorTestHelper::eval("{ jina: 'Alice', umri: 25 }");

    ASSERT_TRUE(std::holds_alternative<ObjectPtr>(result));
    ObjectPtr obj = std::get<ObjectPtr>(result);
    ASSERT_NE(obj, nullptr);

    ASSERT_EQ(obj->properties.count("jina"), 1);
    ASSERT_EQ(obj->properties.count("umri"), 1);

    EXPECT_EQ(std::get<std::string>(obj->properties["jina"].value), "Alice");
    EXPECT_DOUBLE_EQ(std::get<double>(obj->properties["umri"].value), 25.0);
}

// ============================================================================
// NULL TESTS
// ============================================================================

TEST(EvaluatorTest, EvaluatesNull) {
    Value result = EvaluatorTestHelper::eval("null");

    ASSERT_TRUE(std::holds_alternative<std::monostate>(result));
}

TEST(EvaluatorTest, NullIsNotEqualToNumber) {
    Value result = EvaluatorTestHelper::eval("null == 0");

    ASSERT_TRUE(std::holds_alternative<bool>(result));
    EXPECT_FALSE(std::get<bool>(result));
}

// ============================================================================
// TYPE CHECKING TESTS
// ============================================================================

TEST(EvaluatorTest, ChecksNumberType) {
    Value result = EvaluatorTestHelper::eval("(42).ninamba");
    ASSERT_TRUE(std::holds_alternative<bool>(result));
    EXPECT_TRUE(std::get<bool>(result));
}

TEST(EvaluatorTest, ChecksStringType) {
    Value result = EvaluatorTestHelper::eval("'hello'.nineno");
    ASSERT_TRUE(std::holds_alternative<bool>(result));
    EXPECT_TRUE(std::get<bool>(result));
}
