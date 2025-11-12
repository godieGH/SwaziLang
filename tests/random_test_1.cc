
#include <gtest/gtest.h>
#include "evaluator.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include <sstream>
#include <memory>

using program = std::unique_ptr<ProgramNode>;

static program getAstFromSrc(const std::string& source) {
  Lexer lexer(source, "random_test.sl");
  std::vector<Token> tokens = lexer.tokenize();

  Parser parser(tokens);
  std::unique_ptr<ProgramNode> ast = parser.parse();
  
  return ast;
}


TEST(RandomEvaluatorTest, EvaluateFunctionDeclaration) {
  std::string src = R"(
  kazi fn() {
    
  })";
  Evaluator evaluator;
  evaluator.evaluate(getAstFromSrc(src).get());
}

TEST(RandomEvaluatorTest, EvaluateFunctionDeclarationParams) {
  std::string src = R"(
  kazi fn(a, b) {
    rudisha a + b
  }
  fn(5,7))";
  Evaluator evaluator;
  evaluator.evaluate(getAstFromSrc(src).get());
}

TEST(RandomEvaluatorTest, EvaluateFunctionDeclarationParamsWithDefaultValue) {
  std::string src = R"(
  kazi fn(a, b=4) {
    rudisha a + b
  }
  fn(5))";
  Evaluator evaluator;
  evaluator.evaluate(getAstFromSrc(src).get());
}

TEST(RandomEvaluatorTest, EvaluateFunctionDeclarationParamsWithRestParams) {
  std::string src = R"(
  kazi fn(a,...rest) {
    rudisha a + rest[0];
  }
  fn(5, 6, 8))";
  Evaluator evaluator;
  evaluator.evaluate(getAstFromSrc(src).get());
}

TEST(RandomEvaluatorTest, EvaluateFnDeclParamsWithRestFinite) {
  std::string src = R"(
  kazi fn(a,...rest[3]) {
    rudisha a + rest[0];
  }
  fn(5, 6, 8, 4))";
  std::string src_2 = R"(
  kazi fn(a,...rest[3]) {
    rudisha a + rest[0];
  }
  fn(5, 6, 8))";
  
  // no param needed after rest
  std::string src_3 = R"(
  kazi fn(a,...rest, p) {
    rudisha a + rest[0];
  }
  fn(5, 6, 8))";
  
  
  Evaluator evaluator;
  evaluator.evaluate(getAstFromSrc(src).get());
  
  ASSERT_THROW(evaluator.evaluate(getAstFromSrc(src_2).get()), std::runtime_error);
  ASSERT_THROW(evaluator.evaluate(getAstFromSrc(src_3).get()), std::runtime_error);
}