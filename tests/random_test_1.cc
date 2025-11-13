#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "evaluator.hpp"
#include "lexer.hpp"
#include "parser.hpp"

// Use a convenient alias for the AST pointer type
using program = std::unique_ptr<ProgramNode>;

// Produce an AST from source. This may throw if lex/parse fails.
static program getAstFromSrc(const std::string& source) {
    Lexer lexer(source, "random_test.sl");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    return parser.parse();
}

// Try to parse+evaluate; return true if either parse or evaluation throws.
static bool parseOrEvalThrows(const std::string& src) {
    try {
        program ast = getAstFromSrc(src);
        Evaluator ev;
        ev.set_entry_point("<test>");
        ev.evaluate(ast.get());
        return false;  // no exception
    } catch (...) {
        return true;  // something threw
    }
}

TEST(RandomEvaluatorTest, EvaluateFunctionDeclaration) {
    // Use the language's indentation/colon style (parser expects `kazi name(...):` with indented body)
    std::string src = R"sw(
kazi fn():
  rudisha 0
)sw";

    // Ensure we can parse the program and evaluate it without throwing.
    program ast;
    ASSERT_NO_THROW(ast = getAstFromSrc(src));
    Evaluator evaluator;
    evaluator.set_entry_point("<test>");
    EXPECT_NO_THROW(evaluator.evaluate(ast.get()));
}

TEST(RandomEvaluatorTest, EvaluateFunctionDeclarationParams) {
    std::string src = R"sw(
kazi fn(a, b):
  rudisha a + b

fn(5, 7)
)sw";

    program ast;
    ASSERT_NO_THROW(ast = getAstFromSrc(src));
    Evaluator evaluator;
    evaluator.set_entry_point("<test>");
    EXPECT_NO_THROW(evaluator.evaluate(ast.get()));
}

TEST(RandomEvaluatorTest, EvaluateFunctionDeclarationParamsWithDefaultValue) {
    std::string src = R"sw(
kazi fn(a, b = 4):
  rudisha a + b

fn(5)
)sw";

    program ast;
    ASSERT_NO_THROW(ast = getAstFromSrc(src));
    Evaluator evaluator;
    evaluator.set_entry_point("<test>");
    EXPECT_NO_THROW(evaluator.evaluate(ast.get()));
}

TEST(RandomEvaluatorTest, EvaluateFunctionDeclarationParamsWithRestParams) {
    std::string src = R"sw(
kazi fn(a, ...rest):
  rudisha a + rest[0]

fn(5, 6, 8)
)sw";

    program ast;
    ASSERT_NO_THROW(ast = getAstFromSrc(src));
    Evaluator evaluator;
    evaluator.set_entry_point("<test>");
    EXPECT_NO_THROW(evaluator.evaluate(ast.get()));
}

TEST(RandomEvaluatorTest, EvaluateFnDeclParamsWithRestFiniteAndErrors) {
    // valid: rest with required finite count satisfied
    std::string src_ok = R"sw(
kazi fn(a, ...rest[3]):
  rudisha a + rest[0]

fn(5, 6, 8, 4)
)sw";

    // insufficient arguments for rest-required-count -> should either parse or evaluate to an error
    std::string src_insufficient = R"sw(
kazi fn(a, ...rest[3]):
  rudisha a + rest[0]

fn(5, 6, 8)
)sw";

    // invalid: rest followed by another parameter (syntax disallowed) -> parser or evaluator should error
    std::string src_invalid_rest_followed = R"sw(
kazi fn(a, ...rest, p):
  rudisha a + rest[0]

fn(5, 6, 8)
)sw";

    // First case must succeed
    program ast_ok;
    ASSERT_NO_THROW(ast_ok = getAstFromSrc(src_ok));
    Evaluator evaluator_ok;
    evaluator_ok.set_entry_point("<test>");
    EXPECT_NO_THROW(evaluator_ok.evaluate(ast_ok.get()));

    // The next two cases should produce an error at parse or evaluation time.
    EXPECT_TRUE(parseOrEvalThrows(src_insufficient));
    EXPECT_TRUE(parseOrEvalThrows(src_invalid_rest_followed));
}