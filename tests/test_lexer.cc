#include <gtest/gtest.h>

#include "lexer.hpp"
#include "token.hpp"

// Helper to get token types from source
std::vector<TokenType> getTokenTypes(const std::string& source) {
    Lexer lexer(source, "<test>");
    auto tokens = lexer.tokenize();
    std::vector<TokenType> types;
    for (const auto& tok : tokens) {
        types.push_back(tok.type);
    }
    return types;
}

// Basic tokenization
TEST(LexerTest, TokenizesNumbers) {
    Lexer lexer("123", "<test>");
    auto tokens = lexer.tokenize();

    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::NUMBER);
    EXPECT_EQ(tokens[0].value, "123");
}

TEST(LexerTest, TokenizesFloats) {
    Lexer lexer("3.14", "<test>");
    auto tokens = lexer.tokenize();

    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::NUMBER);
    EXPECT_EQ(tokens[0].value, "3.14");
}

TEST(LexerTest, TokenizesIdentifiers) {
    Lexer lexer("variable", "<test>");
    auto tokens = lexer.tokenize();

    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[0].value, "variable");
}

// Keywords
TEST(LexerTest, TokenizesKeywords) {
    auto types = getTokenTypes("kama vinginevyo kazi");
    EXPECT_EQ(types[0], TokenType::KAMA);
    EXPECT_EQ(types[1], TokenType::VINGINEVYO);
    EXPECT_EQ(types[2], TokenType::KAZI);
}

TEST(LexerTest, TokenizesBooleans) {
    Lexer lexer("kweli sikweli", "<test>");
    auto tokens = lexer.tokenize();

    EXPECT_EQ(tokens[0].type, TokenType::BOOLEAN);
    EXPECT_EQ(tokens[0].value, "kweli");
    EXPECT_EQ(tokens[1].type, TokenType::BOOLEAN);
    EXPECT_EQ(tokens[1].value, "sikweli");
}

// Strings
TEST(LexerTest, TokenizesDoubleQuotedStrings) {
    Lexer lexer("\"hello world\"", "<test>");
    auto tokens = lexer.tokenize();

    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::STRING);
    EXPECT_EQ(tokens[0].value, "hello world");
}

TEST(LexerTest, TokenizesSingleQuotedStrings) {
    Lexer lexer("'hello'", "<test>");
    auto tokens = lexer.tokenize();

    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::SINGLE_QUOTED_STRING);
    EXPECT_EQ(tokens[0].value, "hello");
}

TEST(LexerTest, HandlesStringEscapes) {
    Lexer lexer("\"line1\\nline2\"", "<test>");
    auto tokens = lexer.tokenize();

    EXPECT_EQ(tokens[0].value, "line1\nline2");
}

// Operators
TEST(LexerTest, TokenizesArithmeticOperators) {
    auto types = getTokenTypes("a + b - c * d / e % f");
    EXPECT_EQ(types[0], TokenType::IDENTIFIER); // a
    EXPECT_EQ(types[1], TokenType::PLUS);
    EXPECT_EQ(types[2], TokenType::IDENTIFIER); // b
    EXPECT_EQ(types[3], TokenType::MINUS);
    EXPECT_EQ(types[4], TokenType::IDENTIFIER); // c
    EXPECT_EQ(types[5], TokenType::STAR);
    EXPECT_EQ(types[6], TokenType::IDENTIFIER); // d
    EXPECT_EQ(types[7], TokenType::SLASH);
    EXPECT_EQ(types[8], TokenType::IDENTIFIER); // e
    EXPECT_EQ(types[9], TokenType::PERCENT);
}
TEST(LexerTest, TokenizesComparisonOperators) {
    auto types = getTokenTypes("== != < > <= >=");
    EXPECT_EQ(types[0], TokenType::EQUALITY);
    EXPECT_EQ(types[1], TokenType::NOTEQUAL);
    EXPECT_EQ(types[2], TokenType::LESSTHAN);
    EXPECT_EQ(types[3], TokenType::GREATERTHAN);
    EXPECT_EQ(types[4], TokenType::LESSOREQUALTHAN);
    EXPECT_EQ(types[5], TokenType::GREATEROREQUALTHAN);
}

TEST(LexerTest, TokenizesLogicalOperators) {
    auto types = getTokenTypes("na au si && ||");
    EXPECT_EQ(types[0], TokenType::AND);
    EXPECT_EQ(types[1], TokenType::OR);
    EXPECT_EQ(types[2], TokenType::NOT);
    EXPECT_EQ(types[3], TokenType::AND);
    EXPECT_EQ(types[4], TokenType::OR);
}

TEST(LexerTest, TokenizesPowerOperator) {
    auto types = getTokenTypes("2 ** 3");
    EXPECT_EQ(types[0], TokenType::NUMBER);
    EXPECT_EQ(types[1], TokenType::POWER);
    EXPECT_EQ(types[2], TokenType::NUMBER);
}

// Comments
TEST(LexerTest, SkipsLineComments) {
    Lexer lexer("x # comment\ny", "<test>");
    auto tokens = lexer.tokenize();

    // Should have: x, NEWLINE, y, NEWLINE, EOF
    EXPECT_EQ(tokens[0].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[0].value, "x");
    EXPECT_EQ(tokens[1].type, TokenType::NEWLINE);
    EXPECT_EQ(tokens[2].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[2].value, "y");
}

TEST(LexerTest, SkipsBlockComments) {
    Lexer lexer("x /* comment */ y", "<test>");
    auto tokens = lexer.tokenize();

    EXPECT_EQ(tokens[0].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[0].value, "x");
    EXPECT_EQ(tokens[1].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[1].value, "y");
}

// Indentation
TEST(LexerTest, TracksIndentation) {
    Lexer lexer("x\n  y\nz", "<test>");
    auto tokens = lexer.tokenize();

    bool hasIndent = false;
    bool hasDedent = false;
    for (const auto& tok : tokens) {
        if (tok.type == TokenType::INDENT) hasIndent = true;
        if (tok.type == TokenType::DEDENT) hasDedent = true;
    }

    EXPECT_TRUE(hasIndent);
    EXPECT_TRUE(hasDedent);
}

// Template literals
TEST(LexerTest, TokenizesTemplateLiterals) {
    Lexer lexer("`hello`", "<test>");
    auto tokens = lexer.tokenize();

    EXPECT_EQ(tokens[0].type, TokenType::TEMPLATE_CHUNK);
    EXPECT_EQ(tokens[1].type, TokenType::TEMPLATE_END);
}

TEST(LexerTest, TokenizesTemplateInterpolation) {
    Lexer lexer("`hello ${name}`", "<test>");
    auto tokens = lexer.tokenize();

    EXPECT_EQ(tokens[0].type, TokenType::TEMPLATE_CHUNK);
    EXPECT_EQ(tokens[1].type, TokenType::TEMPLATE_EXPR_START);
    EXPECT_EQ(tokens[2].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[3].type, TokenType::TEMPLATE_EXPR_END);
}

// Special tokens
TEST(LexerTest, TokenizesLambda) {
    auto types = getTokenTypes("=>");
    EXPECT_EQ(types[0], TokenType::LAMBDA);
}

TEST(LexerTest, TokenizesEllipsis) {
    auto types = getTokenTypes("...");
    EXPECT_EQ(types[0], TokenType::ELLIPSIS);
}

TEST(LexerTest, TokenizesQuestionDot) {
    auto types = getTokenTypes("?.");
    EXPECT_EQ(types[0], TokenType::QUESTION_DOT);
}

// Location tracking
TEST(LexerTest, TracksTokenLocation) {
    Lexer lexer("x", "<test>");
    auto tokens = lexer.tokenize();

    EXPECT_EQ(tokens[0].loc.filename, "<test>");
    EXPECT_EQ(tokens[0].loc.line, 1);
    EXPECT_GT(tokens[0].loc.col, 0);
}

// Edge cases
TEST(LexerTest, HandlesEmptyInput) {
    Lexer lexer("", "<test>");
    auto tokens = lexer.tokenize();

    EXPECT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens.back().type, TokenType::EOF_TOKEN);
}

TEST(LexerTest, HandlesNumberUnderscore) {
    Lexer lexer("1_000", "<test>");
    auto tokens = lexer.tokenize();

    EXPECT_EQ(tokens[0].type, TokenType::NUMBER);
    EXPECT_EQ(tokens[0].value, "1000");
}

TEST(LexerTest, TokenizesIncrementDecrement) {
    auto types = getTokenTypes("++ --");
    EXPECT_EQ(types[0], TokenType::INCREMENT);
    EXPECT_EQ(types[1], TokenType::DECREMENT);
}

TEST(LexerTest, TokenizesCompoundAssignment) {
    auto types = getTokenTypes("+= -= *=");
    EXPECT_EQ(types[0], TokenType::PLUS_ASSIGN);
    EXPECT_EQ(types[1], TokenType::MINUS_ASSIGN);
    EXPECT_EQ(types[2], TokenType::TIMES_ASSIGN);
}