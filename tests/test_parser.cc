
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>

#include "lexer.hpp"
#include "parser.hpp"
#include "ast.hpp"
#include "token.hpp"
#include "SwaziError.hpp"

using namespace std;

// Helper: run lexer + parser
static unique_ptr<ProgramNode> parseProgram(const string& src, const string& filename = "<test>") {
    Lexer lx(src, filename);
    auto toks = lx.tokenize();
    Parser p(toks);
    return p.parse();
}

// Helper: get tokens
static vector<Token> lexTokens(const string& src, const string& filename = "<test>") {
    Lexer lx(src, filename);
    return lx.tokenize();
}

static bool tokens_contain(const vector<Token>& toks, TokenType t) {
    return any_of(toks.begin(), toks.end(), [&](const Token& tk){ return tk.type == t; });
}

// --- Tests --------------------------------------------------------------

TEST(ParserBasic, VariableDeclarationSimple) {
    string src = "data x = 42\n";
    unique_ptr<ProgramNode> prog;
    ASSERT_NO_THROW(prog = parseProgram(src));
    ASSERT_NE(prog, nullptr);
    ASSERT_GT(prog->body.size(), 0u);
    auto vd = dynamic_cast<VariableDeclarationNode*>(prog->body[0].get());
    ASSERT_NE(vd, nullptr);
    EXPECT_EQ(vd->identifier, "x");
}

TEST(ParserBasic, VariableDeclarationDestructureArray) {
    string src = "data [a, b, ...rest] = [1, 2, 3, 4]\n";
    unique_ptr<ProgramNode> prog;
    ASSERT_NO_THROW(prog = parseProgram(src));
    ASSERT_GT(prog->body.size(), 0u);
    auto vd = dynamic_cast<VariableDeclarationNode*>(prog->body[0].get());
    ASSERT_NE(vd, nullptr);
    EXPECT_TRUE(vd->identifier.empty());
    ASSERT_NE(vd->pattern, nullptr);
}

TEST(ParserImportExport, SideEffectImport) {
    string src = "tumia \"./mod\"\n";
    unique_ptr<ProgramNode> prog;
    ASSERT_NO_THROW(prog = parseProgram(src));
    ASSERT_GT(prog->body.size(), 0u);
    auto imp = dynamic_cast<ImportDeclarationNode*>(prog->body[0].get());
    ASSERT_NE(imp, nullptr);
    EXPECT_TRUE(imp->side_effect_only);
}

TEST(ParserImportExport, StarImport) {
    string src = "tumia * kutoka \"./lib\"\n";
    unique_ptr<ProgramNode> prog;
    ASSERT_NO_THROW(prog = parseProgram(src));
    auto imp = dynamic_cast<ImportDeclarationNode*>(prog->body[0].get());
    ASSERT_NE(imp, nullptr);
    EXPECT_TRUE(imp->import_all);
}

TEST(ParserImportExport, NamedImportList) {
    string src = "tumia { app, util kama u } kutoka \"./pkg\"\n";
    unique_ptr<ProgramNode> prog;
    ASSERT_NO_THROW(prog = parseProgram(src));
    auto imp = dynamic_cast<ImportDeclarationNode*>(prog->body[0].get());
    ASSERT_NE(imp, nullptr);
    ASSERT_EQ(imp->specifiers.size(), 2u);
}

TEST(ParserFunctions, SimpleFunctionDeclaration) {
    string src = "kazi add(a, b):\n  rudisha a + b\n";
    unique_ptr<ProgramNode> prog;
    ASSERT_NO_THROW(prog = parseProgram(src));
    ASSERT_GT(prog->body.size(), 0u);
    auto fn = dynamic_cast<FunctionDeclarationNode*>(prog->body[0].get());
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->name, "add");
    EXPECT_FALSE(fn->is_async);
}

TEST(ParserFunctions, AsyncFunction) {
    string src = "kazi ASYNC doit():\n  rudisha 1\n";
    unique_ptr<ProgramNode> prog;
    ASSERT_NO_THROW(prog = parseProgram(src));
    auto fn = dynamic_cast<FunctionDeclarationNode*>(prog->body[0].get());
    ASSERT_NE(fn, nullptr);
    EXPECT_TRUE(fn->is_async);
}

TEST(ParserFunctions, GeneratorFunctionDisallowedAsync) {
    string srcBad = "kazi* ASYNC bad():\n  rudisha 1\n";
    ASSERT_THROW(parseProgram(srcBad), SwaziError);
}

TEST(ParserClasses, ClassWithPropertyAndMethod) {
    string src = "muundo Person:\n  name = \"alice\"\n  tabia greet():\n    chapisha(name)\n";
    unique_ptr<ProgramNode> prog;
    ASSERT_NO_THROW(prog = parseProgram(src));
    auto cls = dynamic_cast<ClassDeclarationNode*>(prog->body[0].get());
    ASSERT_NE(cls, nullptr);
    EXPECT_EQ(cls->name->name, "Person");
}

TEST(ParserClasses, ConstructorAndDestructor) {
    string src = "muundo X:\n  X():\n    chapisha(\"ctor\")\n  ~X():\n    chapisha(\"dtor\")\n";
    unique_ptr<ProgramNode> prog;
    ASSERT_NO_THROW(prog = parseProgram(src));
    auto cls = dynamic_cast<ClassDeclarationNode*>(prog->body[0].get());
    ASSERT_NE(cls, nullptr);
    bool hasCtor = false, hasDtor = false;
    for (auto &m : cls->body->methods) {
        if (m->is_constructor) hasCtor = true;
        if (m->is_destructor) hasDtor = true;
    }
    EXPECT_TRUE(hasCtor);
    EXPECT_TRUE(hasDtor);
}

TEST(ParserControlFlow, IfElseIfElse) {
    string src = "kama x > 0:\n  chapisha(\"pos\")\nvinginevyo kama x == 0:\n  chapisha(\"zero\")\nvinginevyo:\n  chapisha(\"neg\")\n";
    unique_ptr<ProgramNode> prog;
    ASSERT_NO_THROW(prog = parseProgram(src));
    auto iff = dynamic_cast<IfStatementNode*>(prog->body[0].get());
    ASSERT_NE(iff, nullptr);
    EXPECT_TRUE(iff->has_else);
}

TEST(ParserControlFlow, ForInLoop) {
    string src = "kwa kila item katika arr:\n  chapisha(item)\n";
    unique_ptr<ProgramNode> p1;
    ASSERT_NO_THROW(p1 = parseProgram(src));
    auto forin = dynamic_cast<ForInStatementNode*>(p1->body[0].get());
    ASSERT_NE(forin, nullptr);
}

TEST(ParserControlFlow, WhileLoop) {
    string src = "wakati x < 10:\n  chapisha(x)\n";
    unique_ptr<ProgramNode> p;
    ASSERT_NO_THROW(p = parseProgram(src));
    auto w = dynamic_cast<WhileStatementNode*>(p->body[0].get());
    ASSERT_NE(w, nullptr);
}

TEST(ParserExceptions, TryCatchFinally) {
    string src = "jaribu:\n  chapisha(1)\nmakosa e:\n  chapisha(e)\nkisha:\n  chapisha(\"done\")\n";
    unique_ptr<ProgramNode> prog;
    ASSERT_NO_THROW(prog = parseProgram(src));
    auto tc = dynamic_cast<TryCatchNode*>(prog->body[0].get());
    ASSERT_NE(tc, nullptr);
    EXPECT_EQ(tc->errorVar, "e");
}

TEST(ParserExceptions, ThrowRequiresExpression) {
    string src = "throw\n";
    ASSERT_THROW(parseProgram(src), std::runtime_error);
}

TEST(ParserExpressions, TemplateLiteralSimple) {
    string src = "chapisha(`hello world`)\n";
    auto toks = lexTokens(src);
    if (!tokens_contain(toks, TokenType::TEMPLATE_STRING) && !tokens_contain(toks, TokenType::TEMPLATE_CHUNK)) {
        GTEST_SKIP() << "Template tokens not supported";
    }
    ASSERT_NO_THROW(parseProgram(src));
}

TEST(ParserExpressions, LambdaExpression) {
    string src = "chapisha((x) => x + 1)\n";
    unique_ptr<ProgramNode> p1;
    ASSERT_NO_THROW(p1 = parseProgram(src));
}

TEST(ParserExpressions, ObjectAndArrayLiterals) {
    string src = "chapisha({ x: 1, y })\nchapisha([1, 2, 3])\n";
    unique_ptr<ProgramNode> prog;
    ASSERT_NO_THROW(prog = parseProgram(src));
    ASSERT_GE(prog->body.size(), 2u);
}

TEST(ParserDiagnostics, UnexpectedTokenMessage) {
    string src = "kama\n";
    try {
        parseProgram(src);
        FAIL() << "Expected parse to throw";
    } catch (const std::runtime_error& e) {
        string msg = e.what() ? string(e.what()) : string();
        EXPECT_FALSE(msg.empty());
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

// Arithmetic & Binary Operations
TEST(ParserExpressions, ArithmeticOperations) {
    string src = "data x = 2 + 3 * 4 - 5 / 2\n";
    ASSERT_NO_THROW(parseProgram(src));
}

TEST(ParserExpressions, PowerOperator) {
    string src = "data x = 2 ** 3 ** 2\n";
    ASSERT_NO_THROW(parseProgram(src));
}

TEST(ParserExpressions, LogicalOperators) {
    string src = "data x = kweli na sikweli au kweli\n";
    ASSERT_NO_THROW(parseProgram(src));
}

TEST(ParserExpressions, ComparisonChain) {
    string src = "data x = a < b na b <= c na c > d\n";
    ASSERT_NO_THROW(parseProgram(src));
}

// Assignment Variants
TEST(ParserStatements, CompoundAssignment) {
    string src = "x += 5\ny -= 3\nz *= 2\n";
    ASSERT_NO_THROW(parseProgram(src));
}

TEST(ParserStatements, IncrementDecrement) {
    string src = "x++\ny--\n";
    ASSERT_NO_THROW(parseProgram(src));
}

TEST(ParserStatements, ArrayIndexAssignment) {
    string src = "arr[0] = 10\n";
    ASSERT_NO_THROW(parseProgram(src));
}

TEST(ParserStatements, MemberAssignment) {
    string src = "obj.prop = 42\n";
    ASSERT_NO_THROW(parseProgram(src));
}

// Function Features
TEST(ParserFunctions, DefaultParameters) {
    string src = "kazi greet(name = \"world\"):\n  chapisha(name)\n";
    auto prog = parseProgram(src);
    auto fn = dynamic_cast<FunctionDeclarationNode*>(prog->body[0].get());
    ASSERT_NE(fn, nullptr);
    ASSERT_GT(fn->parameters.size(), 0u);
    EXPECT_NE(fn->parameters[0]->defaultValue, nullptr);
}

TEST(ParserFunctions, RestParameters) {
    string src = "kazi sum(...nums):\n  rudisha 0\n";
    auto prog = parseProgram(src);
    auto fn = dynamic_cast<FunctionDeclarationNode*>(prog->body[0].get());
    ASSERT_NE(fn, nullptr);
    ASSERT_GT(fn->parameters.size(), 0u);
    EXPECT_TRUE(fn->parameters[0]->is_rest);
}

TEST(ParserFunctions, MixedParameters) {
    string src = "kazi func(a, b = 5, ...rest):\n  rudisha a\n";
    ASSERT_NO_THROW(parseProgram(src));
}

TEST(ParserFunctions, GeneratorFunction) {
    string src = "kazi* gen():\n  yield 1\n  yield 2\n";
    auto prog = parseProgram(src);
    auto fn = dynamic_cast<FunctionDeclarationNode*>(prog->body[0].get());
    ASSERT_NE(fn, nullptr);
    EXPECT_TRUE(fn->is_generator);
}

// Lambda Variations
TEST(ParserExpressions, LambdaNoParams) {
    string src = "data f = () => 42\n";
    ASSERT_NO_THROW(parseProgram(src));
}

TEST(ParserExpressions, LambdaBlockBody) {
    string src = "data f = (x) => { rudisha x * 2 }\n";
    ASSERT_NO_THROW(parseProgram(src));
}

TEST(ParserExpressions, AsyncLambda) {
    string src = "data f = ASYNC (x) => x\n";
    ASSERT_NO_THROW(parseProgram(src));
}

// Class Features
TEST(ParserClasses, StaticMethods) {
    string src = "muundo Util:\n  *tabia helper():\n    rudisha 1\n";
    ASSERT_NO_THROW(parseProgram(src));
}

TEST(ParserClasses, PrivateProperties) {
    string src = "muundo Box:\n  @secret = 42\n";
    auto prog = parseProgram(src);
    auto cls = dynamic_cast<ClassDeclarationNode*>(prog->body[0].get());
    ASSERT_NE(cls, nullptr);
    ASSERT_GT(cls->body->properties.size(), 0u);
    EXPECT_TRUE(cls->body->properties[0]->is_private);
}

TEST(ParserClasses, Inheritance) {
    string src = "muundo Child rithi Parent:\n  name = \"child\"\n";
    auto prog = parseProgram(src);
    auto cls = dynamic_cast<ClassDeclarationNode*>(prog->body[0].get());
    ASSERT_NE(cls, nullptr);
    EXPECT_NE(cls->superClass, nullptr);
}

TEST(ParserClasses, GetterMethod) {
    string src = "muundo Thing:\n  tabia thabiti value:\n    rudisha 42\n";
    ASSERT_NO_THROW(parseProgram(src));
}

// Control Flow
TEST(ParserControlFlow, SwitchStatement) {
    string src = "chagua x:\n  ikiwa 1:\n    chapisha(\"one\")\n  kaida:\n    chapisha(\"other\")\n";
    auto prog = parseProgram(src);
    auto sw = dynamic_cast<SwitchNode*>(prog->body[0].get());
    ASSERT_NE(sw, nullptr);
    EXPECT_GT(sw->cases.size(), 0u);
}

TEST(ParserControlFlow, DoWhileLoop) {
    string src = "fanya:\n  chapisha(x)\nwakati x > 0\n";
    auto prog = parseProgram(src);
    auto dw = dynamic_cast<DoWhileStatementNode*>(prog->body[0].get());
    ASSERT_NE(dw, nullptr);
}

TEST(ParserControlFlow, ClassicForLoop) {
    string src = "kwa (i = 0; i < 10; i++):\n  chapisha(i)\n";
    auto prog = parseProgram(src);
    auto f = dynamic_cast<ForStatementNode*>(prog->body[0].get());
    ASSERT_NE(f, nullptr);
}

TEST(ParserControlFlow, BreakContinue) {
    string src = "wakati kweli:\n  simama\nkwa kila x katika arr:\n  endelea\n";
    ASSERT_NO_THROW(parseProgram(src));
}

// Object & Array Features
TEST(ParserExpressions, SpreadInArray) {
    string src = "data arr = [1, ...mid, 3]\n";
    ASSERT_NO_THROW(parseProgram(src));
}

TEST(ParserExpressions, SpreadInObject) {
    string src = "data obj = { a: 1, ...other }\n";
    ASSERT_NO_THROW(parseProgram(src));
}

TEST(ParserExpressions, ComputedPropertyName) {
    string src = "data obj = { [key]: value }\n";
    ASSERT_NO_THROW(parseProgram(src));
}

TEST(ParserExpressions, ObjectMethod) {
    string src = "data obj = { tabia greet(): chapisha(\"hi\") }\n";
    ASSERT_NO_THROW(parseProgram(src));
}

// Optional Chaining
TEST(ParserExpressions, OptionalMember) {
    string src = "data x = obj?.prop\n";
    ASSERT_NO_THROW(parseProgram(src));
}

TEST(ParserExpressions, OptionalCall) {
    string src = "data x = func?.()\n";
    ASSERT_NO_THROW(parseProgram(src));
}

TEST(ParserExpressions, OptionalIndex) {
    string src = "data x = arr?.[0]\n";
    ASSERT_NO_THROW(parseProgram(src));
}

// Ternary
TEST(ParserExpressions, TernaryOperator) {
    string src = "data x = cond ? a : b\n";
    ASSERT_NO_THROW(parseProgram(src));
}

TEST(ParserExpressions, NestedTernary) {
    string src = "data x = a ? b : c ? d : e\n";
    ASSERT_NO_THROW(parseProgram(src));
}

// Special Constructs
TEST(ParserExpressions, NewExpression) {
    string src = "data obj = unda MyClass(1, 2)\n";
    ASSERT_NO_THROW(parseProgram(src));
}

TEST(ParserExpressions, SuperCall) {
    string src = "muundo Child rithi Parent:\n  Child():\n    super()\n";
    ASSERT_NO_THROW(parseProgram(src));
}

TEST(ParserExpressions, DeleteExpression) {
    string src = "futa obj\n";
    ASSERT_NO_THROW(parseProgram(src));
}

TEST(ParserExpressions, AwaitExpression) {
    string src = "kazi ASYNC f():\n  data x = await promise\n";
    ASSERT_NO_THROW(parseProgram(src));
}

TEST(ParserExpressions, YieldExpression) {
    string src = "kazi* gen():\n  yield 42\n";
    ASSERT_NO_THROW(parseProgram(src));
}

// Edge Cases
TEST(ParserEdgeCases, EmptyFunction) {
    string src = "kazi empty():\n  chapisha(\"\")\n";
    ASSERT_NO_THROW(parseProgram(src));
}

TEST(ParserEdgeCases, EmptyClass) {
    string src = "muundo Empty:\n  x = 1\n";
    ASSERT_NO_THROW(parseProgram(src));
}

TEST(ParserEdgeCases, NestedObjects) {
    string src = "data x = { a: { b: { c: 1 } } }\n";
    ASSERT_NO_THROW(parseProgram(src));
}

TEST(ParserEdgeCases, ChainedCalls) {
    string src = "obj.method1().method2().method3()\n";
    ASSERT_NO_THROW(parseProgram(src));
}

TEST(ParserEdgeCases, MixedBraceAndIndent) {
    string src = "kazi f() { rudisha 1 }\nkazi g():\n  rudisha 2\n";
    ASSERT_NO_THROW(parseProgram(src));
}

// Error Cases
TEST(ParserErrors, UnclosedBrace) {
    string src = "kazi f() {\n";
    ASSERT_THROW(parseProgram(src), std::runtime_error);
}

TEST(ParserErrors, MissingColon) {
    string src = "kazi f()\n  rudisha 1\n";
    ASSERT_THROW(parseProgram(src), std::runtime_error);
}

TEST(ParserErrors, InvalidAssignment) {
    string src = "5 = x\n";
    ASSERT_THROW(parseProgram(src), std::runtime_error);
}

TEST(ParserErrors, MultipleRestParams) {
    string src = "kazi f(...a, ...b):\n  rudisha 1\n";
    ASSERT_THROW(parseProgram(src), std::runtime_error);
}

TEST(ParserErrors, ParamAfterRest) {
    string src = "kazi f(...rest, x):\n  rudisha 1\n";
    ASSERT_THROW(parseProgram(src), std::runtime_error);
}
